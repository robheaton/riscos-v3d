/* SPDX-License-Identifier: GPL-2.0-or-later */
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
    unsigned int screen_start;   /* os_VDUVAR_SCREEN_START (&94) - logical
                                   * address of the screen bank the VDU is
                                   * currently WRITING to (kernel: copied
                                   * from DriverBankAddr by the CLS, and
                                   * changed by OS_Byte 112) */
    unsigned int display_start;  /* os_VDUVAR_DISPLAY_START (&95) - logical
                                   * address of the bank currently being
                                   * DISPLAYED (OS_Byte 113). Differs from
                                   * screen_start while a program is
                                   * double-buffering. (These two comments
                                   * were the wrong way round until v0.07.) */
    unsigned int total_screen_size; /* os_VDUVAR_TOTAL_SCREEN_SIZE - the
                                   * OS's own authoritative size of the
                                   * genuinely allocated screen memory,
                                   * in bytes. Deliberately NOT the same
                                   * thing as line_length*(ywind_limit+1)
                                   * - that's this driver's own derived
                                   * estimate of the VISIBLE area, which
                                   * turned out to be a real, hardware-
                                   * confirmed wrong assumption to check
                                   * memory validity against (see
                                   * v3d_get_framestore_phys). */
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
 * Gets the screen framestore's physical base address, needed for the
 * same reason as always: V3D's tile store addressing needs one plain
 * physical base + stride (see cl_fill.c), and screen memory is
 * described to the rest of this driver only by its logical address
 * (ScreenStart) - something has to bridge that gap.
 *
 * ORIGINAL APPROACH (replaced 2026-09-26, hardware-diagnosed): walked
 * every page of the screen via v3d_logical_to_physical (OS_Memory 0),
 * starting from ScreenStart, checking each consecutive pair's physical
 * addresses differed by exactly one page - genuine contiguity
 * verification, not an assumption. Failed immediately on real
 * hardware ("Address not recognised") on the very FIRST call, meaning
 * the Pi's screen framestore isn't presented as ordinary
 * OS_Memory-0-trackable RAM at all (plausibly a HAL-managed region
 * locked down outside normal page-allocation bookkeeping).
 *
 * CURRENT APPROACH: `GraphicsV 9` ("Framestore information") is the
 * actual, direct, PRM-documented mechanism for this exact question -
 * "R0 = Framestore physical base address, R1 = Framestore size".
 * Called via OS_CallAVector (SWI &34) as a plain vector call, not a
 * claim - asking the currently active display driver directly, the
 * same way the kernel itself would. See v3d_get_framestore_phys's own
 * comment in screen_info.c for the full detail, including what happens
 * if the active driver doesn't answer this call at all (some drivers
 * legitimately don't - see the PRM's own note on GraphicsV 8/9).
 *
 * This also settles the "is it contiguous" question differently than
 * the original approach: a driver reporting a single (base, size) pair
 * is implicitly asserting that's one contiguous region - there's no
 * separate contiguity check to make once GraphicsV 9 has answered.
 *
 * On success, *phys_base_out is the framestore's physical base
 * address and *size_out its size in bytes, sanity-checked against
 * info->total_screen_size (fails if the reported size is smaller than
 * expected).
 */
_kernel_oserror *v3d_get_framestore_phys(const v3d_screen_info_t *info,
                                          unsigned int *phys_base_out,
                                          unsigned int *size_out);

#endif /* SCREEN_INFO_H */
