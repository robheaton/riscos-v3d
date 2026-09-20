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
 * UNVERIFIED ASSUMPTION (flagged, not guessed silently):
 *   HAL_IOAddress() is the standard ROOL HAL entry point that turns a
 *   device physical address into a logical address a module can read
 *   /write directly (IO space is pre-mapped by the kernel at boot).
 *   Confirm the exact signature against an existing hardware module in
 *   ROOL's source tree (e.g. grep HAL_IOAddress under RiscOS/Sources)
 *   before relying on this - do not ship against this guess unverified.
 */

#include "kernel.h"
#include "swis.h"
#include "v3d_regs.h"

extern void *HAL_IOAddress(unsigned int physical_address);

typedef struct v3d_state
{
    volatile unsigned int *pm;
    volatile unsigned int *asb;
    volatile unsigned int *mbox;
    volatile unsigned int *v3d_hub;
    volatile unsigned int *v3d_core0;
    int powered_on;
} v3d_state_t;

static v3d_state_t g_state;

static volatile unsigned int *map_block(unsigned int phys)
{
    return (volatile unsigned int *) HAL_IOAddress(phys);
}

/* offset in bytes -> word index, since our maps are unsigned int* */
#define REG(base, off) ((base)[(off) / 4])

static void pm_write(volatile unsigned int *pm_base, unsigned int off,
                      unsigned int value)
{
    REG(pm_base, off - PM_BASE) = PM_PASSWORD | value;
}

static unsigned int pm_read(volatile unsigned int *pm_base, unsigned int off)
{
    return REG(pm_base, off - PM_BASE);
}

/*
 * Power-on sequence, transcribed from the MIT-licensed bare-metal
 * reference (pm.cpp) - direct PM_GRAFX reset deassert + ASB master/slave
 * gate release. No firmware mailbox call required for this step.
 */
static _kernel_oserror *v3d_power_on(v3d_state_t *st)
{
    unsigned int val;
    unsigned int spin;

    if (!st->pm || !st->asb)
        return _kernel_last_oserror(); /* not mapped - init failed earlier */

    /* Deassert V3D reset via PM_GRAFX */
    val = pm_read(st->pm, PM_GRAFX);
    pm_write(st->pm, PM_GRAFX, val | PM_V3DRSTN);

    /* Release ASB master gate, then wait for ACK to clear */
    REG(st->asb, ASB_V3D_M_CTRL - RPIVID_ASB_BASE) =
        REG(st->asb, ASB_V3D_M_CTRL - RPIVID_ASB_BASE) & ~ASB_REQ_STOP;

    spin = 0;
    while (REG(st->asb, ASB_V3D_M_CTRL - RPIVID_ASB_BASE) & ASB_ACK)
    {
        if (++spin > 1000000)
            return _kernel_last_oserror(); /* TODO: real error block, timeout */
    }

    /* Release ASB slave gate, then wait for ACK to clear */
    REG(st->asb, ASB_V3D_S_CTRL - RPIVID_ASB_BASE) =
        REG(st->asb, ASB_V3D_S_CTRL - RPIVID_ASB_BASE) & ~ASB_REQ_STOP;

    spin = 0;
    while (REG(st->asb, ASB_V3D_S_CTRL - RPIVID_ASB_BASE) & ASB_ACK)
    {
        if (++spin > 1000000)
            return _kernel_last_oserror(); /* TODO: real error block, timeout */
    }

    st->powered_on = 1;
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

    g_state.pm        = map_block(PM_BASE);
    g_state.asb       = map_block(RPIVID_ASB_BASE);
    g_state.mbox      = map_block(VIDEOCORE_MBOX);
    g_state.v3d_hub   = map_block(V3D_HUB);
    g_state.v3d_core0 = map_block(V3D_CORE0);
    g_state.powered_on = 0;

    if (!g_state.pm || !g_state.asb || !g_state.v3d_hub || !g_state.v3d_core0)
        return _kernel_last_oserror(); /* TODO: proper "couldn't map hardware" error */

    return NULL;
}

void module_final(int fatal, int podule_base, void *pw)
{
    (void) fatal;
    (void) podule_base;
    (void) pw;
    /* Nothing to release: IO mappings are not owned by us, just referenced. */
}

/* ---------------------------------------------------------------- */
/* SWI handler - dispatch table order must match the .cmhg file     */
/* ---------------------------------------------------------------- */

enum
{
    SWI_V3D_PowerOn = 0,
    SWI_V3D_ReadIdent = 1,
};

_kernel_oserror *v3d_swi_handler(int swi_no, _kernel_swi_regs *r, void *pw)
{
    (void) pw;

    switch (swi_no)
    {
        case SWI_V3D_PowerOn:
        {
            _kernel_oserror *err = v3d_power_on(&g_state);
            if (err)
                return err;
            r->r[0] = 1; /* success flag */
            return NULL;
        }

        case SWI_V3D_ReadIdent:
        {
            if (!g_state.powered_on)
                return _kernel_last_oserror(); /* TODO: "V3D not powered on" error */

            r->r[0] = REG(g_state.v3d_hub, V3D_HUB_IDENT0 - V3D_HUB);
            r->r[1] = REG(g_state.v3d_hub, V3D_HUB_IDENT1 - V3D_HUB);
            r->r[2] = REG(g_state.v3d_hub, V3D_HUB_IDENT2 - V3D_HUB);
            r->r[3] = REG(g_state.v3d_core0, V3D_CTL_IDENT0 - V3D_CORE0);
            return NULL;
        }

        default:
            return _kernel_last_oserror();
    }
}
