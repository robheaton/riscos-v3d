/*
 * screen_info.h - read-only groundwork for GraphicsV Render acceleration
 * (see README.md's "GraphicsV Render accelerator" section): fetch the
 * current screen mode's pixel format, stride, and framestore address,
 * with nothing V3D-specific in here at all - this only reads OS state,
 * touches no hardware, and needs no PMP/clock/IRQ machinery.
 *
 * VDU variable numbers below are transcribed from a real, shipped
 * GCCSDK header (env/include/oslib/os.h's os_VDUVAR_* enum), not
 * guessed or derived from memory of the PRM text - see v3d_read_screen_info's
 * own comment for exactly which ones and why.
 */
#ifndef SCREEN_INFO_H
#define SCREEN_INFO_H

#include "kernel.h"

typedef struct v3d_screen_info
{
    unsigned int mode_flags;     /* os_VDUVAR_MODE_FLAGS */
    unsigned int log2_bpp;       /* os_VDUVAR_LOG2_BPP - 0=1bpp .. 5=32bpp */
    unsigned int line_length;    /* os_VDUVAR_LINE_LENGTH - bytes per scan line */
    unsigned int xwind_limit;    /* os_VDUVAR_XWIND_LIMIT - rightmost pixel index */
    unsigned int ywind_limit;    /* os_VDUVAR_YWIND_LIMIT - topmost pixel index */
    unsigned int screen_start;   /* os_VDUVAR_SCREEN_START - logical address
                                   * of the screen bank currently displayed */
    unsigned int display_start;  /* os_VDUVAR_DISPLAY_START - logical address
                                   * of the screen bank currently being
                                   * written to (may differ from screen_start
                                   * under hardware scrolling/double-buffering) */
} v3d_screen_info_t;

/*
 * Fills `out` from the OS's own live VDU variables via OS_ReadVduVariables.
 * Read-only, no side effects, safe to call at any time (including before
 * V3D_PowerOn) - this is the OS's own screen-mode bookkeeping, nothing to
 * do with V3D itself.
 */
_kernel_oserror *v3d_read_screen_info(v3d_screen_info_t *out);

#endif /* SCREEN_INFO_H */
