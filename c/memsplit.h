/*
 * memsplit.h - query the ARM/VideoCore memory split via the firmware's
 * property-tag mailbox interface.
 *
 * Does NOT talk to the mailbox hardware directly - see memsplit.c for
 * why (BCMSupport already owns that IRQ). No physical buffer is needed
 * either, so this has no dependency on phys_buffer.h/pmp_area_t.
 */
#ifndef MEMSPLIT_H
#define MEMSPLIT_H

#include "kernel.h"

_kernel_oserror *v3d_read_mem_split(unsigned int *arm_base, unsigned int *arm_size,
                                     unsigned int *vc_base, unsigned int *vc_size);

#endif
