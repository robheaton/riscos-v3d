/*
 * gfxv_accel.c - see gfxv_accel.h.
 */

#include <stddef.h>
#include "gfxv_accel.h"
#include "V3DDriver.h" /* for v3d_gfxv_entry - the cmunge-generated
                         * veneer symbol from cmhg's vector-handlers
                         * directive; only its address is ever used,
                         * never called directly - same pattern as
                         * v3d_irq.c's own v3d_irq_entry usage. */

#define SWI_OS_Claim   0x1Fu
#define SWI_OS_Release 0x20u
#define VECTOR_GRAPHICSV 42u

#define GRAPHICSV_REASON_MASK 0xFFFFu
#define GRAPHICSV_REASON_RENDER 13u

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

/* Same idea, for op 1 (copy) - see module.c's own comment on
 * v3d_accel_render_copy. Non-overlapping only (see cl_copy.h) -
 * v3d_gfxv_handler checks for overlap itself before ever calling this,
 * since that check needs no module.c-private state and belongs at the
 * protocol-decoding layer. */
extern _kernel_oserror *v3d_accel_render_copy(unsigned int src_left, unsigned int src_bottom,
                                                unsigned int dest_left, unsigned int dest_bottom,
                                                unsigned int width, unsigned int height);

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
 * Checks the 64-byte ECF block at `ecf` for the "genuine solid fill"
 * pattern - see gfxv_accel.h's top comment for the OR=0xFFFFFFFF /
 * constant-EOR derivation. Returns 1 and sets *colour_out if so, 0
 * otherwise (a real dithered/patterned fill, or something this driver
 * doesn't recognise - either way, not ours to accelerate).
 */
static int ecf_decode_solid(const unsigned int *ecf, unsigned int *colour_out)
{
    unsigned int line;
    unsigned int eor0 = ecf[1];

    for (line = 0; line < 8u; line++)
    {
        unsigned int or_mask = ecf[line * 2u];
        unsigned int eor_mask = ecf[line * 2u + 1u];

        if (or_mask != 0xFFFFFFFFu || eor_mask != eor0)
            return 0;
    }

    *colour_out = ~eor0;
    return 1;
}

static void handle_render_fill(_kernel_swi_regs *r)
{
    const unsigned int *params;
    unsigned int left, bottom, right, top;
    const unsigned int *ecf;
    unsigned int colour;
    _kernel_oserror *err;

    g_gfxv_fill_op_count++;

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

    if (!ecf_decode_solid(ecf, &colour))
        return; /* a real pattern/dither fill - not ours to accelerate */

    g_gfxv_solid_count++;

    {
        unsigned int area = (right - left + 1u) * (top - bottom + 1u);

        err = v3d_accel_render_fill(left, bottom, right, top, colour);
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
            return; /* GPU fill failed (e.g. timeout) - leave unclaimed
                      * so the OS's own CPU fallback still draws
                      * correctly; see gfxv_accel.h's top comment on
                      * why silently wrong output is worse than
                      * falling back here. */
        }

        if (area < g_gfxv_success_min_area) g_gfxv_success_min_area = area;
        if (area > g_gfxv_success_max_area) g_gfxv_success_max_area = area;
    }

    g_gfxv_claimed_count++;
    r->r[4] = 0; /* claimed - see GraphicsV's own R4=0 convention */
}

static void handle_render_copy(_kernel_swi_regs *r)
{
    const unsigned int *params;
    unsigned int src_left, src_bottom, dest_left, dest_bottom, width, height;
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

    /* Non-overlapping only for now (see cl_copy.h). AABB overlap test:
     * NOT (one rect is entirely to one side of the other on either
     * axis). Checked here, at the protocol layer, before ever calling
     * into module.c - the same check V3D_ScreenCopyTest's SWI handler
     * makes for itself, duplicated rather than shared since the two
     * call sites take differently-shaped inputs (this one decodes
     * straight from GraphicsV's own bottom-up rectangle-plus-size
     * form; the test SWI takes an already-parsed width/height pair). */
    if (src_left < dest_left + width && dest_left < src_left + width &&
        src_bottom < dest_bottom + height && dest_bottom < src_bottom + height)
    {
        g_gfxv_copy_overlap_count++;
        return; /* overlapping - not ours to accelerate yet */
    }

    err = v3d_accel_render_copy(src_left, src_bottom, dest_left, dest_bottom,
                                  width, height);
    if (err)
    {
        g_gfxv_copy_error_count++;
        g_gfxv_copy_last_fail_errnum = (unsigned int) err->errnum;
        return; /* GPU copy failed - leave unclaimed, same fail-safe
                  * reasoning as handle_render_fill. */
    }

    g_gfxv_copy_claimed_count++;
    r->r[4] = 0; /* claimed */
}

int v3d_gfxv_handler(_kernel_swi_regs *r, void *pw)
{
    unsigned int reason;
    unsigned int op;

    (void) pw;

    g_gfxv_call_count++;

    reason = ((unsigned int) r->r[4]) & GRAPHICSV_REASON_MASK;
    if (reason != GRAPHICSV_REASON_RENDER)
        return 1; /* not ours - VECTOR_PASSON, R4 untouched */

    g_gfxv_render_count++;

    op = (unsigned int) r->r[1];
    if (op == RENDER_OP_FILL)
        handle_render_fill(r);
    else if (op == RENDER_OP_COPY)
        handle_render_copy(r);
    /* op 0 (no-op) and any other value: not ours, R4 left untouched */

    return 1; /* VECTOR_PASSON regardless - see gfxv_accel.h */
}
