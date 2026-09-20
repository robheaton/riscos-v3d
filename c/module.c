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

#include "kernel.h"
#include "swis.h"
#include "v3d_regs.h"
#include "io_map.h"

static int g_powered_on = 0;

/*
 * Power-on sequence, transcribed from the MIT-licensed bare-metal
 * reference (pm.cpp) - direct PM_GRAFX reset deassert + ASB master/slave
 * gate release. No firmware mailbox call required for this step.
 */
static _kernel_oserror *v3d_power_on(void)
{
    io_mapping_t pm, v3d;
    _kernel_oserror *err;
    unsigned int val;
    unsigned int spin;

    /* --- PM_GRAFX: deassert V3D reset --- */
    err = io_map(PM_MB_BASE, &pm);
    if (err)
        return err;

    val = IO_WORD(pm, PM_GRAFX);
    IO_WORD(pm, PM_GRAFX) = PM_PASSWORD | val | PM_V3DRSTN;

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
            io_unmap(&v3d);
            return _kernel_last_oserror(); /* TODO: real error block, timeout */
        }
    }

    IO_WORD(v3d, ASB_V3D_S_CTRL) = IO_WORD(v3d, ASB_V3D_S_CTRL) & ~ASB_REQ_STOP;
    for (spin = 0; IO_WORD(v3d, ASB_V3D_S_CTRL) & ASB_ACK; spin++)
    {
        if (spin > 1000000)
        {
            io_unmap(&v3d);
            return _kernel_last_oserror(); /* TODO: real error block, timeout */
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
        return _kernel_last_oserror(); /* TODO: "V3D not powered on" error */

    /* V3D_HUB_IDENT0/1/2 and V3D_CTL_IDENT0 all sit in the same
     * megabyte window (V3D_MB_BASE) - one mapping covers all four. */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err)
        return err;

    if (n > 0) ident[0] = IO_WORD(v3d, V3D_HUB_IDENT0);
    if (n > 1) ident[1] = IO_WORD(v3d, V3D_HUB_IDENT1);
    if (n > 2) ident[2] = IO_WORD(v3d, V3D_HUB_IDENT2);
    if (n > 3) ident[3] = IO_WORD(v3d, V3D_CTL_IDENT0);

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

void module_final(int fatal, int podule_base, void *pw)
{
    (void) fatal;
    (void) podule_base;
    (void) pw;
    /* No persistent IO mapping is held, so nothing to release. */
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
            _kernel_oserror *err = v3d_power_on();
            if (err)
                return err;
            r->r[0] = 1; /* success flag */
            return NULL;
        }

        case SWI_V3D_ReadIdent:
        {
            unsigned int ident[4];
            _kernel_oserror *err = v3d_read_ident(ident, 4);
            if (err)
                return err;
            r->r[0] = ident[0];
            r->r[1] = ident[1];
            r->r[2] = ident[2];
            r->r[3] = ident[3];
            return NULL;
        }

        default:
            return _kernel_last_oserror();
    }
}
