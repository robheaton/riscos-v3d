/*
 * cl_copy.h - GraphicsV Render op 1 (rectangle copy) for the planned
 * GraphicsV Render accelerator (see README.md), needed for window-
 * scroll acceleration. Mirrors cl_fill.c's structure and its own
 * top-comment reasoning almost exactly, but populates each tile via
 * LoadTileBufferGeneral (reading from a source address) instead of
 * ClearTileBuffers, then stores to a separate destination address -
 * see cl_packets.h's emit_load_tile_buffer_rt0 for why this needs no
 * shader/texture-sampling machinery at all.
 *
 * Deliberately a SEPARATE file from cl_fill.c, same reasoning as
 * cl_fill.c being separate from cl_clear.c: cl_fill.c is now the
 * hardware-proven multi-tile path, and Load+Store is genuinely new,
 * unproven territory (the reference project defines
 * LoadTileBufferGeneral but never actually uses it in its own example
 * code) - keeping them apart means a bug here can't regress the fill
 * path, and the two can be diffed directly if something doesn't work.
 *
 * NON-OVERLAPPING ONLY for now, matching the original scoping's own
 * plan ("op 1 (copy), non-overlapping only, as a later pass"). The
 * PRM's own GraphicsV 13 1 doc explicitly allows overlapping source/
 * dest rectangles ("they may be overlapping" - exactly what a window
 * scroll is), but this driver controls the exact per-tile processing
 * order itself (it emits the RCL), which in principle allows a
 * memmove-style direction choice to make overlapping copies safe too -
 * genuinely more subtle than it sounds once sub-tile-granularity
 * pixel shifts are considered (a tile's load window can straddle the
 * boundary between already-overwritten and not-yet-touched memory even
 * with correct macro-level tile ordering), so it's deliberately
 * deferred rather than half-solved. Callers must reject overlapping
 * rectangles themselves before calling anything here.
 */
#ifndef CL_COPY_H
#define CL_COPY_H

#include "kernel.h"
#include "phys_buffer.h"

/*
 * Runs a `width` x `height` copy using PMP pages starting at
 * `base_page` for CL/tile_alloc/tile_state scratch buffers, from a
 * freshly-allocated+filled synthetic source buffer to a freshly-
 * allocated destination buffer (not the real screen yet). Source is
 * filled with a simple per-pixel test pattern (not a flat colour - a
 * flat colour couldn't distinguish "genuinely copied" from "happened
 * to already be there"/"left over from a previous test") before the
 * copy runs.
 *
 * On success, *src_log_out / *dest_log_out point at the two buffers'
 * starts (CPU-readable directly) and *stride_bytes_out is their shared
 * row stride, for a caller to verify dest now matches src pixel-for-
 * pixel.
 */
_kernel_oserror *v3d_copy_test(pmp_area_t *pmp, unsigned int base_page,
                                 unsigned int width, unsigned int height,
                                 void **src_log_out, void **dest_log_out,
                                 unsigned int *stride_bytes_out,
                                 unsigned int *tiles_x_out,
                                 unsigned int *tiles_y_out);

/*
 * Same Load+Store copy, but targeting the real screen framestore
 * directly - no PMP allocation for source or dest at all (that memory
 * already exists; only the CL/tile_alloc/tile_state scratch buffers
 * still come from `pmp`, starting at `base_page`). (src_x, src_y) and
 * (dest_x, dest_y) are both in this driver's own top-down-from-
 * ScreenStart convention (see cl_fill.h's own comment on this - the
 * same applies here), NOT GraphicsV's bottom-left-origin one.
 *
 * The CALLER MUST have already confirmed the two `width` x `height`
 * rectangles at (src_x,src_y) and (dest_x,dest_y) do not overlap -
 * this function does not check that itself (see this header's top
 * comment on why overlapping copies are out of scope for now).
 */
_kernel_oserror *v3d_copy_screen(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int src_x, unsigned int src_y,
                                   unsigned int dest_x, unsigned int dest_y,
                                   unsigned int width, unsigned int height,
                                   unsigned int screen_phys_base,
                                   unsigned int screen_stride,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out);

#endif /* CL_COPY_H */
