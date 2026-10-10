/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cl_builder.h - tiny byte-buffer writer for V3D control-list packets.
 *
 * Deliberately not C bitfields: the reference implementation's packet
 * structs (MIT-licensed, github.com/Random06457/rpi4-gpu-bare-metal-examples,
 * src/device/v3d/v3d_cl.hpp) use C++ packed bitfields compiled for
 * AArch64. Relying on our own, different toolchain (arm-unknown-riscos-gcc,
 * AArch32) to lay out bitfields identically is an assumption not worth
 * making when getting it wrong produces silently-wrong GPU behaviour
 * instead of a compile error. Every packet below is built by explicit
 * shift/OR into named byte positions instead, with the field layout
 * (bit widths, order, byte totals) transcribed from that same source
 * file and cross-checked against its own static_assert(sizeof(...)==N)
 * lines - see cl_clear.c for the packet-by-packet derivation.
 */
#ifndef CL_BUILDER_H
#define CL_BUILDER_H

typedef struct cl_buf
{
    unsigned char *base;  /* logical pointer, writable by the CPU */
    unsigned int phys_base;
    unsigned int offset;
    unsigned int capacity;
    unsigned int overflow; /* set if a write would have passed `capacity` -
                             * the write is dropped, never performed. Checked
                             * by the job runner before anything is submitted
                             * (added 2026-10-04: the writer used to trust its
                             * callers' size estimates completely) */
} cl_buf_t;

static inline void cl_init(cl_buf_t *b, void *logical, unsigned int phys,
                            unsigned int capacity)
{
    b->base = (unsigned char *) logical;
    b->phys_base = phys;
    b->offset = 0;
    b->capacity = capacity;
    b->overflow = 0;
}

static inline unsigned int cl_paddr(const cl_buf_t *b)
{
    return b->phys_base + b->offset;
}

static inline void cl_u8(cl_buf_t *b, unsigned int v)
{
    if (b->offset >= b->capacity)
    {
        b->overflow = 1;
        return;
    }
    b->base[b->offset++] = (unsigned char) v;
}

static inline void cl_u16(cl_buf_t *b, unsigned int v)
{
    cl_u8(b, v & 0xFFu);
    cl_u8(b, (v >> 8) & 0xFFu);
}

static inline void cl_u24(cl_buf_t *b, unsigned int v)
{
    cl_u8(b, v & 0xFFu);
    cl_u8(b, (v >> 8) & 0xFFu);
    cl_u8(b, (v >> 16) & 0xFFu);
}

static inline void cl_u32(cl_buf_t *b, unsigned int v)
{
    cl_u16(b, v & 0xFFFFu);
    cl_u16(b, (v >> 16) & 0xFFFFu);
}

static inline void cl_u64(cl_buf_t *b, unsigned long long v)
{
    cl_u32(b, (unsigned int) (v & 0xFFFFFFFFull));
    cl_u32(b, (unsigned int) ((v >> 32) & 0xFFFFFFFFull));
}

static inline void cl_f32(cl_buf_t *b, float f)
{
    union { float f; unsigned int u; } conv;
    conv.f = f;
    cl_u32(b, conv.u);
}

#endif /* CL_BUILDER_H */
