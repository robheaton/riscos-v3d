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
#include "cl_common.h"
#include "cl_builder.h"
#include "cl_packets.h"
#include "v3d_regs.h"
#include "io_map.h"
#include "v3d_irq.h"

/* TILE_SIZE, the scratch cache, the completion wait, the barriers, the
 * control-list upload and the job kick sequence all live in cl_common.c
 * now (v0.07) - see its header for why they were pulled out of here. */

static inline unsigned int div_round_up(unsigned int a, unsigned int b)
{
    return v3d_div_round_up(a, b);
}

static inline unsigned int bytes_to_pages(unsigned int bytes)
{
    return v3d_bytes_to_pages(bytes);
}

/*
 * Real budget for the tiny dedicated v3d_fill_test_16bpp PMP slot
 * (module.c's FILL_TEST_16BPP_EXTRA_PAGES) - MUST be kept equal to
 * that constant by hand (no shared header for it, same reasoning as
 * cl_copy.c's own matching constant). Added 2026-09-30 after an
 * adversarial code review found v3d_fill_test_16bpp's original bounds
 * check only compared against the generic PMP_MAX_PAGES_PER_CALL=2048
 * cap, far bigger than this dedicated slot actually is - a moderately
 * large test call would have silently overflowed past this slot's real
 * reservation and aliased the NEXT dedicated PMP slot instead of
 * failing cleanly. The fixed worst-case scratch cost
 * (V3D_SLOT_PAGES = 49: 1 cl + 6 rcl + 10 tile_alloc + 32 tile_state,
 * ALWAYS claimed regardless of actual request size) leaves only
 * SLOT_PAGES-V3D_SLOT_PAGES for the render buffer itself.
 */
#define FILL_TEST_16BPP_SLOT_PAGES 100u
#define FILL_TEST_16BPP_SCRATCH_WORST_CASE_PAGES V3D_SLOT_PAGES

/*
 * Shared core: builds and submits a multi-tile clear+store control
 * list targeting `target_phys`/`target_stride` directly (the caller
 * decides whether that's a PMP test buffer or the real framestore),
 * using PMP pages starting at `base_page` for the CL/tile_alloc/
 * tile_state scratch buffers only - never for the render target
 * itself. Returns *tiles_x_out / *tiles_y_out (diagnostic) on success.
 *
 * Generalises cl_clear.c's fixed-single-64x64-tile clear+store: one
 * explicit clear+store pair per tile, no primitives (see this file's
 * top comment).
 */
static _kernel_oserror *run_fill_job(pmp_area_t *pmp, unsigned int base_page,
                                       unsigned int width, unsigned int height,
                                       unsigned int clear_color_rgba,
                                       unsigned int target_phys,
                                       unsigned int target_stride,
                                       unsigned int format_code,
                                       unsigned int *tiles_x_out,
                                       unsigned int *tiles_y_out)
{
    unsigned int tiles_x, tiles_y, n_tiles;
    unsigned int col, row;
    unsigned int t_start;
    v3d_scratch_t *s;
    cl_buf_t bcl, rcl;
    v3d_job_t job;
    _kernel_oserror *err;

    t_start = v3d_time_us();

    err = v3d_gpu_check();
    if (err)
        return err;

    err = v3d_check_dims(width, height);
    if (err)
        return err;

    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);
    n_tiles = tiles_x * tiles_y;

    err = v3d_scratch_get(pmp, base_page, n_tiles, &s);
    if (err)
        return err;

    v3d_scratch_zero(s, n_tiles);

    /* --- BCL: no primitives, so nothing here scales with tile count. --- */
    cl_init(&bcl, s->cl_log, s->cl_phys, PAGE_SIZE);
    job.bcl_start = cl_paddr(&bcl);
    v3d_build_bcl(&bcl, width, height);
    job.bcl_end = cl_paddr(&bcl);

    /* --- RCL: real multi-tile grid, one clear+store pair per tile --- */
    cl_init(&rcl, s->rcl_log, s->rcl_phys, s->rcl_pages * PAGE_SIZE);
    job.rcl_start = cl_paddr(&rcl);
    v3d_build_rcl_header(&rcl, width, height, clear_color_rgba,
                          s->tile_alloc_phys, tiles_x, tiles_y);

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
            emit_store_tile_buffer_rt0(&rcl, format_code, target_stride, target_phys);
            emit_end_of_tile_marker(&rcl);
        }
    }

    emit_end_of_rendering(&rcl);
    job.rcl_end = cl_paddr(&rcl);

    job.kind = V3D_JOB_FILL;
    job.t_start_us = t_start;
    job.tile_alloc_phys = s->tile_alloc_phys;
    job.tile_alloc_bytes = v3d_tile_alloc_bytes(n_tiles);
    job.tile_state_phys = s->tile_state_phys;
    job.overflow = (bcl.overflow || rcl.overflow) ? 1 : 0;

    err = v3d_job_run(&job);
    if (err)
        return err;

    *tiles_x_out = tiles_x;
    *tiles_y_out = tiles_y;
    return NULL;
}

/*
 * A fill of any size. One GPU job holds at most WORST_CASE_N_TILES tiles (the scratch
 * slot is sized for that once and never grown inside a GraphicsV call), so a bigger
 * rectangle is cut into horizontal BANDS, each a whole number of tile rows tall, and each
 * band is its own job. Bands are disjoint rectangles, so a fill needs no ordering and no
 * care at the joins: band k starts exactly done = k * band_h rows below the top, its
 * target address is that many rows further on, and its tile grid is the tile grid of the
 * whole rectangle (band_h is a multiple of 64). A rectangle of up to 510 tiles takes the
 * unchanged single-job path (511 and 512 tiles, which the old page-granular scratch check happened
 * to accept as one job, are now two: one extra ~9 us job on a ~1.5 ms operation). Everything that can be checked up front (dimensions, the
 * scratch slot) is checked by the first band, before anything is drawn.
 */
static _kernel_oserror *run_fill(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int width, unsigned int height,
                                   unsigned int clear_color_rgba,
                                   unsigned int target_phys,
                                   unsigned int target_stride,
                                   unsigned int format_code,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out)
{
    unsigned int band_h, nbands, k, y0, h, tx, ty;
    _kernel_oserror *err;

    err = v3d_band_height(width, height, &band_h);
    if (err)
        return err;

    if (height <= band_h)
        return run_fill_job(pmp, base_page, width, height, clear_color_rgba,
                             target_phys, target_stride, format_code,
                             tiles_x_out, tiles_y_out);

    nbands = v3d_band_count(height, band_h);
    for (k = 0; k < nbands; k++)
    {
        v3d_band_span(height, band_h, k, &y0, &h);
        err = run_fill_job(pmp, base_page, width, h, clear_color_rgba,
                            target_phys + y0 * target_stride, target_stride,
                            format_code, &tx, &ty);
        if (err)
            return err;
    }

    *tiles_x_out = div_round_up(width, TILE_SIZE);
    *tiles_y_out = div_round_up(height, TILE_SIZE);
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
                    render_phys, stride, 27 /* RGBA8 */, tiles_x_out, tiles_y_out);
    if (err) return err;

    *target_phys_out = render_phys;
    *target_log_out = render_log;
    *target_stride_bytes_out = stride;
    return NULL;
}

_kernel_oserror *v3d_fill_test_16bpp(pmp_area_t *pmp, unsigned int base_page,
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
            10, "V3DDriver: V3D_FillTest16bpp width/height must be non-zero"
        };
        return &err_bad_size;
    }

    tiles_x = div_round_up(width, TILE_SIZE);
    tiles_y = div_round_up(height, TILE_SIZE);
    padded_w = tiles_x * TILE_SIZE;
    padded_h = tiles_y * TILE_SIZE;
    stride = padded_w * 2u; /* 16bpp: 2 bytes/pixel, not v3d_fill_test's 4 */
    render_bytes = stride * padded_h;
    render_pages = bytes_to_pages(render_bytes);

    if (render_pages > PMP_MAX_PAGES_PER_CALL ||
        render_pages > FILL_TEST_16BPP_SLOT_PAGES - FILL_TEST_16BPP_SCRATCH_WORST_CASE_PAGES)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: V3D_FillTest16bpp rectangle too large for its "
                "dedicated test PMP slot - try a smaller width/height."
        };
        return &err_too_big;
    }

    err = pmp_alloc(pmp, base_page, render_pages, &render_phys, &render_log);
    if (err) return err;

    err = run_fill(pmp, base_page + render_pages, width, height, clear_color_rgba,
                    render_phys, stride, 45 /* RGBA5551 */, tiles_x_out, tiles_y_out);
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
                                   unsigned int bytes_per_pixel,
                                   unsigned int format_code,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out)
{
    unsigned int target_phys = screen_phys_base + y * screen_stride + x * bytes_per_pixel;

    return run_fill(pmp, base_page, width, height, clear_color_rgba,
                     target_phys, screen_stride, format_code, tiles_x_out, tiles_y_out);
}

_kernel_oserror *v3d_fill_raw(pmp_area_t *pmp, unsigned int base_page,
                                unsigned int width, unsigned int height,
                                unsigned int clear_color_rgba,
                                unsigned int target_phys,
                                unsigned int target_stride,
                                unsigned int format_code,
                                unsigned int *tiles_x_out,
                                unsigned int *tiles_y_out)
{
    return run_fill(pmp, base_page, width, height, clear_color_rgba,
                     target_phys, target_stride, format_code, tiles_x_out, tiles_y_out);
}

_kernel_oserror *v3d_striped_fill_raw(pmp_area_t *pmp, unsigned int base_page,
                                        unsigned int width, unsigned int height,
                                        const unsigned int colours[8],
                                        unsigned int row_phase,
                                        unsigned int target_phys_row0,
                                        unsigned int target_stride,
                                        unsigned int format_code)
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
                        dest_phys, dest_stride, format_code, &tiles_x, &tiles_y);
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
                                 colours, row_phase, render_phys, stride,
                                 27 /* RGBA8 */);
    if (err) return err;

    *target_phys_out = render_phys;
    *target_log_out = render_log;
    *target_stride_bytes_out = stride;
    return NULL;
}
