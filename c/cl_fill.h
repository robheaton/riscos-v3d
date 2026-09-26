/*
 * cl_fill.h - step 2 of the planned GraphicsV Render accelerator (see
 * README.md's "GraphicsV Render accelerator" section): generalise
 * cl_clear.c's fixed-64x64-single-tile clear+store to an arbitrary
 * rectangle spanning any number of tiles, still writing into a PMP-
 * owned test buffer (not the real framestore yet - that's step 3).
 *
 * Deliberately a SEPARATE file from cl_clear.c rather than a rewrite of
 * v3d_clear_run: cl_clear.c is the hardware-proven M2/M3 path (see its
 * own history of hard-won fixes), and this multi-tile generalisation is
 * new, unproven code - keeping them apart means a bug here can't
 * regress ClearTest/ClearToSprite, and the two can be diffed directly
 * against each other if something doesn't work on hardware.
 *
 * Multi-tile sizing formulas (tile_alloc's "align(N*64,4096) + 64*64*2",
 * tile_state's "N*256 bytes", the supertile-count-selection algorithm)
 * are transcribed from the same MIT-licensed reference this whole
 * project verifies against (github.com/Random06457/rpi4-gpu-bare-metal-
 * examples, src/kernel/examples/gl_rainbow_triangle.cpp - getSuperTiles()
 * and the tile_alloc_size/tile_state sizing lines), fetched and read
 * directly 2026-09-26, not guessed or extrapolated from the single-tile
 * case already in cl_clear.c.
 */
#ifndef CL_FILL_H
#define CL_FILL_H

#include "kernel.h"
#include "phys_buffer.h"

/*
 * Runs a solid-colour fill of a `width` x `height` rectangle using PMP
 * pages starting at `base_page`, into a freshly-allocated test buffer
 * (not the real screen - see README's step 3 for why that's a separate,
 * later piece of work). The buffer is deliberately padded up to a whole
 * number of 64x64 tiles in each dimension (not just `width`x`height`),
 * so a partial edge tile's full-tile store can never write past the
 * buffer even if V3D's per-tile store doesn't clip to the declared
 * frame size as tightly as expected - real hardware behaviour for that
 * edge case hasn't been verified yet, so this sidesteps the question
 * for the test rather than assuming an answer.
 *
 * On success, *target_log_out points at the padded buffer's start (CPU-
 * readable directly), *target_stride_bytes_out is its real row stride
 * (== padded width * 4, not `width` * 4 - needed to correctly compute
 * sample offsets when checking readback), and *tiles_x_out / *tiles_y_out
 * report the tile grid used (diagnostic).
 */
_kernel_oserror *v3d_fill_test(pmp_area_t *pmp, unsigned int base_page,
                                 unsigned int width, unsigned int height,
                                 unsigned int clear_color_rgba,
                                 unsigned int *target_phys_out,
                                 void **target_log_out,
                                 unsigned int *target_stride_bytes_out,
                                 unsigned int *tiles_x_out,
                                 unsigned int *tiles_y_out);

/*
 * Step 3: same multi-tile fill, but targeting the real screen
 * framestore directly - no PMP allocation for the render target at
 * all (that memory already exists; only the CL/tile_alloc/tile_state
 * scratch buffers still come from `pmp`, starting at `base_page`).
 *
 * (x, y) is the rectangle's origin and `screen_phys_base`/
 * `screen_stride` describe the framestore, all in the caller's own
 * convention - THIS FUNCTION DOES NOT KNOW OR CARE about GraphicsV's
 * bottom-left-origin Cartesian coordinate convention (see GraphicsV 13
 * 2's parameter block doc). Reconciling row 0 in memory (whatever's at
 * `screen_phys_base`) with "top" or "bottom" of the visible display,
 * and with GraphicsV's own (x,y) convention, is deliberately deferred
 * to step 4 (claiming the vector) - this step only proves "can this
 * driver write a correctly-shaped, correctly-clipped rectangle
 * directly into real screen memory and have it appear on the real
 * display", which needs none of that yet.
 *
 * Callers MUST have already confirmed the framestore is physically
 * contiguous across the full rectangle (see screen_info.h's
 * v3d_get_framestore_phys) - this function does not check
 * that itself, since re-checking per call would be wasteful once the
 * whole screen has already been verified once.
 */
_kernel_oserror *v3d_fill_screen(pmp_area_t *pmp, unsigned int base_page,
                                   unsigned int x, unsigned int y,
                                   unsigned int width, unsigned int height,
                                   unsigned int clear_color_rgba,
                                   unsigned int screen_phys_base,
                                   unsigned int screen_stride,
                                   unsigned int *tiles_x_out,
                                   unsigned int *tiles_y_out);

#endif /* CL_FILL_H */
