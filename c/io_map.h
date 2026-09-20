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

#include "kernel.h"

#define SWI_OS_Memory 0x68
#define OS_Memory_MapIO   14
#define OS_Memory_UnmapIO 15

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

/* word offset within a mapped megabyte, from a full register address */
#define IO_WORD(mapping, reg_addr) \
    ((mapping).base[((reg_addr) & 0xFFFFFu) / 4])

#endif /* IO_MAP_H */
