/*
 * cl_fill.c - see cl_fill.h.
 *
 * Generalises cl_clear.c's fixed-single-64x64-tile clear+store to an
 * arbitrary tiles_x x tiles_y grid, using the same emitters (cl_packets.h)
 * and the same explicit-per-tile TileCoordinates approach cl_clear.c
 * already uses for its one tile - NOT the reference's SupertileCoordinates/
 * generic-indirect-tile-list mechanism (BranchToImplicitTileList,
 * PrimListFormat, OP_TILE_COORDINATES_IMPLICIT, OP_RETURN_FROM_SUB_LIST -
 * see gl_rainbow_triangle.cpp's RCL). That mechanism exists to keep the
 * RCL compact when walking thousands of tiles for real primitive
 * rendering; a fill has no primitives and needs no shader/indirect-list
 * machinery at all, so explicitly unrolling one clear+store pair per
 * tile is simpler, avoids implementing packets this project has never
 * used before, and costs only ~40 bytes/tile (about 20KB of RCL for a
 * full 1920x1080 screen - trivial next to the pages already involved).
 *
 * MulticoreTileListSetBase/MulticoreSupertileCfg are still emitted with
 * real (non-1x1) values, even though SupertileCoordinates is never
 * used to walk them - they establish the tile_alloc addressing scheme
 * the binner used, which each explicit TileCoordinates call still
 * relies on to find its own tile's bin data. Confirmed this is a
 * genuinely separate concern from SupertileCoordinates by reading
 * TileBinningModeCfg's own packet (cl_packets.h) - it carries no
 * supertile parameters at all, only frame width/height, so supertile
 * grouping is purely an RCL/render-side addressing detail.
 *
 * v3d_fill_test (step 2) and v3d_fill_screen (step 3) share a common
 * core (run_fill) that builds and submits the control lists against
 * whatever target physical address/stride it's given - step 2 points
 * that at a freshly-allocated, padded PMP test buffer; step 3 points
 * it directly at the real screen framestore (no PMP allocation for the
 * render target at all - that memory already exists).
 */

#include <stddef.h>
#include "cl_fill.h"
#include "cl_builder.h"
#include "cl_packets.h"
#include "v3d_regs.h"
#include "io_map.h"
#include "v3d_irq.h"

#define TILE_SIZE 64u

static unsigned int div_round_up(unsigned int a, unsigned int b)
{
    return (a + b - 1u) / b;
}

static unsigned int bytes_to_pages(unsigned int bytes)
{
    return div_round_up(bytes, PAGE_SIZE);
}

/*
 * Picks the smallest supertile_w/supertile_h (starting at 1x1, growing
 * whichever dimension is currently smaller) that keeps the resulting
 * supertile-grid cell count under 256 - transcribed from the reference's
 * getSuperTiles(), gl_rainbow_triangle.cpp lines ~49-69, not derived
 * independently (the "grow whichever is smaller" tie-break and the
 * exact <256 threshold are both real choices from that source, not
 * obvious ones - e.g. a >= vs > off-by-one here would silently under-
 * or over-allocate the supertile grid).
 */
static void get_super_tiles(unsigned int tiles_x, unsigned int tiles_y,
                             unsigned int *supertile_w, unsigned int *supertile_h,
                             unsigned int *frame_w_supertile,
                             unsigned int *frame_h_supertile)
{
    unsigned int sw = 1u, sh = 1u, fw, fh;

    for (;;)
    {
        fw = div_round_up(tiles_x, sw);
        fh = div_round_up(tiles_y, sh);
        if (fw * fh < 256u)
            break;
        if (sw < sh)
            sw++;
        else
            sh++;
    }

    *supertile_w = sw;
    *supertile_h = sh;
    *frame_w_supertile = fw;
    *frame_h_supertile = fh;
}

/*
 * Waits for a CLE IRQ-driven completion counter to advance, exactly
 * like cl_clear.c's v3d_poll_counter, but without its rich
 * diagnose_timeout() diagnostics - deliberately NOT sharing that code
 * (duplicating this one small loop instead) to avoid touching the
 * hardware-proven M2/M3 path while this multi-tile generalisation is
 * still unproven. If a real hardware timeout ever shows up here, port
 * the diagnostics over then, with real failure data to shape them
 * around instead of guessing what's worth logging in advance.
 *
 * CIRCUIT BREAKER: bounded by real wall-clock time
 * (v3d_read_centiseconds, an OS_ReadMonotonicTime wrapper), not just a
 * raw spin-count - a tight loop's real-world duration depends on
 * system load, and a stalled GPU/interrupt pipeline doesn't make the
 * loop itself run any slower, so counting iterations alone doesn't
 * bound real elapsed time. Checked only every 4096 fast inner
 * iterations, not every single one, since a SWI call is far more
 * expensive than a plain memory read and doing it every iteration
 * would itself throttle the poll rate. Giving up here is always safe:
 * the caller's own existing fail-safe handling (see gfxv_accel.c)
 * leaves the GraphicsV call unclaimed on any error, falling back to
 * normal CPU rendering.
 */
extern unsigned int v3d_read_centiseconds(void);
#define CIRCUIT_BREAKER_TIMEOUT_CS 30u /* 300ms real time */

static _kernel_oserror *wait_for_irq(volatile unsigned int *counter,
                                      unsigned int initial,
                                      _kernel_oserror *timeout_err)
{
    unsigned int spin;
    unsigned int start_cs = 0;
    int have_start = 0;

    for (spin = 0; spin < 5000000u; spin++)
    {
        if (*counter != initial)
            return NULL;

        if ((spin & 0xFFFu) == 0u)
        {
            unsigned int now_cs = v3d_read_centiseconds();
            if (!have_start)
            {
                start_cs = now_cs;
                have_start = 1;
            }
            else if (now_cs - start_cs >= CIRCUIT_BREAKER_TIMEOUT_CS)
                return timeout_err;
        }
    }
    return timeout_err; /* absolute safety net if the clock itself is
                          * ever unavailable - see v3d_read_centiseconds's
                          * own defensive 0-on-error return */
}

/*
 * CACHED SCRATCH ALLOCATION - claims scratch pages ONCE per distinct
 * `base_page` (generously, for a worst-case full-screen-sized
 * operation), then reuses the same already-claimed pages on every
 * subsequent call at that base_page, rather than calling `pmp_alloc`
 * fresh every time. This matters beyond just avoiding SWI overhead:
 * `pmp_alloc`'s page-claim step (OS_DynamicArea reason 21)
 * unconditionally broadcasts `Service_MemoryMoved`, a genuinely
 * synchronous service call reaching directly into every registered
 * handler - including the Wimp module's own, which does real SVC-mode
 * bookkeeping. Every accelerated fill used to call `pmp_alloc` four
 * separate times (cl/rcl/tile_alloc/tile_state), each broadcasting
 * this from INSIDE a GraphicsV call the Wimp itself had just
 * dispatched to us - the Wimp's own reentrant bookkeeping running
 * nested inside a redraw it was still mid-dispatch of. This was the
 * real root cause of a hard machine freeze under specific live desktop
 * conditions (Task Manager's own Dynamic Areas display watching this
 * driver's own memory churn) - see README's "Task Manager freeze"
 * section for the full investigation.
 *
 * Safe by construction: the CL/RCL are always rebuilt from scratch
 * below, and tile_alloc/tile_state are always explicitly zeroed before
 * use, regardless of whether the underlying pages were freshly claimed
 * or are being reused - only the SOURCE of the physical/logical
 * addresses changes, none of the control-list-building or GPU-
 * submission logic does. A request exceeding the worst-case cached
 * size (an unusually large screen) falls back to the original uncached
 * behaviour for that one call rather than trying to grow the cache.
 */
#define WORST_CASE_TILES_X 30u  /* covers up to 1920px wide (30*64) */
#define WORST_CASE_TILES_Y 17u  /* covers up to 1088px tall (17*64) */
#define WORST_CASE_N_TILES (WORST_CASE_TILES_X * WORST_CASE_TILES_Y)
#define MAX_CACHED_SCRATCH 4u

typedef struct
{
    unsigned int base_page; /* 0xFFFFFFFFu = unused slot */
    unsigned int cl_phys;         void *cl_log;
    unsigned int rcl_phys;        void *rcl_log;        unsigned int rcl_pages;
    unsigned int tile_alloc_phys; void *tile_alloc_log;  unsigned int tile_alloc_pages;
    unsigned int tile_state_phys; void *tile_state_log;  unsigned int tile_state_pages;
} fill_scratch_cache_t;

static fill_scratch_cache_t g_fill_scratch_cache[MAX_CACHED_SCRATCH];
static unsigned int g_fill_scratch_cache_count = 0;

/*
 * Finds the cache slot for `base_page`, claiming one (at generous
 * worst-case sizing) if this is the first time it's been seen. Returns
 * NULL (with *err_out left NULL) if the actual request exceeds the
 * worst-case cached size or the cache table is full - the caller falls
 * back to its own original uncached pmp_alloc sequence in that case.
 * Returns NULL with *err_out SET if the one-time claim itself fails.
 */
static fill_scratch_cache_t *find_or_create_fill_scratch(pmp_area_t *pmp, unsigned int base_page,
                                                            unsigned int tile_alloc_pages_needed,
                                                            unsigned int tile_state_pages_needed,
                                                            unsigned int rcl_pages_needed,
                                                            _kernel_oserror **err_out)
{
    unsigned int i;
    fill_scratch_cache_t *slot;
    unsigned int worst_tile_alloc_bytes, worst_tile_state_bytes, worst_rcl_bytes;
    unsigned int worst_tile_alloc_pages, worst_tile_state_pages, worst_rcl_pages;
    unsigned int page;

    *err_out = NULL;

    for (i = 0; i < g_fill_scratch_cache_count; i++)
        if (g_fill_scratch_cache[i].base_page == base_page)
            return &g_fill_scratch_cache[i];

    if (g_fill_scratch_cache_count >= MAX_CACHED_SCRATCH)
        return NULL;

    worst_tile_alloc_bytes = WORST_CASE_N_TILES * TILE_SIZE;
    worst_tile_alloc_bytes = (worst_tile_alloc_bytes + 4095u) & ~4095u;
    worst_tile_alloc_bytes += TILE_SIZE * TILE_SIZE * 2u;
    worst_tile_alloc_pages = bytes_to_pages(worst_tile_alloc_bytes);

    worst_tile_state_bytes = WORST_CASE_N_TILES * 256u;
    worst_tile_state_pages = bytes_to_pages(worst_tile_state_bytes);

    worst_rcl_bytes = 64u + WORST_CASE_N_TILES * 40u + 64u;
    worst_rcl_pages = bytes_to_pages(worst_rcl_bytes);

    if (tile_alloc_pages_needed > worst_tile_alloc_pages ||
        tile_state_pages_needed > worst_tile_state_pages ||
        rcl_pages_needed > worst_rcl_pages)
        return NULL; /* bigger than our worst case - caller falls back */

    slot = &g_fill_scratch_cache[g_fill_scratch_cache_count];
    page = base_page;

    *err_out = pmp_alloc(pmp, page, 1u, &slot->cl_phys, &slot->cl_log);
    if (*err_out) return NULL;
    page += 1u;

    slot->rcl_pages = worst_rcl_pages;
    *err_out = pmp_alloc(pmp, page, worst_rcl_pages, &slot->rcl_phys, &slot->rcl_log);
    if (*err_out) return NULL;
    page += worst_rcl_pages;

    slot->tile_alloc_pages = worst_tile_alloc_pages;
    *err_out = pmp_alloc(pmp, page, worst_tile_alloc_pages, &slot->tile_alloc_phys, &slot->tile_alloc_log);
    if (*err_out) return NULL;
    page += worst_tile_alloc_pages;

    slot->tile_state_pages = worst_tile_state_pages;
    *err_out = pmp_alloc(pmp, page, worst_tile_state_pages, &slot->tile_state_phys, &slot->tile_state_log);
    if (*err_out) return NULL;

    slot->base_page = base_page;
    g_fill_scratch_cache_count++;
    return slot;
}

/*
 * Shared core: builds and submits a multi-tile clear+store control
 * list targeting `target_phys`/`target_stride` directly (the caller
 * decides whether that's a PMP test buffer or the real framestore),
 * using PMP pages starting at `base_page` for the CL/tile_alloc/
 * tile_state scratch buffers only - never for the render target
 * itself. Returns *tiles_x_out / *tiles_y_out (diagnostic) on success.
 */
static _kernel_oserror *run_fill(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int width, unsigned int height,
                                   unsigned int clear_color_rgba,
                                   unsigned int target_phys,
                                   unsigned int target_stride,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out)
{
    unsigned int tiles_x, tiles_y, n_tiles;
    unsigned int tile_alloc_bytes, tile_state_bytes, rcl_bytes;
    unsigned int tile_alloc_pages, tile_state_pages, rcl_pages;
    unsigned int supertile_w, supertile_h, frame_w_supertile, frame_h_supertile;
    unsigned int cl_phys, tile_alloc_phys, tile_state_phys, rcl_phys;
    void *cl_log, *tile_alloc_log, *tile_state_log, *rcl_log_ptr;
    _kernel_oserror *err;
    cl_buf_t bcl, rcl;
    unsigned int bcl_start, bcl_end, rcl_start, rcl_end;
    io_mapping_t v3d;
    unsigned int last_bin_irq, last_render_irq;
    unsigned int page = base_page;
    unsigned int col, row;

    if (width == 0u || height == 0u)
    {
        static _kernel_oserror err_bad_size = {
            10, "V3DDriver: fill width/height must be non-zero"
        };
        return &err_bad_size;
    }

    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);
    n_tiles = tiles_x * tiles_y;

    /* tile_alloc: align(N*64,4096) + 64*64*2 - transcribed verbatim
     * from gl_rainbow_triangle.cpp (see cl_fill.h's top comment); the
     * "+ 64*64*2" term is a literal constant (8192 bytes), not scaled
     * by tile count - confirmed by reading that source directly, not
     * inferred from cl_clear.c's single-tile case where the two would
     * have been indistinguishable (N=1 there too). */
    tile_alloc_bytes = n_tiles * TILE_SIZE;
    tile_alloc_bytes = (tile_alloc_bytes + 4095u) & ~4095u;
    tile_alloc_bytes += TILE_SIZE * TILE_SIZE * 2u;
    tile_alloc_pages = bytes_to_pages(tile_alloc_bytes);

    /* tile_state: 256 bytes/tile (V3D>=40 - this hardware is 4.2),
     * matching cl_clear.c's existing single-tile constant scaled up. */
    tile_state_bytes = n_tiles * 256u;
    tile_state_pages = bytes_to_pages(tile_state_bytes);

    /* RCL: fixed ~52-byte header + ~40 bytes/tile (two TileCoordinates
     * blocks per tile - clear, then store, matching cl_clear.c's
     * existing per-tile pattern) + 1 byte EndOfRendering, plus a page
     * of slack. BCL is unaffected by tile count (no primitives are
     * ever emitted regardless of how many tiles there are - see this
     * file's top comment) so it keeps cl_clear.c's fixed small size. */
    rcl_bytes = 64u + n_tiles * 40u + 64u;
    rcl_pages = bytes_to_pages(rcl_bytes);

    /* pmp_alloc caps a single call at PMP_MAX_PAGES_PER_CALL pages (see
     * phys_buffer.c - a fixed-size stack array, not a real hardware
     * limit). tile_alloc/tile_state/rcl stay comfortably under that
     * even at full 1920x1080 (32/3/5 pages respectively) - this only
     * ever bites the CALLER if it also PMP-allocates a huge render
     * buffer (v3d_fill_test's synthetic-buffer case), not here. */
    if (tile_alloc_pages > PMP_MAX_PAGES_PER_CALL || tile_state_pages > PMP_MAX_PAGES_PER_CALL || rcl_pages > PMP_MAX_PAGES_PER_CALL)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: fill rectangle too large - tile_alloc/"
                "tile_state/RCL scratch buffers exceed pmp_alloc's "
                "pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL)"
        };
        return &err_too_big;
    }

    {
        fill_scratch_cache_t *cache = find_or_create_fill_scratch(
            pmp, base_page, tile_alloc_pages, tile_state_pages, rcl_pages, &err);
        if (err) return err;

        if (cache)
        {
            cl_phys = cache->cl_phys; cl_log = cache->cl_log;
            rcl_phys = cache->rcl_phys; rcl_log_ptr = cache->rcl_log;
            tile_alloc_phys = cache->tile_alloc_phys; tile_alloc_log = cache->tile_alloc_log;
            tile_state_phys = cache->tile_state_phys; tile_state_log = cache->tile_state_log;
        }
        else
        {
            /* Bigger than our cached worst-case (or the cache table is
             * full) - fall back to the original uncached sequence for
             * just this one call. */
            err = pmp_alloc(pmp, page, 1u, &cl_phys, &cl_log);
            if (err) return err;
            page += 1u;

            err = pmp_alloc(pmp, page, rcl_pages, &rcl_phys, &rcl_log_ptr);
            if (err) return err;
            page += rcl_pages;

            err = pmp_alloc(pmp, page, tile_alloc_pages, &tile_alloc_phys, &tile_alloc_log);
            if (err) return err;
            page += tile_alloc_pages;

            err = pmp_alloc(pmp, page, tile_state_pages, &tile_state_phys, &tile_state_log);
            if (err) return err;
        }
    }

    /* Zero tile-alloc/tile-state scratch - see cl_clear.c's own comment
     * on why (recycled RISC OS pages, not guaranteed-zero fresh memory). */
    {
        unsigned int i;
        volatile unsigned int *p;

        p = (volatile unsigned int *) tile_alloc_log;
        for (i = 0; i < (tile_alloc_pages * PAGE_SIZE) / 4u; i++)
            p[i] = 0;

        p = (volatile unsigned int *) tile_state_log;
        for (i = 0; i < (tile_state_pages * PAGE_SIZE) / 4u; i++)
            p[i] = 0;
    }

    get_super_tiles(tiles_x, tiles_y, &supertile_w, &supertile_h,
                     &frame_w_supertile, &frame_h_supertile);

    /* --- BCL: identical shape to cl_clear.c's, just parameterised on
     * real width/height instead of the fixed 64x64 test constants. No
     * primitives, so nothing here scales with tile count. --- */
    cl_init(&bcl, cl_log, cl_phys, PAGE_SIZE);
    bcl_start = cl_paddr(&bcl);
    emit_tile_binning_mode_cfg(&bcl, width, height);
    emit_flush_vcd_cache(&bcl);
    emit_occlusion_query_counter(&bcl, 0);
    emit_start_tile_binning(&bcl);

    emit_clip_window(&bcl, 0, 0, width, height);
    emit_cfg_bits(&bcl, 1, 1, 1, 0, 0, 0, 0, 7, 0, 0, 1, 0, 0, 0, 0);
    emit_point_size(&bcl, 1.0f);
    emit_line_width(&bcl, 1.0f);
    emit_clipper_xy_scaling(&bcl, (width / 2.0f) * 256.0f, (height / 2.0f) * -256.0f);
    emit_clipper_z_scale_and_offset(&bcl, 0.5f, 0.5f);
    emit_clipper_z_min_max(&bcl, 0.0f, 1.0f);
    emit_viewport_offset(&bcl, width / 2, height / 2, 0, 0);
    emit_color_write_masks(&bcl, 0);
    emit_blend_constant_color(&bcl, 0, 0, 0, 0);
    emit_zero_all_flat_shade_flags(&bcl);
    emit_zero_all_non_perspective_flags(&bcl);
    emit_zero_all_centroid_flags(&bcl);
    emit_transform_feedback_specs(&bcl, 0, 0);
    emit_occlusion_query_counter(&bcl, 0);
    emit_sample_state(&bcl, 0xF, 1.0f);
    emit_vcm_cache_size(&bcl, 4, 4);
    emit_flush(&bcl);
    bcl_end = cl_paddr(&bcl);

    /* --- RCL: real multi-tile grid, one clear+store pair per tile --- */
    cl_init(&rcl, rcl_log_ptr, rcl_phys, rcl_pages * PAGE_SIZE);
    rcl_start = cl_paddr(&rcl);
    emit_tile_rendering_mode_cfg_common(&rcl, width, height, 2);
    emit_tile_rendering_mode_cfg_clear_colors(&rcl, 0, clear_color_rgba, 0);
    emit_tile_rendering_mode_cfg_color_rt0(&rcl, 0, 2, 0);
    emit_tile_rendering_mode_cfg_zs_clear(&rcl, 0, 1.0f);
    emit_tile_list_initial_block_size(&rcl, 0, 1);
    emit_multicore_tile_list_set_base(&rcl, tile_alloc_phys);
    emit_multicore_supertile_cfg(&rcl, supertile_w, supertile_h,
                                   frame_w_supertile, frame_h_supertile,
                                   tiles_x, tiles_y, 1, 0, 0);

    for (row = 0; row < tiles_y; row++)
    {
        for (col = 0; col < tiles_x; col++)
        {
            /*
             * FIX (2026-09-26, hardware-diagnosed): this used to add a
             * manual row*TILE_SIZE*stride + col*TILE_SIZE*4 offset to
             * target_phys per tile. Real hardware testing (a 4x3-tile
             * fill) showed only tiles at even multiples of 512 bytes
             * came out correct while odd multiples read back untouched
             * leftover memory - a pattern that only makes sense if the
             * CLE ALSO derives the column offset from the current
             * TileCoordinates internally and adds it to whatever "addr"
             * is given, so a manually-added offset was compounding with
             * the hardware's own. Matches the reference's own actual
             * usage in gl_rainbow_triangle.cpp - its single, shared
             * store command always passes the plain, un-offset
             * render_target.paddr(), every time, regardless of which
             * tile is selected. Passing the same base address here for
             * every tile - trusting the CLE to add the per-tile offset
             * itself - is the same fix, just with TileCoordinates set
             * explicitly instead of implicitly.
             */
            emit_tile_coordinates(&rcl, col, row);
            emit_end_of_loads(&rcl);
            emit_store_tile_buffer_none(&rcl);
            emit_clear_tile_buffers(&rcl);
            emit_end_of_tile_marker(&rcl);

            /* dummy store: workaround a race condition, per cl_clear.c's
             * own comment (same reference source) at the equivalent point */
            emit_tile_coordinates(&rcl, col, row);
            emit_end_of_loads(&rcl);
            emit_store_tile_buffer_rt0(&rcl, 27 /* RGBA8 */, target_stride, target_phys);
            emit_end_of_tile_marker(&rcl);
        }
    }

    emit_end_of_rendering(&rcl);
    rcl_end = cl_paddr(&rcl);

    /* --- kick off binning --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    IO_WORD(v3d, V3D_PTB_BPOS) = 0;
    v3d_flush_caches(&v3d);
    last_bin_irq = g_v3d_irq_bin_done_count;

    IO_WORD(v3d, V3D_CLE_CT0QMA) = tile_alloc_phys;
    IO_WORD(v3d, V3D_CLE_CT0QMS) = tile_alloc_pages * PAGE_SIZE;
    IO_WORD(v3d, V3D_CLE_CT0QTS) = tile_state_phys | V3D_CLE_CT0QTS_ENABLE;
    IO_WORD(v3d, V3D_CLE_CT0QBA) = bcl_start;
    IO_WORD(v3d, V3D_CLE_CT0QEA) = bcl_end;

    io_unmap(&v3d);

    {
        static _kernel_oserror err_bin_timeout = {
            11, "V3DDriver: fill timed out waiting for binning"
        };
        err = wait_for_irq(&g_v3d_irq_bin_done_count, last_bin_irq, &err_bin_timeout);
        if (err) return err;
    }

    /* --- kick off rendering --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    v3d_flush_caches(&v3d);
    last_render_irq = g_v3d_irq_render_done_count;

    IO_WORD(v3d, V3D_CLE_CT1QBA) = rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = rcl_end;

    io_unmap(&v3d);

    {
        static _kernel_oserror err_render_timeout = {
            12, "V3DDriver: fill timed out waiting for rendering"
        };
        err = wait_for_irq(&g_v3d_irq_render_done_count, last_render_irq, &err_render_timeout);
        if (err) return err;
    }

    *tiles_x_out = tiles_x;
    *tiles_y_out = tiles_y;
    return NULL;
}

_kernel_oserror *v3d_fill_test(pmp_area_t *pmp, unsigned int base_page,
                                 unsigned int width, unsigned int height,
                                 unsigned int clear_color_rgba,
                                 unsigned int *target_phys_out,
                                 void **target_log_out,
                                 unsigned int *target_stride_bytes_out,
                                 unsigned int *tiles_x_out,
                                 unsigned int *tiles_y_out)
{
    unsigned int tiles_x, tiles_y, padded_w, padded_h, stride, render_bytes, render_pages;
    unsigned int render_phys;
    void *render_log;
    _kernel_oserror *err;

    if (width == 0u || height == 0u)
    {
        static _kernel_oserror err_bad_size = {
            10, "V3DDriver: V3D_FillTest width/height must be non-zero"
        };
        return &err_bad_size;
    }

    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);

    /* Padded to a whole tile grid, not just width x height - see
     * cl_fill.h's own comment for why (a partial edge tile's full-tile
     * store writing past the declared frame was an open question this
     * test buffer sidesteps rather than assumed an answer to - now
     * settled by hardware testing, see README, but the padding is kept
     * here as a harmless safety margin for this synthetic buffer). */
    padded_w = tiles_x * TILE_SIZE;
    padded_h = tiles_y * TILE_SIZE;
    stride = padded_w * 4u;
    render_bytes = stride * padded_h;
    render_pages = bytes_to_pages(render_bytes);

    /* Same pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL) discussed in run_fill - this one specifically
     * bounds how big a rectangle THIS TEST HARNESS can exercise via its
     * synthetic PMP render buffer (a full 1920x1088 padded buffer alone
     * is ~2040 pages). Does not bound the real feature (v3d_fill_screen
     * never PMP-allocates a render target at all). */
    if (render_pages > PMP_MAX_PAGES_PER_CALL)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: V3D_FillTest rectangle too large for this test "
                "harness (pmp_alloc's pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL)) - try a smaller "
                "width/height. Not a limit of the fill algorithm itself."
        };
        return &err_too_big;
    }

    err = pmp_alloc(pmp, base_page, render_pages, &render_phys, &render_log);
    if (err) return err;

    err = run_fill(pmp, base_page + render_pages, width, height, clear_color_rgba,
                    render_phys, stride, tiles_x_out, tiles_y_out);
    if (err) return err;

    *target_phys_out = render_phys;
    *target_log_out = render_log;
    *target_stride_bytes_out = stride;
    return NULL;
}

_kernel_oserror *v3d_fill_screen(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int x, unsigned int y,
                                   unsigned int width, unsigned int height,
                                   unsigned int clear_color_rgba,
                                   unsigned int screen_phys_base,
                                   unsigned int screen_stride,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out)
{
    unsigned int target_phys = screen_phys_base + y * screen_stride + x * 4u;

    return run_fill(pmp, base_page, width, height, clear_color_rgba,
                     target_phys, screen_stride, tiles_x_out, tiles_y_out);
}

_kernel_oserror *v3d_fill_raw(pmp_area_t *pmp, unsigned int base_page,
                                unsigned int width, unsigned int height,
                                unsigned int clear_color_rgba,
                                unsigned int target_phys,
                                unsigned int target_stride,
                                unsigned int *tiles_x_out,
                                unsigned int *tiles_y_out)
{
    return run_fill(pmp, base_page, width, height, clear_color_rgba,
                     target_phys, target_stride, tiles_x_out, tiles_y_out);
}

_kernel_oserror *v3d_striped_fill_raw(pmp_area_t *pmp, unsigned int base_page,
                                        unsigned int width, unsigned int height,
                                        const unsigned int colours[8],
                                        unsigned int row_phase,
                                        unsigned int target_phys_row0,
                                        unsigned int target_stride)
{
    unsigned int c;

    /* row_phase is which colours[] index the rectangle's own row 0
     * (target_phys_row0) falls on - see cl_fill.h's own comment for
     * why this matters (ECF patterns are screen-anchored, not
     * rectangle-anchored, to avoid a "marching ants" effect as content
     * scrolls behind a fixed dither pattern - unconfirmed by the PRM
     * text itself, but standard RISC OS dither-pattern behaviour;
     * callers unsure of the real anchor can just pass 0, treating the
     * rectangle's own top as the pattern's own top). For each distinct
     * colour index c, the first row needing it is the smallest row
     * R >= 0 with (row_phase + R) % 8 == c, i.e. R = (c - row_phase +
     * 8) % 8 - then every 8th row after that, until height is reached.
     */
    for (c = 0; c < 8u; c++)
    {
        unsigned int first_row = (c + 8u - (row_phase % 8u)) % 8u;
        unsigned int num_rows, dest_phys, dest_stride;
        unsigned int tiles_x, tiles_y;
        _kernel_oserror *err;

        if (first_row >= height)
            continue; /* this colour index never occurs in this rect */

        num_rows = 1u + (height - 1u - first_row) / 8u;
        dest_phys = target_phys_row0 + first_row * target_stride;
        dest_stride = target_stride * 8u;

        err = run_fill(pmp, base_page, width, num_rows, colours[c],
                        dest_phys, dest_stride, &tiles_x, &tiles_y);
        if (err)
            return err;
    }

    return NULL;
}

_kernel_oserror *v3d_striped_fill_test(pmp_area_t *pmp, unsigned int base_page,
                                         unsigned int width, unsigned int height,
                                         const unsigned int colours[8],
                                         unsigned int row_phase,
                                         unsigned int *target_phys_out,
                                         void **target_log_out,
                                         unsigned int *target_stride_bytes_out)
{
    unsigned int tiles_x, tiles_y, padded_w, padded_h, stride, render_bytes, render_pages;
    unsigned int render_phys;
    void *render_log;
    _kernel_oserror *err;

    if (width == 0u || height == 0u)
    {
        static _kernel_oserror err_bad_size = {
            10, "V3DDriver: V3D_StripedFillTest width/height must be non-zero"
        };
        return &err_bad_size;
    }

    /* Same padded-to-a-whole-tile-grid synthetic buffer as v3d_fill_test
     * - see its own comment. Padding is computed from the REAL height,
     * not the inflated per-stripe height each run_fill call underneath
     * actually sees, since it must be big enough to hold every stripe's
     * rows at their real (non-inflated) positions. */
    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);
    padded_w = tiles_x * TILE_SIZE;
    padded_h = tiles_y * TILE_SIZE;
    stride = padded_w * 4u;
    render_bytes = stride * padded_h;
    render_pages = bytes_to_pages(render_bytes);

    if (render_pages > PMP_MAX_PAGES_PER_CALL)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: V3D_StripedFillTest rectangle too large for "
                "this test harness (pmp_alloc's per-call page cap) - try "
                "a smaller width/height."
        };
        return &err_too_big;
    }

    err = pmp_alloc(pmp, base_page, render_pages, &render_phys, &render_log);
    if (err) return err;

    err = v3d_striped_fill_raw(pmp, base_page + render_pages, width, height,
                                 colours, row_phase, render_phys, stride);
    if (err) return err;

    *target_phys_out = render_phys;
    *target_log_out = render_log;
    *target_stride_bytes_out = stride;
    return NULL;
}
