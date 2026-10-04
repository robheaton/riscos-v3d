/*
 * cl_copy.c - see cl_copy.h.
 *
 * Structurally mirrors cl_fill.c's run_fill almost exactly (tile/
 * tile_state sizing, supertile selection, BCL, submission/IRQ-wait) -
 * see that file's own comments for the sourcing/derivation of each of
 * those pieces, not repeated here. The only real difference is the
 * per-tile RCL body: Load(source)+EndOfLoads+Store(dest) instead of
 * Store-none+Clear+Store.
 */

#include <stddef.h>
#include "cl_copy.h"
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

/* Identical to cl_fill.c's own get_super_tiles - see its comment for
 * the derivation. Not shared via a header to keep cl_fill.c and
 * cl_copy.c independently diffable while this is new/unproven. */
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
 * CIRCUIT BREAKER - see cl_fill.c's own wait_for_irq for the full
 * writeup. Bounded by real wall-clock time (v3d_read_centiseconds),
 * not just a spin count, since a backlogged GPU/interrupt pipeline
 * doesn't make the loop itself run any slower. Checked every 4096
 * iterations to avoid throttling the poll rate with a SWI call every
 * single spin. Giving up here is always safe - the caller's existing
 * fail-safe handling (gfxv_accel.c) leaves the call unclaimed on any
 * error, falling back to normal CPU rendering.
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

        if ((spin & 0xFFFFu) == 0xFFFFu)
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
    return timeout_err;
}

/*
 * CACHED SCRATCH ALLOCATION - see cl_fill.c's own identical mechanism
 * (find_or_create_fill_scratch) for the full writeup (README's "Task
 * Manager freeze" section) - not shared via a header, same "keep
 * cl_fill.c and cl_copy.c independently diffable" reasoning as
 * get_super_tiles/wait_for_irq above. Claims scratch pages ONCE per
 * distinct base_page (generously, worst-case full-screen sized), then
 * reuses them on every subsequent call, skipping pmp_alloc (and the
 * Service_MemoryMoved broadcast it triggers) entirely after the first.
 */
#define WORST_CASE_TILES_X 30u
#define WORST_CASE_TILES_Y 17u
#define WORST_CASE_N_TILES (WORST_CASE_TILES_X * WORST_CASE_TILES_Y)
#define MAX_CACHED_SCRATCH 4u

/*
 * Real budget for the tiny dedicated v3d_copy_test_16bpp PMP slot
 * (module.c's COPY_TEST_16BPP_EXTRA_PAGES) - MUST be kept equal to
 * that constant by hand (no shared header between module.c and this
 * file for it - same "independently diffable" reasoning as
 * get_super_tiles/wait_for_irq above). Added 2026-09-30 after an
 * adversarial code review found the ORIGINAL bounds check below only
 * compared against the generic PMP_MAX_PAGES_PER_CALL=2048 cap, which
 * is far bigger than this dedicated slot actually is - a moderately
 * large test call (e.g. 200x200) would have silently overflowed past
 * this slot's real reservation and aliased V3D_FillTest16bpp's
 * adjacent slot instead of failing cleanly, corrupting whichever
 * test's buffer was read back later. The fixed worst-case copy-
 * scratch cost below (1 cl + 6 rcl + 10 tile_alloc + 32 tile_state =
 * 49 pages, via find_or_create_copy_scratch, ALWAYS claimed regardless
 * of actual request size - see that function's own sizing) leaves only
 * SLOT_PAGES-SCRATCH_WORST_CASE_PAGES for the two (src+dest) render
 * buffers COMBINED.
 */
#define COPY_TEST_16BPP_SLOT_PAGES 100u
#define COPY_TEST_16BPP_SCRATCH_WORST_CASE_PAGES 49u

typedef struct
{
    unsigned int base_page;
    unsigned int cl_phys;         void *cl_log;
    unsigned int rcl_phys;        void *rcl_log;        unsigned int rcl_pages;
    unsigned int tile_alloc_phys; void *tile_alloc_log;  unsigned int tile_alloc_pages;
    unsigned int tile_state_phys; void *tile_state_log;  unsigned int tile_state_pages;
} copy_scratch_cache_t;

static copy_scratch_cache_t g_copy_scratch_cache[MAX_CACHED_SCRATCH];
static unsigned int g_copy_scratch_cache_count = 0;

static copy_scratch_cache_t *find_or_create_copy_scratch(pmp_area_t *pmp, unsigned int base_page,
                                                            unsigned int tile_alloc_pages_needed,
                                                            unsigned int tile_state_pages_needed,
                                                            unsigned int rcl_pages_needed,
                                                            _kernel_oserror **err_out)
{
    unsigned int i;
    copy_scratch_cache_t *slot;
    unsigned int worst_tile_alloc_bytes, worst_tile_state_bytes, worst_rcl_bytes;
    unsigned int worst_tile_alloc_pages, worst_tile_state_pages, worst_rcl_pages;
    unsigned int page;

    *err_out = NULL;

    for (i = 0; i < g_copy_scratch_cache_count; i++)
        if (g_copy_scratch_cache[i].base_page == base_page)
            return &g_copy_scratch_cache[i];

    if (g_copy_scratch_cache_count >= MAX_CACHED_SCRATCH)
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
        return NULL;

    slot = &g_copy_scratch_cache[g_copy_scratch_cache_count];
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
    g_copy_scratch_cache_count++;
    return slot;
}

/*
 * Shared core: builds and submits a multi-tile Load+Store control list
 * copying from `src_phys` (row stride `src_stride`) to `dest_phys` (row
 * stride `dest_stride`) - separate strides so this can also copy
 * between two buffers of different widths (needed for the overlap-safe
 * scratch-buffer path below; real GraphicsV op 1 copies within the
 * same framestore, so its two calls always pass the same stride
 * twice). No ClearTileBuffers at all - every tile is populated from
 * `src_phys` via LoadTileBufferGeneral, not cleared.
 */
static _kernel_oserror *run_copy(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int width, unsigned int height,
                                   unsigned int src_phys, unsigned int dest_phys,
                                   unsigned int src_stride, unsigned int dest_stride,
                                   unsigned int format_code,
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
            10, "V3DDriver: copy width/height must be non-zero"
        };
        return &err_bad_size;
    }

    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);
    n_tiles = tiles_x * tiles_y;

    /* Same sizing formulas as cl_fill.c's run_fill - see its comments
     * for the reference-source derivation. */
    tile_alloc_bytes = n_tiles * TILE_SIZE;
    tile_alloc_bytes = (tile_alloc_bytes + 4095u) & ~4095u;
    tile_alloc_bytes += TILE_SIZE * TILE_SIZE * 2u;
    tile_alloc_pages = bytes_to_pages(tile_alloc_bytes);

    tile_state_bytes = n_tiles * 256u;
    tile_state_pages = bytes_to_pages(tile_state_bytes);

    /* RCL: ~52-byte header + ~24 bytes/tile (one TileCoordinates block
     * per tile - Load+EndOfLoads+Store+EndOfTileMarker, no dummy-store
     * workaround and no Clear, both fill-specific) + slack. */
    rcl_bytes = 64u + n_tiles * 40u + 64u;
    rcl_pages = bytes_to_pages(rcl_bytes);

    if (tile_alloc_pages > PMP_MAX_PAGES_PER_CALL || tile_state_pages > PMP_MAX_PAGES_PER_CALL || rcl_pages > PMP_MAX_PAGES_PER_CALL)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: copy rectangle too large - tile_alloc/"
                "tile_state/RCL scratch buffers exceed pmp_alloc's "
                "pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL)"
        };
        return &err_too_big;
    }

    {
        copy_scratch_cache_t *cache = find_or_create_copy_scratch(
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

    /* --- BCL: identical shape to cl_fill.c's - no primitives, so
     * nothing here differs between a fill and a copy. --- */
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

    /* --- RCL: one Load(source)+Store(dest) pair per tile, no Clear. --- */
    cl_init(&rcl, rcl_log_ptr, rcl_phys, rcl_pages * PAGE_SIZE);
    rcl_start = cl_paddr(&rcl);
    emit_tile_rendering_mode_cfg_common(&rcl, width, height, 2);
    emit_tile_rendering_mode_cfg_clear_colors(&rcl, 0, 0, 0);
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
            /* Same base-address-only addressing as cl_fill.c's store -
             * the CLE derives each tile's offset itself from the
             * preceding TileCoordinates, for BOTH Load and Store (an
             * inference from the fill fix, not yet independently
             * hardware-confirmed for Load specifically - that's
             * exactly what this file's own test exists to check). */
            emit_tile_coordinates(&rcl, col, row);
            emit_load_tile_buffer_rt0(&rcl, format_code, src_stride, src_phys);
            emit_end_of_loads(&rcl);
            emit_store_tile_buffer_rt0(&rcl, format_code, dest_stride, dest_phys);
            emit_end_of_tile_marker(&rcl);
        }
    }

    emit_end_of_rendering(&rcl);
    rcl_end = cl_paddr(&rcl);

    err = v3d_regs_map(&v3d);
    if (err) return err;

    IO_WORD(v3d, V3D_PTB_BPOS) = 0;
    v3d_flush_caches(&v3d);
    last_bin_irq = g_v3d_irq_bin_done_count;

    IO_WORD(v3d, V3D_CLE_CT0QMA) = tile_alloc_phys;
    IO_WORD(v3d, V3D_CLE_CT0QMS) = tile_alloc_pages * PAGE_SIZE;
    IO_WORD(v3d, V3D_CLE_CT0QTS) = tile_state_phys | V3D_CLE_CT0QTS_ENABLE;
    IO_WORD(v3d, V3D_CLE_CT0QBA) = bcl_start;
    IO_WORD(v3d, V3D_CLE_CT0QEA) = bcl_end;

    v3d_regs_unmap(&v3d);

    {
        static _kernel_oserror err_bin_timeout = {
            11, "V3DDriver: copy timed out waiting for binning"
        };
        err = wait_for_irq(&g_v3d_irq_bin_done_count, last_bin_irq, &err_bin_timeout);
        if (err) return err;
    }

    err = v3d_regs_map(&v3d);
    if (err) return err;

    v3d_flush_caches(&v3d);
    last_render_irq = g_v3d_irq_render_done_count;

    IO_WORD(v3d, V3D_CLE_CT1QBA) = rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = rcl_end;

    v3d_regs_unmap(&v3d);

    {
        static _kernel_oserror err_render_timeout = {
            12, "V3DDriver: copy timed out waiting for rendering"
        };
        err = wait_for_irq(&g_v3d_irq_render_done_count, last_render_irq, &err_render_timeout);
        if (err) return err;
    }

    *tiles_x_out = tiles_x;
    *tiles_y_out = tiles_y;
    return NULL;
}

_kernel_oserror *v3d_copy_test(pmp_area_t *pmp, unsigned int base_page,
                                 unsigned int width, unsigned int height,
                                 void **src_log_out, void **dest_log_out,
                                 unsigned int *stride_bytes_out,
                                 unsigned int *tiles_x_out,
                                 unsigned int *tiles_y_out)
{
    unsigned int tiles_x, tiles_y, padded_w, padded_h, stride, buf_bytes, buf_pages;
    unsigned int src_phys, dest_phys;
    void *src_log, *dest_log;
    _kernel_oserror *err;
    unsigned int x, y;

    if (width == 0u || height == 0u)
    {
        static _kernel_oserror err_bad_size = {
            10, "V3DDriver: V3D_CopyTest width/height must be non-zero"
        };
        return &err_bad_size;
    }

    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);
    padded_w = tiles_x * TILE_SIZE;
    padded_h = tiles_y * TILE_SIZE;
    stride = padded_w * 4u;
    buf_bytes = stride * padded_h;
    buf_pages = bytes_to_pages(buf_bytes);

    if (buf_pages > PMP_MAX_PAGES_PER_CALL)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: V3D_CopyTest rectangle too large for this "
                "test harness (pmp_alloc's pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL))"
        };
        return &err_too_big;
    }

    err = pmp_alloc(pmp, base_page, buf_pages, &src_phys, &src_log);
    if (err) return err;

    err = pmp_alloc(pmp, base_page + buf_pages, buf_pages, &dest_phys, &dest_log);
    if (err) return err;

    /* Fill source with a per-pixel test pattern (not a flat colour -
     * a flat colour can't distinguish "genuinely copied" from "left
     * over from a previous test"/"happened to already be there").
     * Dest is deliberately left whatever recycled content it already
     * has - untouched pixels after the copy should NOT match this
     * pattern, confirming the copy actually wrote them. */
    for (y = 0; y < padded_h; y++)
    {
        volatile unsigned int *row_ptr =
            (volatile unsigned int *) ((unsigned char *) src_log + y * stride);
        for (x = 0; x < padded_w; x++)
            row_ptr[x] = 0xFF000000u | (x << 12) | y;
    }

    err = run_copy(pmp, base_page + 2u * buf_pages, width, height,
                    src_phys, dest_phys, stride, stride, 27 /* RGBA8 */,
                    tiles_x_out, tiles_y_out);
    if (err) return err;

    *src_log_out = src_log;
    *dest_log_out = dest_log;
    *stride_bytes_out = stride;
    return NULL;
}

/*
 * 16bpp screen-mode-support Phase 3 diagnostic (2026-09-30): isolated
 * synthetic-buffer proof for the copy path's LOAD side at 16-bit
 * width, before anything touches the real screen framestore. Phase 2
 * only proved the STORE side (a GPU-side clear+store, never a load);
 * emit_load_tile_buffer_rt0 was already flagged elsewhere in this
 * project as "genuinely new territory, never exercised in the
 * reference implementation" even at the already-proven 32bpp width,
 * and has never been tested at 16-bit granularity at all until this.
 *
 * Near-identical to v3d_copy_test above, with three changes: 2-byte
 * stride instead of 4, format_code=45 (RGBA5551 - confirmed correct
 * for RISC OS's real 16bpp layout, see README/memory) instead of 27,
 * and a 16-bit-safe test pattern (x/y both masked to 5 bits so the
 * combined value always fits 16 bits, still unique enough per-pixel to
 * tell "genuinely copied" apart from leftover content).
 */
_kernel_oserror *v3d_copy_test_16bpp(pmp_area_t *pmp, unsigned int base_page,
                                       unsigned int width, unsigned int height,
                                       void **src_log_out, void **dest_log_out,
                                       unsigned int *stride_bytes_out,
                                       unsigned int *tiles_x_out,
                                       unsigned int *tiles_y_out)
{
    unsigned int tiles_x, tiles_y, padded_w, padded_h, stride, buf_bytes, buf_pages;
    unsigned int src_phys, dest_phys;
    void *src_log, *dest_log;
    _kernel_oserror *err;
    unsigned int x, y;

    if (width == 0u || height == 0u)
    {
        static _kernel_oserror err_bad_size = {
            10, "V3DDriver: V3D_CopyTest16bpp width/height must be non-zero"
        };
        return &err_bad_size;
    }

    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);
    padded_w = tiles_x * TILE_SIZE;
    padded_h = tiles_y * TILE_SIZE;
    stride = padded_w * 2u;
    buf_bytes = stride * padded_h;
    buf_pages = bytes_to_pages(buf_bytes);

    if (buf_pages > PMP_MAX_PAGES_PER_CALL ||
        2u * buf_pages > COPY_TEST_16BPP_SLOT_PAGES - COPY_TEST_16BPP_SCRATCH_WORST_CASE_PAGES)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: V3D_CopyTest16bpp rectangle too large for its "
                "dedicated test PMP slot - try a smaller width/height"
        };
        return &err_too_big;
    }

    err = pmp_alloc(pmp, base_page, buf_pages, &src_phys, &src_log);
    if (err) return err;

    err = pmp_alloc(pmp, base_page + buf_pages, buf_pages, &dest_phys, &dest_log);
    if (err) return err;

    for (y = 0; y < padded_h; y++)
    {
        volatile unsigned short *row_ptr =
            (volatile unsigned short *) ((unsigned char *) src_log + y * stride);
        for (x = 0; x < padded_w; x++)
            row_ptr[x] = (unsigned short) (((x & 0x1Fu) << 5) | (y & 0x1Fu));
    }

    err = run_copy(pmp, base_page + 2u * buf_pages, width, height,
                    src_phys, dest_phys, stride, stride, 45 /* RGBA5551 */,
                    tiles_x_out, tiles_y_out);
    if (err) return err;

    *src_log_out = src_log;
    *dest_log_out = dest_log;
    *stride_bytes_out = stride;
    return NULL;
}

_kernel_oserror *v3d_copy_screen(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int src_x, unsigned int src_y,
                                   unsigned int dest_x, unsigned int dest_y,
                                   unsigned int width, unsigned int height,
                                   unsigned int screen_phys_base,
                                   unsigned int screen_stride,
                                   unsigned int bytes_per_pixel,
                                   unsigned int format_code,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out)
{
    unsigned int src_phys = screen_phys_base + src_y * screen_stride + src_x * bytes_per_pixel;
    unsigned int dest_phys = screen_phys_base + dest_y * screen_stride + dest_x * bytes_per_pixel;

    return run_copy(pmp, base_page, width, height, src_phys, dest_phys,
                     screen_stride, screen_stride, format_code, tiles_x_out, tiles_y_out);
}

/*
 * Overlap-safe copy: routes through a freshly-allocated scratch buffer
 * instead of a single direct Load(source)+Store(dest) pass, so it's
 * safe for ANY overlap, not just shifts of at least a full tile.
 *
 * WHY a direct pass isn't safe for overlapping rectangles even with
 * careful per-tile ordering: a tile's Load and Store both read/write a
 * fixed 64x64-pixel block, but source and destination are usually
 * shifted by something OTHER than a whole number of tiles (a scroll by
 * 20 pixels, say). A single tile's Store can then overwrite pixels
 * that belong to a DIFFERENT, not-yet-processed tile's Load region -
 * no ordering of the SAME tile grid avoids this once the shift is
 * smaller than one tile in either dimension. Routing through scratch
 * sidesteps the question entirely: source->scratch and scratch->dest
 * are each individually a fresh-buffer copy, which is always non-
 * overlapping by construction (scratch is memory nothing else is
 * using), so both passes reuse run_copy's already-proven, unmodified
 * core with zero new tile-ordering logic to get wrong.
 *
 * Bounded to pmp_alloc's pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL) for the scratch buffer
 * itself (same cap already governing every synthetic-buffer test in
 * this project) - large overlapping copies (e.g. a full-screen
 * scroll) are refused with a clear error rather than silently
 * chunked, so the caller's own fallback (leaving the GraphicsV call
 * unclaimed) takes over instead of this driver attempting something
 * only partially tested.
 */
/*
 * Cached scratch PIXEL buffer for the overlap-safe path - same
 * reasoning and mechanism as find_or_create_copy_scratch above, but
 * for the actual scratch pixel data (not the CL/tile_alloc/tile_state
 * machinery run_copy claims for itself). Claimed once at
 * PMP_MAX_PAGES_PER_CALL (the largest size this path ever allows
 * anyway - see the size check this replaces), keyed by base_page, and
 * reused thereafter regardless of the ACTUAL width/height being
 * copied on any given call (the buffer is always big enough; only the
 * portion actually used varies). This also fixes a second, related
 * problem: the offset this function hands to run_copy for ITS OWN
 * scratch (previously `base_page + scratch_pages`, where
 * `scratch_pages` varies with width/height) needs to be STABLE across
 * calls of different sizes for run_copy's own cache lookup to ever
 * hit - it now always passes `base_page + PMP_MAX_PAGES_PER_CALL`, a
 * fixed constant, instead.
 */
#define MAX_CACHED_OVERLAP_SCRATCH 2u

typedef struct
{
    unsigned int base_page;
    unsigned int scratch_phys; void *scratch_log;
} overlap_scratch_cache_t;

static overlap_scratch_cache_t g_overlap_scratch_cache[MAX_CACHED_OVERLAP_SCRATCH];
static unsigned int g_overlap_scratch_cache_count = 0;

static overlap_scratch_cache_t *find_or_create_overlap_scratch(pmp_area_t *pmp, unsigned int base_page,
                                                                  _kernel_oserror **err_out)
{
    unsigned int i;
    overlap_scratch_cache_t *slot;

    *err_out = NULL;

    for (i = 0; i < g_overlap_scratch_cache_count; i++)
        if (g_overlap_scratch_cache[i].base_page == base_page)
            return &g_overlap_scratch_cache[i];

    if (g_overlap_scratch_cache_count >= MAX_CACHED_OVERLAP_SCRATCH)
        return NULL;

    slot = &g_overlap_scratch_cache[g_overlap_scratch_cache_count];

    *err_out = pmp_alloc(pmp, base_page, PMP_MAX_PAGES_PER_CALL, &slot->scratch_phys, &slot->scratch_log);
    if (*err_out) return NULL;

    slot->base_page = base_page;
    g_overlap_scratch_cache_count++;
    return slot;
}

_kernel_oserror *v3d_copy_screen_overlapping(pmp_area_t *pmp, unsigned int base_page,
                                               unsigned int src_x, unsigned int src_y,
                                               unsigned int dest_x, unsigned int dest_y,
                                               unsigned int width, unsigned int height,
                                               unsigned int screen_phys_base,
                                               unsigned int screen_stride,
                                               unsigned int bytes_per_pixel,
                                               unsigned int format_code,
                                               unsigned int *tiles_x_out,
                                               unsigned int *tiles_y_out)
{
    unsigned int tiles_x, tiles_y, padded_w, padded_h, scratch_stride;
    unsigned int scratch_bytes, scratch_pages;
    unsigned int scratch_phys, src_phys, dest_phys;
    void *scratch_log;
    _kernel_oserror *err;

    if (width == 0u || height == 0u)
    {
        static _kernel_oserror err_bad_size = {
            10, "V3DDriver: overlapping copy width/height must be non-zero"
        };
        return &err_bad_size;
    }

    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);
    padded_w = tiles_x * TILE_SIZE;
    padded_h = tiles_y * TILE_SIZE;
    scratch_stride = padded_w * bytes_per_pixel;
    scratch_bytes = scratch_stride * padded_h;
    scratch_pages = bytes_to_pages(scratch_bytes);

    if (scratch_pages > PMP_MAX_PAGES_PER_CALL)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: overlapping copy too large for a scratch-"
                "buffer pass (pmp_alloc's pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL))"
        };
        return &err_too_big;
    }

    {
        overlap_scratch_cache_t *cache = find_or_create_overlap_scratch(pmp, base_page, &err);
        if (err) return err;

        if (cache)
        {
            scratch_phys = cache->scratch_phys;
            scratch_log = cache->scratch_log;
        }
        else
        {
            err = pmp_alloc(pmp, base_page, scratch_pages, &scratch_phys, &scratch_log);
            if (err) return err;
        }
    }

    src_phys = screen_phys_base + src_y * screen_stride + src_x * bytes_per_pixel;
    dest_phys = screen_phys_base + dest_y * screen_stride + dest_x * bytes_per_pixel;

    /* Pass 1: real source -> scratch (always non-overlapping - scratch
     * is fresh driver memory nothing else touches). Fixed offset
     * (PMP_MAX_PAGES_PER_CALL, not the variable `scratch_pages`) so
     * run_copy's own scratch cache lookup is stable across differently-
     * sized calls - see this function's own top comment. */
    err = run_copy(pmp, base_page + PMP_MAX_PAGES_PER_CALL, width, height,
                    src_phys, scratch_phys, screen_stride, scratch_stride,
                    format_code, &tiles_x, &tiles_y);
    if (err) return err;

    /* Pass 2: scratch -> real destination (same reasoning). */
    err = run_copy(pmp, base_page + PMP_MAX_PAGES_PER_CALL, width, height,
                    scratch_phys, dest_phys, scratch_stride, screen_stride,
                    format_code, &tiles_x, &tiles_y);
    if (err) return err;

    *tiles_x_out = tiles_x;
    *tiles_y_out = tiles_y;
    return NULL;
}

_kernel_oserror *v3d_copy_overlap_test(pmp_area_t *pmp, unsigned int base_page,
                                         unsigned int width, unsigned int height,
                                         unsigned int shift_x, unsigned int shift_y,
                                         void **buf_log_out,
                                         unsigned int *stride_bytes_out,
                                         unsigned int *tiles_x_out,
                                         unsigned int *tiles_y_out)
{
    unsigned int bound_w, bound_h, bound_tiles_x, bound_tiles_y;
    unsigned int padded_w, padded_h, stride, buf_bytes, buf_pages;
    unsigned int buf_phys;
    void *buf_log;
    _kernel_oserror *err;
    unsigned int x, y;

    if (width == 0u || height == 0u)
    {
        static _kernel_oserror err_bad_size = {
            10, "V3DDriver: V3D_OverlapCopyTest width/height must be non-zero"
        };
        return &err_bad_size;
    }

    /* Bounding box covering both the source rect at (0,0) and the
     * dest rect at (shift_x, shift_y), padded to a whole tile grid -
     * same safety-margin reasoning as v3d_fill_test's own padding
     * (this is a test buffer we control the size of; the real screen
     * target has no such margin, which is exactly why the per-call cap
     * matters more there). */
    bound_w = width + shift_x;
    bound_h = height + shift_y;
    bound_tiles_x = div_round_up(bound_w, TILE_SIZE);
    bound_tiles_y = div_round_up(bound_h, TILE_SIZE);
    padded_w = bound_tiles_x * TILE_SIZE;
    padded_h = bound_tiles_y * TILE_SIZE;
    stride = padded_w * 4u;
    buf_bytes = stride * padded_h;
    buf_pages = bytes_to_pages(buf_bytes);

    if (buf_pages > PMP_MAX_PAGES_PER_CALL)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: V3D_OverlapCopyTest rectangle+shift too "
                "large for this test harness (pmp_alloc's pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL))"
        };
        return &err_too_big;
    }

    err = pmp_alloc(pmp, base_page, buf_pages, &buf_phys, &buf_log);
    if (err) return err;

    /* Per-pixel test pattern across the WHOLE bounding buffer, source
     * and dest regions alike - see this function's own header comment
     * for why the expected post-copy value is computed analytically
     * from this formula rather than saved beforehand. */
    for (y = 0; y < padded_h; y++)
    {
        volatile unsigned int *row_ptr =
            (volatile unsigned int *) ((unsigned char *) buf_log + y * stride);
        for (x = 0; x < padded_w; x++)
            row_ptr[x] = 0xFF000000u | (x << 12) | y;
    }

    err = v3d_copy_screen_overlapping(pmp, base_page + buf_pages,
                                        0, 0, shift_x, shift_y, width, height,
                                        buf_phys, stride, 4u /* bytes/pixel */,
                                        27 /* RGBA8 */, tiles_x_out, tiles_y_out);
    if (err) return err;

    *buf_log_out = buf_log;
    *stride_bytes_out = stride;
    return NULL;
}
