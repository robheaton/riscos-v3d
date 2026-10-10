/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * phys_buffer.h - physically-contiguous buffer allocation for the V3D
 * control lists / shader / tile-alloc memory that M2 onward will need.
 *
 * This is new, more involved territory than M0/M1's IO register mapping.
 * The procedure (create a Physical Memory Pool dynamic area, then find +
 * claim + map physical pages into it) is transcribed from ROOL developer
 * Jeffrey Lee's own explanation on the forum, not guessed:
 *   https://www.riscosopen.org/forum/forums/11/topics/9650
 * and cross-checked against the PRM pages for each SWI involved:
 *   OS_DynamicArea 0/21/22   - https://www.riscosopen.org/wiki/documentation/show/OS_DynamicArea%20{0,21,22}
 *   OS_Memory 12             - https://www.riscosopen.org/wiki/documentation/show/OS_Memory%2012
 *   Dynamic Area ResizePMP   - https://www.riscosopen.org/wiki/documentation/show/Dynamic%20Area%20ResizePMP
 *
 * PAGE_SIZE=4096 is not derived from a queried system value - it is the
 * standard, universally-assumed RISC OS page size on modern (ARM, 4K
 * page) systems; ROOL's own "Kernel notes" page states plainly that
 * "lots of code assumes 4K page size anyway". Flagging this rather than
 * treating it as bulletproof: a future revision could query it properly
 * if this ever needs to run somewhere that isn't 4K.
 */
#ifndef PHYS_BUFFER_H
#define PHYS_BUFFER_H

#include "kernel.h"

#define PAGE_SIZE 4096u

/*
 * The largest number of pages a single pmp_alloc() call can claim in
 * one go (pmp_release() has no such limit - see phys_buffer.c's own
 * comment, it chunks internally). Exposed here (not just as a private
 * enum in phys_buffer.c) so every caller sizing a request against this
 * cap - cl_fill.c, cl_copy.c - references the one real value instead
 * of a hardcoded number that can silently drift out of sync with it,
 * which is exactly what happened when this was raised from 64 to 2048
 * (2026-09-26, see phys_buffer.c's own comment for why) and several
 * `> 64u` checks elsewhere had to be found and updated by hand.
 */
#define PMP_MAX_PAGES_PER_CALL 2048u

typedef struct pmp_area
{
    unsigned int area_num;
    unsigned char *logical_base; /* base of the PMP's logical window */
    unsigned int max_pages;      /* physical size limit, in pages */
} pmp_area_t;

/*
 * Creates a Physical Memory Pool dynamic area named `name`, reserving
 * `max_logical_bytes` of logical address space (rounded up to a page)
 * and allowing up to `max_pages` physical pages to be claimed into it
 * later via pmp_alloc(). Does not allocate any physical RAM itself.
 */
_kernel_oserror *pmp_create(const char *name, unsigned int max_logical_bytes,
                             unsigned int max_pages, pmp_area_t *out);

/*
 * Finds `num_pages` physically-contiguous pages, claims them into
 * `area` starting at PMP page index `page_index`, and maps them into
 * the matching logical page range. On success, *phys_addr_out is the
 * physical base address of the region and *logical_ptr_out is a CPU
 * pointer the module can read/write directly (until/unless unmapped).
 */
_kernel_oserror *pmp_alloc(pmp_area_t *area, unsigned int page_index,
                            unsigned int num_pages,
                            unsigned int *phys_addr_out,
                            void **logical_ptr_out);

/* Memory type for the logical mapping of claimed pages. */
#define PMP_ATTR_STRONGLY_ORDERED 0u /* non-cacheable, non-bufferable */
#define PMP_ATTR_NC_BUFFERABLE    1u /* non-cacheable, bufferable (Normal) */

/* pmp_alloc with an explicit memory type (pmp_alloc itself uses
 * PMP_ATTR_STRONGLY_ORDERED, as it always has). */
_kernel_oserror *pmp_alloc_policy(pmp_area_t *area, unsigned int page_index,
                                   unsigned int num_pages, unsigned int attr,
                                   unsigned int *phys_addr_out,
                                   void **logical_ptr_out);

/* Removes the whole area and every page in it (OS_DynamicArea 1). */
_kernel_oserror *pmp_destroy(pmp_area_t *area);

/*
 * Releases (unmaps, then unclaims) any pages currently held at
 * [page_index, page_index+num_pages) in `area`. Safe to call on a
 * range that was never allocated (see phys_buffer.c's own comment) -
 * module_final calls this for the whole area's page range unconditionally,
 * regardless of which specific sub-ranges any particular session
 * actually used, so a module reload never leaves physical pages
 * locked into a now-orphaned zombie Dynamic Area.
 */
_kernel_oserror *pmp_release(pmp_area_t *area, unsigned int page_index,
                              unsigned int num_pages);

/*
 * Defensive crash-recovery cleanup (see phys_buffer.c for the full
 * reasoning): finds any existing Dynamic Area already named `name`
 * (left behind by a previous instance that never got to run
 * module_final, or by v0.06 and earlier which never removed theirs) and
 * removes it, pages and all (pmp_destroy). Call once per name at module_init, before
 * this instance creates its own PMP area of that name. Never fails
 * module_init - always returns NULL.
 */
_kernel_oserror *pmp_cleanup_stale(const char *name);

/*
 * Dynamic Area ResizePMP handler (reason 6), wired up via the .cmhg
 * generic-veneers mechanism. Unconditionally refuses any resize
 * requested via OS_ChangeDynamicArea - this PMP's size is only ever
 * changed explicitly by our own code, never by the user dragging a
 * Task Manager bar (which the DA flags also disable, belt and braces).
 */
_kernel_oserror *v3d_da_resize_handler(_kernel_swi_regs *r, void *pw);

#endif /* PHYS_BUFFER_H */
