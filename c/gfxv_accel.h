/*
 * gfxv_accel.h - step 4 of the planned GraphicsV Render accelerator
 * (see README.md's "GraphicsV Render accelerator" section): claim
 * GraphicsV (vector 42) and intercept reason code 13 (Render), op 2
 * (rectangle fill) only, for a genuine solid-colour ECF pattern only -
 * everything else is left unclaimed (R4 untouched) so it falls through
 * to whatever else is on the chain (the kernel's own HAL-bridge
 * claimant, confirmed to exist by the earlier GraphicsV/HAL Video API
 * research - see README).
 *
 * Deliberately NOT auto-claimed on module load - see V3D_AccelStart/
 * V3D_AccelStop below. A silently wrong accelerated fill corrupts
 * visible desktop state, which is a worse failure mode than a crash;
 * this stays opt-in until proven reliable under real desktop use.
 *
 * ECF (Extended Colour Fill) block format - confirmed against the real
 * PRM (riscosopen.org/wiki/documentation/show/ECF%20Block, fetched
 * 2026-09-26), NOT the "8 bytes, one per pattern row" guess a forum
 * thread suggested (that's the OLD, pre-32bpp-era VIDC1 format - wrong
 * for this hardware): 8 lines x (OR mask, EOR mask), 64 bytes total,
 * 64-byte aligned. Each output pixel is computed as
 * `(existing_pixel OR or_mask) EOR eor_mask`. The classic idiom for a
 * genuine SOLID fill (independent of whatever pixel was already there)
 * is or_mask=0xFFFFFFFF (forces every bit to 1 regardless of the
 * existing pixel) + eor_mask=~colour (flips down to the exact desired
 * value) - `colour = ~eor_mask` follows algebraically. Detected here
 * by checking every one of the 8 lines has or_mask==0xFFFFFFFF and an
 * IDENTICAL eor_mask (a genuine dithered/patterned fill would vary
 * eor_mask per line) - anything else is left unclaimed, not guessed at.
 */
#ifndef GFXV_ACCEL_H
#define GFXV_ACCEL_H

#include "kernel.h"

void v3d_gfxv_set_private_word(void *pw);

/* Real invocation counters, not guesswork - see gfxv_accel.c's own
 * comment. Exposed for V3D_AccelStats. */
extern volatile unsigned int g_gfxv_call_count;
extern volatile unsigned int g_gfxv_render_count;
extern volatile unsigned int g_gfxv_fill_op_count;
extern volatile unsigned int g_gfxv_solid_count;
extern volatile unsigned int g_gfxv_claimed_count;
extern volatile unsigned int g_gfxv_error_count;

/*
 * OS_Claim/OS_Release on vector 42 (GraphicsV), using this module's
 * own private word - same veneer mechanism already proven correct for
 * the hardware IRQ vector (v3d_irq_claim), see cmhg's vector-handlers
 * directive for v3d_gfxv_entry/v3d_gfxv_handler.
 *
 * Idempotent, mirroring v3d_irq_ensure_ready/v3d_irq_shutdown's own
 * pattern: v3d_gfxv_start() claims exactly once no matter how many
 * times called (the explicit opt-in - see gfxv_accel.h's top comment),
 * v3d_gfxv_stop() releases exactly once, and v3d_gfxv_shutdown() is a
 * safe no-op if never started, for module_final to call unconditionally.
 */
_kernel_oserror *v3d_gfxv_start(void);
_kernel_oserror *v3d_gfxv_stop(void);
_kernel_oserror *v3d_gfxv_shutdown(void);

/*
 * The vector handler itself - always returns 1 (VECTOR_PASSON, i.e.
 * "let the chain continue"), NEVER 0. Claiming is signalled purely via
 * R4=0, per GraphicsV's own documented protocol - deliberately not
 * using the generic vector-chain-stopping mechanism at all, since a
 * well-behaved further claimant (the kernel's own HAL bridge) already
 * checks R4 itself and does nothing if it's already 0. Always passing
 * on also means any reason code or op this driver doesn't recognise
 * flows through completely unaffected, as if this claimant weren't
 * even there.
 */
int v3d_gfxv_handler(_kernel_swi_regs *r, void *pw);

#endif /* GFXV_ACCEL_H */
