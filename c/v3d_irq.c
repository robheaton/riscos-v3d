/*
 * v3d_irq.c - see v3d_irq.h.
 */

#include "v3d_irq.h"
#include "V3DDriver.h" /* for v3d_irq_entry - the cmunge-generated
                         * veneer symbol from cmhg's vector-handlers
                         * directive; only its address is ever used,
                         * it must never be called directly (per
                         * cmunge's own vector-handlers documentation). */

#define SWI_OS_ClaimDeviceVector   0x4Bu
#define SWI_OS_ReleaseDeviceVector 0x4Cu
#define SWI_OS_Hardware            0x7Au

#define OSHW_CallRoutine    0u /* OS_Hardware 0: R8=0, R9=entry number */
#define HALEntry_IRQEnable  1u
#define HALEntry_IRQDisable 2u
/* HALEntry_IRQClear (3) is deliberately not used from here - see the
 * header comment: it must be called from inside the interrupt handler
 * itself, in a later stage, via the raw-pointer+static-base mechanism,
 * not this OS_Hardware-0 SWI path. */

volatile unsigned int g_v3d_irq_spurious_count = 0;

int v3d_irq_handler(_kernel_swi_regs *r, void *pw)
{
    (void) r;
    (void) pw;
    g_v3d_irq_spurious_count++;
    return 1; /* VECTOR_PASSON - never claims in this stage; see header */
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
