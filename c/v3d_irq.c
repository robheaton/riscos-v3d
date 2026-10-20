/* SPDX-License-Identifier: GPL-2.0-or-later */
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
#include "cl_common.h" /* g_v3d_wedged - the fail-stop latch */
#include <stddef.h>
#include "v3d_errors.h"

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
volatile unsigned int g_v3d_irq_bin_done_count = 0;
volatile unsigned int g_v3d_irq_render_done_count = 0;

static void *g_hal_irqclear_addr = NULL;
static unsigned int g_hal_irqclear_sb = 0;
static io_mapping_t g_v3d_permanent_mapping;
static int g_v3d_permanent_mapped = 0;
static int g_v3d_irq_ready = 0;
static int g_v3d_irq_vec_claimed = 0; /* device vector claimed, ready or not */
static unsigned int g_v3d_irq_empty_run = 0; /* consecutive empty interrupts */
static void *g_module_private_word = NULL;

int v3d_irq_is_ready(void)
{
    return g_v3d_irq_ready;
}

void v3d_irq_set_private_word(void *pw)
{
    g_module_private_word = pw;
}

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

/*
 * This handler holds the device vector UNSHARED (OS_ClaimDeviceVector with
 * R0 bit 31 clear), so it is the only thing that will ever run for this
 * interrupt. That changes what "pass on" means: with no later handler the
 * kernel falls to its default, NOIRQ, which calls HAL_IRQDisable and
 * switches device 10 off for good - after which every accelerated job
 * waits for an interrupt that cannot come. v0.06 passed on whenever it
 * found no FLDONE/FRDONE/hub bit set, so one empty interrupt (a spurious
 * assertion, a late one, one that raced the acknowledgement) was enough to
 * cripple the accelerator until reboot.
 *
 * So: always claim, always acknowledge at the interrupt controller. The
 * level-triggered line is only dropped by clearing the SOURCE, so every
 * status bit seen is cleared. A genuinely stuck source would now retrigger
 * forever instead of being disabled, so a run of empty interrupts trips a
 * breaker that masks everything and switches acceleration off.
 */
#define IRQ_EMPTY_RUN_LIMIT 64u

int v3d_irq_handler(_kernel_swi_regs *r, void *pw)
{
    unsigned int sts, hub_sts;

    (void) r;
    (void) pw;

    if (!g_v3d_permanent_mapped)
    {
        /* Cannot happen: v3d_irq_resolve_clear() maps the registers before
         * the vector is ever claimed. If it somehow did, there is nothing
         * safe to do with the hardware, so leave it to the kernel. */
        g_v3d_irq_spurious_count++;
        return 1; /* VECTOR_PASSON */
    }

    sts = IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_STS);
    if (sts)
    {
        /* Acknowledge exactly the bits found - standard write-1-to-
         * clear, matches the real Linux v3d driver's own IRQ handler. */
        IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_CLR) = sts;
        if (sts & V3D_INT_FLDONE) g_v3d_irq_bin_done_count++;
        if (sts & V3D_INT_FRDONE) g_v3d_irq_render_done_count++;
    }

    /* Hub and core share one GIC line on this hardware (V3D 4.2) - see the
     * long comment on V3D_HUB_INT_STS in v3d_regs.h. Hub sources are
     * masked off entirely (v3d_irq_unmask), but read+acknowledge
     * unconditionally anyway: this is a level-triggered line, and anything
     * left set here would make the GIC re-assert forever the instant
     * HAL_IRQClear runs - exactly what froze the machine before this was
     * added. */
    hub_sts = IO_WORD(g_v3d_permanent_mapping, V3D_HUB_INT_STS);
    if (hub_sts)
        IO_WORD(g_v3d_permanent_mapping, V3D_HUB_INT_CLR) = hub_sts;

    if (sts & (V3D_INT_FLDONE | V3D_INT_FRDONE))
    {
        g_v3d_irq_claimed_count++;
        g_v3d_irq_empty_run = 0;
    }
    else
    {
        g_v3d_irq_spurious_count++;
        if (++g_v3d_irq_empty_run >= IRQ_EMPTY_RUN_LIMIT)
        {
            IO_WORD(g_v3d_permanent_mapping, V3D_HUB_INT_MSK_SET) = 0xFFFFFFFFu;
            IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_MSK_SET) = 0xFFFFFFFFu;
            g_v3d_wedged = 1;
        }
    }

    v3d_irq_call_clear();
    return 0; /* VECTOR_CLAIM */
}

_kernel_oserror *v3d_irq_claim(void)
{
    _kernel_swi_regs regs;

    regs.r[0] = (int) V3D_IRQ_DEVICE_NUMBER;
    regs.r[1] = (int) v3d_irq_entry;
    regs.r[2] = (int) g_module_private_word; /* becomes R12 when the
                     * handler is invoked - see v3d_irq_set_private_word's
                     * comment in v3d_irq.h for why this must be the
                     * module's own real private word, not 0 */
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
    regs.r[2] = (int) g_module_private_word;
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
            V3D_ERR(1), "V3DDriver: HAL_IRQClear not resolved yet - "
               "call v3d_irq_resolve_clear first"
        };
        return &err_not_resolved;
    }

    v3d_irq_call_clear();
    return NULL;
}

/*
 * Mask everything off, then selectively unmask FLDONE|FRDONE - matches
 * the real Linux v3d driver's own v3d_irq_enable() pattern
 * (drivers/gpu/drm/v3d/v3d_irq.c: V3D_CORE_WRITE(INT_MSK_SET,
 * ~V3D_CORE_IRQS) then V3D_CORE_WRITE(INT_MSK_CLR, V3D_CORE_IRQS)),
 * not guessed. Hub sources are masked off entirely and never
 * unmasked - we don't need any of them (no MMU, no TFU) - see the long
 * comment on V3D_HUB_INT_STS in v3d_regs.h for why this hardware makes
 * that matter more than it otherwise would (shared GIC line with
 * core). Requires the permanent mapping to already exist.
 */
static _kernel_oserror *v3d_irq_unmask(void)
{
    if (!g_v3d_permanent_mapped)
    {
        static _kernel_oserror err_not_mapped = {
            V3D_ERR(2), "V3DDriver: v3d_irq_unmask called before registers mapped"
        };
        return &err_not_mapped;
    }

    IO_WORD(g_v3d_permanent_mapping, V3D_HUB_INT_MSK_SET) = 0xFFFFFFFFu;
    IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_MSK_SET) = 0xFFFFFFFFu;

    /* Drop anything already pending before enabling, as Linux's
     * v3d_irq_enable does: a stale FLDONE/FRDONE left by an earlier
     * instance would otherwise fire the instant it is unmasked and be
     * counted as the first job's completion. */
    IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_CLR) = 0xFFFFFFFFu;
    IO_WORD(g_v3d_permanent_mapping, V3D_HUB_INT_CLR) = 0xFFFFFFFFu;

    IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_MSK_CLR) =
        (V3D_INT_FLDONE | V3D_INT_FRDONE);
    return NULL;
}

/*
 * V3D_IRQTest's own, fully isolated exercise of the real live-fire
 * path: unmask, HAL_IRQEnable, then genuinely wait (not just enable-
 * then-immediately-disable, which never gives real hardware time to
 * actually latch and deliver anything) while watching the handler's
 * own counters, then HAL_IRQDisable and re-mask everything off before
 * returning. If nothing at all is pending or spuriously triggerable,
 * both counts stay 0 for the whole wait. Never submits any GPU job -
 * FLDONE/FRDONE cannot legitimately fire here, so *any* handler
 * invocation during this window is exactly the class of problem this
 * function exists to catch safely, in isolation, before real
 * submission code ever calls v3d_irq_ensure_ready() for real.
 */
_kernel_oserror *v3d_irq_test_live_cycle(unsigned int *out_claimed,
                                           unsigned int *out_spurious)
{
    _kernel_oserror *err;
    unsigned int spin;

    err = v3d_irq_unmask();
    if (err)
        return err;

    err = v3d_irq_hal_enable();
    if (err)
        return err;

    for (spin = 0; spin < 10000000u; spin++)
        ; /* deliberately idle - see comment above */

    *out_claimed = g_v3d_irq_claimed_count;
    *out_spurious = g_v3d_irq_spurious_count;

    err = v3d_irq_hal_disable();

    /* Re-mask everything at the core level regardless of the disable
     * result above, so we leave nothing enabled behind us. */
    if (g_v3d_permanent_mapped)
        IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_MSK_SET) = 0xFFFFFFFFu;

    return err;
}

const io_mapping_t *v3d_irq_permanent_mapping(void)
{
    return g_v3d_permanent_mapped ? &g_v3d_permanent_mapping : NULL;
}

_kernel_oserror *v3d_irq_ensure_ready(void)
{
    _kernel_oserror *err;

    if (g_v3d_irq_ready)
        return NULL;

    err = v3d_irq_claim();
    if (err)
        return err;
    g_v3d_irq_vec_claimed = 1;

    err = v3d_irq_resolve_clear();
    if (!err)
        err = v3d_irq_unmask();
    if (!err)
        err = v3d_irq_hal_enable();

    if (err)
    {
        /* Undo what was done: a half-built state used to leave the vector
         * claimed with shutdown believing there was nothing to release. */
        if (g_v3d_permanent_mapped)
        {
            IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_MSK_SET) = 0xFFFFFFFFu;
            IO_WORD(g_v3d_permanent_mapping, V3D_HUB_INT_MSK_SET) = 0xFFFFFFFFu;
        }
        (void) v3d_irq_release();
        g_v3d_irq_vec_claimed = 0;
        return err;
    }

    g_v3d_irq_empty_run = 0;
    g_v3d_irq_ready = 1;
    return NULL;
}

/*
 * Must be called from module_final, unconditionally - a no-op if
 * v3d_irq_ensure_ready() was never called (module unloaded without
 * any GPU submission ever happening). Otherwise mask everything back
 * off, HAL_IRQDisable, and release the vector.
 *
 * This matters more than ordinary cleanup hygiene: OS_ClaimDeviceVector
 * only recognises a later claim as replacing an earlier one when the
 * device number, handler address and R12 all match exactly - a
 * *reloaded* module gets a different handler address every time (a
 * fresh RMA allocation), so it is never recognised as "the same
 * driver" replacing the old claim. Without this, every reload leaves
 * the previous instance's claim permanently in the vector's chain of
 * claimants, pointing at code and data that no longer exist once that
 * instance is unloaded - a dangling entry a later real interrupt could
 * still reach (e.g. if some future version of this handler ever
 * legitimately passes on an unrecognised interrupt) with unpredictable
 * results. BCMSupport's own real module does the equivalent in its
 * mailbox_shutdown().
 */
_kernel_oserror *v3d_irq_shutdown(void)
{
    _kernel_oserror *err = NULL;

    if (!g_v3d_irq_ready && !g_v3d_irq_vec_claimed)
        return NULL;

    if (g_v3d_permanent_mapped)
    {
        /* Mask, THEN clear - Linux's v3d_irq_disable order. Leaving a
         * status bit set behind us would hand the next instance a stale
         * "job done". */
        IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_MSK_SET) = 0xFFFFFFFFu;
        IO_WORD(g_v3d_permanent_mapping, V3D_HUB_INT_MSK_SET) = 0xFFFFFFFFu;
        IO_WORD(g_v3d_permanent_mapping, V3D_CTL_INT_CLR) = 0xFFFFFFFFu;
        IO_WORD(g_v3d_permanent_mapping, V3D_HUB_INT_CLR) = 0xFFFFFFFFu;
    }

    if (g_v3d_irq_ready)
        err = v3d_irq_hal_disable();

    {
        _kernel_oserror *release_err = v3d_irq_release();
        if (!err)
            err = release_err;
    }

    g_v3d_irq_vec_claimed = 0;
    g_v3d_irq_ready = 0;
    return err;
}
