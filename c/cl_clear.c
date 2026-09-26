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
 * UPDATE (2026-09-20, after first hardware test): a BCL with just
 * TileBinningModeCfg + OP_START_TILE_BINNING + OP_FLUSH and nothing
 * else left V3D_CLE_BFC never advancing - binning never completed.
 * Confirms the risk flagged in the original version of this comment.
 * Fix: reproduced the reference's full always-present viewport/clip/
 * raster state-setup block (ClipWindow through VcmCacheSize) verbatim,
 * values and all, minus only GlShaderState/VertexArrayPrims since we
 * draw nothing.
 *
 * UPDATE (2026-09-24): that fixed binning - V3D_CTL_INT_STS read back
 * V3D_INT_FLDONE (bin done) on a later hardware test, and BFC advanced
 * correctly. Rendering then hung instead: CT1CA stuck at 0x00000000
 * with CT1CS showing CTRTSD=1 (one level into a nested sub-list) and
 * CTRUN=1 (genuinely running, not stalled/errored). The render thread
 * had followed a branch into memory we never set up: our RCL called
 * SupertileCoordinates (which the reference always pairs with a prior
 * StartAddressOfGenericTileList, pointing at a small "indirect" list
 * of its own) without ever writing that indirect-list address
 * ourselves - so the branch target was left at its zero default.
 * Removed the SupertileCoordinates call entirely: both TileCoordinates
 * blocks above it already do our real work (clear + store) inline, so
 * we have no supertiles to walk and don't need the generic-tile-list
 * indirection at all for a single hardcoded tile. Not yet confirmed on
 * hardware.
 */

#include "cl_clear.h"
#include "cl_builder.h"
#include "v3d_regs.h"
#include "io_map.h"
#include "sprite.h"

#define CL_PAGE_COUNT           1u  /* BCL+RCL combined, both tiny */
#define RENDER_TARGET_PAGE_COUNT 4u /* 64*64*4 bytes = 16384 = 4 pages */
#define TILE_ALLOC_PAGE_COUNT    3u /* align(1*1*1*64,4096) + 64*64*2, per
                                      * the reference's own sizing formula */
#define TILE_STATE_PAGE_COUNT    1u /* 256 bytes/tile (V3D>=40 - we're 4.2) */
#define CLEAR_TEST_TOTAL_PAGES  (CL_PAGE_COUNT + RENDER_TARGET_PAGE_COUNT + \
                                  TILE_ALLOC_PAGE_COUNT + TILE_STATE_PAGE_COUNT)

/* M3: same render target, prefixed with a sprite area+header (60
 * bytes - SPRITE_HEADER_BYTES) so the GPU's own output buffer is
 * directly usable as a RISC OS sprite, no copy needed. 64*64*4 +
 * 60 = 16444 bytes, rounded up to whole pages. */
#define SPRITE_RENDER_PAGE_COUNT \
    (((CL_CLEAR_TEST_WIDTH * CL_CLEAR_TEST_HEIGHT * 4u) + SPRITE_HEADER_BYTES \
      + PAGE_SIZE - 1u) / PAGE_SIZE)

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

/*
 * Reference's BCL always includes the block below between
 * OP_START_TILE_BINNING and its shader-state/VertexArrayPrims/OP_FLUSH
 * epilogue - the risk noted at the top of this file was that skipping
 * it (since we draw nothing) might leave the CLE unable to reach
 * completion. Hardware confirmed binning never completed with it
 * skipped, so it's reproduced here verbatim (values copied from the
 * reference's own call, which are otherwise geometry-related and
 * inert without any primitives to apply them to).
 */

/* opcode(1) + address u32(4) = 5. */
static void emit_occlusion_query_counter(cl_buf_t *b, unsigned int addr)
{
    cl_u8(b, 92);
    cl_u32(b, addr);
}

/* opcode(1) + left(2) + bottom(2) + width(2) + height(2) = 9. */
static void emit_clip_window(cl_buf_t *b, unsigned int left, unsigned int bottom,
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
static void emit_cfg_bits(cl_buf_t *b, int ff_prim, int rf_prim, int clockwise,
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

static void emit_point_size(cl_buf_t *b, float size) { cl_u8(b, 104); cl_f32(b, size); }
static void emit_line_width(cl_buf_t *b, float w) { cl_u8(b, 105); cl_f32(b, w); }

static void emit_clipper_xy_scaling(cl_buf_t *b, float w, float h)
{
    cl_u8(b, 110);
    cl_f32(b, w);
    cl_f32(b, h);
}

static void emit_clipper_z_scale_and_offset(cl_buf_t *b, float scale, float offset)
{
    cl_u8(b, 111);
    cl_f32(b, scale);
    cl_f32(b, offset);
}

static void emit_clipper_z_min_max(cl_buf_t *b, float zmin, float zmax)
{
    cl_u8(b, 109);
    cl_f32(b, zmin);
    cl_f32(b, zmax);
}

/* opcode(1) + [centre_x:22(fp14.8)|coarse_x:10](4) +
 * [centre_y:22(fp14.8)|coarse_y:10](4) = 9. */
static void emit_viewport_offset(cl_buf_t *b, float x, float y,
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

static void emit_color_write_masks(cl_buf_t *b, unsigned int mask)
{
    cl_u8(b, 87);
    cl_u32(b, mask);
}

static void emit_blend_constant_color(cl_buf_t *b, unsigned int r, unsigned int g,
                                       unsigned int bl, unsigned int a)
{
    cl_u8(b, 86);
    cl_u16(b, r);
    cl_u16(b, g);
    cl_u16(b, bl);
    cl_u16(b, a);
}

static void emit_zero_all_flat_shade_flags(cl_buf_t *b) { cl_u8(b, 97); }
static void emit_zero_all_non_perspective_flags(cl_buf_t *b) { cl_u8(b, 99); }
static void emit_zero_all_centroid_flags(cl_buf_t *b) { cl_u8(b, 88); }

/* opcode(1) + [count:5|unused:2|enable:1](1) = 2. */
static void emit_transform_feedback_specs(cl_buf_t *b, unsigned int count, int enable)
{
    cl_u8(b, 74);
    cl_u8(b, (count & 0x1Fu) | ((enable ? 1u : 0u) << 7));
}

/* opcode(1) + [mask:4|unused:4](1) + unused(1) + coverage u16(2) = 5.
 * coverage is the top 16 bits of the f32 bit pattern. */
static void emit_sample_state(cl_buf_t *b, unsigned int mask, float coverage)
{
    union { float f; unsigned int u; } conv;
    conv.f = coverage;
    cl_u8(b, 91);
    cl_u8(b, mask & 0xFu);
    cl_u8(b, 0);
    cl_u16(b, (conv.u >> 16) & 0xFFFFu);
}

/* opcode(1) + [bin:4|render:4](1) = 2. */
static void emit_vcm_cache_size(cl_buf_t *b, unsigned int bin_count, unsigned int render_count)
{
    cl_u8(b, 71);
    cl_u8(b, (bin_count & 0xFu) | ((render_count & 0xFu) << 4));
}

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

static void v3d_flush_caches(io_mapping_t *v3d)
{
    IO_WORD(*v3d, V3D_CTL_L2TFLSTA) = 0;
    IO_WORD(*v3d, V3D_CTL_L2TFLEND) = 0xFFFFFFFFu;
    IO_WORD(*v3d, V3D_CTL_L2TCACTL) = V3D_L2TCACTL_L2TFLS | V3D_L2TCACTL_FLM_FLUSH;
    IO_WORD(*v3d, V3D_CTL_SLCACTL) = 0xFFFFFFFFu;
}

/*
 * Writes the documented CTSUBS restart trigger to a CLE thread's CS
 * register, then waits for the hardware to actually finish processing
 * that write before returning.
 *
 * Added after a hardware test showed CTSUBS clearing CA/EA to zero as
 * part of leaving halt - meaning the documented "restarts if CA != EA"
 * condition can never be true in that same instant, so the thread just
 * re-stops immediately. The new job's QMA/QMS/QTS/QBA/QEA were written
 * right after, with no gap at all - a write-after-write hazard against
 * a hardware state machine still mid-transition - and that test's
 * readback showed the new job's registers were never latched.
 *
 * v1 of this fix used a fixed-count delay loop instead of polling
 * anything, reasoning that there was "nothing meaningful to wait for
 * until new work is queued". That conflated two different things: a
 * job completing (genuinely nothing to poll for yet) and the hardware
 * finishing its response to *this* write (CTSUBS clearing back to 0,
 * which the PRM documents happens once the thread leaves "stopped at
 * halt"). The fixed delay worked once, then the identical failure mode
 * came back on a later run - exactly what you'd expect from timing
 * that sometimes isn't long enough rather than a real fix. Poll the
 * actual condition instead, bounded rather than assumed.
 *
 * v2 (this version) also stopped writing CTSUBS *unconditionally*
 * before every submission. Hardware testing on a freshly-loaded module
 * (thread never halted at all - CTRUN should already be sensible)
 * reproduced the exact same failure this fix was meant to solve. The
 * PRM only documents CTSUBS as clearing CA/EA when actually leaving
 * "stopped at halt" - writing it to a thread that was never halted is
 * outside what's documented, and evidently doesn't leave CA/EA alone
 * either, inflicting the same CA==EA race onto a thread that had no
 * need to be restarted at all. Now reads CS first and only issues the
 * restart write (and its poll) when the thread is actually stopped
 * with CTSUBS or CTERR set - a thread that's running, or cleanly
 * stopped-at-end, is left alone.
 *
 * v3 (this version): v2's "cleanly stopped-at-end, leave it alone"
 * case turned out to be exactly wrong for a *second* submission in one
 * session. Hardware testing (2026-09-26) found a job that completes
 * normally (CTRUN clears itself, CS reads right back to 0x00000000 -
 * indistinguishable from a truly virgin thread by CS alone) leaves CA
 * sitting at that job's own address. The next submission's fresh
 * QBA/QEA write is never picked up - CA/EA at the following timeout
 * still showed the *previous* job's addresses, not the new one, even
 * though V3D_PowerOn's reset had already been confirmed clean via
 * V3D_ReadIdent (CT0CS=CT1CS=0) and the new job used entirely
 * different, never-before-used physical pages (ruling out a stale-
 * address or cache-reuse explanation). CS alone can't tell "never run
 * a job" from "just finished one" - but CA can: V3D_ReadIdent already
 * showed CA reads 0 on a genuinely virgin thread. So: never touch a
 * running thread (CTRUN); skip the kick only when CA is still 0 *and*
 * CS shows nothing already latched (the v2 case, preserved as-is);
 * otherwise - any thread that has run a job before, however it
 * stopped - always issue the kick.
 */
static void v3d_restart_thread(io_mapping_t *v3d, unsigned int cs_reg,
                                 unsigned int ca_reg)
{
    unsigned int spin;
    unsigned int cs = IO_WORD(*v3d, cs_reg);
    unsigned int ca = IO_WORD(*v3d, ca_reg);

    if (cs & V3D_CT_CS_CTRUN)
        return; /* running - never write CTSUBS to a running thread */

    if (!ca && !(cs & (V3D_CT_CS_CTSUBS | V3D_CT_CS_CTERR)))
        return; /* genuinely virgin (CA==0, nothing latched) - nothing to do */

    IO_WORD(*v3d, cs_reg) = V3D_CT_CS_CTSUBS;

    for (spin = 0; spin < 1000000u; spin++)
    {
        if (!(IO_WORD(*v3d, cs_reg) & V3D_CT_CS_CTSUBS))
            return;
    }
    /* Gave it a bounded chance; proceed regardless and let the
     * submission + BFC/RFC timeout (with full diagnostics) be the
     * real verdict rather than silently looping forever here. */
}

/*
 * Waits for a CLE frame counter (V3D_CLE_BFC/RFC) to advance past
 * `initial`, polling in short bursts rather than holding one long-lived
 * OS_Memory 14 mapping. That matters here, not just as a style choice:
 * the PRM is explicit that OS_Memory 14 hands out a single, system-wide
 * shared mapping window, and warns that holding it across a long
 * operation risks an interrupt handler or another driver's own
 * temporary mapping silently stealing it from under us - after which
 * every further read in this loop would be looking at some other
 * device's registers, not V3D's, and would spin until the outer
 * timeout regardless of what the GPU actually did. Re-mapping fresh
 * every `burst` iterations keeps any single hold on the shared window
 * short. Same total iteration ceiling as the original single-mapping
 * version (500 * 10000 = 5,000,000), just no longer in one unbroken
 * hold.
 */
/* Must match _kernel_oserror.errmess's declared size (kernel.h: char
 * errmess[252]). Hard-capped here rather than just relying on the
 * call sites keeping the message short - a first version of this
 * diagnostic added enough fields that its worst-case length (291
 * bytes, computed after the fact) would have overflowed a 252-byte
 * buffer in a kernel module. That's not a cosmetic bug. Every append
 * below is bounds-checked against this regardless of how the message
 * is edited in future. */
#define ERRMESS_CAPACITY 252u

static void append_str(char *dst, unsigned int *pos, const char *s)
{
    while (*s && *pos < ERRMESS_CAPACITY - 1u)
        dst[(*pos)++] = *s++;
}

static void append_hex32(char *dst, unsigned int *pos, unsigned int v)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    if (*pos + 10u >= ERRMESS_CAPACITY)
        return; /* not enough room left for "0x" + 8 hex digits */
    dst[(*pos)++] = '0';
    dst[(*pos)++] = 'x';
    for (i = 28; i >= 0; i -= 4)
        dst[(*pos)++] = hex[(v >> i) & 0xFu];
}

/*
 * On timeout, reads V3D_CTL_INT_STS and decodes it into out_err's
 * message before returning it - a raw "timed out" is a dead end to
 * debug further; this at least says whether the hardware itself
 * reports anything (in particular V3D_INT_OUTOMEM - see the register's
 * comment in v3d_regs.h for why a stalled-on-OOM binner would look
 * identical to a never-started one from BFC's perspective alone).
 *
 * Also attempts recovery, not just diagnosis: v3d_restart_thread()
 * deliberately leaves a thread alone whenever CS shows CTRUN set,
 * treating that as "already running, nothing to do" (see its own
 * comment - writing the CTSUBS restart to a thread that was never
 * halted reproduced the very race it exists to fix). But CTRUN just
 * means "running" per the PRM - it doesn't distinguish "genuinely
 * making progress" from "hung mid-job", and a timed-out poll is
 * exactly the case where a thread reporting CTRUN=1 is almost
 * certainly the latter. Left alone, that thread stays wedged in this
 * state for the rest of the session: restart_thread() keeps declining
 * to touch it on every later submission (any SWI, any buffer address),
 * since CTRUN never clears on its own. That fits everything logged in
 * "attempt 7"/"attempt 8" far better than an address-range theory
 * does: BCMSupport's real ARM/VC split (queried in attempt 8) sits at
 * 0x3C000000 (960MB) but every failing CA/EA/QBA/QEA on record has
 * been ~0x03000000-0x030C0000 (~48-50MB) - nowhere near it. What those
 * attempts actually had in common is that ClearToSprite had already
 * been run (and presumably wedged the thread) earlier in the same
 * session, and every attempt after that point failed identically
 * regardless of address, including a previously-100%-reliable
 * ClearTest. So: if CTRUN is set here, request a stop the documented
 * way (V3D_CT_CS_CTRUN is R/W; per its own comment in v3d_regs.h,
 * writing 1 stops the thread rather than starting one) and poll
 * briefly for it to actually leave the running state, so the *next*
 * submission's restart_thread() finds a genuinely stopped thread
 * (CTSUBS/CTERR set) instead of a permanently wedged CTRUN=1 it will
 * never touch. Bounded and best-effort: if the stop request itself
 * doesn't take, the caller still gets the timeout error, just with
 * that outcome recorded in the message instead of silently retried.
 */
/*
 * `bufs` lets us check CA/EA against every physical buffer this job
 * actually used, not just what we wrote to QBA/QEA - added after EA
 * read back a value that didn't match either of those, and matching
 * it to a buffer address by hand (page-layout arithmetic, not a
 * logged value) turned out to be exactly the kind of guess-disguised-
 * as-a-fact this project has been trying to avoid.
 */
typedef struct diag_bufs
{
    unsigned int cl, render_target, tile_alloc, tile_state;
} diag_bufs_t;

static void diagnose_timeout(_kernel_oserror *out_err, const char *what,
                              int is_bin, unsigned int qba, unsigned int qea,
                              const diag_bufs_t *bufs)
{
    io_mapping_t v3d;
    unsigned int sts = 0, cs = 0, ca = 0, ea = 0;
    unsigned int rb_qba = 0, rb_qea = 0;
    unsigned int mmu_ctl = 0, hub_sts = 0, vio_id = 0, vio_addr = 0;
    unsigned int pos = 0;
    int recover_attempted = 0, recover_ok = 0;

    if (!io_map(V3D_MB_BASE, &v3d))
    {
        sts = IO_WORD(v3d, V3D_CTL_INT_STS);
        mmu_ctl = IO_WORD(v3d, V3D_MMU_CTL);
        hub_sts = IO_WORD(v3d, V3D_HUB_INT_STS);
        if (mmu_ctl & V3D_MMU_CTL_ENABLE)
        {
            vio_id = IO_WORD(v3d, V3D_MMU_VIO_ID);
            vio_addr = IO_WORD(v3d, V3D_MMU_VIO_ADDR);
        }
        if (is_bin)
        {
            cs = IO_WORD(v3d, V3D_CLE_CT0CS);
            ca = IO_WORD(v3d, V3D_CLE_CT0CA);
            ea = IO_WORD(v3d, V3D_CLE_CT0EA);
            rb_qba = IO_WORD(v3d, V3D_CLE_CT0QBA);
            rb_qea = IO_WORD(v3d, V3D_CLE_CT0QEA);
        }
        else
        {
            cs = IO_WORD(v3d, V3D_CLE_CT1CS);
            ca = IO_WORD(v3d, V3D_CLE_CT1CA);
            ea = IO_WORD(v3d, V3D_CLE_CT1EA);
            rb_qba = IO_WORD(v3d, V3D_CLE_CT1QBA);
            rb_qea = IO_WORD(v3d, V3D_CLE_CT1QEA);
        }

        if (cs & V3D_CT_CS_CTRUN)
        {
            unsigned int cs_reg = is_bin ? V3D_CLE_CT0CS : V3D_CLE_CT1CS;
            unsigned int spin;

            recover_attempted = 1;
            IO_WORD(v3d, cs_reg) = V3D_CT_CS_CTRUN; /* request stop - see comment above */
            for (spin = 0; spin < 100000u; spin++)
            {
                if (!(IO_WORD(v3d, cs_reg) & V3D_CT_CS_CTRUN))
                {
                    recover_ok = 1;
                    break;
                }
            }
        }

        io_unmap(&v3d);
    }

    append_str(out_err->errmess, &pos, "V3DDriver: timeout: ");
    append_str(out_err->errmess, &pos, what);
    append_str(out_err->errmess, &pos, " CS=");
    append_hex32(out_err->errmess, &pos, cs);
    append_str(out_err->errmess, &pos, (cs & V3D_CT_CS_CTRUN) ? "(RUN" : "(STOP");
    if (cs & V3D_CT_CS_CTERR) append_str(out_err->errmess, &pos, " ERR");
    append_str(out_err->errmess, &pos, ")");
    if (recover_attempted)
        append_str(out_err->errmess, &pos, recover_ok ? " REC=stopped" : " REC=timeout");
    append_str(out_err->errmess, &pos, " STS=");
    append_hex32(out_err->errmess, &pos, sts);
    if (sts & V3D_INT_OUTOMEM) append_str(out_err->errmess, &pos, "(OOM)");
    if (sts & V3D_INT_GMPV) append_str(out_err->errmess, &pos, "(GMPV)");
    append_str(out_err->errmess, &pos, " MMU=");
    append_hex32(out_err->errmess, &pos, mmu_ctl & 1u);
    if (mmu_ctl & V3D_MMU_CTL_ENABLE)
    {
        append_str(out_err->errmess, &pos, " HUBSTS=");
        append_hex32(out_err->errmess, &pos, hub_sts);
        append_str(out_err->errmess, &pos, " VioID=");
        append_hex32(out_err->errmess, &pos, vio_id);
        append_str(out_err->errmess, &pos, " VioAddr=");
        append_hex32(out_err->errmess, &pos, vio_addr);
    }
    append_str(out_err->errmess, &pos, " CA=");
    append_hex32(out_err->errmess, &pos, ca);
    append_str(out_err->errmess, &pos, " EA=");
    append_hex32(out_err->errmess, &pos, ea);
    append_str(out_err->errmess, &pos, " QBA=w");
    append_hex32(out_err->errmess, &pos, qba);
    append_str(out_err->errmess, &pos, "/r");
    append_hex32(out_err->errmess, &pos, rb_qba);
    append_str(out_err->errmess, &pos, " QEA=w");
    append_hex32(out_err->errmess, &pos, qea);
    append_str(out_err->errmess, &pos, "/r");
    append_hex32(out_err->errmess, &pos, rb_qea);
    append_str(out_err->errmess, &pos, " tAl=");
    append_hex32(out_err->errmess, &pos, bufs->tile_alloc);
    append_str(out_err->errmess, &pos, " rTg=");
    append_hex32(out_err->errmess, &pos, bufs->render_target);
    append_str(out_err->errmess, &pos, " tSt=");
    append_hex32(out_err->errmess, &pos, bufs->tile_state);
    out_err->errmess[pos] = '\0';
}

static _kernel_oserror *v3d_poll_counter(unsigned int reg, unsigned int initial,
                                          _kernel_oserror *timeout_err,
                                          const char *what, int is_bin,
                                          unsigned int qba, unsigned int qea,
                                          const diag_bufs_t *bufs)
{
    unsigned int outer;

    for (outer = 0; outer < 500u; outer++)
    {
        io_mapping_t v3d;
        unsigned int burst;
        _kernel_oserror *err = io_map(V3D_MB_BASE, &v3d);
        if (err)
            return err;

        for (burst = 0; burst < 10000u; burst++)
        {
            if ((IO_WORD(v3d, reg) & 0xFFu) != initial)
            {
                io_unmap(&v3d);
                return NULL;
            }
        }

        io_unmap(&v3d);
    }

    diagnose_timeout(timeout_err, what, is_bin, qba, qea, bufs);
    return timeout_err;
}

/*
 * Shared by v3d_clear_test and v3d_clear_to_sprite. `render_page_count`
 * sizes the render-target buffer (callers needing extra room before
 * the pixel data - e.g. a sprite header - pass more pages);
 * `render_header_bytes` is how far into that buffer the GPU's actual
 * pixel store target starts, letting a caller prefix the buffer with
 * its own data (written through *render_log_out, which always points
 * at the very start of the buffer, not the pixel data) without it
 * being clobbered by the GPU.
 */
static _kernel_oserror *v3d_clear_run(pmp_area_t *pmp, unsigned int base_page,
                                       unsigned int clear_color_rgba,
                                       unsigned int render_page_count,
                                       unsigned int render_header_bytes,
                                       unsigned int *render_target_phys_out,
                                       void **render_target_log_out,
                                       unsigned int *readback_pixel_out)
{
    unsigned int cl_phys, render_phys, tile_alloc_phys, tile_state_phys;
    void *cl_log, *render_log, *tile_alloc_log, *tile_state_log;
    _kernel_oserror *err;
    cl_buf_t bcl, rcl;
    unsigned int bcl_start, bcl_end, rcl_start, rcl_end;
    unsigned int pixel_phys;
    io_mapping_t v3d;
    unsigned int last_bfc, last_rfc;
    unsigned int page = base_page;
    diag_bufs_t diag_bufs;

    err = pmp_alloc(pmp, page, CL_PAGE_COUNT, &cl_phys, &cl_log);
    if (err) return err;
    page += CL_PAGE_COUNT;

    err = pmp_alloc(pmp, page, render_page_count, &render_phys, &render_log);
    if (err) return err;
    page += render_page_count;
    pixel_phys = render_phys + render_header_bytes;

    err = pmp_alloc(pmp, page, TILE_ALLOC_PAGE_COUNT, &tile_alloc_phys, &tile_alloc_log);
    if (err) return err;
    page += TILE_ALLOC_PAGE_COUNT;

    err = pmp_alloc(pmp, page, TILE_STATE_PAGE_COUNT, &tile_state_phys, &tile_state_log);
    if (err) return err;

    /*
     * Zero the tile-alloc and tile-state scratch buffers before the
     * binner ever touches them. The bare-metal reference never does
     * this - its memory comes straight from a fresh boot, pristine
     * zero from power-on. RISC OS instead hands out recycled physical
     * pages via OS_Memory 12, which can carry arbitrary leftover
     * content from whatever last used them. If the binner's own
     * bookkeeping in this scratch space is sensitive to its initial
     * content, that would explain a hang whose presence depends on
     * exactly which physical pages happen to get allocated - which
     * fits: this was never reproducible with V3D_ClearTest's page
     * range, only V3D_ClearToSprite's.
     */
    {
        unsigned int i;
        volatile unsigned int *p;

        p = (volatile unsigned int *) tile_alloc_log;
        for (i = 0; i < (TILE_ALLOC_PAGE_COUNT * PAGE_SIZE) / 4u; i++)
            p[i] = 0;

        p = (volatile unsigned int *) tile_state_log;
        for (i = 0; i < (TILE_STATE_PAGE_COUNT * PAGE_SIZE) / 4u; i++)
            p[i] = 0;
    }

    diag_bufs.cl = cl_phys;
    diag_bufs.render_target = render_phys;
    diag_bufs.tile_alloc = tile_alloc_phys;
    diag_bufs.tile_state = tile_state_phys;

    /* --- BCL: empty bin (no primitives - see the reference's full
     * always-present state-setup block below, added after hardware
     * confirmed a bare TileBinningModeCfg+OP_START_TILE_BINNING+
     * OP_FLUSH left binning never completing; order and values copied
     * verbatim from the reference's gl_rainbow_triangle.cpp) --- */
    cl_init(&bcl, cl_log, cl_phys, PAGE_SIZE);
    bcl_start = cl_paddr(&bcl);
    emit_tile_binning_mode_cfg(&bcl, CL_CLEAR_TEST_WIDTH, CL_CLEAR_TEST_HEIGHT);
    emit_flush_vcd_cache(&bcl);
    emit_occlusion_query_counter(&bcl, 0);
    emit_start_tile_binning(&bcl);

    emit_clip_window(&bcl, 0, 0, CL_CLEAR_TEST_WIDTH, CL_CLEAR_TEST_HEIGHT);
    emit_cfg_bits(&bcl, 1, 1, 1, 0, 0, 0, 0, 7, 0, 0, 1, 0, 0, 0, 0);
    emit_point_size(&bcl, 1.0f);
    emit_line_width(&bcl, 1.0f);
    emit_clipper_xy_scaling(&bcl, (CL_CLEAR_TEST_WIDTH / 2) * 256.0f,
                             (CL_CLEAR_TEST_HEIGHT / 2) * -256.0f);
    emit_clipper_z_scale_and_offset(&bcl, 0.5f, 0.5f);
    emit_clipper_z_min_max(&bcl, 0.0f, 1.0f);
    emit_viewport_offset(&bcl, CL_CLEAR_TEST_WIDTH / 2, CL_CLEAR_TEST_HEIGHT / 2, 0, 0);
    emit_color_write_masks(&bcl, 0);
    emit_blend_constant_color(&bcl, 0, 0, 0, 0);
    emit_zero_all_flat_shade_flags(&bcl);
    emit_zero_all_non_perspective_flags(&bcl);
    emit_zero_all_centroid_flags(&bcl);
    emit_transform_feedback_specs(&bcl, 0, 0);
    emit_occlusion_query_counter(&bcl, 0);
    emit_sample_state(&bcl, 0xF, 1.0f);
    emit_vcm_cache_size(&bcl, 4, 4);

    /* No GlShaderState / VertexArrayPrims - we draw nothing. */
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
                                CL_CLEAR_TEST_WIDTH * 4, pixel_phys);
    emit_end_of_tile_marker(&rcl);

    /* No SupertileCoordinates / StartAddressOfGenericTileList here -
     * see the "not verified" note above emit_supertile_coordinates.
     * Both TileCoordinates blocks above already did our real work
     * (clear + store) inline; we have no supertiles to walk. */
    emit_end_of_rendering(&rcl);
    rcl_end = cl_paddr(&rcl);

    /* --- kick off binning (short-held mapping - see v3d_poll_counter's
     * comment for why long-held ones are dangerous) --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    v3d_flush_caches(&v3d);
    last_bfc = IO_WORD(v3d, V3D_CLE_BFC) & 0xFFu;

    /* Explicit, documented restart (Broadcom PRM Table 49): clears any
     * "stopped at halt"/error condition left over from a previous run
     * before relying on QBA/QEA to kick things off. Never write
     * V3D_CT_CS_CTRUN here - that bit STOPS a thread, it doesn't start
     * one. See v3d_restart_thread's comment for why there's a settle
     * delay before the writes that follow. */
    v3d_restart_thread(&v3d, V3D_CLE_CT0CS, V3D_CLE_CT0CA);

    IO_WORD(v3d, V3D_CLE_CT0QMA) = tile_alloc_phys;
    IO_WORD(v3d, V3D_CLE_CT0QMS) = TILE_ALLOC_PAGE_COUNT * PAGE_SIZE;
    IO_WORD(v3d, V3D_CLE_CT0QTS) = tile_state_phys | V3D_CLE_CT0QTS_ENABLE;
    IO_WORD(v3d, V3D_CLE_CT0QBA) = bcl_start;
    IO_WORD(v3d, V3D_CLE_CT0QEA) = bcl_end; /* kicks off binning */

    io_unmap(&v3d);

    {
        static _kernel_oserror err_bin_timeout = { 7, "" };
        err = v3d_poll_counter(V3D_CLE_BFC, last_bfc, &err_bin_timeout,
                                "BFC (binning)", 1, bcl_start, bcl_end,
                                &diag_bufs);
        if (err)
            return err;
    }

    /* --- kick off rendering --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    v3d_flush_caches(&v3d);
    last_rfc = IO_WORD(v3d, V3D_CLE_RFC) & 0xFFu;

    v3d_restart_thread(&v3d, V3D_CLE_CT1CS, V3D_CLE_CT1CA); /* see thread 0's comment */

    IO_WORD(v3d, V3D_CLE_CT1QBA) = rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = rcl_end; /* kicks off rendering */

    io_unmap(&v3d);

    {
        static _kernel_oserror err_render_timeout = { 8, "" };
        err = v3d_poll_counter(V3D_CLE_RFC, last_rfc, &err_render_timeout,
                                "RFC (render)", 0, rcl_start, rcl_end,
                                &diag_bufs);
        if (err)
            return err;
    }

    *render_target_phys_out = render_phys;
    *render_target_log_out = render_log;
    *readback_pixel_out =
        *(volatile unsigned int *) ((unsigned char *) render_log + render_header_bytes);
    return NULL;
}

_kernel_oserror *v3d_clear_test(pmp_area_t *pmp, unsigned int base_page,
                                 unsigned int clear_color_rgba,
                                 unsigned int *render_target_phys_out,
                                 unsigned int *readback_pixel_out)
{
    void *render_log;
    return v3d_clear_run(pmp, base_page, clear_color_rgba,
                          RENDER_TARGET_PAGE_COUNT, 0,
                          render_target_phys_out, &render_log,
                          readback_pixel_out);
}

_kernel_oserror *v3d_clear_to_sprite(pmp_area_t *pmp, unsigned int base_page,
                                      unsigned int clear_color_rgba,
                                      unsigned int *sprite_area_phys_out,
                                      void **sprite_area_log_out,
                                      unsigned int *readback_pixel_out)
{
    _kernel_oserror *err = v3d_clear_run(
        pmp, base_page, clear_color_rgba,
        SPRITE_RENDER_PAGE_COUNT, SPRITE_HEADER_BYTES,
        sprite_area_phys_out, sprite_area_log_out, readback_pixel_out);
    if (err)
        return err;

    /* Headers only - the GPU already wrote the pixel data (at
     * *sprite_area_log_out + SPRITE_HEADER_BYTES) as part of
     * v3d_clear_run above. Written after, but there's no race: CPU and
     * GPU touch disjoint byte ranges of the same buffer. */
    sprite_write_header(*sprite_area_log_out, CL_CLEAR_TEST_WIDTH,
                         CL_CLEAR_TEST_HEIGHT, "v3dtest");
    return NULL;
}
