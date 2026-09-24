/*
 * memsplit.h - query the VideoCore firmware mailbox for the ARM/VC
 * memory split, to check directly whether there's a physical-address
 * ceiling the V3D GPU can't reach - rather than continuing to infer
 * one from where V3D_ClearTest/V3D_ClearToSprite happen to fail.
 *
 * Property-tag format confirmed via public documentation of the
 * standard Raspberry Pi firmware mailbox protocol (tags 0x00010005
 * "Get ARM memory" and 0x00010006 "Get VC memory", both: empty
 * request, response = base(u32) + size(u32)) - not guessed.
 */
#ifndef MEMSPLIT_H
#define MEMSPLIT_H

#include "kernel.h"
#include "phys_buffer.h"

_kernel_oserror *v3d_read_mem_split(pmp_area_t *pmp, unsigned int page_index,
                                     unsigned int *arm_base, unsigned int *arm_size,
                                     unsigned int *vc_base, unsigned int *vc_size);

#endif /* MEMSPLIT_H */
