/* SPDX-License-Identifier: GPL-2.0-or-later */
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
 * Real budget for the tiny dedicated v3d_copy_test_16bpp PMP slot
 * (module.c's COPY_TEST_16BPP_EXTRA_PAGES) - MUST be kept equal to
 * that constant by hand (no shared header between module.c and this
 * file for it). Added 2026-09-30 after an adversarial code review found
 * the ORIGINAL bounds check only compared against the generic
 * PMP_MAX_PAGES_PER_CALL=2048 cap, which is far bigger than this
 * dedicated slot actually is - a moderately large test call (e.g.
 * 200x200) would have silently overflowed past this slot's real
 * reservation and aliased V3D_FillTest16bpp's adjacent slot instead of
 * failing cleanly. The fixed worst-case scratch cost (V3D_SLOT_PAGES =
 * 49: 1 cl + 6 rcl + 10 tile_alloc + 32 tile_state, ALWAYS claimed
 * regardless of actual request size) leaves only
 * SLOT_PAGES-V3D_SLOT_PAGES for the two (src+dest) render buffers
 * COMBINED.
 */
#define COPY_TEST_16BPP_SLOT_PAGES 100u
#define COPY_TEST_16BPP_SCRATCH_WORST_CASE_PAGES V3D_SLOT_PAGES

/*
 * Shared core: builds and submits a multi-tile Load+Store control list
 * copying from `src_phys` (row stride `src_stride`) to `dest_phys` (row
 * stride `dest_stride`) - separate strides so this can also copy
 * between two buffers of different widths (needed for the overlap-safe
 * scratch-buffer path below; real GraphicsV op 1 copies within the
 * same framestore, so its two calls always pass the same stride
 * twice). No ClearTileBuffers at all - every tile is populated from
 * `src_phys` via LoadTileBufferGeneral, not cleared.
 *
 * Structurally mirrors cl_fill.c's run_fill: the only real difference is
 * the per-tile RCL body, Load(source)+EndOfLoads+Store(dest) instead of
 * Store-none+Clear+Store.
 */
/* Tile visiting order for run_copy (see v3d_copy_screen_overlapping). */
#define COPY_COLS_DESCENDING 1u
#define COPY_ROWS_DESCENDING 2u

static _kernel_oserror *run_copy_job(pmp_area_t *pmp, unsigned int base_page,
                                       unsigned int width, unsigned int height,
                                       unsigned int src_phys, unsigned int dest_phys,
                                       unsigned int src_stride, unsigned int dest_stride,
                                       unsigned int format_code,
                                       unsigned int *tiles_x_out,
                                       unsigned int *tiles_y_out,
                                       unsigned int order)
{
    unsigned int tiles_x, tiles_y, n_tiles;
    unsigned int col, row, ci, ri;
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

    /* --- BCL: identical to a fill's - no primitives. --- */
    cl_init(&bcl, s->cl_log, s->cl_phys, PAGE_SIZE);
    job.bcl_start = cl_paddr(&bcl);
    v3d_build_bcl(&bcl, width, height);
    job.bcl_end = cl_paddr(&bcl);

    /* --- RCL: one Load(source)+Store(dest) pair per tile, no Clear. --- */
    cl_init(&rcl, s->rcl_log, s->rcl_phys, s->rcl_pages * PAGE_SIZE);
    job.rcl_start = cl_paddr(&rcl);
    v3d_build_rcl_header(&rcl, width, height, 0, s->tile_alloc_phys,
                          tiles_x, tiles_y);

    for (ri = 0; ri < tiles_y; ri++)
    {
        row = (order & COPY_ROWS_DESCENDING) ? tiles_y - 1u - ri : ri;
        for (ci = 0; ci < tiles_x; ci++)
        {
            col = (order & COPY_COLS_DESCENDING) ? tiles_x - 1u - ci : ci;

            /* Same base-address-only addressing as cl_fill.c's store -
             * the CLE derives each tile's offset itself from the
             * preceding TileCoordinates, for BOTH Load and Store
             * (hardware-confirmed for Load by this file's own tests). */
            emit_tile_coordinates(&rcl, col, row);
            emit_load_tile_buffer_rt0(&rcl, format_code, src_stride, src_phys);
            emit_end_of_loads(&rcl);
            emit_store_tile_buffer_rt0(&rcl, format_code, dest_stride, dest_phys);
            emit_end_of_tile_marker(&rcl);
        }
    }

    emit_end_of_rendering(&rcl);
    job.rcl_end = cl_paddr(&rcl);

    job.kind = V3D_JOB_COPY;
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
 * A copy of any size, in the same bands as a fill (see run_fill in cl_fill.c): a rectangle
 * of more than WORST_CASE_N_TILES tiles becomes several jobs, each a strip a whole number
 * of tile rows tall, with the source and destination addresses advanced by the same
 * number of rows.
 *
 * Splitting must not change the RESULT of an overlapping copy (a scroll), and it does not:
 * the ordered one-pass copy in v3d_copy_screen_overlapping is correct because tiles are
 * visited in a particular sequence (rows from the far end when the shift is downwards,
 * columns from the far end when it is to the right). The bands are visited in the same
 * direction as the rows (last band first when `order` has COPY_ROWS_DESCENDING) and each
 * band applies `order` inside itself, so the sequence of tiles - and therefore every load
 * and store, including the clipping of the final partial tile row, which is in the last
 * band either way - is exactly the sequence of the one big job.
 * test/host/overlap_order_test.c checks this by simulation against memmove.
 *
 * One thing is NOT the same as a single job: if band k >= 2 fails (a GPU timeout, or the
 * fail-stop latch being set by the interrupt handler in between) after earlier bands have
 * stored, the caller leaves the call to the CPU, which redoes the whole scroll from a source the
 * earlier bands have already moved, so that one scroll is drawn wrongly. A failure at the start
 * of a single big job happens before anything is stored, so the CPU redo was always right
 * there. This only exists for rectangles of more than max_job_tiles tiles (at 1920x1200 or
 * above), which used to be refused up front and done by the CPU. The failure itself is the
 * GPU fail-stop case: the GPU is then off and the next redraw repairs the screen.
 * Everything that can be refused up front is refused by the first band.
 */
static _kernel_oserror *run_copy(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int width, unsigned int height,
                                   unsigned int src_phys, unsigned int dest_phys,
                                   unsigned int src_stride, unsigned int dest_stride,
                                   unsigned int format_code,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out,
                                   unsigned int order)
{
    unsigned int band_h, nbands, k, y0, h, tx, ty;
    _kernel_oserror *err;

    err = v3d_band_height(width, height, &band_h);
    if (err)
        return err;

    if (height <= band_h)
        return run_copy_job(pmp, base_page, width, height, src_phys, dest_phys,
                             src_stride, dest_stride, format_code,
                             tiles_x_out, tiles_y_out, order);

    nbands = v3d_band_count(height, band_h);
    for (k = 0; k < nbands; k++)
    {
        v3d_band_span(height, band_h,
                      (order & COPY_ROWS_DESCENDING) ? nbands - 1u - k : k, &y0, &h);
        err = run_copy_job(pmp, base_page, width, h,
                            src_phys + y0 * src_stride,
                            dest_phys + y0 * dest_stride,
                            src_stride, dest_stride, format_code, &tx, &ty, order);
        if (err)
            return err;
    }

    *tiles_x_out = div_round_up(width, TILE_SIZE);
    *tiles_y_out = div_round_up(height, TILE_SIZE);
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
                    tiles_x_out, tiles_y_out, 0u);
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
                    tiles_x_out, tiles_y_out, 0u);
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
                     screen_stride, screen_stride, format_code, tiles_x_out, tiles_y_out, 0u);
}

/*
 * Overlap-safe copy: routes through a freshly-allocated scratch buffer
 * instead of a single direct Load(source)+Store(dest) pass, so it's
 * safe for ANY overlap, not just shifts of at least a full tile.
 *
 * TWO WAYS TO DO IT (V3D_AccelTune 7, overlap_mode):
 *
 * overlap_mode 0 (v0.06/0.07/0.08's behaviour, kept as a fallback): two
 * passes through a scratch buffer - source->scratch then scratch->dest. Each
 * pass is non-overlapping by construction, so it reuses run_copy unchanged.
 * Simple, but it costs two GPU jobs and an 8MB bounce buffer.
 *
 * overlap_mode 1 (the default from v0.09, hardware-verified): ONE pass, tiles
 * visited in a chosen order. The earlier
 * analysis in this file (and the README) rejected this on the grounds that a
 * tile's Store can overwrite the source of a not-yet-processed tile once the
 * shift is under 64 pixels. That is true for a FIXED forward order, and it is
 * the only thing the counterexample showed. It is not true if the order follows
 * the shift: a store at tile T lands on T's own source plus the source tiles
 * AHEAD of it in the direction of the shift, and nowhere else (a shift of any
 * size moves a tile's footprint only in that direction). So visit the tiles
 * ahead first - columns last-to-first when the destination is to the right of
 * the source, first-to-last otherwise; rows likewise by whether the destination
 * is lower in memory - and every source tile has been loaded before anything is
 * stored over it. Loads of later tiles can never see an earlier tile's store,
 * since an earlier tile's footprint is further along the shift than any later
 * tile's source. This reduces to memmove's choice of copy direction, per axis.
 * test/host/overlap_order_test.c simulates exactly that (full unclipped 64x64
 * loads, clipped stores) over 148,059 random rectangles and shifts, 74,210 of
 * them overlapping: 0 failures in sign order, 45,151 in forward order. What it
 * assumes of the hardware - tiles run strictly one after another in the order
 * emitted, which a single-core CLE walking an explicit TileCoordinates list
 * does - is what test/VerifySuite.bas checks on the real machine.
 *
 * Size limits depend on the mode (g_v3d_tune.overlap_mode):
 *   1 (default, one pass): no cap of its own. run_copy cuts a rectangle of more than
 *     max_job_tiles tiles into bands (0.12), so a full-screen scroll at 1920x1200 or 4K
 *     reaches the GPU; only what v3d_band_height refuses is refused.
 *   0 (two passes through the bounce buffer): bounded by the 8MB scratch buffer
 *     (PMP_MAX_PAGES_PER_CALL pages); a bigger copy is refused with a clear error and the
 *     caller leaves the GraphicsV call unclaimed, so the CPU does it.
 */
/*
 * Cached scratch PIXEL buffer for the overlap-safe path
 * (v3d_overlap_buffer_get in cl_common.c) - the actual scratch pixel
 * data, not the CL/tile_alloc/tile_state machinery run_copy claims for
 * itself. Claimed once at
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

    if (g_v3d_tune.overlap_mode == 1u)
    {
        /* One pass, tiles ahead of the shift first (see the comment above).
         * Coordinates here are top-down, so a larger y is lower in memory. */
        unsigned int order = (dest_x > src_x ? COPY_COLS_DESCENDING : 0u) |
                              (dest_y > src_y ? COPY_ROWS_DESCENDING : 0u);

        src_phys = screen_phys_base + src_y * screen_stride + src_x * bytes_per_pixel;
        dest_phys = screen_phys_base + dest_y * screen_stride + dest_x * bytes_per_pixel;
        return run_copy(pmp, base_page + PMP_MAX_PAGES_PER_CALL, width, height,
                         src_phys, dest_phys, screen_stride, screen_stride,
                         format_code, tiles_x_out, tiles_y_out, order);
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
                "buffer pass (the bounce buffer's page cap, PMP_MAX_PAGES_PER_CALL)"
        };
        return &err_too_big;
    }

    /* One large pixel buffer, claimed once and reused (see
     * v3d_overlap_buffer_get). If it cannot be had the call is simply
     * declined - never claimed per call from inside a GraphicsV Render. */
    err = v3d_overlap_buffer_get(pmp, base_page, scratch_pages,
                                  &scratch_phys, &scratch_log);
    if (err) return err;

    src_phys = screen_phys_base + src_y * screen_stride + src_x * bytes_per_pixel;
    dest_phys = screen_phys_base + dest_y * screen_stride + dest_x * bytes_per_pixel;

    /* Pass 1: real source -> scratch (always non-overlapping - scratch
     * is fresh driver memory nothing else touches). Fixed offset
     * (PMP_MAX_PAGES_PER_CALL, not the variable `scratch_pages`) so
     * run_copy's own scratch cache lookup is stable across differently-
     * sized calls - see this function's own top comment. */
    err = run_copy(pmp, base_page + PMP_MAX_PAGES_PER_CALL, width, height,
                    src_phys, scratch_phys, screen_stride, scratch_stride,
                    format_code, &tiles_x, &tiles_y, 0u);
    if (err) return err;

    /* Pass 2: scratch -> real destination (same reasoning). */
    err = run_copy(pmp, base_page + PMP_MAX_PAGES_PER_CALL, width, height,
                    scratch_phys, dest_phys, scratch_stride, screen_stride,
                    format_code, &tiles_x, &tiles_y, 0u);
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
