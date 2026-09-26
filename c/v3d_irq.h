/*
 * v3d_irq.h - real IRQ handling.
 *
 * Stage 1 (done, hardware-confirmed 2026-09-26): claim the V3D
 * interrupt vector via OS_ClaimDeviceVector and exercise
 * HAL_IRQEnable/HAL_IRQDisable via the safe OS_Hardware 0 SWI path.
 * See device-number sourcing/citations below - unchanged from stage 1.
 *
 * Stage 2a (this version): everything needed for the handler to
 * actually service a real interrupt, built and testable while V3D's
 * own interrupt bits remain masked (V3D_CTL_INT_MSK_CLR is never
 * written anywhere in this stage) - so nothing here can be exercised
 * by a genuine hardware interrupt yet, only by direct SWI-driven
 * testing, same safety margin as stage 1:
 *   - v3d_irq_resolve_clear(): looks up HAL_IRQClear's raw address and
 *     "static base" (R9) value via OS_Hardware 1, once, outside
 *     interrupt context - https://www.riscosopen.org/wiki/documentation/show/OS_Hardware%201
 *     (fetched 2026-09-26): "Calls made to the routine should be in a
 *     privileged mode, with R9 (sb) set to the static base value
 *     returned by this SWI."
 *   - v3d_irq_call_clear(): the actual raw-pointer + R9 call, safe to
 *     use from inside the interrupt handler (a plain branch, no SWI
 *     trap, no R14_svc hazard - see the PRM's OS_ClaimDeviceVector
 *     page, which explicitly warns a handler must "strictly avoid the
 *     use of non re-entrant SWIs"). Also directly SWI-testable outside
 *     interrupt context via V3D_IRQTest, to prove the resolved
 *     address/sb/branch sequence itself doesn't crash before it's
 *     ever exercised for real.
 *   - a permanent (not temporary/shared) IO mapping for V3D's own
 *     registers, established once, so the handler never needs to call
 *     OS_Memory 14 itself (a SWI call, and the exact shared-window-
 *     theft hazard v3d_poll_counter's own comment already warns
 *     about).
 *   - the handler itself now has real logic: check V3D_CTL_INT_STS,
 *     if FLDONE/FRDONE is set acknowledge it (V3D_CTL_INT_CLR +
 *     HAL_IRQClear) and claim; otherwise pass on untouched.
 *
 * Device number 10 = iDev_GPU_VC3D. Not available through any
 * documented enumeration API (RiscOSOpen's own "List of HAL devices"
 * page lists only Video/Audio/SysPeri/Comms/ExpCtl/HID categories -
 * no 3D/GPU one) - taken from HAL_BCM2835's own private header
 * (hdr/BCM2835, gitlab.riscosopen.org/RiscOS/Sources/HAL/HAL_BCM2835,
 * fetched 2026-09-26), and cross-checked independently two ways: (a)
 * that header's own comment chain (iDev_ARM_VC3D = 64+12, "copy of GPU
 * IRQ 10"), and (b) HAL_BCM2835's GIC-400 driver (s/IntVC6)'s comment
 * "GIC SPI 64-127 map to iDev_GPU", giving GIC SPI 64+10=74 - which
 * matches the real upstream Linux device tree's v3d node exactly
 * (arch/arm/boot/dts/broadcom/bcm2711.dtsi: `interrupts = <GIC_SPI 74
 * IRQ_TYPE_LEVEL_HIGH>`, fetched from
 * raw.githubusercontent.com/raspberrypi/linux). Confirmed correct on
 * real hardware (2026-09-26): V3D_IRQTest claims/enables/disables/
 * releases cleanly with zero spurious handler invocations.
 */
#ifndef V3D_IRQ_H
#define V3D_IRQ_H

#include "kernel.h"

#define V3D_IRQ_DEVICE_NUMBER 10u /* iDev_GPU_VC3D - see comment above */

/* Incremented by v3d_irq_handler every time it actually claims the
 * interrupt (i.e. found a real FLDONE/FRDONE bit set) - distinct from
 * g_v3d_irq_spurious_count, which counts invocations where it found
 * nothing recognisable and passed on. Both should read 0 until stage
 * 2b actually unmasks a V3D interrupt bit. */
extern volatile unsigned int g_v3d_irq_claimed_count;
extern volatile unsigned int g_v3d_irq_spurious_count;

_kernel_oserror *v3d_irq_claim(void);
_kernel_oserror *v3d_irq_release(void);
_kernel_oserror *v3d_irq_hal_enable(void);
_kernel_oserror *v3d_irq_hal_disable(void);

/*
 * Resolves HAL_IRQClear's raw address + static-base value (OS_Hardware
 * 1) and establishes the permanent IO mapping the handler will read/
 * write V3D registers through. Must be called once, outside interrupt
 * context (e.g. as part of claiming the vector), before the handler
 * can safely do anything beyond count-and-pass-on.
 */
_kernel_oserror *v3d_irq_resolve_clear(void);

/*
 * Directly exercises the raw-pointer HAL_IRQClear call (v3d_irq_call_
 * clear, defined in v3d_irq.c) from ordinary SWI-handler context, for
 * V3D_IRQTest to prove the resolved address/sb/branch sequence itself
 * doesn't crash before it's ever exercised for real inside a genuine
 * interrupt. v3d_irq_resolve_clear() must have been called first.
 */
_kernel_oserror *v3d_irq_test_call_clear(void);

#endif
