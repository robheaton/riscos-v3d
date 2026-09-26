/*
 * module.c - RISC OS module skeleton for milestone M0/M1 of an
 * independent V3D (Pi 4 / CM4 GPU) driver: power on the block, read
 * back its IDENT registers, and expose that over two SWIs.
 *
 * This is a SKETCH to validate the approach end-to-end on real hardware
 * before committing to anything bigger (control lists, shaders, Mesa).
 * It intentionally does NOT touch the CLE/render path yet.
 *
 * Build: GCCSDK cross-toolchain + cmhg (see ../Makefile).
 *
 * v2: replaced the earlier HAL_IOAddress() guess with OS_Memory 14/15,
 * the documented public API for temporarily mapping device physical
 * memory (see io_map.h). Each access now maps its megabyte window, does
 * its reads/writes, and releases it immediately - nothing is cached
 * across calls, per the PRM's explicit warning against long-life
 * mappings from this shared window.
 */

#include <stddef.h>
#include "kernel.h"
#include "swis.h"
#include "v3d_regs.h"
#include "io_map.h"
#include "phys_buffer.h"
#include "cl_clear.h"
#include "memsplit.h"
#include "V3DDriver.h" /* cmunge-generated: SWI numbers + entry-point
                         * prototypes. Included so a signature mismatch
                         * (like the module_final one below) is a compile
                         * error, not a silent ABI bug. */

static int g_powered_on = 0;

/* M2 in progress: physical buffer infrastructure for control lists.
 * Created lazily on first use rather than in module_init, so a module
 * load with no buffer use touches no PMP state at all. Shared by every
 * SWI that needs a physical buffer - each picks its own non-overlapping
 * page range within it (see the page_index comments at each call site). */
static pmp_area_t g_pmp;
static int g_pmp_created = 0;

static _kernel_oserror *ensure_pmp(void)
{
    _kernel_oserror *err;

    if (g_pmp_created)
        return NULL;

    /* 64 pages (256KB) covers V3D_TestBuffer's 1 page plus
     * v3d_clear_test's 9 pages with plenty of headroom to grow. */
    err = pmp_create("V3DBuf", 1024u * 1024u, 64, &g_pmp);
    if (err)
        return err;

    g_pmp_created = 1;
    return NULL;
}

/* Uses PMP page 0. */
static _kernel_oserror *v3d_test_buffer(unsigned int *phys_addr_out,
                                         unsigned int *readback_out,
                                         unsigned int *logical_addr_out)
{
    _kernel_oserror *err;
    void *log_ptr;
    unsigned int phys_addr;
    volatile unsigned int *word;

    err = ensure_pmp();
    if (err)
        return err;

    err = pmp_alloc(&g_pmp, 0, 1, &phys_addr, &log_ptr);
    if (err)
        return err;

    word = (volatile unsigned int *) log_ptr;
    *word = 0xDEADBEEFu;

    *phys_addr_out = phys_addr;
    *readback_out = *word;
    *logical_addr_out = (unsigned int) log_ptr;
    return NULL;
}

/* Uses PMP pages 16-24 (9 pages) - well clear of V3D_TestBuffer's page 0. */
#define CLEAR_TEST_BASE_PAGE 16u

/* Uses PMP pages 32-41 (10 pages) - well clear of ClearTest's range. */
#define CLEAR_SPRITE_BASE_PAGE 32u

/*
 * Power-on sequence, transcribed from the MIT-licensed bare-metal
 * reference (pm.cpp) - direct PM_GRAFX reset deassert + ASB master/slave
 * gate release. No firmware mailbox call required for this step.
 *
 * DIVERGES FROM THE REFERENCE HERE, deliberately: the reference only
 * ever sets PM_V3DRSTN (deassert reset), never clears it, because it
 * runs once on a chip fresh from power-on reset, where the bit is
 * already guaranteed clear - their single "set" call is the real,
 * only reset-deassertion transition that ever happens. We call
 * V3D_PowerOn repeatedly across a whole debugging session without an
 * intervening reboot, so after the first call the bit is already set
 * and every subsequent call was a complete no-op - including, we
 * discovered via CT0CA/CT0RA reading a stale address from several
 * test runs ago, doing nothing to un-wedge a CLE thread that got
 * stuck on an early malformed submission. Explicitly asserting reset
 * first, then deasserting it, forces a genuine pulse every call.
 */
static _kernel_oserror *v3d_power_on(void)
{
    io_mapping_t pm, v3d;
    _kernel_oserror *err;
    unsigned int val;
    volatile unsigned int spin; /* volatile: the reset-pulse delay loop
                                  * below has no other side effect, and
                                  * -O2 is entitled to delete a plain
                                  * empty counting loop entirely */

    /* --- PM_GRAFX: assert then deassert V3D reset (real pulse) --- */
    err = io_map(PM_MB_BASE, &pm);
    if (err)
        return err;

    val = IO_WORD(pm, PM_GRAFX);
    IO_WORD(pm, PM_GRAFX) = PM_PASSWORD | (val & ~PM_V3DRSTN); /* assert reset */

    for (spin = 0; spin < 100000u; spin++)
        ; /* crude delay - no documented minimum pulse width to go on */

    val = IO_WORD(pm, PM_GRAFX);
    IO_WORD(pm, PM_GRAFX) = PM_PASSWORD | val | PM_V3DRSTN; /* deassert reset */

    io_unmap(&pm);

    /* --- ASB master/slave gates (same megabyte as V3D_HUB/CORE0) --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err)
        return err;

    IO_WORD(v3d, ASB_V3D_M_CTRL) = IO_WORD(v3d, ASB_V3D_M_CTRL) & ~ASB_REQ_STOP;
    for (spin = 0; IO_WORD(v3d, ASB_V3D_M_CTRL) & ASB_ACK; spin++)
    {
        if (spin > 1000000)
        {
            static _kernel_oserror err_asb_m = {
                2, "V3DDriver: timed out waiting for ASB master gate ACK "
                   "to clear during V3D power-on"
            };
            io_unmap(&v3d);
            return &err_asb_m;
        }
    }

    IO_WORD(v3d, ASB_V3D_S_CTRL) = IO_WORD(v3d, ASB_V3D_S_CTRL) & ~ASB_REQ_STOP;
    for (spin = 0; IO_WORD(v3d, ASB_V3D_S_CTRL) & ASB_ACK; spin++)
    {
        if (spin > 1000000)
        {
            static _kernel_oserror err_asb_s = {
                3, "V3DDriver: timed out waiting for ASB slave gate ACK "
                   "to clear during V3D power-on"
            };
            io_unmap(&v3d);
            return &err_asb_s;
        }
    }

    io_unmap(&v3d);

    g_powered_on = 1;
    return NULL;
}

static _kernel_oserror *v3d_read_ident(unsigned int *ident, unsigned int n)
{
    io_mapping_t v3d;
    _kernel_oserror *err;

    if (!g_powered_on)
    {
        static _kernel_oserror err_not_on = {
            4, "V3DDriver: V3D_ReadIdent called before V3D_PowerOn"
        };
        return &err_not_on;
    }

    /* V3D_HUB_IDENT0/1/2 and V3D_CTL_IDENT0 all sit in the same
     * megabyte window (V3D_MB_BASE) - one mapping covers all four. */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err)
        return err;

    if (n > 0) ident[0] = IO_WORD(v3d, V3D_HUB_IDENT0);
    if (n > 1) ident[1] = IO_WORD(v3d, V3D_HUB_IDENT1);
    if (n > 2) ident[2] = IO_WORD(v3d, V3D_HUB_IDENT2);
    if (n > 3) ident[3] = IO_WORD(v3d, V3D_CTL_IDENT0);
    /* CT0CS/CT1CS added to check what a freshly-reset thread's state
     * actually is *before* any submission ever happens - v3d_restart_
     * thread's "skip restart if CTRUN already set" logic assumes a
     * fresh thread reads CTRUN=0, and that assumption has never
     * actually been checked in isolation like this. */
    if (n > 4) ident[4] = IO_WORD(v3d, V3D_CLE_CT0CS);
    if (n > 5) ident[5] = IO_WORD(v3d, V3D_CLE_CT1CS);

    io_unmap(&v3d);
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Module entry points (names must match the .cmhg file)            */
/* ---------------------------------------------------------------- */

_kernel_oserror *module_init(const char *cmd_tail, int podule_base, void *pw)
{
    (void) cmd_tail;
    (void) podule_base;
    (void) pw;

    g_powered_on = 0;
    /* Deliberately doing nothing else here: no hardware is touched, and
     * no IO region is mapped, until a SWI actually asks for it. */
    return NULL;
}

_kernel_oserror *module_final(int fatal, int podule_base, void *pw)
{
    (void) fatal;
    (void) podule_base;
    (void) pw;
    /* No persistent IO mapping is held, so nothing to release. */
    return NULL;
}

/* ---------------------------------------------------------------- */
/* SWI handler - dispatch table order must match the .cmhg file     */
/* ---------------------------------------------------------------- */

enum
{
    SWI_V3D_PowerOn = 0,
    SWI_V3D_ReadIdent = 1,
    SWI_V3D_TestBuffer = 2,
    SWI_V3D_ClearTest = 3,
    SWI_V3D_ClearToSprite = 4,
    SWI_V3D_MemSplit = 5,
};

_kernel_oserror *v3d_swi_handler(int swi_no, _kernel_swi_regs *r, void *pw)
{
    (void) pw;

    switch (swi_no)
    {
        case SWI_V3D_PowerOn:
        {
            _kernel_oserror *err = v3d_power_on();
            if (err)
                return err;
            r->r[0] = 1; /* success flag */
            return NULL;
        }

        case SWI_V3D_ReadIdent:
        {
            unsigned int ident[6] = { 0 };
            _kernel_oserror *err = v3d_read_ident(ident, 6);
            if (err)
                return err;
            r->r[0] = ident[0];
            r->r[1] = ident[1];
            r->r[2] = ident[2];
            r->r[3] = ident[3];
            r->r[4] = ident[4]; /* CT0CS - diagnostic, see v3d_read_ident */
            r->r[5] = ident[5]; /* CT1CS */
            return NULL;
        }

        case SWI_V3D_TestBuffer:
        {
            unsigned int phys_addr, readback, logical_addr;
            _kernel_oserror *err =
                v3d_test_buffer(&phys_addr, &readback, &logical_addr);
            if (err)
                return err;
            r->r[0] = phys_addr;
            r->r[1] = readback;
            r->r[2] = logical_addr;
            return NULL;
        }

        case SWI_V3D_ClearTest:
        {
            unsigned int phys_addr, readback;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    5, "V3DDriver: V3D_ClearTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp();
            if (err)
                return err;

            err = v3d_clear_test(&g_pmp, CLEAR_TEST_BASE_PAGE, r->r[0],
                                  &phys_addr, &readback);
            if (err)
                return err;

            r->r[0] = phys_addr;
            r->r[1] = readback;
            return NULL;
        }

        case SWI_V3D_ClearToSprite:
        {
            unsigned int phys_addr, readback;
            void *log_addr;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    9, "V3DDriver: V3D_ClearToSprite called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp();
            if (err)
                return err;

            err = v3d_clear_to_sprite(&g_pmp, CLEAR_SPRITE_BASE_PAGE, r->r[0],
                                       &phys_addr, &log_addr, &readback);
            if (err)
                return err;

            r->r[0] = (unsigned int) log_addr; /* sprite area - pass to OS_SpriteOp */
            r->r[1] = readback;
            r->r[2] = phys_addr;
            return NULL;
        }

        case SWI_V3D_MemSplit:
        {
            unsigned int arm_base, arm_size, vc_base, vc_size;
            _kernel_oserror *err = v3d_read_mem_split(&arm_base, &arm_size,
                                                        &vc_base, &vc_size);
            if (err)
                return err;

            r->r[0] = arm_base;
            r->r[1] = arm_size;
            r->r[2] = vc_base;
            r->r[3] = vc_size;
            return NULL;
        }

        default:
            /* error_BAD_SWI is cmunge's own generated macro (see
             * V3DDriver.h) for exactly this case - the documented
             * correct return value for a SWI number out of range for
             * this module, not a made-up error. */
            return error_BAD_SWI;
    }
}
