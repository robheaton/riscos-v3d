/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * v3d_errors.h - the module's error numbers.
 *
 * ROOL allocated the V3DDriver error block &823100 (256 errors) on 2026-10-20. Every error this module returns is
 * V3D_ERROR_BASE + n, with n the number it had before the allocation (1 to 52), so n is still what the notes in the
 * source and the README call "error n". Errors that come from the OS, not from this module, are passed on as they are.
 *
 * What the driver REPORTS to a program for diagnosis (V3D_AccelProfile's "last error number", the event trace, the
 * statistics SWIs) is the plain number n, not the full number: V3D_ERR_INDEX turns one into the other, and leaves
 * any number outside the block unchanged. The test scripts print those plain numbers.
 */
#ifndef V3D_ERRORS_H
#define V3D_ERRORS_H

#define V3D_ERROR_BASE 0x00823100u

#define V3D_ERR(n) (V3D_ERROR_BASE + (n))

#define V3D_ERR_INDEX(e) \
    (((unsigned int) (e) >= V3D_ERROR_BASE && (unsigned int) (e) < V3D_ERROR_BASE + 256u) \
         ? (unsigned int) (e) - V3D_ERROR_BASE : (unsigned int) (e))

#endif /* V3D_ERRORS_H */
