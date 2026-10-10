/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bcmsupport.h - shared helper for calling into RISC OS's BCMSupport
 * module's property-tag mailbox SWI. See memsplit.c for why we go
 * through this instead of touching the mailbox hardware ourselves.
 */
#ifndef BCMSUPPORT_H
#define BCMSUPPORT_H

#include "kernel.h"

/*
 * R0/R1 = request/response buffers (standard property-tag message
 * format, word-aligned, word 0 = total length in bytes, >= 24).
 * Looks up BCMSupport_SendTempPropertyBuffer's SWI number the first
 * time it's called (via OS_SWINumberFromString) and caches it.
 */
_kernel_oserror *bcm_send_temp_property_buffer(void *req, void *resp);

#endif
