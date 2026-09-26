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

/*
 * Translates a single logical address to its backing physical address,
 * via OS_Memory 0 (SWI &68 reason 0, "General page block operations")
 * with flags GIVEN_LOG_ADDR|RETURN_PHYS_ADDR (0x200|0x2000=0x2200).
 * Confirmed against the real PRM (riscosopen.org/wiki/.../OS_Memory 0
 * and .../OS_Memory 0 Flags, fetched 2026-09-26) - not the older, MEMC-
 * only OS_ReadMemMapEntries (SWI &52), which doesn't apply to this
 * hardware's MMU-based memory model. Also cross-referenced against
 * that PRM page's own "Linked from" list, which includes GraphicsV 6
 * (SetDAG) - i.e. this is the same mechanism real GraphicsV drivers use
 * to get a framestore's physical address, not something improvised for
 * this project. The PRM notes addresses need not be page-aligned, and
 * that a physical address >= 2^32 fails this call (OS_Memory 64 would
 * be needed instead) - not a concern on this 32-bit-addressed Pi 4/CM4
 * setup, per V3D_MemSplit's own confirmed ARM/VC split (960MB/3GB).
 */
_kernel_oserror *v3d_logical_to_physical(void *log_addr, unsigned int *phys_addr_out);

/*
 * Verifies the screen framestore described by `info` is physically
 * contiguous across its ENTIRE size (line_length * (ywind_limit+1)
 * bytes from screen_start), by translating every single page boundary
 * within that range and confirming each consecutive pair's physical
 * addresses differ by exactly PAGE_SIZE. This is NOT a formality: a
 * Dynamic Area's logical window has no general guarantee of physically
 * contiguous backing, and the planned GraphicsV Render accelerator's
 * whole approach (a single physical base address + stride, matching
 * how V3D's tile store addressing works - see cl_fill.c) silently
 * breaks if that assumption is wrong for even one page in the range,
 * writing GPU output to the wrong physical memory entirely. In
 * practice this is expected to hold (display hardware scanout
 * generally needs a contiguous framestore too), but "expected" is
 * exactly the kind of assumption this project has been burned by
 * before (see README's power-on/reset history) - so it's checked for
 * real, once, rather than trusted.
 *
 * On success, *phys_base_out is the confirmed physical address
 * corresponding to info->screen_start.
 */
_kernel_oserror *v3d_verify_framestore_contiguous(const v3d_screen_info_t *info,
                                                    unsigned int *phys_base_out);

#endif /* SCREEN_INFO_H */
