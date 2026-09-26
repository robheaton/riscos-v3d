/*
 * cl_packets.h - V3D control-list packet emitters, shared by cl_clear.c
 * (hardware-proven M2/M3 single-64x64-tile path) and cl_fill.c (step 2's
 * multi-tile generalisation).
 *
 * Pulled out of cl_clear.c verbatim (byte-for-byte identical emission -
 * this is a pure move, not a rewrite) so the multi-tile code doesn't
 * have to duplicate every packet emitter by hand. Every field's bit
 * width/order/position is transcribed from the MIT-licensed bare-metal
 * reference's C++ struct definitions (src/device/v3d/v3d_cl.hpp,
 * github.com/Random06457/rpi4-gpu-bare-metal-examples), not reproduced
 * by relying on our own compiler's bitfield layout - see cl_clear.c's
 * original top-of-file comment (still there) for the full derivation
 * history and hardware-confirmed fixes (the always-present viewport/
 * clip/raster state-setup block, the removed SupertileCoordinates call,
 * etc.) that shaped these exact packets.
 */
#ifndef CL_PACKETS_H
#define CL_PACKETS_H

#include "cl_builder.h"
#include "v3d_regs.h"
#include "io_map.h"

/* opcode(1) + [init/alloc block size=0, ntargets-1=0|maxbpp|msaa|dbuf=0,
 * unused, unused](4) + width-1(2) + height-1(2) = 9 bytes. */
static inline void emit_tile_binning_mode_cfg(cl_buf_t *b, unsigned int w, unsigned int h)
{
    cl_u8(b, 120);
    cl_u8(b, 0);
    cl_u8(b, 0);
    cl_u8(b, 0);
    cl_u8(b, 0);
    cl_u16(b, w - 1);
    cl_u16(b, h - 1);
}

static inline void emit_flush_vcd_cache(cl_buf_t *b) { cl_u8(b, 19); }
static inline void emit_start_tile_binning(cl_buf_t *b) { cl_u8(b, 6); }
static inline void emit_flush(cl_buf_t *b) { cl_u8(b, 4); }

/* opcode(1) + address u32(4) = 5. */
static inline void emit_occlusion_query_counter(cl_buf_t *b, unsigned int addr)
{
    cl_u8(b, 92);
    cl_u32(b, addr);
}

/* opcode(1) + left(2) + bottom(2) + width(2) + height(2) = 9. */
static inline void emit_clip_window(cl_buf_t *b, unsigned int left, unsigned int bottom,
                              unsigned int w, unsigned int h)
{
    cl_u8(b, 107);
    cl_u16(b, left);
    cl_u16(b, bottom);
    cl_u16(b, w);
    cl_u16(b, h);
}

/* opcode(1) + byte1[ff:1|rf:1|cw:1|depthoff:1|linerast:2|oversample:2]
 * + byte2[unused:3|d3dwire:1|depthfunc:3|zupdate:1]
 * + byte3[earlyz:1|earlyzupd:1|stencil:1|blend:1|d3dpointfill:1|
 *         d3dprovoking:1|unused:2] = 4. */
static inline void emit_cfg_bits(cl_buf_t *b, int ff_prim, int rf_prim, int clockwise,
                           int depth_off, unsigned int line_raster,
                           unsigned int oversample, int d3d_wireframe,
                           unsigned int depth_test_func, int z_update,
                           int early_z, int early_z_update, int stencil,
                           int blend, int d3d_point_fill, int d3d_provoking)
{
    unsigned int b1 = (ff_prim ? 1u : 0u) | ((rf_prim ? 1u : 0u) << 1) |
        ((clockwise ? 1u : 0u) << 2) | ((depth_off ? 1u : 0u) << 3) |
        ((line_raster & 0x3u) << 4) | ((oversample & 0x3u) << 6);
    unsigned int b2 = ((d3d_wireframe ? 1u : 0u) << 3) |
        ((depth_test_func & 0x7u) << 4) | ((z_update ? 1u : 0u) << 7);
    unsigned int b3 = (early_z ? 1u : 0u) | ((early_z_update ? 1u : 0u) << 1) |
        ((stencil ? 1u : 0u) << 2) | ((blend ? 1u : 0u) << 3) |
        ((d3d_point_fill ? 1u : 0u) << 4) | ((d3d_provoking ? 1u : 0u) << 5);
    cl_u8(b, 96);
    cl_u8(b, b1);
    cl_u8(b, b2);
    cl_u8(b, b3);
}

static inline void emit_point_size(cl_buf_t *b, float size) { cl_u8(b, 104); cl_f32(b, size); }
static inline void emit_line_width(cl_buf_t *b, float w) { cl_u8(b, 105); cl_f32(b, w); }

static inline void emit_clipper_xy_scaling(cl_buf_t *b, float w, float h)
{
    cl_u8(b, 110);
    cl_f32(b, w);
    cl_f32(b, h);
}

static inline void emit_clipper_z_scale_and_offset(cl_buf_t *b, float scale, float offset)
{
    cl_u8(b, 111);
    cl_f32(b, scale);
    cl_f32(b, offset);
}

static inline void emit_clipper_z_min_max(cl_buf_t *b, float zmin, float zmax)
{
    cl_u8(b, 109);
    cl_f32(b, zmin);
    cl_f32(b, zmax);
}

/* opcode(1) + [centre_x:22(fp14.8)|coarse_x:10](4) +
 * [centre_y:22(fp14.8)|coarse_y:10](4) = 9. */
static inline void emit_viewport_offset(cl_buf_t *b, float x, float y,
                                  unsigned int coarse_x, unsigned int coarse_y)
{
    unsigned int vx = ((unsigned int) (x * 256.0f) & 0x3FFFFFu) |
        ((coarse_x & 0x3FFu) << 22);
    unsigned int vy = ((unsigned int) (y * 256.0f) & 0x3FFFFFu) |
        ((coarse_y & 0x3FFu) << 22);
    cl_u8(b, 108);
    cl_u32(b, vx);
    cl_u32(b, vy);
}

static inline void emit_color_write_masks(cl_buf_t *b, unsigned int mask)
{
    cl_u8(b, 87);
    cl_u32(b, mask);
}

static inline void emit_blend_constant_color(cl_buf_t *b, unsigned int r, unsigned int g,
                                       unsigned int bl, unsigned int a)
{
    cl_u8(b, 86);
    cl_u16(b, r);
    cl_u16(b, g);
    cl_u16(b, bl);
    cl_u16(b, a);
}

static inline void emit_zero_all_flat_shade_flags(cl_buf_t *b) { cl_u8(b, 97); }
static inline void emit_zero_all_non_perspective_flags(cl_buf_t *b) { cl_u8(b, 99); }
static inline void emit_zero_all_centroid_flags(cl_buf_t *b) { cl_u8(b, 88); }

/* opcode(1) + [count:5|unused:2|enable:1](1) = 2. */
static inline void emit_transform_feedback_specs(cl_buf_t *b, unsigned int count, int enable)
{
    cl_u8(b, 74);
    cl_u8(b, (count & 0x1Fu) | ((enable ? 1u : 0u) << 7));
}

/* opcode(1) + [mask:4|unused:4](1) + unused(1) + coverage u16(2) = 5.
 * coverage is the top 16 bits of the f32 bit pattern. */
static inline void emit_sample_state(cl_buf_t *b, unsigned int mask, float coverage)
{
    union { float f; unsigned int u; } conv;
    conv.f = coverage;
    cl_u8(b, 91);
    cl_u8(b, mask & 0xFu);
    cl_u8(b, 0);
    cl_u16(b, (conv.u >> 16) & 0xFFFFu);
}

/* opcode(1) + [bin:4|render:4](1) = 2. */
static inline void emit_vcm_cache_size(cl_buf_t *b, unsigned int bin_count, unsigned int render_count)
{
    cl_u8(b, 71);
    cl_u8(b, (bin_count & 0xFu) | ((render_count & 0xFu) << 4));
}

/* opcode(1) + [sub_id=0|ntargets-1=0](1) + width(2) + height(2)
 * + [bpp:2|msaa:1|dbuf:1|unused:1|earlyz_dir:1|earlyz_dis:1|depth_type:4
 *    |earlyzs_clear:1|unused:4](2) + unused(1) = 9 bytes. */
static inline void emit_tile_rendering_mode_cfg_common(cl_buf_t *b, unsigned int w,
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
static inline void emit_tile_rendering_mode_cfg_clear_colors(cl_buf_t *b,
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
static inline void emit_tile_rendering_mode_cfg_color_rt0(cl_buf_t *b,
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
static inline void emit_tile_rendering_mode_cfg_zs_clear(cl_buf_t *b,
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
static inline void emit_tile_list_initial_block_size(cl_buf_t *b,
                                               unsigned int block_size,
                                               int auto_chained)
{
    cl_u8(b, 126);
    cl_u8(b, (block_size & 0x3u) | ((auto_chained ? 1u : 0u) << 2));
}

/* opcode(1) + [tile_list_set:4=0|unused:2|address:26](4) = 5.
 * addr must be 64-byte aligned (guaranteed: it's page-aligned). */
static inline void emit_multicore_tile_list_set_base(cl_buf_t *b, unsigned int addr)
{
    cl_u8(b, 123);
    cl_u32(b, (addr >> 6) << 6);
}

/* opcode(1) + w-1(1) + h-1(1) + frame_w_super(1) + frame_h_super(1)
 * + [frame_w_tile:12|frame_h_tile:12](3)
 * + [multicore:1|unused:3|raster:1|bin_lists-1:3](1) = 9. */
static inline void emit_multicore_supertile_cfg(cl_buf_t *b,
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
static inline void emit_tile_coordinates(cl_buf_t *b, unsigned int col, unsigned int row)
{
    cl_u8(b, 124);
    cl_u24(b, (col & 0xFFFu) | ((row & 0xFFFu) << 12));
}

static inline void emit_end_of_loads(cl_buf_t *b) { cl_u8(b, 26); }
static inline void emit_end_of_tile_marker(cl_buf_t *b) { cl_u8(b, 27); }
static inline void emit_end_of_rendering(cl_buf_t *b) { cl_u8(b, 13); }

/* opcode(1) + [buffer=8(NONE):4|fmt:3|flip:1](1) + dither/decimate/fmt(2)
 * + [unused:4|height_ub_stride:20](3) + height(2) + address(4) = 13.
 * All-zero payload except buffer_to_store=BUFFER_NONE, matching the
 * reference's no-arg StoreTileBufferGeneral() used as an end-of-loads
 * marker before a clear (nothing has been loaded, nothing to store yet). */
static inline void emit_store_tile_buffer_none(cl_buf_t *b)
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
static inline void emit_store_tile_buffer_rt0(cl_buf_t *b, unsigned int output_format,
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
static inline void emit_clear_tile_buffers(cl_buf_t *b)
{
    cl_u8(b, 25);
    cl_u8(b, 3);
}

static inline void v3d_flush_caches(io_mapping_t *v3d)
{
    IO_WORD(*v3d, V3D_CTL_L2TFLSTA) = 0;
    IO_WORD(*v3d, V3D_CTL_L2TFLEND) = 0xFFFFFFFFu;
    IO_WORD(*v3d, V3D_CTL_L2TCACTL) = V3D_L2TCACTL_L2TFLS | V3D_L2TCACTL_FLM_FLUSH;
    IO_WORD(*v3d, V3D_CTL_SLCACTL) = 0xFFFFFFFFu;
}

#endif /* CL_PACKETS_H */
