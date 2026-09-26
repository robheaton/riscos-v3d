/*
 * screen_info.c - see screen_info.h.
 *
 * OS_ReadVduVariables (SWI &31): R0 = pointer to a list of VDU variable
 * numbers terminated by -1, R1 = pointer to a same-length array of words
 * to receive the results, one per listed variable in order. Confirmed
 * against a real, shipped GCCSDK header (env/include/oslib/os.h) rather
 * than guessed from a hazy memory of the PRM - see os_ReadVduVariables's
 * declaration there (SWI number 0x31) and the os_VDUVAR_* enum for the
 * variable numbers themselves.
 */
#include <stddef.h>
#include "screen_info.h"
#include "swis.h"

#define SWI_OS_ReadVduVariables 0x31

#define VDUVAR_MODE_FLAGS    0x00u
#define VDUVAR_LINE_LENGTH   0x06u
#define VDUVAR_LOG2_BPP      0x09u
#define VDUVAR_XWIND_LIMIT   0x0Bu
#define VDUVAR_YWIND_LIMIT   0x0Cu
#define VDUVAR_SCREEN_START  0x94u
#define VDUVAR_DISPLAY_START 0x95u

_kernel_oserror *v3d_read_screen_info(v3d_screen_info_t *out)
{
    static const int vars[8] = {
        VDUVAR_MODE_FLAGS, VDUVAR_LOG2_BPP, VDUVAR_LINE_LENGTH,
        VDUVAR_XWIND_LIMIT, VDUVAR_YWIND_LIMIT, VDUVAR_SCREEN_START,
        VDUVAR_DISPLAY_START, -1
    };
    int results[7];
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    regs.r[0] = (int) vars;
    regs.r[1] = (int) results;

    err = _kernel_swi(SWI_OS_ReadVduVariables, &regs, &regs);
    if (err)
        return err;

    out->mode_flags    = (unsigned int) results[0];
    out->log2_bpp      = (unsigned int) results[1];
    out->line_length    = (unsigned int) results[2];
    out->xwind_limit    = (unsigned int) results[3];
    out->ywind_limit    = (unsigned int) results[4];
    out->screen_start   = (unsigned int) results[5];
    out->display_start  = (unsigned int) results[6];
    return NULL;
}
