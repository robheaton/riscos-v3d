/*
 * v3d_irq.c - see v3d_irq.h.
 */

#include "v3d_irq.h"
#include "V3DDriver.h" /* for v3d_irq_entry - the cmunge-generated
                         * veneer symbol from cmhg's vector-handlers
                         * directive; only its address is ever used,
                         * it must never be called directly (per
                         * cmunge's own vector-handlers documentation). */
#include "io_map.h"
#include "v3d_regs.h"
#include <stddef.h>

#define SWI_OS_ClaimDeviceVector   0x4Bu
#define SWI_OS_ReleaseDeviceVector 0x4Cu
#define SWI_OS_Hardware            0x7Au

#define OSHW_CallRoutine    0u /* OS_Hardware 0: R8=0, R9=entry number */
#define OSHW_LookupRoutine  1u /* OS_Hardware 1: R8=1, R9=entry number,
                                 * exit R0=address R1=static base */
#define HALEntry_IRQEnable  1u
#define HALEntry_IRQDisable 2u
#define HALEntry_IRQClear   3u

volatile unsigned int g_v3d_irq_claimed_count = 0;
volatile unsigned int g_v3d_irq_spurious_count = 0;

static void *g_hal_irqclear_addr = NULL;
static unsigned int g_hal_irqclear_sb = 0;
static io_mapping_t g_v3d_permanent_mapping;
static int g_v3d_permanent_mapped = 0;

/*
 * Raw call to a resolved HAL routine: sets R9 (sb) to the static-base
 * value OS_Hardware 1 returned for this routine, branches with link
 * to its address, device number in R0 - exactly the convention
 * riscosopen.org/wiki/documentation/show/OS_Hardware%201 documents:
 * "Calls made to the routine should be in a privileged mode, with R9
 * (sb) set to the static base value returned by this SWI." A plain
 * branch instruction, no SWI trap - safe to use from inside an IRQ-
 * mode interrupt handler, unlike _kernel_swi() (see the PRM's
 * OS_ClaimDeviceVector page: a handler must "strictly avoid the use
 * of non re-entrant SWIs", since a normal SWI call from IRQ mode risks
 * corrupting R14_svc if it interrupted other SVC-mode code mid-SWI).
 *
 * r1/r2/r3/r12/lr are marked clobbered because ATPCS/AAPCS treat them
 * as caller-saved scratch registers the callee is free to modify - the
 * HAL routine we're branching to is not our own code, we have no
 * guarantee it preserves anything beyond what the calling convention
 * requires. r0 and r9 are declared as fixed-register operands (their
 * values are supplied going in; what happens to them afterward is for
 * GCC to treat as unspecified, same reasoning, without needing an
 * explicit clobber - they're not also listed as outputs).
 */
static void call_hal_routine(void *addr, unsigned int sb, unsigned int arg0)
{
    register unsigned int r0 __asm__("r0") = arg0;
    register unsigned int r9 __asm__("r9") = sb;
    register void *r4 __asm__("r4") = addr;

    __asm__ __volatile__(
        "blx %[addr]\n\t"
        : "+r" (r0)
        : [addr] "r" (r4), "r" (r9)
        : "r1", "r2", "r3", "r12", "lr", "cc", "memory"
    );
}

static void v3d_irq_call_clear(void)
{
    if (g_hal_irqclear_addr)
        call_hal_routine(g_hal_irqclear_addr, g_hal_irqclear_sb,
                          V3D_IRQ_DEVICE_NUMBER);
}

int v3d_irq_handler(_kernel_swi_regs *r, void *pw)
{
    unsigned int sts;

    (void) r;
    (void) pw;

    if (!g_v3d_permanent_mapped)
    {
        /* Shouldn't happen - v3d_irq_resolve_clear() always runs
         * before the vector is claimed - but never touch V3D
         * registers through a pointer we never actually mapped. */
        g_v3d_irq_spurious_count++;
        return 1; /* VECTOR_PASSON */
    }

    sts = IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_STS);

    if (sts & (V3D_INT_FLDONE | V3D_INT_FRDONE))
    {
        /* Acknowledge exactly the bits found - standard write-1-to-
         * clear, matches the real Linux v3d driver's own IRQ handler. */
        IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_CLR) = sts;
        v3d_irq_call_clear();
        g_v3d_irq_claimed_count++;
        return 0; /* VECTOR_CLAIM */
    }

    g_v3d_irq_spurious_count++;
    return 1; /* VECTOR_PASSON - not ours, or nothing to service */
}

_kernel_oserror *v3d_irq_claim(void)
{
    _kernel_swi_regs regs;

    regs.r[0] = (int) V3D_IRQ_DEVICE_NUMBER;
    regs.r[1] = (int) v3d_irq_entry;
    regs.r[2] = 0; /* R12 value passed to the handler - private word,
                     * unused by v3d_irq_handler for now */
    regs.r[3] = 0; /* only meaningful for podule device numbers 8/13 */
    regs.r[4] = 0;
    return _kernel_swi((int) SWI_OS_ClaimDeviceVector, &regs, &regs);
}

_kernel_oserror *v3d_irq_release(void)
{
    _kernel_swi_regs regs;

    /* R0-R2 must exactly match the claim call, per OS_ReleaseDeviceVector's
     * own documentation. */
    regs.r[0] = (int) V3D_IRQ_DEVICE_NUMBER;
    regs.r[1] = (int) v3d_irq_entry;
    regs.r[2] = 0;
    return _kernel_swi((int) SWI_OS_ReleaseDeviceVector, &regs, &regs);
}

static _kernel_oserror *call_hal_entry(unsigned int entry_no)
{
    _kernel_swi_regs regs;

    regs.r[0] = (int) V3D_IRQ_DEVICE_NUMBER; /* the HAL routine's own arg */
    regs.r[8] = (int) OSHW_CallRoutine;
    regs.r[9] = (int) entry_no;
    return _kernel_swi((int) SWI_OS_Hardware, &regs, &regs);
}

_kernel_oserror *v3d_irq_hal_enable(void)
{
    return call_hal_entry(HALEntry_IRQEnable);
}

_kernel_oserror *v3d_irq_hal_disable(void)
{
    return call_hal_entry(HALEntry_IRQDisable);
}

_kernel_oserror *v3d_irq_resolve_clear(void)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;
    volatile unsigned int *base;

    regs.r[8] = (int) OSHW_LookupRoutine;
    regs.r[9] = (int) HALEntry_IRQClear;
    err = _kernel_swi((int) SWI_OS_Hardware, &regs, &regs);
    if (err)
        return err;

    g_hal_irqclear_addr = (void *) regs.r[0];
    g_hal_irqclear_sb = (unsigned int) regs.r[1];

    if (!g_v3d_permanent_mapped)
    {
        err = io_map_permanent(V3D_MB_BASE, 0x100000u, &base);
        if (err)
            return err;

        g_v3d_permanent_mapping.base = base;
        g_v3d_permanent_mapping.key = 0; /* unused - permanent, never unmapped */
        g_v3d_permanent_mapped = 1;
    }

    return NULL;
}

_kernel_oserror *v3d_irq_test_call_clear(void)
{
    if (!g_hal_irqclear_addr)
    {
        static _kernel_oserror err_not_resolved = {
            1, "V3DDriver: HAL_IRQClear not resolved yet - "
               "call v3d_irq_resolve_clear first"
        };
        return &err_not_resolved;
    }

    v3d_irq_call_clear();
    return NULL;
}
