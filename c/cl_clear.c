/*
 * cl_clear.c - see cl_clear.h.
 *
 * Every packet's byte layout below is transcribed from the MIT-licensed
 * bare-metal reference's C++ struct definitions (src/device/v3d/v3d_cl.hpp,
 * github.com/Random06457/rpi4-gpu-bare-metal-examples), NOT reproduced by
 * relying on our own compiler's bitfield layout (see cl_builder.h for why).
 * Each field's bit width/order/position was hand-derived from that source
 * and cross-checked against its own static_assert(sizeof(...)==N) - the
 * comment above each emit_* function shows that derivation so it can be
 * re-checked without re-deriving from scratch.
 *
 * Register submission sequence (CT0QMA/QMS/QTS, CT0/CT1 QBA/QEA, the L2T
 * cache-flush dance) is transcribed the same way from that reference's
 * src/device/v3d/v3d_qpu.cpp (executeBin/executeRender).
 *
 * NOT VERIFIED: whether a BCL with zero primitives/viewport/shader-state
 * setup (as used here, since nothing is actually drawn) is valid on real
 * V3D hardware. The reference never exercises this path - its BCL always
 * sets up a full viewport+shader-state+VertexArrayPrims because it always
 * draws a triangle. This is the single most likely thing to need fixing
 * once this hits hardware.
 */

#include "cl_clear.h"
#include "cl_builder.h"
#include "v3d_regs.h"
#include "io_map.h"

#define CL_PAGE_COUNT           1u  /* BCL+RCL combined, both tiny */
#define RENDER_TARGET_PAGE_COUNT 4u /* 64*64*4 bytes = 16384 = 4 pages */
#define TILE_ALLOC_PAGE_COUNT    3u /* align(1*1*1*64,4096) + 64*64*2, per
                                      * the reference's own sizing formula */
#define TILE_STATE_PAGE_COUNT    1u /* 256 bytes/tile (V3D>=40 - we're 4.2) */
#define CLEAR_TEST_TOTAL_PAGES  (CL_PAGE_COUNT + RENDER_TARGET_PAGE_COUNT + \
                                  TILE_ALLOC_PAGE_COUNT + TILE_STATE_PAGE_COUNT)

/* opcode(1) + [init/alloc block size=0, ntargets-1=0|maxbpp|msaa|dbuf=0,
 * unused, unused](4) + width-1(2) + height-1(2) = 9 bytes. */
static void emit_tile_binning_mode_cfg(cl_buf_t *b, unsigned int w, unsigned int h)
{
    cl_u8(b, 120);
    cl_u8(b, 0);
    cl_u8(b, 0);
    cl_u8(b, 0);
    cl_u8(b, 0);
    cl_u16(b, w - 1);
    cl_u16(b, h - 1);
}

static void emit_flush_vcd_cache(cl_buf_t *b) { cl_u8(b, 19); }
static void emit_start_tile_binning(cl_buf_t *b) { cl_u8(b, 6); }
static void emit_flush(cl_buf_t *b) { cl_u8(b, 4); }

/* opcode(1) + [sub_id=0|ntargets-1=0](1) + width(2) + height(2)
 * + [bpp:2|msaa:1|dbuf:1|unused:1|earlyz_dir:1|earlyz_dis:1|depth_type:4
 *    |earlyzs_clear:1|unused:4](2) + unused(1) = 9 bytes. */
static void emit_tile_rendering_mode_cfg_common(cl_buf_t *b, unsigned int w,
                                                 unsigned int h,
                                                 unsigned int internal_depth_type)
{
    cl_u8(b, 121);
    cl_u8(b, 0);
    cl_u16(b, w);
    cl_u16(b, h);
    cl_u16(b, (internal_depth_type & 0xFu) << 7);
    cl_u8(b, 0);
}

/* opcode(1) + 64-bit[sub_id=3:4|target:4|color_low:32|color_next:24] = 9. */
static void emit_tile_rendering_mode_cfg_clear_colors(cl_buf_t *b,
                                                       unsigned int target_idx,
                                                       unsigned int color_low,
                                                       unsigned int color_next24)
{
    unsigned long long v =
        3ull |
        ((unsigned long long) (target_idx & 0xFu) << 4) |
        ((unsigned long long) color_low << 8) |
        ((unsigned long long) (color_next24 & 0xFFFFFFu) << 40);
    cl_u8(b, 121);
    cl_u64(b, v);
}

/* opcode(1) + 64-bit[sub_id=1:4|bpp0:2|type0:4|clamp0:2|...rt1..3=0] = 9.
 * Only render target 0 is used - rt1-3 fields left zero. */
static void emit_tile_rendering_mode_cfg_color_rt0(cl_buf_t *b,
                                                    unsigned int bpp0,
                                                    unsigned int type0,
                                                    unsigned int clamp0)
{
    unsigned long long v =
        1ull |
        ((unsigned long long) (bpp0 & 0x3u) << 4) |
        ((unsigned long long) (type0 & 0xFu) << 6) |
        ((unsigned long long) (clamp0 & 0x3u) << 10);
    cl_u8(b, 121);
    cl_u64(b, v);
}

/* opcode(1) + sub_id=2(1) + stencil(1) + z_clear f32(4) + unused(2) = 9. */
static void emit_tile_rendering_mode_cfg_zs_clear(cl_buf_t *b,
                                                   unsigned int stencil,
                                                   float z)
{
    cl_u8(b, 121);
    cl_u8(b, 2);
    cl_u8(b, stencil & 0xFFu);
    cl_f32(b, z);
    cl_u8(b, 0);
    cl_u8(b, 0);
}

/* opcode(1) + [block_size:2|auto_chained:1|unused:5](1) = 2. */
static void emit_tile_list_initial_block_size(cl_buf_t *b,
                                               unsigned int block_size,
                                               int auto_chained)
{
    cl_u8(b, 126);
    cl_u8(b, (block_size & 0x3u) | ((auto_chained ? 1u : 0u) << 2));
}

/* opcode(1) + [tile_list_set:4=0|unused:2|address:26](4) = 5.
 * addr must be 64-byte aligned (guaranteed: it's page-aligned). */
static void emit_multicore_tile_list_set_base(cl_buf_t *b, unsigned int addr)
{
    cl_u8(b, 123);
    cl_u32(b, (addr >> 6) << 6);
}

/* opcode(1) + w-1(1) + h-1(1) + frame_w_super(1) + frame_h_super(1)
 * + [frame_w_tile:12|frame_h_tile:12](3)
 * + [multicore:1|unused:3|raster:1|bin_lists-1:3](1) = 9. */
static void emit_multicore_supertile_cfg(cl_buf_t *b,
    unsigned int supertile_w, unsigned int supertile_h,
    unsigned int frame_w_supertile, unsigned int frame_h_supertile,
    unsigned int frame_w_tile, unsigned int frame_h_tile,
    unsigned int bin_tile_lists, int raster_order, int multicore)
{
    cl_u8(b, 122);
    cl_u8(b, supertile_w - 1);
    cl_u8(b, supertile_h - 1);
    cl_u8(b, frame_w_supertile);
    cl_u8(b, frame_h_supertile);
    cl_u24(b, (frame_w_tile & 0xFFFu) | ((frame_h_tile & 0xFFFu) << 12));
    cl_u8(b, (multicore ? 1u : 0u) |
             ((raster_order ? 1u : 0u) << 4) |
             (((bin_tile_lists - 1u) & 0x7u) << 5));
}

/* opcode(1) + [col:12|row:12](3) = 4. */
static void emit_tile_coordinates(cl_buf_t *b, unsigned int col, unsigned int row)
{
    cl_u8(b, 124);
    cl_u24(b, (col & 0xFFFu) | ((row & 0xFFFu) << 12));
}

static void emit_end_of_loads(cl_buf_t *b) { cl_u8(b, 26); }
static void emit_end_of_tile_marker(cl_buf_t *b) { cl_u8(b, 27); }
static void emit_end_of_rendering(cl_buf_t *b) { cl_u8(b, 13); }

/* opcode(1) + [buffer=8(NONE):4|fmt:3|flip:1](1) + dither/decimate/fmt(2)
 * + [unused:4|height_ub_stride:20](3) + height(2) + address(4) = 13.
 * All-zero payload except buffer_to_store=BUFFER_NONE, matching the
 * reference's no-arg StoreTileBufferGeneral() used as an end-of-loads
 * marker before a clear (nothing has been loaded, nothing to store yet). */
static void emit_store_tile_buffer_none(cl_buf_t *b)
{
    cl_u8(b, 29);
    cl_u8(b, 8);
    cl_u16(b, 0);
    cl_u24(b, 0);
    cl_u16(b, 0);
    cl_u32(b, 0);
}

/* Same layout as above, populated to actually store render target 0,
 * raster tiling, RGBA8 output, at `addr` with row stride `stride`. */
static void emit_store_tile_buffer_rt0(cl_buf_t *b, unsigned int output_format,
                                        unsigned int stride, unsigned int addr)
{
    cl_u8(b, 29);
    cl_u8(b, 0); /* BUFFER_RENDER_TARGET_0=0, memory_format=RASTER=0, flip=0 */
    cl_u16(b, (output_format & 0x3Fu) << 4);
    cl_u24(b, (stride & 0xFFFFFu) << 4);
    cl_u16(b, 0); /* height: 0 = derive from TileRenderingModeCfgCommon */
    cl_u32(b, addr);
}

/* opcode(1) + [clear_all:1|clear_zs:1|unused:6](1) = 2. */
static void emit_clear_tile_buffers(cl_buf_t *b)
{
    cl_u8(b, 25);
    cl_u8(b, 3);
}

/* opcode(1) + x(1) + y(1) = 3. */
static void emit_supertile_coordinates(cl_buf_t *b, unsigned int x, unsigned int y)
{
    cl_u8(b, 23);
    cl_u8(b, x & 0xFFu);
    cl_u8(b, y & 0xFFu);
}

static void v3d_flush_caches(io_mapping_t *v3d)
{
    IO_WORD(*v3d, V3D_CTL_L2TFLSTA) = 0;
    IO_WORD(*v3d, V3D_CTL_L2TFLEND) = 0xFFFFFFFFu;
    IO_WORD(*v3d, V3D_CTL_L2TCACTL) = V3D_L2TCACTL_L2TFLS | V3D_L2TCACTL_FLM_FLUSH;
    IO_WORD(*v3d, V3D_CTL_SLCACTL) = 0xFFFFFFFFu;
}

_kernel_oserror *v3d_clear_test(pmp_area_t *pmp, unsigned int base_page,
                                 unsigned int clear_color_rgba,
                                 unsigned int *render_target_phys_out,
                                 unsigned int *readback_pixel_out)
{
    unsigned int cl_phys, render_phys, tile_alloc_phys, tile_state_phys;
    void *cl_log, *render_log, *tile_alloc_log, *tile_state_log;
    _kernel_oserror *err;
    cl_buf_t bcl, rcl;
    unsigned int bcl_start, bcl_end, rcl_start, rcl_end;
    io_mapping_t v3d;
    unsigned int last_bfc, last_rfc, spin;
    unsigned int page = base_page;

    err = pmp_alloc(pmp, page, CL_PAGE_COUNT, &cl_phys, &cl_log);
    if (err) return err;
    page += CL_PAGE_COUNT;

    err = pmp_alloc(pmp, page, RENDER_TARGET_PAGE_COUNT, &render_phys, &render_log);
    if (err) return err;
    page += RENDER_TARGET_PAGE_COUNT;

    err = pmp_alloc(pmp, page, TILE_ALLOC_PAGE_COUNT, &tile_alloc_phys, &tile_alloc_log);
    if (err) return err;
    page += TILE_ALLOC_PAGE_COUNT;

    err = pmp_alloc(pmp, page, TILE_STATE_PAGE_COUNT, &tile_state_phys, &tile_state_log);
    if (err) return err;
    (void) tile_alloc_log;
    (void) tile_state_log;

    /* --- BCL: empty bin (no primitives, no viewport/shader setup -
     * see the NOT VERIFIED note at the top of this file) --- */
    cl_init(&bcl, cl_log, cl_phys, PAGE_SIZE);
    bcl_start = cl_paddr(&bcl);
    emit_tile_binning_mode_cfg(&bcl, CL_CLEAR_TEST_WIDTH, CL_CLEAR_TEST_HEIGHT);
    emit_flush_vcd_cache(&bcl);
    emit_start_tile_binning(&bcl);
    emit_flush(&bcl);
    bcl_end = cl_paddr(&bcl);

    /* --- RCL: clear the single tile, store it, done --- */
    cl_init(&rcl, (unsigned char *) cl_log + bcl.offset,
            cl_phys + bcl.offset, PAGE_SIZE - bcl.offset);
    rcl_start = cl_paddr(&rcl);
    emit_tile_rendering_mode_cfg_common(&rcl, CL_CLEAR_TEST_WIDTH,
                                         CL_CLEAR_TEST_HEIGHT, 2);
    emit_tile_rendering_mode_cfg_clear_colors(&rcl, 0, clear_color_rgba, 0);
    emit_tile_rendering_mode_cfg_color_rt0(&rcl, 0, 2, 0);
    emit_tile_rendering_mode_cfg_zs_clear(&rcl, 0, 1.0f);
    emit_tile_list_initial_block_size(&rcl, 0, 1);
    emit_multicore_tile_list_set_base(&rcl, tile_alloc_phys);
    emit_multicore_supertile_cfg(&rcl, 1, 1, 1, 1, 1, 1, 1, 0, 0);

    emit_tile_coordinates(&rcl, 0, 0);
    emit_end_of_loads(&rcl);
    emit_store_tile_buffer_none(&rcl);
    emit_clear_tile_buffers(&rcl);
    emit_end_of_tile_marker(&rcl);

    /* dummy store: workaround a race condition, per the reference's own
     * comment at the equivalent point in gl_rainbow_triangle.cpp */
    emit_tile_coordinates(&rcl, 0, 0);
    emit_end_of_loads(&rcl);
    emit_store_tile_buffer_rt0(&rcl, 27 /* RGBA8 */,
                                CL_CLEAR_TEST_WIDTH * 4, render_phys);
    emit_end_of_tile_marker(&rcl);

    emit_supertile_coordinates(&rcl, 0, 0);
    emit_end_of_rendering(&rcl);
    rcl_end = cl_paddr(&rcl);

    /* --- submit + wait, bounded (not the reference's infinite loop -
     * a module hanging the desktop on a GPU stall is worse than an
     * honest timeout error) --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    v3d_flush_caches(&v3d);
    last_bfc = IO_WORD(v3d, V3D_CLE_BFC) & 0xFFu;

    IO_WORD(v3d, V3D_CLE_CT0QMA) = tile_alloc_phys;
    IO_WORD(v3d, V3D_CLE_CT0QMS) = TILE_ALLOC_PAGE_COUNT * PAGE_SIZE;
    IO_WORD(v3d, V3D_CLE_CT0QTS) = tile_state_phys | V3D_CLE_CT0QTS_ENABLE;
    IO_WORD(v3d, V3D_CLE_CT0QBA) = bcl_start;
    IO_WORD(v3d, V3D_CLE_CT0QEA) = bcl_end; /* kicks off binning */

    for (spin = 0; (IO_WORD(v3d, V3D_CLE_BFC) & 0xFFu) == last_bfc; spin++)
    {
        if (spin > 5000000u)
        {
            io_unmap(&v3d);
            return _kernel_last_oserror(); /* TODO: real "bin timeout" error */
        }
    }

    v3d_flush_caches(&v3d);
    last_rfc = IO_WORD(v3d, V3D_CLE_RFC) & 0xFFu;

    IO_WORD(v3d, V3D_CLE_CT1QBA) = rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = rcl_end; /* kicks off rendering */

    for (spin = 0; (IO_WORD(v3d, V3D_CLE_RFC) & 0xFFu) == last_rfc; spin++)
    {
        if (spin > 5000000u)
        {
            io_unmap(&v3d);
            return _kernel_last_oserror(); /* TODO: real "render timeout" error */
        }
    }

    io_unmap(&v3d);

    *render_target_phys_out = render_phys;
    *readback_pixel_out = *(volatile unsigned int *) render_log;
    return NULL;
}
