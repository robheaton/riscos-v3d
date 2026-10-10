/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gfxv_accel.c - see gfxv_accel.h.
 */

#include <stddef.h>
#include "gfxv_accel.h"
#include "accel_api.h"
#include "V3DDriver.h" /* for v3d_gfxv_entry - the cmunge-generated
                         * veneer symbol from cmhg's vector-handlers
                         * directive; only its address is ever used,
                         * never called directly - same pattern as
                         * v3d_irq.c's own v3d_irq_entry usage. */

#define SWI_OS_Claim   0x1Fu
#define SWI_OS_Release 0x20u
#define VECTOR_GRAPHICSV 42u

#define GRAPHICSV_REASON_MASK 0xFFFFu
#define GRAPHICSV_REASON_SETMODE 2u
#define GRAPHICSV_REASON_SETDMAADDRESS 6u
#define GRAPHICSV_REASON_RENDER 13u
#define GVDAG_VRENDER 3u /* GraphicsV 6 R0: the address VDU output goes to */

#define RENDER_OP_COPY 1u
#define RENDER_OP_FILL 2u

/* exposed by module.c - the one function that knows how to actually
 * perform an accelerated fill using this driver's existing PMP/IRQ/
 * screen-framestore infrastructure, in GraphicsV's own inclusive
 * (left, bottom, right, top) coordinate convention (0,0 = bottom-left,
 * positive up - see GraphicsV 13 2's own parameter block doc). Kept
 * there, not here, because it needs module.c's private PMP/screen-info
 * state directly - see module.c's own comment on v3d_accel_render_fill
 * for the coordinate-flip reasoning (GraphicsV is bottom-up, this
 * driver's own internal convention - confirmed by step 3's hardware
 * test - is top-down from ScreenStart). */
extern _kernel_oserror *v3d_accel_render_fill(unsigned int left, unsigned int bottom,
                                                unsigned int right, unsigned int top,
                                                unsigned int colour_rgba);

/* Dithered/patterned twin of v3d_accel_render_fill - see module.c's own
 * comment on v3d_accel_render_striped_fill. `colours[8]` and `row_phase`
 * follow cl_fill.h's v3d_striped_fill_raw convention exactly (this
 * function's caller, handle_render_fill, decodes them straight out of
 * the real ECF block via ecf_decode_striped below). */
extern _kernel_oserror *v3d_accel_render_striped_fill(unsigned int left, unsigned int bottom,
                                                         unsigned int right, unsigned int top,
                                                         const unsigned int colours[8]);

/* Same idea, for op 1 (copy) - see module.c's own comment on
 * v3d_accel_render_copy. Direct, single-pass, non-overlapping only
 * (see cl_copy.h) - v3d_gfxv_handler checks for overlap itself before
 * ever calling this, since that check needs no module.c-private state
 * and belongs at the protocol-decoding layer. */
extern _kernel_oserror *v3d_accel_render_copy(unsigned int src_left, unsigned int src_bottom,
                                                unsigned int dest_left, unsigned int dest_bottom,
                                                unsigned int width, unsigned int height);

/* Overlap-safe twin, used instead when handle_render_copy's own
 * overlap check finds the rectangles intersect (a scroll). By default one
 * ordered pass over the tiles (see cl_copy.c's v3d_copy_screen_overlapping),
 * correct for any overlap; with overlap_mode 0 two passes through a bounce
 * buffer, bounded to 8MB - a bigger copy then falls back to this handler
 * leaving the call unclaimed, same as any other failure. */
extern _kernel_oserror *v3d_accel_render_copy_overlapping(unsigned int src_left, unsigned int src_bottom,
                                                             unsigned int dest_left, unsigned int dest_bottom,
                                                             unsigned int width, unsigned int height);

/* Screen-depth accessor (2026-09-30, 16bpp fill support) - see
 * module.c's own comment on v3d_accel_get_screen_log2_bpp. Needed
 * here, one step before v3d_accel_render_fill/_striped_fill are ever
 * called, because ecf_decode_striped's own sub-pixel-packing logic
 * depends on it. */
extern _kernel_oserror *v3d_accel_get_screen_log2_bpp(unsigned int *log2_bpp_out);

static void *g_gfxv_private_word = NULL;
static int g_gfxv_claimed = 0;

/*
 * Real instrumentation, not guesswork, for the first live-desktop test
 * of a handler now sitting in the actual Wimp/VDU graphics pipeline -
 * same reasoning as v3d_irq.c's own claimed/spurious counters. Exposed
 * via V3D_AccelStats so a test script can confirm what actually
 * happened (how many calls arrived at all, how many were reason 13,
 * how many were op 2 with a genuine solid ECF, how many were actually
 * accelerated) rather than inferring it purely from visual effects.
 */
volatile unsigned int g_gfxv_call_count = 0;
volatile unsigned int g_gfxv_render_count = 0;
volatile unsigned int g_gfxv_fill_op_count = 0;
volatile unsigned int g_gfxv_solid_count = 0;
volatile unsigned int g_gfxv_claimed_count = 0;
volatile unsigned int g_gfxv_error_count = 0;

/*
 * ADDED (2026-09-26, hardware-diagnosed): a first live-desktop run
 * showed only 217/13123 (1.7%) of real solid fills actually
 * accelerated - the rest failed at GPU submission (e.g. a timeout in
 * v3d_fill_screen). The fail-safe worked as designed (nothing on
 * screen looked wrong - failures fall back to normal CPU rendering),
 * but a 98% failure rate needs real diagnosis, not a guess. Rather
 * than assume it's about rectangle size (tiny fills vs huge ones) or
 * call frequency (throughput), both are captured for real: the exact
 * rectangle and error number of the LAST failure, and the min/max
 * pixel area seen among failures vs successes - if failures cluster
 * at one extreme of size while successes cluster at the other, that's
 * real evidence pointing at a size-dependent bug (or a genuine
 * throughput ceiling); if both ranges overlap heavily, size isn't the
 * differentiator and something else is. Exposed via V3D_AccelFailInfo.
 */
volatile unsigned int g_gfxv_last_fail_errnum = 0;
volatile unsigned int g_gfxv_last_fail_left = 0;
volatile unsigned int g_gfxv_last_fail_bottom = 0;
volatile unsigned int g_gfxv_last_fail_right = 0;
volatile unsigned int g_gfxv_last_fail_top = 0;
volatile unsigned int g_gfxv_fail_min_area = 0xFFFFFFFFu;
volatile unsigned int g_gfxv_fail_max_area = 0;
volatile unsigned int g_gfxv_success_min_area = 0xFFFFFFFFu;
volatile unsigned int g_gfxv_success_max_area = 0;

/*
 * Dithered/patterned fill instrumentation (see ecf_decode_striped and
 * v3d_accel_render_striped_fill) - a genuinely different code path from
 * the solid-fill one above (up to 8 GPU submissions instead of 1), so
 * tracked separately rather than folded into the existing solid
 * counters, same reasoning as op 1 (copy) getting its own counters
 * instead of reusing op 2's.
 */
volatile unsigned int g_gfxv_striped_count = 0;
volatile unsigned int g_gfxv_striped_claimed_count = 0;
volatile unsigned int g_gfxv_striped_error_count = 0;
volatile unsigned int g_gfxv_striped_last_fail_errnum = 0;

/*
 * Reentrancy guard around the actual fill/copy work: if a second
 * GraphicsV Render call ever lands while this handler is still
 * mid-flight on a first one (e.g. still spinning in wait_for_irq,
 * which does not disable interrupts), both calls would otherwise
 * share the exact same static PMP scratch pages (g_pmp_fill at
 * SCREEN_FILL_BASE_PAGE/SCREEN_COPY_BASE_PAGE) - two concurrent
 * run_fill/run_copy calls could submit interleaved, mutually-
 * corrupting control lists to the same CL/tile_alloc/tile_state
 * buffers. The nested call is left completely unclaimed (no GPU work
 * attempted, R4 untouched) rather than risking that - same fail-safe
 * philosophy as every other failure path in this driver.
 */
static volatile int g_gfxv_busy = 0;
volatile unsigned int g_gfxv_reentrant_count = 0;

/*
 * Op 1 (copy) instrumentation - added from the start this time, having
 * learned from op 2's own live-test experience (a real bug and a real
 * throughput/reliability question both only became visible once actual
 * invocation counts were captured, not from visual effects alone).
 * Exposed via V3D_AccelCopyStats.
 */
volatile unsigned int g_gfxv_copy_op_count = 0;
volatile unsigned int g_gfxv_copy_overlap_count = 0;
volatile unsigned int g_gfxv_copy_claimed_count = 0;
volatile unsigned int g_gfxv_copy_error_count = 0;
volatile unsigned int g_gfxv_copy_last_fail_errnum = 0;

/* Overlap-safe (scratch-buffer) path specifically - see
 * v3d_accel_render_copy_overlapping. g_gfxv_copy_overlap_count above
 * still counts every overlapping call seen; these track what happened
 * to them now that they're actually attempted instead of always
 * skipped. */
volatile unsigned int g_gfxv_copy_overlap_claimed_count = 0;
volatile unsigned int g_gfxv_copy_overlap_error_count = 0;

/*
 * SIZE LIMITS (2026-10-04). Every accelerated call pays a fixed GPU round
 * trip (build the control lists, submit, wait for the bin and render
 * interrupts) regardless of rectangle size, while the CPU fills or copies
 * a small rectangle in microseconds. Measured on real hardware with
 * test/FillCopyBench.bas (32bpp, CPU vs GPU in one session, every GPU
 * call confirmed accelerated): the fixed cost is about 119us per call
 * (8x8 and 32x32 both take 119us on the GPU, against 2us and 7us for a
 * CPU fill), so claiming everything made small operations up to ~60x
 * SLOWER and only large ones faster.
 *   fill: at 32bpp the GPU breaks even at about 192x192 (36,864 pixels)
 *         and wins by only 1.2-1.3x even at 512x512; at 16bpp (solid
 *         colour; first measured 2026-10-04 after fixing the benchmark's
 *         accidental dither flag) it is worse: 256x256 1.05x, 384x384
 *         1.09x, 512x512 1.02x, because the CPU writes half as many
 *         bytes. Wide/short rectangles (1024x16, 512x32) lose badly
 *         (0.17x-0.27x), because the GPU pays per 64x64 tile touched while
 *         the CPU pays per pixel. So GPU fills are only a marginal win
 *         anywhere, and the default limit is set high enough to admit
 *         only rectangles where it is a worthwhile 1.2x+ at 32bpp.
 *   copy: GPU wins from about 64x64 (1.16x) up to 2-3.5x at 128-512
 *         squares at 32bpp, and from about 96x96 (1.32x) up to 3.08x at
 *         256x256 at 16bpp; wide/short copies (1024x16, 512x32) still
 *         lose (0.62x, 0.84x). The byte-based copy limit sits correctly
 *         at both depths (64x64 at 16bpp is declined, 96x96 accepted).
 * An operation below its limit is simply left UNCLAIMED, so the kernel's
 * own CPU code does it - exactly the fail-safe path every error already
 * takes, so this can never produce a wrong picture. Limits are in BYTES
 * of pixel data (width*height*bytes-per-pixel) so they scale with screen
 * depth, plus a minimum height, since short rectangles waste most of each
 * 64-row tile. The overlapping-copy default was an extrapolation for the
 * two-pass scroll; one-pass scrolls (v0.09) cost the same as a copy, so it is now
 * the copy limit (see the 2026-10-07 calibration below).
 * All are tunable at run time with V3D_AccelLimits; setting every limit
 * to 0 accelerates everything, which is how the benchmark measures the
 * GPU's raw capability.
 *
 * RECALIBRATED FOR v0.08 (2026-10-07). The ~116us fixed cost above turned out
 * to be ~86% CPU work (zeroing and byte-writing into strongly-ordered memory),
 * not GPU time, and v0.07/0.08 removes most of it: test/TuneBench.bas measured
 * a tiny job at 8us (setup 1, bin 2, render 4) instead of 111us, with the
 * output pixel-identical to the CPU's. Per operation, through OS_Plot on the
 * real hardware, the GPU now takes about 15us for a 16x16 fill, 25us at
 * 128x128, 246us at 512x512; 15us for a 32x32 copy, 39us at 128x128, 426us
 * at 512x512; 68us / 214us for a 128x128 / 256x256 scroll (two passes). The
 * CPU figures in the table above (2.8us for an 8x8 fill, 24us at 64x64, 93us
 * at 128x128; 6.5us for an 8x8 copy, 145us at 64x64) put the break-even at
 * about 55x55 pixels for a fill and about 16 rows for a copy. The defaults sit
 * a little above those so that, as before, the default is never meaningfully
 * slower than the CPU; they are a first calibration to be confirmed by the
 * "Dflt" columns of test/FillCopyBench.bas, not a measured optimum.
 *
 * CALIBRATED FROM HARDWARE FOR v0.10 (2026-10-07, FillCopyBench on 0.09, 13 shapes
 * x 3 operations at 32bpp and at 16bpp). Fill: the GPU wins from 64x64 (1.84x at
 * 32bpp), loses at 32x32 (0.64x) and for wide-short strips (1024x16: 0.60x) - the
 * limits above leave exactly those to the CPU. Copy: the GPU wins from 16x16
 * (1.08x) and by 3x at 32x32 and 9x at 64x64. Scroll: now one GPU pass, it costs
 * the same as a copy (34 us either way at 128x128), so its limit was lowered from
 * 8192 to the copy limit: a 32x32 scroll is 3.4x faster on the GPU and the old limit
 * had left it to the CPU. The one remaining loser was fills of a few awkward shapes
 * at 16bpp (512x32 0.87x, 16x512 0.90x): they touch 8 tiles but carry little work
 * per tile, which is what the GFXV_FILL_MIN_BYTES_PER_TILE rule in handle_render_fill
 * catches (it also needs a rows term, GFXV_FILL_ROW_WEIGHT, or it would wrongly drop
 * the 32bpp 16x512 fill, which the GPU wins by 1.31x because the CPU is slow on tall
 * rectangles). A
 * full cost model (CPU = a + b*bytes + c*rows, GPU = g0 + g1*tiles) was fitted to
 * all 52 measurements and gave the same overall gain as these limits (1.61x
 * geometric mean for fills, 3.85x for copies), so it was not adopted.
 */
#define GFXV_FILL_MIN_BYTES_PER_TILE 4500u
#define GFXV_FILL_ROW_WEIGHT         15u /* bytes of CPU work a row's overhead is worth */
#define GFXV_DEFAULT_MIN_FILL_BYTES      16384u  /* 4,096 px at 32bpp (64x64) */
#define GFXV_DEFAULT_MIN_FILL_ROWS       32u
#define GFXV_DEFAULT_MIN_COPY_BYTES      2048u   /* 512 px at 32bpp (~23x23) */
#define GFXV_DEFAULT_MIN_COPY_ROWS       16u
#define GFXV_DEFAULT_MIN_OVERLAP_BYTES   2048u   /* same as a copy: one pass now costs the same */

volatile unsigned int g_gfxv_min_fill_bytes = GFXV_DEFAULT_MIN_FILL_BYTES;
volatile unsigned int g_gfxv_min_fill_rows = GFXV_DEFAULT_MIN_FILL_ROWS;
volatile unsigned int g_gfxv_min_copy_bytes = GFXV_DEFAULT_MIN_COPY_BYTES;
volatile unsigned int g_gfxv_min_copy_rows = GFXV_DEFAULT_MIN_COPY_ROWS;
volatile unsigned int g_gfxv_min_overlap_bytes = GFXV_DEFAULT_MIN_OVERLAP_BYTES;

/* Calls left unclaimed purely because they were below a limit above
 * (genuine, otherwise-acceleratable fills/copies - not errors). */
volatile unsigned int g_gfxv_fill_skipped_count = 0;
volatile unsigned int g_gfxv_copy_skipped_count = 0;

/*
 * 0.11 event trace - see gfxv_accel.h. g_trace_outcome/errnum are set by the
 * fill and copy handlers just before each return and read by the dispatcher.
 */
volatile unsigned int g_gfxv_screen_decline_count = 0;
volatile unsigned int g_gfxv_screen_decline_last_errnum = 0;
static gfxv_trace_t g_trace[GFXV_TRACE_MAX];
static volatile unsigned int g_trace_n = 0;
static volatile unsigned int g_trace_armed = 0;
static unsigned int g_trace_seq = 0;
static unsigned int g_trace_outcome = TRACE_NONE;

/* Census storage (see census_note below). */
extern unsigned int v3d_time_us(void);
typedef struct { unsigned int n_gpu, n_cpu, kpix_gpu, kpix_cpu; } census_t;
static census_t g_census[V3D_CENSUS_KINDS][V3D_CENSUS_BUCKETS];
static unsigned int g_census_start_us = 0;
static unsigned int g_census_scroll = 0; /* set by handle_render_copy when it takes the overlapping branch */
static unsigned int g_trace_errnum = 0;

void v3d_gfxv_trace_arm(void)
{
    g_trace_armed = 0;
    g_trace_n = 0;
    g_trace_seq = 0;
    g_trace_armed = 1;
}

const gfxv_trace_t *v3d_gfxv_trace_get(unsigned int idx, unsigned int *stored_out,
                                         unsigned int *armed_out)
{
    *stored_out = g_trace_n;
    *armed_out = g_trace_armed;
    return idx < g_trace_n ? &g_trace[idx] : NULL;
}

static void trace_event(unsigned int kind, unsigned int outcome, unsigned int errnum,
                         unsigned int a, unsigned int b, unsigned int c,
                         unsigned int d, unsigned int e)
{
    gfxv_trace_t *t;
    unsigned int n;

    if (!g_trace_armed)
        return;
    n = g_trace_n; /* read once: the check and the index must agree */
    if (n >= GFXV_TRACE_MAX)
    {
        g_trace_armed = 0;
        return;
    }
    t = &g_trace[n];
    t->kind = kind;
    t->outcome = outcome;
    t->errnum = errnum;
    t->a = a; t->b = b; t->c = c; t->d = d; t->e = e;
    t->seq = g_trace_seq++;
    g_trace_n = n + 1u;
}

/* The screen could not be used for this call: remember why (this decline
 * used to leave no trace at all). */
static void note_no_screen(const _kernel_oserror *err)
{
    g_gfxv_screen_decline_count++;
    g_gfxv_screen_decline_last_errnum = (unsigned int) err->errnum;
    g_trace_outcome = TRACE_NO_SCREEN;
    g_trace_errnum = (unsigned int) err->errnum;
}

/*
 * Returns non-zero if a width x height rectangle is big enough to be worth
 * a GPU round trip. `log2_bpp` is the screen's (4 or 5 here - the caller
 * has already confirmed the depth is supported). Absurd dimensions (far
 * beyond any real screen) return 1 so the existing downstream validation
 * rejects them and the call stays unclaimed anyway, rather than this check
 * having to reason about arithmetic overflow.
 */
static int accel_worthwhile(unsigned int width, unsigned int height,
                             unsigned int log2_bpp,
                             unsigned int min_bytes, unsigned int min_rows)
{
    unsigned int shift = log2_bpp - 3u;
    unsigned int min_pixels = (min_bytes + (1u << shift) - 1u) >> shift;

    if (width > 0xFFFFu || height > 0xFFFFu)
        return 1;
    if (height < min_rows)
        return 0;
    return width * height >= min_pixels;
}

/*
 * ADDED (2026-09-26, hardware-diagnosed): all 376 overlap-copy attempts
 * in a live-desktop run failed with error number 465 - not the "too
 * big" check's own number (14), so genuinely new territory, and a
 * bare number wasn't enough to know what actually went wrong (the
 * same diagnostic gap that hid the earlier top/bottom-swap bug until
 * the error's exact rectangle was captured, not just a count). Copies
 * the real OS error MESSAGE text (err->errmess), not just err->errnum,
 * for both copy paths - exposed via V3D_AccelCopyLastError's returned
 * address, readable directly in BASIC via `$addr%`.
 */
static char g_gfxv_copy_last_fail_msg[252] = "";

static void copy_last_fail_msg(const char *msg)
{
    unsigned int i;
    for (i = 0; i < sizeof(g_gfxv_copy_last_fail_msg) - 1u && msg[i]; i++)
        g_gfxv_copy_last_fail_msg[i] = msg[i];
    g_gfxv_copy_last_fail_msg[i] = '\0';
}

const char *v3d_gfxv_copy_last_fail_msg(void)
{
    return g_gfxv_copy_last_fail_msg;
}

void v3d_gfxv_set_private_word(void *pw)
{
    g_gfxv_private_word = pw;
}

_kernel_oserror *v3d_gfxv_start(void)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    if (g_gfxv_claimed)
        return NULL;

    regs.r[0] = (int) VECTOR_GRAPHICSV;
    regs.r[1] = (int) v3d_gfxv_entry;
    regs.r[2] = (int) g_gfxv_private_word;
    err = _kernel_swi((int) SWI_OS_Claim, &regs, &regs);
    if (err)
        return err;

    g_gfxv_claimed = 1;
    return NULL;
}

_kernel_oserror *v3d_gfxv_stop(void)
{
    _kernel_swi_regs regs;
    _kernel_oserror *err;

    if (!g_gfxv_claimed)
        return NULL;

    regs.r[0] = (int) VECTOR_GRAPHICSV;
    regs.r[1] = (int) v3d_gfxv_entry;
    regs.r[2] = (int) g_gfxv_private_word;
    err = _kernel_swi((int) SWI_OS_Release, &regs, &regs);

    g_gfxv_claimed = 0;
    return err;
}

_kernel_oserror *v3d_gfxv_shutdown(void)
{
    return v3d_gfxv_stop(); /* already a safe no-op if never claimed */
}

/*
 * Widens a 16bpp raw pixel value into this driver's own 8-bit-per-
 * channel colour_rgba convention (R=byte0, G=byte1, B=byte2, A=byte3 -
 * the same packing v3d_fill_raw/v3d_accel_render_fill already expect
 * for 32bpp, and which the V3D clear-colour packet itself always
 * accepts regardless of the render target's real depth - see
 * cl_clear.c/README's own finding, so no further conversion is needed
 * once widened here).
 *
 * CORRECTED (2026-09-30, same day as first written): this hardware's
 * real 16bpp screen mode is genuine RGB565 (5-6-5, all 16 bits used
 * for colour), NOT the RGB555 (5-5-5 + 1 reserved bit) originally
 * assumed from the classic RISC OS PRM sprite-format doc - that doc
 * describes the older VIDC-era format this hardware's HAL video driver
 * doesn't even offer (confirmed: "32 thousand colours" is greyed out
 * in the mode chooser on this hardware - it was never available; the
 * Pi's firmware framebuffer is hardwired to 565).
 *
 * Bit layout CONFIRMED ON REAL HARDWARE (V3D format code 7/BGR565, via
 * test/Format565Test.bas - R bits 11-15, G bits 5-10, B bits 0-4) -
 * NOT the order a first reading of the Raspberry Pi BCMVideo driver's
 * own source comment ("16bpp is RGB565, i.e. RRRRRGGGGGGBBBBB") would
 * suggest by analogy with how the V3D enum's OWN format names read
 * (letters left-to-right = low-bit-to-high-bit, the same convention
 * that explained format 4/ABGR1555's R/B-swap surprise in Phase 2) -
 * that analogy was wrong here: BCMVideo's comment turned out to use
 * the opposite, more conventional MSB-first documentation style,
 * unrelated to the V3D enum's own naming quirk. The V3D enum name
 * "BGR565" read low-to-high (B=low, G=mid, R=high) DOES match what was
 * actually measured - the same Phase 2 naming rule held, once applied
 * to the right name. Takeaway: two different naming sources (a vendor
 * driver's comment vs. a GPU vendor's format enum) can use opposite
 * bit-order conventions - verify each independently, don't assume one
 * predicts the other even when a plausible-looking analogy exists.
 *
 * Uses the standard N-to-8-bit expansion round(v*255/max), not a naive
 * left-shift (which would leave the low bits always zero, visibly
 * darkening/banding colours) - green gets its own 6-bit version since
 * RGB565's green field is wider than red/blue's. Alpha is always fully
 * opaque (0xFF) - RISC OS's 16bpp mode has no alpha channel of its own.
 */
static unsigned int widen_565_to_8bit_rgba(unsigned int raw16)
{
    unsigned int b5 = raw16 & 0x1Fu;
    unsigned int g6 = (raw16 >> 5) & 0x3Fu;
    unsigned int r5 = (raw16 >> 11) & 0x1Fu;
    /* round(v*255/31) and round(v*255/63) as multiply-shift: (v*527+23)>>6
     * and (v*259+33)>>6. Brute-force checked (2026-10-04) to equal the
     * divisions (v*255+15)/31 and (v*255+31)/63 for EVERY 5-bit (0-31) and
     * 6-bit (0-63) input, endpoints 0->0 and max->255. The divisions were
     * software calls on this toolchain, 3 per ECF line x 8 lines on every
     * 16bpp fill. */
    unsigned int r8 = (r5 * 527u + 23u) >> 6;
    unsigned int g8 = (g6 * 259u + 33u) >> 6;
    unsigned int b8 = (b5 * 527u + 23u) >> 6;

    return r8 | (g8 << 8) | (b8 << 16) | (0xFFu << 24);
}

/*
 * Checks the 64-byte ECF block at `ecf` for a pattern this driver can
 * accelerate: at 32bpp (log2_bpp==5) each line's OR/EOR mask is a full
 * 32-bit word - exactly one pixel wide - so the pattern can only vary
 * VERTICALLY (per scanline, cycling every 8 rows), never horizontally
 * within a row. That means ANY ECF block with OR=0xFFFFFFFF on every
 * line (forcing every bit to 1 regardless of the existing pixel, same
 * as the original solid-fill derivation - see gfxv_accel.h's top
 * comment) decodes to up to 8 distinct row-colours, `colours_out[line]
 * = ~ecf[line*2+1]` - a real 2D dithered texture (one that also varies
 * horizontally) is NOT expressible this way and isn't seen here, since
 * its OR mask couldn't be all-1s on every line.
 *
 * At 16bpp (log2_bpp==4, added 2026-09-30) each 32-bit OR/EOR word
 * instead packs TWO horizontally-adjacent 16-bit sub-pixels - so
 * OR=0xFFFFFFFF alone isn't sufficient proof of a horizontally-uniform
 * row: the low and high 16 bits of ~eor_mask must ALSO decode to the
 * same raw pixel value, or the row is a genuine 2-pixel horizontal
 * repeat this driver can't express (fill only takes one colour per
 * row - see cl_fill.h's v3d_striped_fill_raw) and is treated exactly
 * like any other unrecognised pattern: the whole block fails to
 * decode, same fail-safe philosophy as everywhere else in this driver
 * (leave unclaimed, let the CPU render it) rather than guess.
 *
 * Returns 1 and fills *colours_out[0..7] (already widened to this
 * driver's 8-bit-per-channel convention, regardless of log2_bpp) if
 * every line decodes cleanly, 0 otherwise. A genuine solid fill is
 * simply the case where all 8 decoded colours happen to be equal - the
 * caller (handle_render_fill) checks for that itself to keep using the
 * cheaper single-fill path instead of up to 8 GPU submissions.
 */
static int ecf_decode_striped(const unsigned int *ecf, unsigned int log2_bpp,
                               unsigned int colours_out[8])
{
    unsigned int line;

    for (line = 0; line < 8u; line++)
    {
        unsigned int or_mask = ecf[line * 2u];
        unsigned int eor_mask = ecf[line * 2u + 1u];

        if (or_mask != 0xFFFFFFFFu)
            return 0;

        if (log2_bpp == 4u)
        {
            unsigned int inv = ~eor_mask;
            unsigned int lo = inv & 0xFFFFu;
            unsigned int hi = (inv >> 16) & 0xFFFFu;

            if (lo != hi)
                return 0;

            colours_out[line] = widen_565_to_8bit_rgba(lo);
        }
        else
        {
            colours_out[line] = ~eor_mask;
        }
    }

    return 1;
}

static void handle_render_fill(_kernel_swi_regs *r)
{
    const unsigned int *params;
    unsigned int left, bottom, right, top;
    const unsigned int *ecf;
    unsigned int colours[8];
    unsigned int line;
    unsigned int log2_bpp;
    int is_solid;
    _kernel_oserror *err;

    g_gfxv_fill_op_count++;

    /* Needed before the ECF block can even be decoded (log2_bpp==4
     * packs two sub-pixels per 32-bit OR/EOR word - see
     * ecf_decode_striped) - if the screen depth can't be determined at
     * all (e.g. a framestore-geometry read failure), there's nothing
     * safe to decode, so leave unclaimed just like a real content-
     * dependent blend would. */
    err = v3d_accel_get_screen_log2_bpp(&log2_bpp);
    if (err)
    {
        note_no_screen(err);
        return;
    }

    /* GraphicsV 13 2 parameter block: word0=left, word1=TOP, word2=right,
     * word3=BOTTOM (all inclusive), word4=ECF block pointer - confirmed
     * against the real PRM (fetched twice, once before this bug and
     * once again to double-check after finding it - see README). A
     * first version of this code had word1/word3 swapped (bottom/top
     * reversed) - real hardware testing found it: with top<bottom (the
     * exact symptom of this swap on a real rectangle, where the true
     * top edge is numerically >= the true bottom edge), `top - bottom
     * + 1` wraps around as unsigned arithmetic, producing a huge,
     * nonsensical height that cascaded into garbage tile counts and a
     * ~98% real-desktop failure rate (see git history / README for the
     * full diagnosis). */
    params = (const unsigned int *) r->r[2];
    left = params[0];
    top = params[1];
    right = params[2];
    bottom = params[3];
    ecf = (const unsigned int *) params[4];

    /* Reject small rectangles BEFORE touching the ECF block (see the SIZE
     * LIMITS comment above). Measured 2026-10-04: with the check after the
     * decode, a declined 8x8 fill at 16bpp cost ~1.3us more than the CPU
     * fill itself (3.8us vs 2.5us, i.e. 0.67x) because every call widened
     * all 8 ECF lines first; declined copies, which do no decoding, cost
     * only ~0.15us. Doing the cheap size test first makes the common case
     * - a tiny fill the CPU will do - almost free. The stricter limit for
     * dithered fills is applied further down, once we know there is one. */
    if (!accel_worthwhile(right - left + 1u, top - bottom + 1u, log2_bpp,
                           g_gfxv_min_fill_bytes, g_gfxv_min_fill_rows))
    {
        g_gfxv_fill_skipped_count++;
        g_trace_outcome = TRACE_SKIP_SMALL;
        return;
    }

    /* Enough work per 64x64 tile to be worth a GPU job. A GPU fill costs about
     * 8.6 us + 3 us per tile touched; the CPU's cost follows the bytes, plus a
     * per-row overhead that makes TALL rectangles dear for the CPU. The rule
     * "bytes + 15*rows >= 4500*tiles" was fitted to the 26 fill shapes measured on
     * the hardware at both depths (FillCopyBench, 2026-10-07): it declines the two
     * shapes the GPU lost (16bpp 512x32 and 16x512, about 0.9x) and keeps every
     * measured winner (32bpp 512x32 and 16x512 at 1.25x/1.31x, 16bpp 96x96 at
     * 1.24x, everything larger). It is only a refinement of the byte/row limits, so
     * it is skipped when those are set to zero - "all limits 0 = everything goes to
     * the GPU" is what the benchmarks and VerifySuite rely on - and only evaluated
     * for sizes small enough for plain 32-bit arithmetic (bigger ones always win). */
    if (g_gfxv_min_fill_bytes != 0u || g_gfxv_min_fill_rows != 0u)
    {
        unsigned int fw = right - left + 1u, fh = top - bottom + 1u;

        if (fw <= 0xFFFFu && fh <= 0xFFFFu && fw * fh <= 0x3FFFFFu)
        {
            unsigned int bytes = (fw * fh) << (log2_bpp - 3u);
            unsigned int tiles = ((fw + 63u) >> 6) * ((fh + 63u) >> 6);

            if (bytes + GFXV_FILL_ROW_WEIGHT * fh < tiles * GFXV_FILL_MIN_BYTES_PER_TILE)
            {
                g_gfxv_fill_skipped_count++;
                g_trace_outcome = TRACE_SKIP_DENSITY;
                return;
            }
        }
    }

    if (!ecf_decode_striped(ecf, log2_bpp, colours))
    {
        g_trace_outcome = TRACE_NOT_SOLID;
        return; /* a real content-dependent blend - not ours to accelerate */
    }

    is_solid = 1;
    for (line = 1; line < 8u; line++)
    {
        if (colours[line] != colours[0])
        {
            is_solid = 0;
            break;
        }
    }

    if (is_solid)
    {
        unsigned int area = (right - left + 1u) * (top - bottom + 1u);

        g_gfxv_solid_count++;

        err = v3d_accel_render_fill(left, bottom, right, top, colours[0]);
        if (err)
        {
            g_gfxv_error_count++;
            g_gfxv_last_fail_errnum = (unsigned int) err->errnum;
            g_gfxv_last_fail_left = left;
            g_gfxv_last_fail_bottom = bottom;
            g_gfxv_last_fail_right = right;
            g_gfxv_last_fail_top = top;
            if (area < g_gfxv_fail_min_area) g_gfxv_fail_min_area = area;
            if (area > g_gfxv_fail_max_area) g_gfxv_fail_max_area = area;
            g_trace_outcome = TRACE_GPU_ERROR;
            g_trace_errnum = (unsigned int) err->errnum;
            return; /* GPU fill failed (e.g. timeout) - leave unclaimed
                      * so the OS's own CPU fallback still draws
                      * correctly; see gfxv_accel.h's top comment on
                      * why silently wrong output is worse than
                      * falling back here. */
        }

        if (area < g_gfxv_success_min_area) g_gfxv_success_min_area = area;
        if (area > g_gfxv_success_max_area) g_gfxv_success_max_area = area;

        g_gfxv_claimed_count++;
        g_trace_outcome = TRACE_CLAIMED;
        r->r[4] = 0; /* claimed - see GraphicsV's own R4=0 convention */
        return;
    }

    g_gfxv_striped_count++;

    /* A dithered fill is up to 8 separate GPU submissions (one per distinct
     * row colour), so it needs a rectangle ~8x bigger to be worth it. */
    {
        unsigned int min_bytes = g_gfxv_min_fill_bytes;

        if (min_bytes > 0x1FFFFFFFu)
            min_bytes = 0x1FFFFFFFu;
        if (!accel_worthwhile(right - left + 1u, top - bottom + 1u, log2_bpp,
                               min_bytes * 8u, g_gfxv_min_fill_rows))
        {
            g_gfxv_fill_skipped_count++;
            g_trace_outcome = TRACE_SKIP_STRIPED;
            return;
        }
    }

    err = v3d_accel_render_striped_fill(left, bottom, right, top, colours);
    if (err)
    {
        g_gfxv_striped_error_count++;
        g_gfxv_striped_last_fail_errnum = (unsigned int) err->errnum;
        g_trace_outcome = TRACE_STRIPED_ERROR;
        g_trace_errnum = (unsigned int) err->errnum;
        return; /* GPU fill failed (e.g. timeout) - leave unclaimed, same
                  * fail-safe reasoning as the solid path above. */
    }

    g_gfxv_striped_claimed_count++;
    g_trace_outcome = TRACE_CLAIMED;
    r->r[4] = 0; /* claimed */
}

static void handle_render_copy(_kernel_swi_regs *r)
{
    const unsigned int *params;
    unsigned int src_left, src_bottom, dest_left, dest_bottom, width, height;
    unsigned int log2_bpp;
    _kernel_oserror *err;

    g_gfxv_copy_op_count++;

    /* GraphicsV 13 1 parameter block: word0=Source left, word1=Source
     * bottom, word2=Dest left, word3=Dest bottom, word4=Width-1,
     * word5=Height-1 - confirmed against the real PRM (see README).
     * Both rectangles share the same width/height (a copy, not a
     * scale). */
    params = (const unsigned int *) r->r[2];
    src_left = params[0];
    src_bottom = params[1];
    dest_left = params[2];
    dest_bottom = params[3];
    width = params[4] + 1u;
    height = params[5] + 1u;

    /* Needed for the size limit's byte count (see the SIZE LIMITS comment
     * above). If the depth can't be determined there is nothing safe to
     * do, so leave the call unclaimed. */
    err = v3d_accel_get_screen_log2_bpp(&log2_bpp);
    if (err)
    {
        note_no_screen(err);
        return;
    }

    /* AABB overlap test: NOT (one rect is entirely to one side of the
     * other on either axis). Checked here, at the protocol layer,
     * before ever calling into module.c - the same check
     * V3D_ScreenCopyTest's SWI handler makes for itself, duplicated
     * rather than shared since the two call sites take differently-
     * shaped inputs (this one decodes straight from GraphicsV's own
     * bottom-up rectangle-plus-size form; the test SWI takes an
     * already-parsed width/height pair). Overlapping rectangles route
     * through v3d_accel_render_copy_overlapping's scratch-buffer pass
     * instead of the direct single-pass copy - see cl_copy.h/.c for
     * why a direct pass isn't safe once source and destination
     * overlap. */
    if (src_left < dest_left + width && dest_left < src_left + width &&
        src_bottom < dest_bottom + height && dest_bottom < src_bottom + height)
    {
        g_gfxv_copy_overlap_count++;
        g_census_scroll = 1u;

        /* Overlapping copies have their own byte limit (now equal to the
         * copy limit: a scroll is one GPU pass since 0.09). */
        if (!accel_worthwhile(width, height, log2_bpp,
                               g_gfxv_min_overlap_bytes, g_gfxv_min_copy_rows))
        {
            g_gfxv_copy_skipped_count++;
            g_trace_outcome = TRACE_SKIP_SMALL;
            return;
        }

        err = v3d_accel_render_copy_overlapping(src_left, src_bottom,
                                                  dest_left, dest_bottom,
                                                  width, height);
        if (err)
        {
            g_gfxv_copy_overlap_error_count++;
            g_gfxv_copy_last_fail_errnum = (unsigned int) err->errnum;
            copy_last_fail_msg(err->errmess);
            g_trace_outcome = TRACE_GPU_ERROR;
            g_trace_errnum = (unsigned int) err->errnum;
            return; /* too large for the scratch-buffer path, or a GPU
                      * failure - leave unclaimed, same fail-safe
                      * reasoning as everywhere else in this driver. */
        }

        g_gfxv_copy_overlap_claimed_count++;
        g_trace_outcome = TRACE_CLAIMED;
        r->r[4] = 0; /* claimed */
        return;
    }

    if (!accel_worthwhile(width, height, log2_bpp,
                           g_gfxv_min_copy_bytes, g_gfxv_min_copy_rows))
    {
        g_gfxv_copy_skipped_count++;
        g_trace_outcome = TRACE_SKIP_SMALL;
        return;
    }

    err = v3d_accel_render_copy(src_left, src_bottom, dest_left, dest_bottom,
                                  width, height);
    if (err)
    {
        g_gfxv_copy_error_count++;
        g_gfxv_copy_last_fail_errnum = (unsigned int) err->errnum;
        copy_last_fail_msg(err->errmess);
        g_trace_outcome = TRACE_GPU_ERROR;
        g_trace_errnum = (unsigned int) err->errnum;
        return; /* GPU copy failed - leave unclaimed, same fail-safe
                  * reasoning as handle_render_fill. */
    }

    g_gfxv_copy_claimed_count++;
    g_trace_outcome = TRACE_CLAIMED;
    r->r[4] = 0; /* claimed */
}

/*
 * CENSUS (0.12 rev f): how much fill and copy work real use actually sends through GraphicsV, by kind and size, and
 * who did it. Every fill and copy Render that is not a re-entry is counted in one of 3 kinds (fill, copy, scroll =
 * overlapping copy) x 10 size buckets (pixel area up to 64, 256, 1K, 4K, 16K, 64K, 256K, 1M, 4M and above), split
 * into "taken by the GPU" and "left to the CPU" (for any reason), with the kilopixels in each. It is never reset by
 * anything but V3D_AccelProfile 10, so a session can be measured. Cost: a few instructions per call.
 */

void v3d_census_reset(void)
{
    unsigned int k, b;

    for (k = 0; k < V3D_CENSUS_KINDS; k++)
        for (b = 0; b < V3D_CENSUS_BUCKETS; b++)
            g_census[k][b].n_gpu = g_census[k][b].n_cpu = g_census[k][b].kpix_gpu = g_census[k][b].kpix_cpu = 0;
    g_census_start_us = v3d_time_us();
}

/* out[0..3] = n_gpu, n_cpu, kpix_gpu, kpix_cpu; out[4] = microseconds since the last reset. */
void v3d_census_read(unsigned int kind, unsigned int bucket, unsigned int *out)
{
    out[4] = v3d_time_us() - g_census_start_us;
    if (kind >= V3D_CENSUS_KINDS || bucket >= V3D_CENSUS_BUCKETS)
    {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = g_census[kind][bucket].n_gpu;
    out[1] = g_census[kind][bucket].n_cpu;
    out[2] = g_census[kind][bucket].kpix_gpu;
    out[3] = g_census[kind][bucket].kpix_cpu;
}

static void census_note(unsigned int op, const _kernel_swi_regs *r, unsigned int outcome)
{
    const unsigned int *p = (const unsigned int *) r->r[2];
    unsigned int w, h, area, thresh = 64u, bucket = 0u, kind;
    census_t *c;

    if (op == RENDER_OP_FILL)
    {
        w = p[2] - p[0] + 1u;
        h = p[1] - p[3] + 1u;
        kind = 0u;
    }
    else
    {
        w = p[4] + 1u;
        h = p[5] + 1u;
        kind = g_census_scroll ? 2u : 1u;
    }
    if (w > 16384u || h > 16384u)
        return; /* nonsense dimensions: never count garbage */
    area = w * h;
    while (area > thresh && bucket < V3D_CENSUS_BUCKETS - 1u)
    {
        thresh <<= 2;
        bucket++;
    }
    c = &g_census[kind][bucket];
    if (outcome == TRACE_CLAIMED)
    {
        c->n_gpu++;
        c->kpix_gpu += (area + 512u) >> 10;
    }
    else
    {
        c->n_cpu++;
        c->kpix_cpu += (area + 512u) >> 10;
    }
}

/* Record one fill/copy Render in the trace (when armed). */
static void trace_render(unsigned int op, const _kernel_swi_regs *r,
                          unsigned int outcome, unsigned int errnum)
{
    const unsigned int *p;

    if (!g_trace_armed)
        return;
    p = (const unsigned int *) r->r[2];
    if (op == RENDER_OP_FILL)
        trace_event(3u, outcome, errnum, p[0], p[3], p[2], p[1], p[4]);
    else
        trace_event(4u, outcome, errnum, p[0], p[1], p[2], p[3],
                    (p[4] & 0xFFFFu) | (p[5] << 16));
}

int v3d_gfxv_handler(_kernel_swi_regs *r, void *pw)
{
    unsigned int reason;
    unsigned int op;

    (void) pw;

    g_gfxv_call_count++;

    /* R4 = (driver << 24) | (head << 16) | reason. This module only ever
     * looks at driver 0 / head 0 (the framestore it queries and renders to
     * is driver 0's), so a call addressed to any other driver or head is
     * not ours. BCMVideo makes the same check; v0.06 masked R4 down to the
     * reason and so would have claimed another driver's Render. */
    if (((unsigned int) r->r[4] >> 16) != 0u)
        return 1;

    reason = ((unsigned int) r->r[4]) & GRAPHICSV_REASON_MASK;
    op = (unsigned int) r->r[1];

    /* Never claim these - just watch them go by, then let the real driver
     * have them (VECTOR_PASSON, R4 untouched). They are how the kernel
     * tells a display driver that the screen has changed under it, and
     * they arrive BEFORE the full-screen clear of a new mode, which is the
     * one call Service_ModeChange (issued afterwards) cannot protect. */
    if (reason == GRAPHICSV_REASON_SETDMAADDRESS)
    {
        trace_event(2u, TRACE_NONE, 0u, (unsigned int) r->r[1], (unsigned int) r->r[0], 0u, 0u, 0u);
        if ((unsigned int) r->r[0] == GVDAG_VRENDER)
            v3d_accel_note_vrender((unsigned int) r->r[1]);
        return 1;
    }
    if (reason == GRAPHICSV_REASON_SETMODE)
    {
        trace_event(1u, TRACE_NONE, 0u, (unsigned int) r->r[0], 0u, 0u, 0u, 0u);
        v3d_accel_mode_changing();
        return 1;
    }

    if (reason != GRAPHICSV_REASON_RENDER)
        return 1; /* not ours - VECTOR_PASSON, R4 untouched */

    g_gfxv_render_count++;

    if (op == RENDER_OP_FILL || op == RENDER_OP_COPY)
    {
        /* Reentrancy guard - see g_gfxv_busy's own top comment. Checked
         * immediately before touching any shared scratch state, not
         * before the call/render counters above, so those stay accurate
         * diagnostics regardless of whether this guard ever trips. */
        if (g_gfxv_busy)
        {
            g_gfxv_reentrant_count++;
            trace_render(op, r, TRACE_REENTERED, 0u);
            return 1; /* leave unclaimed - do NOT touch shared PMP
                        * scratch state while a call is already in
                        * flight; R4 untouched */
        }

        g_gfxv_busy = 1;
        g_trace_outcome = TRACE_NONE;
        g_trace_errnum = 0;
        g_census_scroll = 0u;
        if (op == RENDER_OP_FILL)
            handle_render_fill(r);
        else
            handle_render_copy(r);
        g_gfxv_busy = 0;
        census_note(op, r, g_trace_outcome);
        trace_render(op, r, g_trace_outcome, g_trace_errnum);
    }
    /* op 0 (no-op) and any other value: not ours, R4 left untouched */

    return 1; /* VECTOR_PASSON regardless - see gfxv_accel.h */
}
