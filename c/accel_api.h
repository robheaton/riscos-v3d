/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * accel_api.h - the functions module.c exports to gfxv_accel.c and the
 * job code, in ONE place.
 *
 * Until v0.07 each user declared its own private `extern` for these, so
 * nothing compared the declaration with the definition: a signature change
 * on one side compiled cleanly and broke at run time (the same class of
 * bug as the old module_final mismatch). module.c now includes this, so a
 * disagreement is a compile error.
 */
#ifndef ACCEL_API_H
#define ACCEL_API_H

#include "kernel.h"

/* GraphicsV 13 operations, in GraphicsV's own inclusive bottom-up
 * coordinates (0,0 = bottom-left). See module.c for each one's contract. */
_kernel_oserror *v3d_accel_render_fill(unsigned int left, unsigned int bottom,
                                         unsigned int right, unsigned int top,
                                         unsigned int colour_rgba);
_kernel_oserror *v3d_accel_render_striped_fill(unsigned int left, unsigned int bottom,
                                                 unsigned int right, unsigned int top,
                                                 const unsigned int colours[8]);
_kernel_oserror *v3d_accel_render_copy(unsigned int src_left, unsigned int src_bottom,
                                         unsigned int dest_left, unsigned int dest_bottom,
                                         unsigned int width, unsigned int height);
_kernel_oserror *v3d_accel_render_copy_overlapping(unsigned int src_left, unsigned int src_bottom,
                                                     unsigned int dest_left, unsigned int dest_bottom,
                                                     unsigned int width, unsigned int height);

/* Screen depth (log2 bits per pixel), self-initialising. */
_kernel_oserror *v3d_accel_get_screen_log2_bpp(unsigned int *log2_bpp_out);

/* Screen-cache maintenance, called from the GraphicsV vector (see the
 * screen cache comment in module.c): forget the cached geometry, and note
 * the physical address the VDU now writes to (GraphicsV 6, R0=3). */
void v3d_accel_screen_invalidate(void);
void v3d_accel_note_vrender(unsigned int phys);
void v3d_accel_mode_changing(void);

/* OS_ReadMonotonicTime wrapper (centiseconds). Only for the slow paths
 * that have no timer mapped - the hot path uses v3d_time_us(). */
unsigned int v3d_read_centiseconds(void);

#endif /* ACCEL_API_H */
