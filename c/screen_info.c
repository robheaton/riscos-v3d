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
#define VDUVAR_TOTAL_SCREEN_SIZE 0x96u

_kernel_oserror *v3d_read_screen_info(v3d_screen_info_t *out)
{
    static const int vars[9] = {
        VDUVAR_MODE_FLAGS, VDUVAR_LOG2_BPP, VDUVAR_LINE_LENGTH,
        VDUVAR_XWIND_LIMIT, VDUVAR_YWIND_LIMIT, VDUVAR_SCREEN_START,
        VDUVAR_DISPLAY_START, VDUVAR_TOTAL_SCREEN_SIZE, -1
    };
    int results[8];
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
    out->total_screen_size = (unsigned int) results[7];
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

/* Same 252-byte cap and append helpers as cl_clear.c's diagnose_timeout
 * - duplicated here rather than shared, this is a small enough amount
 * of code that pulling in cl_clear.c's dependency graph isn't worth it. */
#define ERRMESS_CAPACITY 252u

static void append_str(char *dst, unsigned int *pos, const char *s)
{
    while (*s && *pos < ERRMESS_CAPACITY - 1u)
        dst[(*pos)++] = *s++;
}

static void append_hex32(char *dst, unsigned int *pos, unsigned int v)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    if (*pos + 10u >= ERRMESS_CAPACITY)
        return;
    dst[(*pos)++] = '0';
    dst[(*pos)++] = 'x';
    for (i = 28; i >= 0; i -= 4)
        dst[(*pos)++] = hex[(v >> i) & 0xFu];
}

#define SWI_OS_CallAVector 0x34
#define VECTOR_GRAPHICSV   42u
#define GRAPHICSV_REASON_FRAMESTORE_INFO 9u

/*
 * REPLACED (2026-09-26, hardware-diagnosed): this used to walk every
 * page of the screen via v3d_logical_to_physical (OS_Memory 0),
 * starting from ScreenStart. That failed immediately on real hardware,
 * "Address not recognised", on the very FIRST call - translating
 * ScreenStart itself, before the loop (and its per-offset range) even
 * came into it. OS_Memory 0's own PRM note explains why: "addresses
 * must be in RAM... pages which the OS has assigned a valid physical
 * page number to" - the Pi's screen framestore is evidently not
 * presented as ordinary tracked RAM in the sense that call expects
 * (plausibly a HAL-managed region locked down separately from the
 * normal page-allocation bookkeeping OS_Memory 0 walks).
 *
 * `GraphicsV 9` ("Framestore information") is the actual, direct,
 * PRM-documented mechanism for exactly this question - it exists
 * specifically so a driver's framestore location doesn't need to be
 * rediscovered via generic address translation: "the kernel will issue
 * this call to determine the framestore size and location... R0 =
 * Framestore physical base address, R1 = Framestore size". Called here
 * as a plain vector call via OS_CallAVector (SWI &34: R9 = vector
 * number, R0-R8 = vector parameters - confirmed against the real PRM,
 * not guessed), not a claim - this driver is asking the CURRENTLY
 * ACTIVE display driver directly, the same way the kernel itself would.
 *
 * The PRM notes this call is only answered if the active driver
 * indicated (via GraphicsV 8) that it has a separate framestore -
 * "Drivers which don't have separate framestores should ignore this
 * call and leave it unclaimed." If that happens here (R4 still
 * nonzero on return), this fails with a clear, specific error rather
 * than silently trusting stale/zeroed R0-R1.
 */
_kernel_oserror *v3d_get_framestore_phys(const v3d_screen_info_t *info,
                                          unsigned int *phys_base_out)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;
    unsigned int fs_size;

    regs.r[0] = 0;
    regs.r[1] = 0;
    regs.r[2] = 0;
    regs.r[3] = 0;
    regs.r[4] = (int) (GRAPHICSV_REASON_FRAMESTORE_INFO | (0u << 24)); /* driver 0 */
    regs.r[9] = (int) VECTOR_GRAPHICSV;

    err = _kernel_swi(SWI_OS_CallAVector, &regs, &regs);
    if (err)
        return err;

    if (regs.r[4] != 0)
    {
        static _kernel_oserror err_unclaimed = {
            20, "V3DDriver: GraphicsV 9 (framestore information) was not "
                "claimed by the active display driver - it may not report "
                "a separate framestore this way"
        };
        return &err_unclaimed;
    }

    *phys_base_out = (unsigned int) regs.r[0];
    fs_size = (unsigned int) regs.r[1];

    if (fs_size < info->total_screen_size)
    {
        static _kernel_oserror err_too_small = { 21, "" };
        unsigned int pos = 0;
        append_str(err_too_small.errmess, &pos, "V3DDriver: GraphicsV 9 "
                   "reported framestore size ");
        append_hex32(err_too_small.errmess, &pos, fs_size);
        append_str(err_too_small.errmess, &pos, " smaller than "
                   "TotalScreenSize ");
        append_hex32(err_too_small.errmess, &pos, info->total_screen_size);
        err_too_small.errmess[pos] = '\0';
        return &err_too_small;
    }

    return NULL;
}
