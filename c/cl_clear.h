/*
 * cl_clear.h - M2: the smallest possible real V3D job - bin an empty
 * primitive list, render a single 64x64 tile by clearing it to a solid
 * colour, and store the result to a buffer the CPU can read back.
 *
 * See cl_clear.c for the packet-by-packet derivation of every control
 * list command used here (transcribed from the MIT-licensed bare-metal
 * reference, not guessed), and phys_buffer.h for the physical buffer
 * allocation this depends on.
 */
#ifndef CL_CLEAR_H
#define CL_CLEAR_H

#include "kernel.h"
#include "phys_buffer.h"

#define CL_CLEAR_TEST_WIDTH  64u
#define CL_CLEAR_TEST_HEIGHT 64u

/*
 * Runs the clear job using `pages` consecutive PMP pages starting at
 * `base_page` (9 pages needed - see cl_clear.c). Blocks (with a bounded
 * timeout, not forever) until the GPU reports both binning and
 * rendering complete, then reads back the first pixel of the stored
 * render target directly through its logical pointer.
 *
 * On success, *render_target_phys_out is the render target's physical
 * address and *readback_pixel_out is the first pixel's raw 32 bits -
 * this should equal `clear_color_rgba` if the whole pipeline (control
 * list encoding, submission, completion, and the clear+store itself)
 * worked correctly.
 */
_kernel_oserror *v3d_clear_test(pmp_area_t *pmp, unsigned int base_page,
                                 unsigned int clear_color_rgba,
                                 unsigned int *render_target_phys_out,
                                 unsigned int *readback_pixel_out);

/*
 * M3: same clear-to-colour job as v3d_clear_test, but the render
 * target buffer is prefixed with a valid RISC OS sprite area+header
 * (see sprite.h) so the returned buffer is directly usable with
 * OS_SpriteOp - no copy needed, the GPU writes straight into the
 * sprite's pixel data.
 *
 * *sprite_area_log_out is a *logical* address. That's usable directly
 * from any other task's context without copying: PMP dynamic areas
 * (see phys_buffer.h) live in RISC OS's shared logical address space,
 * not a per-task Application Space, so this pointer means the same
 * thing to a separate BASIC/Wimp program as it does to this module.
 */
_kernel_oserror *v3d_clear_to_sprite(pmp_area_t *pmp, unsigned int base_page,
                                      unsigned int clear_color_rgba,
                                      unsigned int *sprite_area_phys_out,
                                      void **sprite_area_log_out,
                                      unsigned int *readback_pixel_out);

#endif /* CL_CLEAR_H */
