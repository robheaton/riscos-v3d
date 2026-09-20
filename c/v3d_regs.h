/*
 * v3d_regs.h - BCM2711 V3D / power-management / mailbox register offsets.
 *
 * SOURCE OF TRUTH: these offsets are transcribed from the MIT-licensed
 * bare-metal reference implementation at
 *   https://github.com/Random06457/rpi4-gpu-bare-metal-examples
 *   (src/device/mmio.hpp, src/device/pm.hpp), verified working on real
 *   Pi 4 / CM4 hardware by that project as of 2026-08.
 *
 * PERIPHERAL_BASE (0xFE000000) is Pi 4's "low peripheral mode" physical
 * base - the same 32-bit-reachable mapping RISC OS already uses for
 * GPIO/UART/mailbox on this board, so these offsets should hold without
 * translation for a 32-bit RISC OS module.
 */
#ifndef V3D_REGS_H
#define V3D_REGS_H

#define PERIPHERAL_BASE   0xFE000000u

/* ---- VideoCore mailbox (property-tag interface, channel 8) ---- */
#define VIDEOCORE_MBOX    (PERIPHERAL_BASE + 0x0000B880u)
#define MBOX_READ         (VIDEOCORE_MBOX + 0x00u)
#define MBOX_POLL         (VIDEOCORE_MBOX + 0x10u)
#define MBOX_SENDER       (VIDEOCORE_MBOX + 0x14u)
#define MBOX_STATUS       (VIDEOCORE_MBOX + 0x18u)
#define MBOX_CONFIG       (VIDEOCORE_MBOX + 0x1Cu)
#define MBOX_WRITE        (VIDEOCORE_MBOX + 0x20u)
#define MBOX_FULL         0x80000000u
#define MBOX_EMPTY        0x40000000u
/* NOTE: RISC OS's own Firmware module already implements this channel
 * (used for CM4 eMMC detection / display mode queries) - prefer calling
 * into it rather than re-implementing mailbox I/O, if/when it exposes a
 * SWI for arbitrary property-tag requests. Verify against ROOL source. */

/* ---- Power management (password-protected registers) ---- */
#define PM_BASE           (PERIPHERAL_BASE + 0x100000u)
#define PM_GRAFX          (PM_BASE + 0x10Cu)
#define PM_PASSWORD       0x5A000000u   /* OR into every PM register write */
#define PM_V3DRSTN        (1u << 6)     /* deassert V3D reset */

/* ---- Asynchronous AXI bridge (ASB) master/slave gates ---- */
#define RPIVID_ASB_BASE   (PERIPHERAL_BASE + 0xC11000u)
#define ASB_V3D_S_CTRL    (RPIVID_ASB_BASE + 0x8u)
#define ASB_V3D_M_CTRL    (RPIVID_ASB_BASE + 0xCu)
#define ASB_REQ_STOP      (1u << 0)
#define ASB_ACK           (1u << 1)
#define ASB_EMPTY         (1u << 2)
#define ASB_FULL          (1u << 3)

/* ---- V3D hub (shared across cores) ---- */
#define V3D_HUB           (PERIPHERAL_BASE + 0xC00000u)
#define V3D_HUB_AXICFG    (V3D_HUB + 0x00u)
#define V3D_HUB_UIFCFG    (V3D_HUB + 0x04u)
#define V3D_HUB_IDENT0    (V3D_HUB + 0x08u)  /* magic 'V','3','D' + version */
#define V3D_HUB_IDENT1    (V3D_HUB + 0x0Cu)  /* core count, VPM size, etc  */
#define V3D_HUB_IDENT2    (V3D_HUB + 0x10u)

/* ---- V3D core 0 control / CLE (control-list executor) ---- */
#define V3D_CORE0         (PERIPHERAL_BASE + 0xC04000u)
#define V3D_CTL_IDENT0    (V3D_CORE0 + 0x00u)
#define V3D_CTL_SLCACTL   (V3D_CORE0 + 0x24u)
#define V3D_CTL_L2TCACTL  (V3D_CORE0 + 0x30u)
#define V3D_CTL_L2TFLSTA  (V3D_CORE0 + 0x34u)
#define V3D_CTL_L2TFLEND  (V3D_CORE0 + 0x38u)

#define V3D_CLE_CT0QBA    (V3D_CORE0 + 0x160u) /* bin CL start address    */
#define V3D_CLE_CT1QBA    (V3D_CORE0 + 0x164u) /* render CL start address */
#define V3D_CLE_CT0QEA    (V3D_CORE0 + 0x168u) /* bin CL end address      */
#define V3D_CLE_CT1QEA    (V3D_CORE0 + 0x16Cu) /* render CL end address   */
#define V3D_CLE_BFC       (V3D_CORE0 + 0x134u) /* bin frame count         */
#define V3D_CLE_RFC       (V3D_CORE0 + 0x138u) /* render frame count      */

#define V3D_L2TCACTL_L2TFLS      (1u << 0)
#define V3D_L2TCACTL_FLM_FLUSH   (0u << 2) /* field, bits[3:2] = 0 = flush */

#endif /* V3D_REGS_H */
