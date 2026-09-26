/*
 * phys_buffer.c - see phys_buffer.h for the sourcing/verification notes.
 */

#include <stddef.h>
#include "kernel.h"
#include "phys_buffer.h"
#include "V3DDriver.h" /* for v3d_da_resize_veneer's address */

#define SWI_OS_DynamicArea 0x66
#define SWI_OS_Memory       0x68

enum
{
    DA_Create      = 0,
    DA_ClaimPages  = 21,
    DA_MapPages    = 22,
};

enum
{
    Mem_RecommendPage = 12,
};

/* Dynamic Area flags (see riscosopen.org/wiki/.../Dynamic Area Flags) */
#define DA_FLAG_NOT_DRAGGABLE     (1u << 7)  /* no Task Manager resize bar */
#define DA_FLAG_REQUIRES_PAGES    (1u << 8)  /* PMP requirement */
#define DA_FLAG_IS_PMP            (1u << 20)

/* Page flags used by OS_DynamicArea 21/22 (bit 15 = lock to this area) */
#define PAGE_FLAG_LOCK            (1u << 15)

/*
 * Bit 4 (nonbufferable) + bit 5 (noncacheable), cache-policy sub-field
 * (bits 12-14) left at 0 = NCNB = VMSA "Strongly-Ordered" memory (see
 * riscosopen.org/wiki/.../Memory Cache Policies). Every buffer this
 * module allocates is either read or written by the GPU directly via
 * the bus (a non-cache-coherent DMA master) - without this, CPU writes
 * to the BCL/RCL could sit in the ARM data cache indefinitely and the
 * GPU would see stale/garbage physical memory instead of the control
 * list actually built for it. Added after binning still timed out with
 * a correct-looking control list and a demonstrably-not-holding-the-
 * shared-IO-window poll - cache coherency is the next most likely
 * explanation and RISC OS's own docs treat DMA-vs-cache as a real,
 * commonly-hit problem (see OS_Memory 19, "Prepare for DMA" - not used
 * here because its callback-based interface is substantial machinery
 * for what a fixed non-cacheable mapping solves more simply, at the
 * cost of some performance we don't need yet for a handful of tiny
 * buffers).
 */
#define PAGE_FLAG_NONCACHEABLE    ((1u << 4) | (1u << 5))

static inline unsigned int round_up_pages(unsigned int bytes)
{
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

_kernel_oserror *pmp_create(const char *name, unsigned int max_logical_bytes,
                             unsigned int max_pages, pmp_area_t *out)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    regs.r[0] = DA_Create;
    regs.r[1] = (int) -1;                 /* area number: allocate new */
    regs.r[2] = 0;                        /* initial logical size: must be 0 for PMP */
    regs.r[3] = (int) -1;                 /* base: allocate new */
    regs.r[4] = DA_FLAG_NOT_DRAGGABLE | DA_FLAG_REQUIRES_PAGES | DA_FLAG_IS_PMP;
    regs.r[5] = (int) round_up_pages(max_logical_bytes) * (int) PAGE_SIZE;
    regs.r[6] = (int) v3d_da_resize_veneer;
    regs.r[7] = (int) -1;                 /* workspace: use area base */
    regs.r[8] = (int) name;
    regs.r[9] = (int) max_pages;

    err = _kernel_swi(SWI_OS_DynamicArea, &regs, &regs);
    if (err)
        return err;

    out->area_num     = (unsigned int) regs.r[1];
    out->logical_base = (unsigned char *) regs.r[3];
    out->max_pages     = (unsigned int) regs.r[9];
    return NULL;
}

enum { MAX_PAGES_PER_CALL = 64 };

/*
 * Releases (unmaps, then unclaims) any pages currently held at
 * [page_index, page_index+num_pages) in `area`. Safe to call on a
 * range that was never allocated: per the PRM, OS_DynamicArea 21/22
 * treat a page already in the requested state ("unmapped"/"unclaimed",
 * which is the state every PMP page starts in) as a no-op, not an
 * error. Called at the start of pmp_alloc so repeat calls for the same
 * range - e.g. re-running a test SWI without reloading the module -
 * don't try to reclaim pages that are still locked+mapped from the
 * previous call. (Found the hard way: a second V3D_ClearTest without
 * an intervening *RMKill failed with "memory cannot be moved" - the
 * claim step refusing to swap a still-mapped page's physical backing,
 * exactly as the PRM says it will.)
 *
 * Also called from module_final (see phys_buffer.h) - a Dynamic Area
 * is a system-wide OS resource that outlives the module that created
 * it, unless the physical pages locked into it are explicitly
 * released first. Without this, a reloaded module's OS_Memory 12
 * calls could be recommended pages a still-existing, still-locked
 * zombie area from the previous instance already owns.
 */
_kernel_oserror *pmp_release(pmp_area_t *area, unsigned int page_index,
                              unsigned int num_pages)
{
    unsigned int list[MAX_PAGES_PER_CALL][3];
    _kernel_swi_regs regs;
    _kernel_oserror *err;
    unsigned int i;

    /* unmap first - releasing a still-mapped page is refused */
    for (i = 0; i < num_pages; i++)
    {
        list[i][0] = page_index + i; /* DA page number */
        list[i][1] = (unsigned int) -1; /* unmap */
        list[i][2] = 0;
    }
    regs.r[0] = DA_MapPages;
    regs.r[1] = (int) area->area_num;
    regs.r[2] = (int) list;
    regs.r[3] = (int) num_pages;
    err = _kernel_swi(SWI_OS_DynamicArea, &regs, &regs);
    if (err)
        return err;

    for (i = 0; i < num_pages; i++)
    {
        list[i][0] = page_index + i; /* PMP page index */
        list[i][1] = (unsigned int) -1; /* release */
        list[i][2] = 0;
    }
    regs.r[0] = DA_ClaimPages;
    regs.r[1] = (int) area->area_num;
    regs.r[2] = (int) list;
    regs.r[3] = (int) num_pages;
    return _kernel_swi(SWI_OS_DynamicArea, &regs, &regs);
}

_kernel_oserror *pmp_alloc(pmp_area_t *area, unsigned int page_index,
                            unsigned int num_pages,
                            unsigned int *phys_addr_out,
                            void **logical_ptr_out)
{
    /* Small, fixed cap: this milestone only needs a handful of pages
     * (control lists, tile state, small buffers). A bigger cap would
     * need heap allocation instead of a stack array. */
    unsigned int claim_list[MAX_PAGES_PER_CALL][3];
    unsigned int map_list[MAX_PAGES_PER_CALL][3];
    _kernel_swi_regs regs;
    _kernel_oserror *err;
    unsigned int phys_page;
    unsigned int i;

    if (num_pages == 0 || num_pages > MAX_PAGES_PER_CALL)
    {
        static _kernel_oserror err_bad_count = {
            6, "V3DDriver: pmp_alloc requested with an invalid page count"
        };
        return &err_bad_count;
    }

    err = pmp_release(area, page_index, num_pages);
    if (err)
        return err;

    /* --- find physically-contiguous pages (OS_Memory 12) --- */
    regs.r[0] = Mem_RecommendPage;
    regs.r[1] = (int) (num_pages * PAGE_SIZE);
    regs.r[2] = 12; /* log2(PAGE_SIZE) = log2(4096) */
    err = _kernel_swi(SWI_OS_Memory, &regs, &regs);
    if (err)
        return err;
    phys_page = (unsigned int) regs.r[3];

    /* --- claim them into the PMP (OS_DynamicArea 21) --- */
    for (i = 0; i < num_pages; i++)
    {
        claim_list[i][0] = page_index + i;   /* PMP page index */
        claim_list[i][1] = phys_page + i;    /* physical page number */
        claim_list[i][2] = PAGE_FLAG_LOCK;
    }
    regs.r[0] = DA_ClaimPages;
    regs.r[1] = (int) area->area_num;
    regs.r[2] = (int) claim_list;
    regs.r[3] = (int) num_pages;
    err = _kernel_swi(SWI_OS_DynamicArea, &regs, &regs);
    if (err)
        return err;

    /* --- map them into the logical window (OS_DynamicArea 22) --- */
    for (i = 0; i < num_pages; i++)
    {
        map_list[i][0] = page_index + i;     /* DA page number (logical) */
        map_list[i][1] = page_index + i;     /* PMP page index to map in */
        map_list[i][2] = PAGE_FLAG_LOCK | PAGE_FLAG_NONCACHEABLE;
        /* access=0 (full RW), locked, Strongly-Ordered (see above) */
    }
    regs.r[0] = DA_MapPages;
    regs.r[1] = (int) area->area_num;
    regs.r[2] = (int) map_list;
    regs.r[3] = (int) num_pages;
    err = _kernel_swi(SWI_OS_DynamicArea, &regs, &regs);
    if (err)
        return err;

    *phys_addr_out = phys_page * PAGE_SIZE;
    *logical_ptr_out = area->logical_base + (size_t) page_index * PAGE_SIZE;
    return NULL;
}

_kernel_oserror *v3d_da_resize_handler(_kernel_swi_regs *r, void *pw)
{
    static _kernel_oserror err = {
        1, /* not an officially allocated error number - see cmhg notes */
        "V3DDriver: this Physical Memory Pool does not support "
        "OS_ChangeDynamicArea resizing"
    };
    (void) r;
    (void) pw;
    return &err;
}
