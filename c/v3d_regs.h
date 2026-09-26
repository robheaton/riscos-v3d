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

/* ---- VideoCore mailbox (property-tag interface, channel 8) ----
 * There are two independent mailbox peripherals here, not one:
 * MAIL0 (VC -> ARM, offsets 0x00-0x1C) is what we READ replies from,
 * and MAIL1 (ARM -> VC, offsets 0x20-0x3C) is what we WRITE requests
 * to. Each has its own status register - MBOX_STATUS (MAIL0_STA) must
 * only be used to test MBOX_EMPTY before reading MBOX_READ; the FULL
 * check before writing MBOX_WRITE must instead poll MBOX_SEND_STATUS
 * (MAIL1_STA). An earlier version of this header used MBOX_STATUS for
 * both, which polls the wrong peripheral's flag before sending.
 * Verified against the Linux kernel's own mailbox driver,
 * drivers/mailbox/bcm2835-mailbox.c (MAIL0_STA=0x18, MAIL1_WRT=0x20,
 * MAIL1_STA=0x38), fetched from git.kernel.org 2026-09-24. */
#define VIDEOCORE_MBOX    (PERIPHERAL_BASE + 0x0000B880u)
#define MBOX_READ         (VIDEOCORE_MBOX + 0x00u)
#define MBOX_POLL         (VIDEOCORE_MBOX + 0x10u)
#define MBOX_SENDER       (VIDEOCORE_MBOX + 0x14u)
#define MBOX_STATUS       (VIDEOCORE_MBOX + 0x18u)  /* MAIL0_STA: use only for MBOX_EMPTY before reading MBOX_READ */
#define MBOX_CONFIG       (VIDEOCORE_MBOX + 0x1Cu)
#define MBOX_WRITE        (VIDEOCORE_MBOX + 0x20u)  /* MAIL1_WRT */
#define MBOX_SEND_STATUS  (VIDEOCORE_MBOX + 0x38u)  /* MAIL1_STA: use for MBOX_FULL before writing MBOX_WRITE */
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

/*
 * Hub-level interrupt status and MMU registers - checked for the first
 * time after two hardware tests of V3D_ClearToSprite (never
 * V3D_ClearTest) reproducibly showed CT0CA stuck near zero and CT0QEA
 * reading back 0xFFFFFFFF (fault-like) while CTRUN showed genuinely
 * running. V3D_CTL_INT_STS (per-core) has no MMU-related bits at all;
 * MMU violations are reported at the HUB level instead, via
 * V3D_HUB_INT_STS's MMU_WRV/MMU_PTI/MMU_CAP bits. The bare-metal
 * reference never sets up the V3D MMU and assumes physical addresses
 * pass straight through - true on a from-scratch boot, but RISC OS's
 * own firmware/GPU init might have left the MMU enabled with a page
 * table that doesn't cover arbitrary system RAM our PMP hands out,
 * which would produce exactly this symptom. V3D_MMU_CTL bit 0 says
 * definitively whether the MMU is even active - the first thing worth
 * checking before anything else here.
 * Source: real Linux kernel drivers/gpu/drm/v3d/v3d_regs.h.
 */
#define V3D_HUB_INT_STS     (V3D_HUB + 0x50u)
#define V3D_HUB_INT_MMU_CAP (1u << 3)
#define V3D_HUB_INT_MMU_PTI (1u << 4)
#define V3D_HUB_INT_MMU_WRV (1u << 5)
#define V3D_MMU_CTL         (V3D_HUB + 0x1200u)
#define V3D_MMU_CTL_ENABLE  (1u << 0)
#define V3D_MMU_VIO_ID      (V3D_HUB + 0x122Cu)
#define V3D_MMU_VIO_ADDR    (V3D_HUB + 0x1234u)

/* ---- V3D core 0 control / CLE (control-list executor) ---- */
#define V3D_CORE0         (PERIPHERAL_BASE + 0xC04000u)
#define V3D_CTL_IDENT0    (V3D_CORE0 + 0x00u)
#define V3D_CTL_SLCACTL   (V3D_CORE0 + 0x24u)
#define V3D_CTL_L2TCACTL  (V3D_CORE0 + 0x30u)
#define V3D_CTL_L2TFLSTA  (V3D_CORE0 + 0x34u)
#define V3D_CTL_L2TFLEND  (V3D_CORE0 + 0x38u)

/* Interrupt status - diagnostic only, we don't use actual IRQs yet.
 * Source: real Linux kernel drivers/gpu/drm/v3d/v3d_regs.h. Notably,
 * V3D_INT_OUTOMEM: the Linux driver's own comment says the binner
 * stalls waiting for this interrupt to be serviced (more tile-alloc
 * memory fed to it via V3D_PTB_BPOA/BPOS) before it can make further
 * progress - if BFC never advances, this bit is the first thing worth
 * checking, since a bin stalled on OOM looks identical from BFC's
 * perspective to one that never started at all. */
#define V3D_CTL_INT_STS   (V3D_CORE0 + 0x50u)
#define V3D_INT_FRDONE    (1u << 0)  /* render done */
#define V3D_INT_FLDONE    (1u << 1)  /* bin (flush/load) done */
#define V3D_INT_OUTOMEM   (1u << 2)  /* binner stalled, needs more tile-alloc memory */
#define V3D_INT_GMPV      (1u << 5)  /* GPU memory protection violation */

/* Binner overflow-memory pointer/size (PTB = "Primitive Tile Buffer").
 * The real Linux v3d driver writes BPOS=0 before *every* bin job,
 * unconditionally, with the comment "Clear out the overflow
 * allocation, so we don't reuse the overflow attached to a previous
 * job" (drivers/gpu/drm/v3d/v3d_sched.c, v3d_bin_job_run, fetched
 * 2026-09-26) - exactly the "second job in a session" scenario this
 * driver was hanging on before this was added. */
#define V3D_PTB_BPCA      (V3D_CORE0 + 0x300u) /* overflow current addr   */
#define V3D_PTB_BPCS      (V3D_CORE0 + 0x304u) /* overflow current size   */
#define V3D_PTB_BPOA      (V3D_CORE0 + 0x308u) /* overflow alloc addr     */
#define V3D_PTB_BPOS      (V3D_CORE0 + 0x30Cu) /* overflow alloc size     */

#define V3D_CLE_CT0QBA    (V3D_CORE0 + 0x160u) /* bin CL start address    */
#define V3D_CLE_CT1QBA    (V3D_CORE0 + 0x164u) /* render CL start address */
#define V3D_CLE_CT0QEA    (V3D_CORE0 + 0x168u) /* bin CL end address (writing
                                                  * this register is what
                                                  * kicks off binning)     */
#define V3D_CLE_CT1QEA    (V3D_CORE0 + 0x16Cu) /* render CL end address
                                                  * (kicks off rendering)  */
#define V3D_CLE_BFC       (V3D_CORE0 + 0x134u) /* bin frame count         */
#define V3D_CLE_RFC       (V3D_CORE0 + 0x138u) /* render frame count      */
#define V3D_CLE_CT0QTS    (V3D_CORE0 + 0x15Cu) /* bin tile-state addr      */
#define V3D_CLE_CT0QMA    (V3D_CORE0 + 0x170u) /* bin tile-alloc mem addr  */
#define V3D_CLE_CT0QMS    (V3D_CORE0 + 0x174u) /* bin tile-alloc mem size  */

/*
 * "Thread state" registers for CLE thread 0 (bin). V3D_CLE_CT0CA is
 * effectively a program counter into the control list - the real
 * Linux driver's own timeout-detection logic (v3d_cl_job_timedout,
 * drivers/gpu/drm/v3d/v3d_sched.c) reads it and CT0RA specifically to
 * tell "never started" apart from "stuck partway through".
 *
 * CT0CS bit meanings below are NOT guessed - transcribed from
 * Broadcom's own "VideoCore IV 3D Architecture Reference Guide"
 * (docs.broadcom.com/doc/12358545), Table 49 "V3D_CTnCS Register
 * Description". That doc predates V3D (it's for the older VC4
 * generation) and its description of CTnCA/CTnEA as the direct
 * submission registers does NOT apply here - the real V3D driver for
 * this exact silicon (v3d_sched.c) submits via CT0QBA/QEA instead, and
 * that's proven correct by the bare-metal reference actually
 * rendering on real Pi4 hardware. But the CS bit layout, and the CLE's
 * general thread-state model (Stopped/Running/Stopped-at-halt/Error),
 * are architectural concepts that carried forward, and this is the
 * only place they're actually documented at the bit level - used here
 * for diagnostics and for the documented halt-clear/restart sequence.
 */
#define V3D_CLE_CT0CS     (V3D_CORE0 + 0x100u) /* control/status           */
#define V3D_CT_CS_CTRUN   (1u << 5)  /* R/W: 1=running. Writing 1 STOPS the
                                       * thread - never write this to start
                                       * one, only to deliberately halt it. */
#define V3D_CT_CS_CTSUBS  (1u << 4)  /* R/W: writing 1 clears a "stopped at
                                       * halt" condition and restarts the
                                       * thread if CA != EA. Also clears
                                       * CTERR as a side effect ("Cleared on
                                       * restarting" per the PRM). */
#define V3D_CT_CS_CTERR   (1u << 3)  /* R only: set when stopped with an
                                       * error. */
#define V3D_CLE_CT0EA     (V3D_CORE0 + 0x108u) /* active end address       */
#define V3D_CLE_CT0CA     (V3D_CORE0 + 0x110u) /* current address (~ PC)   */
#define V3D_CLE_CT0RA     (V3D_CORE0 + 0x118u) /* return address           */
#define V3D_CLE_CT1CS     (V3D_CORE0 + 0x104u) /* same, thread 1 (render)  */
#define V3D_CLE_CT1EA     (V3D_CORE0 + 0x10Cu)
#define V3D_CLE_CT1CA     (V3D_CORE0 + 0x114u)
#define V3D_CLE_CT1RA     (V3D_CORE0 + 0x11Cu)
#define V3D_CLE_CT0QTS_ENABLE (1u << 1)

#define V3D_L2TCACTL_L2TFLS      (1u << 0)
#define V3D_L2TCACTL_FLM_FLUSH   (0u << 2) /* field, bits[3:2] = 0 = flush */

/*
 * Megabyte-aligned windows for OS_Memory 14 (see map.h). RISC OS only
 * ever has ONE of these mapped at a time - each is a separate region
 * that must be mapped, used, and released before the next.
 *
 *   MBOX_MB_BASE : covers VIDEOCORE_MBOX                      (0xFE000000)
 *   PM_MB_BASE   : covers PM_GRAFX                            (0xFE100000)
 *   V3D_MB_BASE  : covers V3D_HUB, V3D_CORE0 *and* the ASB     (0xFEC00000)
 *                  registers - confirmed all three sit inside the same
 *                  megabyte (0xFEC00000-0xFECFFFFF), so one mapping
 *                  serves the whole power-on + ident-read sequence.
 */
#define MBOX_MB_BASE   (VIDEOCORE_MBOX  & 0xFFF00000u)
#define PM_MB_BASE     (PM_BASE         & 0xFFF00000u)
#define V3D_MB_BASE    (V3D_HUB         & 0xFFF00000u)

#endif /* V3D_REGS_H */
