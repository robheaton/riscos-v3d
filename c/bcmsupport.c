/*
 * bcmsupport.c - see bcmsupport.h.
 */

#include "bcmsupport.h"
#include "beacon.h"

#define SWI_OS_SWINumberFromString 0x39u

static int g_swi_num = 0;

_kernel_oserror *bcm_send_temp_property_buffer(void *req, void *resp)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    if (!g_swi_num)
    {
        regs.r[1] = (int) "BCMSupport_SendTempPropertyBuffer";
        v3d_beacon(B_BCM_LOOKUP_BEFORE);
        err = _kernel_swi((int) SWI_OS_SWINumberFromString, &regs, &regs);
        if (err)
            return err;
        g_swi_num = regs.r[0];
        v3d_beacon(B_BCM_LOOKUP_AFTER);
    }

    regs.r[0] = (int) req;
    regs.r[1] = (int) resp;
    regs.r[2] = 0;
    return _kernel_swi(g_swi_num, &regs, &regs);
}
