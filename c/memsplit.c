/*
 * memsplit.c - see memsplit.h.
 */

#include "memsplit.h"
#include "v3d_regs.h"
#include "io_map.h"

#define MBOX_CHANNEL_PROP 8u

#define MBOX_TAG_GET_ARM_MEMORY 0x00010005u
#define MBOX_TAG_GET_VC_MEMORY  0x00010006u

/*
 * Message layout (standard Raspberry Pi firmware mailbox property
 * protocol - multiple tags in one buffer, each followed by its own
 * response, terminated by a zero tag):
 *   [0] total buffer size in bytes
 *   [1] request code (0 = process request)
 *   [2] tag id (ARM memory)
 *   [3] value buffer size (8: base + size)
 *   [4] request/response indicator (0 on send)
 *   [5],[6] value buffer (base, size) - filled in by the GPU
 *   [7] tag id (VC memory)
 *   [8] value buffer size (8)
 *   [9] request/response indicator (0 on send)
 *   [10],[11] value buffer (base, size)
 *   [12] end tag (0)
 */
#define MSG_WORDS 13u

_kernel_oserror *v3d_read_mem_split(pmp_area_t *pmp, unsigned int page_index,
                                     unsigned int *arm_base, unsigned int *arm_size,
                                     unsigned int *vc_base, unsigned int *vc_size)
{
    unsigned int msg_phys;
    void *msg_log;
    volatile unsigned int *m;
    _kernel_oserror *err;
    io_mapping_t mbox;
    unsigned int ptr;
    unsigned int spin;

    err = pmp_alloc(pmp, page_index, 1, &msg_phys, &msg_log);
    if (err)
        return err;

    m = (volatile unsigned int *) msg_log;
    m[0] = MSG_WORDS * 4u;
    m[1] = 0;
    m[2] = MBOX_TAG_GET_ARM_MEMORY;
    m[3] = 8;
    m[4] = 0;
    m[5] = 0;
    m[6] = 0;
    m[7] = MBOX_TAG_GET_VC_MEMORY;
    m[8] = 8;
    m[9] = 0;
    m[10] = 0;
    m[11] = 0;
    m[12] = 0;

    /* msg_phys is page-aligned, so its low 4 bits are already zero -
     * safe to OR the channel number straight in. */
    ptr = msg_phys | MBOX_CHANNEL_PROP;

    err = io_map(MBOX_MB_BASE, &mbox);
    if (err)
        return err;

    for (spin = 0; IO_WORD(mbox, MBOX_STATUS) & MBOX_FULL; spin++)
    {
        if (spin > 1000000u)
        {
            static _kernel_oserror err_full = {
                10, "V3DDriver: mailbox stayed full waiting to send "
                    "memory-split request"
            };
            io_unmap(&mbox);
            return &err_full;
        }
    }

    IO_WORD(mbox, MBOX_WRITE) = ptr;

    for (;;)
    {
        for (spin = 0; IO_WORD(mbox, MBOX_STATUS) & MBOX_EMPTY; spin++)
        {
            if (spin > 1000000u)
            {
                static _kernel_oserror err_empty = {
                    11, "V3DDriver: mailbox stayed empty waiting for "
                        "memory-split reply"
                };
                io_unmap(&mbox);
                return &err_empty;
            }
        }

        if (IO_WORD(mbox, MBOX_READ) == ptr)
            break; /* reply to our own message, not someone else's */
    }

    io_unmap(&mbox);

    *arm_base = m[5];
    *arm_size = m[6];
    *vc_base = m[10];
    *vc_size = m[11];
    return NULL;
}
