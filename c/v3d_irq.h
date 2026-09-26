/*
 * v3d_irq.h - Stage 1 of real IRQ handling: claim the V3D interrupt
 * vector and prove OS_ClaimDeviceVector/HAL_IRQEnable/HAL_IRQDisable
 * work safely, without yet unmasking any V3D interrupt bit or wiring
 * the handler into real bin/render completion.
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
 * raw.githubusercontent.com/raspberrypi/linux). Two independent real
 * sources agreeing is why this is trusted despite not being a
 * documented, publicly-enumerable device number.
 *
 * HAL_IRQEnable/HAL_IRQDisable are called here via OS_Hardware 0
 * ("Call HAL routine" - SWI &7A, R8=0, R9=entry number, R0=device
 * number - riscosopen.org/wiki/documentation/show/OS_Hardware 0,
 * fetched 2026-09-26), which is safe from ordinary SWI-handler
 * context. This is deliberately NOT the mechanism used to call
 * HAL_IRQClear from *inside* the interrupt handler in a later stage -
 * the PRM's own OS_ClaimDeviceVector page explicitly warns that a
 * device driver's interrupt routine must "strictly avoid the use of
 * non re-entrant SWIs" (calling a normal SWI from IRQ mode risks
 * corrupting R14_svc if it interrupted other SVC-mode code mid-SWI) -
 * that needs the raw HAL routine address + its "static base" (R9)
 * value, resolved once via OS_Hardware 1 outside interrupt context,
 * called directly with R9 set - a later, separate piece of work.
 */
#ifndef V3D_IRQ_H
#define V3D_IRQ_H

#include "kernel.h"

#define V3D_IRQ_DEVICE_NUMBER 10u /* iDev_GPU_VC3D - see comment above */

/* Incremented by v3d_irq_handler every time it's actually invoked.
 * Stage 1 never unmasks any V3D interrupt bit (see v3d_regs.h's
 * V3D_CTL_INT_MSK_CLR), so V3D cannot physically raise this device's
 * IRQ line yet - this should read 0 after V3D_IRQTest. A nonzero value
 * means something unexpected happened (device number collision, a
 * HAL/GIC surprise) and must be understood before going further. */
extern volatile unsigned int g_v3d_irq_spurious_count;

_kernel_oserror *v3d_irq_claim(void);
_kernel_oserror *v3d_irq_release(void);
_kernel_oserror *v3d_irq_hal_enable(void);
_kernel_oserror *v3d_irq_hal_disable(void);

#endif
