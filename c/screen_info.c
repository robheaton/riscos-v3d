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

#define SWI_OS_Memory 0x68
#define MEMORY_PAGE_OP 0
#define MEMORY_GIVEN_LOG_ADDR   0x200u
#define MEMORY_RETURN_PHYS_ADDR 0x2000u

#define SCREEN_PAGE_SIZE 4096u

_kernel_oserror *v3d_logical_to_physical(void *log_addr, unsigned int *phys_addr_out)
{
    /* os_page_block layout (env/include/oslib/os.h): {page_no, log_addr,
     * phys_addr} - three words, in that order. page_no is unused here
     * (not given, not returned) so it's left zeroed. */
    unsigned int block[3];
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    block[0] = 0;
    block[1] = (unsigned int) log_addr;
    block[2] = 0;

    regs.r[0] = MEMORY_PAGE_OP | MEMORY_GIVEN_LOG_ADDR | MEMORY_RETURN_PHYS_ADDR;
    regs.r[1] = (int) block;
    regs.r[2] = 1; /* one entry in the page block list */

    err = _kernel_swi(SWI_OS_Memory, &regs, &regs);
    if (err)
        return err;

    *phys_addr_out = block[2];
    return NULL;
}

_kernel_oserror *v3d_verify_framestore_contiguous(const v3d_screen_info_t *info,
                                                    unsigned int *phys_base_out)
{
    unsigned int total_bytes = info->line_length * (info->ywind_limit + 1u);
    unsigned int offset;
    unsigned int prev_phys = 0;
    unsigned int base_phys;
    _kernel_oserror *err;

    err = v3d_logical_to_physical((void *) info->screen_start, &base_phys);
    if (err)
        return err;

    prev_phys = base_phys;
    for (offset = SCREEN_PAGE_SIZE; offset < total_bytes; offset += SCREEN_PAGE_SIZE)
    {
        unsigned int this_phys;

        err = v3d_logical_to_physical((void *) (info->screen_start + offset), &this_phys);
        if (err)
            return err;

        if (this_phys != prev_phys + SCREEN_PAGE_SIZE)
        {
            static _kernel_oserror err_discontig = {
                15, "V3DDriver: screen framestore is NOT physically "
                    "contiguous - GraphicsV Render acceleration is not "
                    "safe against this mode's memory layout"
            };
            return &err_discontig;
        }

        prev_phys = this_phys;
    }

    *phys_base_out = base_phys;
    return NULL;
}
