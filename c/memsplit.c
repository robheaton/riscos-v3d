/* SPDX-License-Identifier: GPL-2.0-or-later */
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
 * FIX: don't touch the mailbox hardware at all. Go through
 * bcm_send_temp_property_buffer() (bcmsupport.h) instead, which uses
 * BCMSupport_SendTempPropertyBuffer - it cooperates with that IRQ
 * handler instead of racing it. No physical/NCNB buffer of our own is
 * needed either, so this has no dependency on phys_buffer.h.
 */

#include "memsplit.h"
#include "bcmsupport.h"
#include <stddef.h>

#define MBOX_TAG_GET_ARM_MEMORY 0x00010005u
#define MBOX_TAG_GET_VC_MEMORY  0x00010006u

/* [0] total size, [1] request code, then two tags (id, value-buffer
 * size, req/resp indicator, base, size) back to back, [12] end tag. */
#define MSG_WORDS 13u

_kernel_oserror *v3d_read_mem_split(unsigned int *arm_base, unsigned int *arm_size,
                                     unsigned int *vc_base, unsigned int *vc_size)
{
    unsigned int req[MSG_WORDS];
    unsigned int resp[MSG_WORDS];
    _kernel_oserror *err;

    req[0] = MSG_WORDS * 4u;
    req[1] = 0;
    req[2] = MBOX_TAG_GET_ARM_MEMORY; req[3] = 8; req[4] = 0; req[5] = 0; req[6] = 0;
    req[7] = MBOX_TAG_GET_VC_MEMORY;  req[8] = 8; req[9] = 0; req[10] = 0; req[11] = 0;
    req[12] = 0;

    err = bcm_send_temp_property_buffer(req, resp);
    if (err)
        return err;

    *arm_base = resp[5];
    *arm_size = resp[6];
    *vc_base = resp[10];
    *vc_size = resp[11];
    return NULL;
}
