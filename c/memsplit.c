/*
 * memsplit.c - see memsplit.h.
 *
 * ROOT CAUSE OF THE EARLIER HANG: a raw mailbox implementation here
 * (polling VIDEOCORE_MBOX registers directly) never saw a reply,
 * because RISC OS's own BCMSupport module already owns this mailbox's
 * IRQ. Confirmed by reading BCMSupport's real source
 * (gitlab.riscosopen.org/RiscOS/Sources/HWSupport/BCMSupport,
 * c/mailbox and c/BCMSupport, fetched 2026-09-24): its IRQ handler
 * (mailbox_irq_handler) drains every reply out of the read FIFO the
 * instant the GPU firmware raises the interrupt, and silently discards
 * any reply it doesn't recognise (no matching response_queue entry).
 * A second, independent poller reading the same registers structurally
 * loses that race every time - IRQ latency is far below any poll
 * granularity - so our request's reply was being eaten by BCMSupport
 * before we ever saw it. This is not a timing bug to retry; it is two
 * drivers sharing one piece of hardware without arbitration.
 *
 * FIX: don't touch the mailbox hardware at all. BCMSupport exports
 * BCMSupport_SendTempPropertyBuffer for exactly this (one-off,
 * synchronous, small property-tag request/response) - it copies our
 * request into its own buffer, sends it, blocks via its own IRQ-driven
 * response queue (so it's guaranteed to see the reply), copies the
 * result back, and frees the buffer. No physical/NCNB buffer of our
 * own is needed any more - phys_buffer.h is not used here.
 *
 * SWI signature (from BCMSupport's real module_swi source, same repo,
 * c/BCMSupport, case SWINO(SendTempPropertyBuffer)):
 *   R0 = pointer to request buffer (word-aligned; word 0 = total
 *        length in bytes, >= 24; standard property-tag message format)
 *   R1 = pointer to response buffer (word-aligned, same length)
 *   R2 = flags, must be 0 (all bits reserved)
 * Looked up at runtime via OS_SWINumberFromString (SWI &39, R1 = name
 * pointer in, R0 = SWI number out - verified against OSLib's own
 * xos_swi_number_from_string binding in Tools/oslib/os.h) since
 * BCMSupport is a third-party module we don't link against.
 */

#include "memsplit.h"
#include "kernel.h"
#include <stddef.h>

#define SWI_OS_SWINumberFromString 0x39u

#define MBOX_TAG_GET_ARM_MEMORY 0x00010005u
#define MBOX_TAG_GET_VC_MEMORY  0x00010006u

/* [0] total size, [1] request code, then two tags (id, value-buffer
 * size, req/resp indicator, base, size) back to back, [12] end tag. */
#define MSG_WORDS 13u

static _kernel_oserror *swi_number_from_string(const char *name, int *swi_num)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    regs.r[1] = (int) name;
    err = _kernel_swi((int) SWI_OS_SWINumberFromString, &regs, &regs);
    if (err)
        return err;

    *swi_num = regs.r[0];
    return NULL;
}

_kernel_oserror *v3d_read_mem_split(unsigned int *arm_base, unsigned int *arm_size,
                                     unsigned int *vc_base, unsigned int *vc_size)
{
    unsigned int req[MSG_WORDS];
    unsigned int resp[MSG_WORDS];
    int swi_num;
    _kernel_oserror *err;
    _kernel_swi_regs regs;

    req[0] = MSG_WORDS * 4u;
    req[1] = 0;
    req[2] = MBOX_TAG_GET_ARM_MEMORY; req[3] = 8; req[4] = 0; req[5] = 0; req[6] = 0;
    req[7] = MBOX_TAG_GET_VC_MEMORY;  req[8] = 8; req[9] = 0; req[10] = 0; req[11] = 0;
    req[12] = 0;

    err = swi_number_from_string("BCMSupport_SendTempPropertyBuffer", &swi_num);
    if (err)
        return err;

    regs.r[0] = (int) req;
    regs.r[1] = (int) resp;
    regs.r[2] = 0;
    err = _kernel_swi(swi_num, &regs, &regs);
    if (err)
        return err;

    *arm_base = resp[5];
    *arm_size = resp[6];
    *vc_base = resp[10];
    *vc_size = resp[11];
    return NULL;
}
