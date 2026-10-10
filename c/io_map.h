/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * io_map.h - temporary IO-memory mapping via OS_Memory 14/15.
 *
 * VERIFIED against ROOL's own PRM documentation (not guessed):
 *   https://www.riscosopen.org/wiki/documentation/show/OS_Memory%2014
 *   https://www.riscosopen.org/wiki/documentation/show/OS_Memory%2015
 *
 * This replaces the earlier HAL_IOAddress() guess in the M0/M1 sketch -
 * that was flagged unverified and turned out to be the wrong mechanism.
 * OS_Memory 14 is the correct, public, documented way for a module to
 * get at a device's physical registers.
 *
 * IMPORTANT (from the PRM text, not obvious from the call signature):
 * RISC OS keeps only ONE such mapping alive at a time, system-wide. It
 * is explicitly documented as unsafe to map a region in module_init and
 * hold onto the logical address for the module's lifetime - two modules
 * doing that would silently stomp on each other's mapping. Every access
 * must map, use, then release (io_unmap) before returning control.
 */
#ifndef IO_MAP_H
#define IO_MAP_H

#include <stddef.h>
#include "kernel.h"

#define SWI_OS_Memory 0x68
#define OS_Memory_MapIO   14
#define OS_Memory_UnmapIO 15
#define OS_Memory_MapIOPermanent 13

typedef struct io_mapping
{
    volatile unsigned int *base; /* logical address of the mapped megabyte,
                                     word-indexable */
    unsigned int key;            /* reference key, pass back to io_unmap */
} io_mapping_t;

/*
 * Maps in the megabyte containing `phys` (must already be megabyte-
 * aligned - callers use the *_MB_BASE constants from v3d_regs.h).
 * Returns NULL and leaves *err set on failure.
 */
static inline _kernel_oserror *io_map(unsigned int phys_mb_base,
                                       io_mapping_t *out)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    regs.r[0] = OS_Memory_MapIO;
    regs.r[1] = phys_mb_base;

    err = _kernel_swi(SWI_OS_Memory, &regs, &regs);
    if (err)
        return err;

    out->base = (volatile unsigned int *) regs.r[2];
    out->key  = (unsigned int) regs.r[3];
    return NULL;
}

static inline void io_unmap(const io_mapping_t *m)
{
    _kernel_swi_regs regs;
    regs.r[0] = OS_Memory_UnmapIO;
    regs.r[1] = m->key;
    /* best-effort: nothing sensible to do if release itself fails */
    (void) _kernel_swi(SWI_OS_Memory, &regs, &regs);
}

/*
 * Maps in `size` bytes at `phys` PERMANENTLY - unlike io_map/io_unmap
 * above, this mapping is never released and there is no corresponding
 * unmap call (OS_Memory 15 only releases *temporary* IO mappings -
 * see https://www.riscosopen.org/wiki/documentation/show/OS_Memory%2013,
 * fetched 2026-09-26). Deliberately not megabyte-aligned-only like
 * io_map: takes an arbitrary physical address and size. Only meant to
 * be called once, for state that must survive for the module's whole
 * lifetime (e.g. the V3D interrupt handler's own register access - it
 * cannot safely call OS_Memory 14 itself, both because that's a SWI
 * call from IRQ mode and because OS_Memory 14 is explicitly documented
 * as one single, shared, system-wide mapping that any other code could
 * steal from under it - see v3d_poll_counter's own comment on that
 * exact hazard). No cache-policy flags are set (R0's flag bits 8/9
 * both clear = "normal, non-bufferable"/"normal, non-cacheable" per
 * OS_Memory 13 Flags), matching the Strongly-Ordered mapping io_map
 * already relies on for every other register access in this driver.
 */
static inline _kernel_oserror *io_map_permanent(unsigned int phys,
                                                  unsigned int size,
                                                  volatile unsigned int **out_base)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    regs.r[0] = OS_Memory_MapIOPermanent;
    regs.r[1] = (int) phys;
    regs.r[2] = (int) size;

    err = _kernel_swi(SWI_OS_Memory, &regs, &regs);
    if (err)
        return err;

    *out_base = (volatile unsigned int *) regs.r[3];
    return NULL;
}

/* word offset within a mapped megabyte, from a full register address */
#define IO_WORD(mapping, reg_addr) \
    ((mapping).base[((reg_addr) & 0xFFFFFu) / 4])

#endif /* IO_MAP_H */
