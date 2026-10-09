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
 * `v3d_copy_screen` (direct, single-pass) is NON-OVERLAPPING ONLY,
 * matching the original scoping's own plan ("op 1 (copy), non-
 * overlapping only, as a later pass"). The PRM's own GraphicsV 13 1
 * doc explicitly allows overlapping source/dest rectangles ("they may
 * be overlapping" - exactly what a window scroll is) - a direct
 * single-pass copy is NOT safe for this in general: a tile's Load and
 * Store both cover a fixed 64x64-pixel block, but source and
 * destination are usually shifted by something other than a whole
 * number of tiles (a scroll by 20 pixels, say), so one tile's Store
 * can overwrite pixels a DIFFERENT, not-yet-processed tile's Load
 * still needs - no ordering of the same tile grid avoids this once the
 * shift is smaller than one tile in either dimension. `v3d_copy_screen_
 * overlapping` below handles this instead, by routing through a
 * scratch buffer (see its own comment) - callers should use that one
 * whenever the two rectangles might overlap, and reserve the direct
 * `v3d_copy_screen` for cases already known not to.
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
 * 16bpp counterpart of v3d_copy_test (2026-09-30 - see cl_copy.c's own
 * comment for the full reasoning) - isolated synthetic-buffer proof
 * for the copy path's LOAD side at 16-bit width, before the real
 * screen framestore is ever touched at this depth. Same contract as
 * v3d_copy_test, just 2 bytes/pixel and RGBA5551 throughout.
 */
_kernel_oserror *v3d_copy_test_16bpp(pmp_area_t *pmp, unsigned int base_page,
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
 *
 * `bytes_per_pixel`/`format_code` (added for 16bpp support, 2026-09-30
 * - see README/memory for the real-hardware derivation) must match
 * the screen's actual current mode: 4/27 (RGBA8) for 32bpp, 2/45
 * (RGBA5551 - confirmed to share RISC OS's own RGB555 channel
 * positions, not guessed) for 16bpp. Getting these wrong from the
 * actual active mode would corrupt the copy, not just misrender it -
 * callers must derive both from the same screen-info read that
 * produced `screen_phys_base`/`screen_stride`, never a stale or
 * assumed value.
 */
_kernel_oserror *v3d_copy_screen(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int src_x, unsigned int src_y,
                                   unsigned int dest_x, unsigned int dest_y,
                                   unsigned int width, unsigned int height,
                                   unsigned int screen_phys_base,
                                   unsigned int screen_stride,
                                   unsigned int bytes_per_pixel,
                                   unsigned int format_code,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out);

/*
 * Overlap-safe version of v3d_copy_screen, correct for ANY overlap, not just
 * shifts of at least a full tile. By default (g_v3d_tune.overlap_mode 1) it is
 * ONE pass over the tiles in an order chosen from the sign of the shift (see
 * cl_copy.c); with overlap_mode 0 it goes through a bounce buffer instead
 * (source -> scratch, then scratch -> dest).
 *
 * Size: the one-pass mode has no cap of its own - a rectangle of more than
 * max_job_tiles tiles is cut into bands (0.12), so full-screen scrolls at
 * 1920x1200 or 4K are accelerated. The bounce-buffer mode is bounded by the
 * buffer's page cap (PMP_MAX_PAGES_PER_CALL, phys_buffer.h) and refuses a bigger
 * copy with a clear error, so the caller's own fallback (leaving the GraphicsV
 * call unclaimed, same as everywhere else in this driver) takes over.
 *
 * `bytes_per_pixel`/`format_code` - see v3d_copy_screen's own comment,
 * same contract and same real-hardware-derived values.
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
                                               unsigned int *tiles_y_out);

/*
 * Isolated test for v3d_copy_screen_overlapping, against a synthetic
 * PMP buffer instead of the real screen - exactly mirrors a real
 * scroll (same buffer, two overlapping sub-rectangles) rather than the
 * two-separate-buffers shape v3d_copy_test uses, since that's the
 * actual hazard this function exists to handle safely.
 *
 * Allocates one buffer big enough to hold both a `width` x `height`
 * source rectangle at (0,0) and a destination rectangle at
 * (shift_x, shift_y) - deliberately NOT required to be a multiple of
 * the 64px tile size, since a sub-tile shift is exactly the case a
 * naive direct copy gets wrong (see cl_copy.h's top comment). Fills
 * the whole buffer with a per-pixel test pattern (pixel at `(x,y)` =
 * `0xFF000000|(x<<12)|y`) before the copy runs, so the destination
 * rectangle's EXPECTED post-copy
 * value at any pixel is computable analytically (from the
 * corresponding source-side coordinate), without needing to have
 * saved anything - important here specifically because the source
 * region's own pixels may themselves get overwritten by the
 * destination write when they overlap, so "does dest match src now"
 * would be circular; "does dest match what src originally had" is the
 * real question.
 *
 * On success, *buf_log_out points at the buffer's start (CPU-readable
 * directly) and *stride_bytes_out is its row stride, for a caller to
 * compute expected values and sample points itself.
 */
_kernel_oserror *v3d_copy_overlap_test(pmp_area_t *pmp, unsigned int base_page,
                                         unsigned int width, unsigned int height,
                                         unsigned int shift_x, unsigned int shift_y,
                                         void **buf_log_out,
                                         unsigned int *stride_bytes_out,
                                         unsigned int *tiles_x_out,
                                         unsigned int *tiles_y_out);

#endif /* CL_COPY_H */
