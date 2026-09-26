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

static _kernel_oserror *wait_for_irq(volatile unsigned int *counter,
                                      unsigned int initial,
                                      _kernel_oserror *timeout_err)
{
    unsigned int spin;
    for (spin = 0; spin < 5000000u; spin++)
    {
        if (*counter != initial)
            return NULL;
    }
    return timeout_err;
}

/*
 * Shared core: builds and submits a multi-tile Load+Store control list
 * copying from `src_phys` to `dest_phys` (both using `stride` - real
 * GraphicsV op 1 copies within the same framestore, so source and
 * dest always share a stride; the synthetic test below does too, for
 * simplicity). No ClearTileBuffers at all - every tile is populated
 * from `src_phys` via LoadTileBufferGeneral, not cleared.
 */
static _kernel_oserror *run_copy(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int width, unsigned int height,
                                   unsigned int src_phys, unsigned int dest_phys,
                                   unsigned int stride,
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

    if (tile_alloc_pages > 64u || tile_state_pages > 64u || rcl_pages > 64u)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: copy rectangle too large - tile_alloc/"
                "tile_state/RCL scratch buffers exceed pmp_alloc's "
                "64-page/call cap"
        };
        return &err_too_big;
    }

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
            emit_load_tile_buffer_rt0(&rcl, 27 /* RGBA8 */, stride, src_phys);
            emit_end_of_loads(&rcl);
            emit_store_tile_buffer_rt0(&rcl, 27 /* RGBA8 */, stride, dest_phys);
            emit_end_of_tile_marker(&rcl);
        }
    }

    emit_end_of_rendering(&rcl);
    rcl_end = cl_paddr(&rcl);

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
            11, "V3DDriver: copy timed out waiting for binning"
        };
        err = wait_for_irq(&g_v3d_irq_bin_done_count, last_bin_irq, &err_bin_timeout);
        if (err) return err;
    }

    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    v3d_flush_caches(&v3d);
    last_render_irq = g_v3d_irq_render_done_count;

    IO_WORD(v3d, V3D_CLE_CT1QBA) = rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = rcl_end;

    io_unmap(&v3d);

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

    if (buf_pages > 64u)
    {
        static _kernel_oserror err_too_big = {
            14, "V3DDriver: V3D_CopyTest rectangle too large for this "
                "test harness (pmp_alloc's 64-page/call cap)"
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
                    src_phys, dest_phys, stride, tiles_x_out, tiles_y_out);
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
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out)
{
    unsigned int src_phys = screen_phys_base + src_y * screen_stride + src_x * 4u;
    unsigned int dest_phys = screen_phys_base + dest_y * screen_stride + dest_x * 4u;

    return run_copy(pmp, base_page, width, height, src_phys, dest_phys,
                     screen_stride, tiles_x_out, tiles_y_out);
}
