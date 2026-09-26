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

/*
 * RAISED (2026-09-26, hardware-diagnosed): was 64 (256KB) - a real
 * live-desktop test of the overlapping-copy scratch-buffer path
 * (see cl_copy.c) showed 651/778 (84%) of real window-scroll copies
 * too large for that cap, falling back to normal CPU rendering
 * (safely - the fail-safe design worked exactly as intended - but
 * with far less acceleration coverage than hoped: only 16% of real
 * scrolls fit). A modest window scroll alone (800x600) needs ~470
 * pages; 64 was never going to cover realistic cases.
 *
 * Raised to 2048 (8MB) - comfortably covers even a full 1920x1080
 * screen-sized scroll (~2026 pages) - but NOT by just bumping this
 * constant with claim_list/map_list still on the stack: 2048*12 bytes
 * per array (see below) would be 24KB of SVC-mode kernel stack per
 * array, and RISC OS's SVC stack is a genuinely limited, easy-to-
 * overflow resource - a stack overflow in a kernel module crashes the
 * whole machine, a far worse failure mode than anything this project
 * has hit so far. So claim_list/map_list (in pmp_alloc) and list (in
 * pmp_release) were moved from stack-local arrays to function-static
 * ones instead - static/BSS workspace is not remotely constrained the
 * way SVC stack is, and these arrays are never touched from anywhere
 * but ordinary foreground SVC-mode SWI calls (pmp_alloc/pmp_release do
 * real SWI calls throughout, which are never safe from IRQ context
 * anyway - matching how every other call site in this codebase already
 * only ever calls these from foreground code), so there's no
 * reentrancy hazard from making them static.
 */
#define MAX_PAGES_PER_CALL PMP_MAX_PAGES_PER_CALL /* see phys_buffer.h -
                                                     * the one real value,
                                                     * not redefined here */

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
 *
 * NUM_PAGES IS NOT BOUNDED BY MAX_PAGES_PER_CALL, UNLIKE pmp_alloc -
 * found 2026-09-26, while raising MAX_PAGES_PER_CALL for the
 * overlapping-copy scratch buffer: module_final calls this with a
 * whole area's `max_pages` in one go (4096 for g_pmp_fill), which
 * already exceeded even the OLD 64-page cap. Without internal
 * chunking, that write loop would run 4096 iterations into an array
 * sized for far fewer entries - a real, latent buffer overflow (past
 * the end of a stack array in the original version, now past the end
 * of a static one), not merely a theoretical one. Fixed by chunking
 * internally in MAX_PAGES_PER_CALL-sized batches, so this function's
 * own contract ("works for any num_pages") is actually upheld
 * regardless of how big or small that cap is set to.
 */
_kernel_oserror *pmp_release(pmp_area_t *area, unsigned int page_index,
                              unsigned int num_pages)
{
    static unsigned int list[MAX_PAGES_PER_CALL][3]; /* static, not stack
                                                        * - see MAX_PAGES_
                                                        * PER_CALL's own
                                                        * comment */
    _kernel_swi_regs regs;
    _kernel_oserror *err;
    unsigned int done = 0;

    while (done < num_pages)
    {
        unsigned int chunk = num_pages - done;
        unsigned int i;

        if (chunk > MAX_PAGES_PER_CALL)
            chunk = MAX_PAGES_PER_CALL;

        /* unmap first - releasing a still-mapped page is refused */
        for (i = 0; i < chunk; i++)
        {
            list[i][0] = page_index + done + i; /* DA page number */
            list[i][1] = (unsigned int) -1; /* unmap */
            list[i][2] = 0;
        }
        regs.r[0] = DA_MapPages;
        regs.r[1] = (int) area->area_num;
        regs.r[2] = (int) list;
        regs.r[3] = (int) chunk;
        err = _kernel_swi(SWI_OS_DynamicArea, &regs, &regs);
        if (err)
            return err;

        for (i = 0; i < chunk; i++)
        {
            list[i][0] = page_index + done + i; /* PMP page index */
            list[i][1] = (unsigned int) -1; /* release */
            list[i][2] = 0;
        }
        regs.r[0] = DA_ClaimPages;
        regs.r[1] = (int) area->area_num;
        regs.r[2] = (int) list;
        regs.r[3] = (int) chunk;
        err = _kernel_swi(SWI_OS_DynamicArea, &regs, &regs);
        if (err)
            return err;

        done += chunk;
    }

    return NULL;
}

_kernel_oserror *pmp_alloc(pmp_area_t *area, unsigned int page_index,
                            unsigned int num_pages,
                            unsigned int *phys_addr_out,
                            void **logical_ptr_out)
{
    /* Static, not stack - see MAX_PAGES_PER_CALL's own comment. Still
     * a fixed cap (a single call can't exceed MAX_PAGES_PER_CALL - see
     * the check below), but a generous one now, with no SVC-stack risk. */
    static unsigned int claim_list[MAX_PAGES_PER_CALL][3];
    static unsigned int map_list[MAX_PAGES_PER_CALL][3];
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
