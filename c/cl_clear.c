/*
 * cl_clear.c - see cl_clear.h.
 *
 * Every packet's byte layout below is transcribed from the MIT-licensed
 * bare-metal reference's C++ struct definitions (src/device/v3d/v3d_cl.hpp,
 * github.com/Random06457/rpi4-gpu-bare-metal-examples), NOT reproduced by
 * relying on our own compiler's bitfield layout (see cl_builder.h for why).
 * Each field's bit width/order/position was hand-derived from that source
 * and cross-checked against its own static_assert(sizeof(...)==N) - the
 * comment above each emit_* function shows that derivation so it can be
 * re-checked without re-deriving from scratch.
 *
 * Register submission sequence (CT0QMA/QMS/QTS, CT0/CT1 QBA/QEA, the L2T
 * cache-flush dance) is transcribed the same way from that reference's
 * src/device/v3d/v3d_qpu.cpp (executeBin/executeRender).
 *
 * UPDATE (2026-09-20, after first hardware test): a BCL with just
 * TileBinningModeCfg + OP_START_TILE_BINNING + OP_FLUSH and nothing
 * else left V3D_CLE_BFC never advancing - binning never completed.
 * Confirms the risk flagged in the original version of this comment.
 * Fix: reproduced the reference's full always-present viewport/clip/
 * raster state-setup block (ClipWindow through VcmCacheSize) verbatim,
 * values and all, minus only GlShaderState/VertexArrayPrims since we
 * draw nothing.
 *
 * UPDATE (2026-09-24): that fixed binning - V3D_CTL_INT_STS read back
 * V3D_INT_FLDONE (bin done) on a later hardware test, and BFC advanced
 * correctly. Rendering then hung instead: CT1CA stuck at 0x00000000
 * with CT1CS showing CTRTSD=1 (one level into a nested sub-list) and
 * CTRUN=1 (genuinely running, not stalled/errored). The render thread
 * had followed a branch into memory we never set up: our RCL called
 * SupertileCoordinates (which the reference always pairs with a prior
 * StartAddressOfGenericTileList, pointing at a small "indirect" list
 * of its own) without ever writing that indirect-list address
 * ourselves - so the branch target was left at its zero default.
 * Removed the SupertileCoordinates call entirely: both TileCoordinates
 * blocks above it already do our real work (clear + store) inline, so
 * we have no supertiles to walk and don't need the generic-tile-list
 * indirection at all for a single hardcoded tile. Not yet confirmed on
 * hardware.
 */

#include "cl_clear.h"
#include "cl_builder.h"
#include "cl_packets.h"
#include "v3d_regs.h"
#include "io_map.h"
#include "beacon.h"

/* Reload-freeze round 8: in the diagnostic build the legacy submission can be made to look like the accelerated
 * one, chosen at load time (words LPERM / LDSB, see beacon.h): LPERM = use the permanent register mapping instead
 * of the per-job OS_Memory 14/15 temporary window; LDSB = one DSB before the doorbell writes, as v3d_job_run does. */
#ifdef V3D_BEACONS
#define LEG_MAP(m)      (g_v3d_legacy_perm ? v3d_regs_map(m) : io_map(V3D_MB_BASE, (m)))
#define LEG_UNMAP(m)    do { if (g_v3d_legacy_perm) v3d_regs_unmap(m); else io_unmap(m); } while (0)
#define LEG_PRE_KICK()  do { if (g_v3d_legacy_dsb) __asm__ __volatile__(".word 0xF57FF04F" ::: "memory"); } while (0) /* DSB SY, as V3D_DSB */
#else
#define LEG_MAP(m)      io_map(V3D_MB_BASE, (m))
#define LEG_UNMAP(m)    io_unmap(m)
#define LEG_PRE_KICK()  ((void) 0)
#endif
#include "sprite.h"
#include "v3d_irq.h"

#define CL_PAGE_COUNT           1u  /* BCL+RCL combined, both tiny */
#define RENDER_TARGET_PAGE_COUNT 4u /* 64*64*4 bytes = 16384 = 4 pages */
#define TILE_ALLOC_PAGE_COUNT    3u /* align(1*1*1*64,4096) + 64*64*2, per
                                      * the reference's own sizing formula */
#define TILE_STATE_PAGE_COUNT    1u /* 256 bytes/tile (V3D>=40 - we're 4.2) */
#define CLEAR_TEST_TOTAL_PAGES  (CL_PAGE_COUNT + RENDER_TARGET_PAGE_COUNT + \
                                  TILE_ALLOC_PAGE_COUNT + TILE_STATE_PAGE_COUNT)

/* M3: same render target, prefixed with a sprite area+header (60
 * bytes - SPRITE_HEADER_BYTES) so the GPU's own output buffer is
 * directly usable as a RISC OS sprite, no copy needed. 64*64*4 +
 * 60 = 16444 bytes, rounded up to whole pages. */
#define SPRITE_RENDER_PAGE_COUNT \
    (((CL_CLEAR_TEST_WIDTH * CL_CLEAR_TEST_HEIGHT * 4u) + SPRITE_HEADER_BYTES \
      + PAGE_SIZE - 1u) / PAGE_SIZE)

/*
 * Packet emitters (TileBinningModeCfg, ClipWindow, StoreTileBufferRT0,
 * etc.) now live in cl_packets.h, shared with cl_fill.c's multi-tile
 * generalisation - moved out of this file verbatim (2026-09-26), no
 * byte-level change to what gets emitted below.
 */

/*
 * REMOVED (2026-09-26), after four increasingly-precise attempts (v1
 * fixed-delay, v2 CS-gated, v3 CA-gated, v4 reordered) all failed
 * identically on hardware: a CTSUBS-based "restart the thread" kick
 * before submitting a new job to CT0/CT1.
 *
 * The whole premise was wrong. All four versions derived CTSUBS's
 * semantics from Broadcom's 2013 "VideoCore IV 3D Architecture
 * Reference Guide" - but that document is for an *older* V3D
 * generation that submits jobs by directly writing CTnCA/CTnEA (its
 * CTSUBS/CTRUN bit layout is real, but its submission model is not
 * what this hardware uses). The Pi 4/CM4 (V3D 4.2) is a later
 * generation with a genuinely different queue-based submission model:
 * separate V3D_CTnQBA/QEA/QMA/QMS registers exist specifically so a
 * new job's addresses can be queued up, with CTnCA/CTnEA left as
 * plain progress-monitoring registers the hardware updates itself.
 * Confirmed by reading the actual, shipping Linux kernel v3d driver
 * for this same hardware (drivers/gpu/drm/v3d/v3d_sched.c,
 * v3d_bin_job_run/v3d_render_job_run, fetched from
 * raw.githubusercontent.com/torvalds/linux 2026-09-26): it submits
 * every single job - the first ever, the thousandth, no exceptions -
 * with nothing more than QMA/QMS/QTS (if present) then QBA then QEA.
 * No CS read, no CTSUBS, no state check of any kind. Its own comment:
 * "Writing the end register is what starts the job."
 *
 * v3d_clear_run now does exactly that - see its QBA/QEA writes below,
 * with no kick call before or after them.
 */

/*
 * Waits for a CLE frame counter (V3D_CLE_BFC/RFC) to advance past
 * `initial`, polling in short bursts rather than holding one long-lived
 * OS_Memory 14 mapping. That matters here, not just as a style choice:
 * the PRM is explicit that OS_Memory 14 hands out a single, system-wide
 * shared mapping window, and warns that holding it across a long
 * operation risks an interrupt handler or another driver's own
 * temporary mapping silently stealing it from under us - after which
 * every further read in this loop would be looking at some other
 * device's registers, not V3D's, and would spin until the outer
 * timeout regardless of what the GPU actually did. Re-mapping fresh
 * every `burst` iterations keeps any single hold on the shared window
 * short. Same total iteration ceiling as the original single-mapping
 * version (500 * 10000 = 5,000,000), just no longer in one unbroken
 * hold.
 */
/* Must match _kernel_oserror.errmess's declared size (kernel.h: char
 * errmess[252]). Hard-capped here rather than just relying on the
 * call sites keeping the message short - a first version of this
 * diagnostic added enough fields that its worst-case length (291
 * bytes, computed after the fact) would have overflowed a 252-byte
 * buffer in a kernel module. That's not a cosmetic bug. Every append
 * below is bounds-checked against this regardless of how the message
 * is edited in future. */
#define ERRMESS_CAPACITY 252u

static void append_str(char *dst, unsigned int *pos, const char *s)
{
    while (*s && *pos < ERRMESS_CAPACITY - 1u)
        dst[(*pos)++] = *s++;
}

static void append_hex32(char *dst, unsigned int *pos, unsigned int v)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    if (*pos + 10u >= ERRMESS_CAPACITY)
        return; /* not enough room left for "0x" + 8 hex digits */
    dst[(*pos)++] = '0';
    dst[(*pos)++] = 'x';
    for (i = 28; i >= 0; i -= 4)
        dst[(*pos)++] = hex[(v >> i) & 0xFu];
}

static void append_dec(char *dst, unsigned int *pos, unsigned int v)
{
    char tmp[11];
    unsigned int n = 0;

    do { tmp[n++] = (char) ('0' + v % 10u); v /= 10u; } while (v && n < sizeof tmp);
    while (n && *pos < ERRMESS_CAPACITY - 1u)
        dst[(*pos)++] = tmp[--n];
}

/*
 * On timeout, reads V3D_CTL_INT_STS and decodes it into out_err's
 * message before returning it - a raw "timed out" is a dead end to
 * debug further; this at least says whether the hardware itself
 * reports anything (in particular V3D_INT_OUTOMEM - see the register's
 * comment in v3d_regs.h for why a stalled-on-OOM binner would look
 * identical to a never-started one from BFC's perspective alone).
 *
 * Also attempts recovery, not just diagnosis: submission itself (see
 * v3d_clear_run) never proactively restarts anything - it just writes
 * QMA/QMS/QTS/QBA/QEA, matching the real V3D driver (see the removed-
 * code comment further up). But CTRUN just means "running" per the
 * PRM - it doesn't distinguish "genuinely making progress" from "hung
 * mid-job", and a timed-out poll is exactly the case where a thread
 * reporting CTRUN=1 is almost certainly the latter, with nothing else
 * in this driver ever touching it again. That fits everything logged in
 * "attempt 7"/"attempt 8" far better than an address-range theory
 * does: BCMSupport's real ARM/VC split (queried in attempt 8) sits at
 * 0x3C000000 (960MB) but every failing CA/EA/QBA/QEA on record has
 * been ~0x03000000-0x030C0000 (~48-50MB) - nowhere near it. What those
 * attempts actually had in common is that ClearToSprite had already
 * been run (and presumably wedged the thread) earlier in the same
 * session, and every attempt after that point failed identically
 * regardless of address, including a previously-100%-reliable
 * ClearTest. So: if CTRUN is set here, request a stop the documented
 * way (V3D_CT_CS_CTRUN is R/W; per its own comment in v3d_regs.h,
 * writing 1 stops the thread rather than starting one) and poll
 * briefly for it to actually leave the running state, so the *next*
 * submission's restart_thread() finds a genuinely stopped thread
 * (CTSUBS/CTERR set) instead of a permanently wedged CTRUN=1 it will
 * never touch. Bounded and best-effort: if the stop request itself
 * doesn't take, the caller still gets the timeout error, just with
 * that outcome recorded in the message instead of silently retried.
 */
/*
 * `bufs` lets us check CA/EA against every physical buffer this job
 * actually used, not just what we wrote to QBA/QEA - added after EA
 * read back a value that didn't match either of those, and matching
 * it to a buffer address by hand (page-layout arithmetic, not a
 * logged value) turned out to be exactly the kind of guess-disguised-
 * as-a-fact this project has been trying to avoid.
 */
typedef struct diag_bufs
{
    unsigned int cl, render_target, tile_alloc, tile_state;
    unsigned int bfc0, rfc0; /* BFC/RFC read just before the kick, for the timeout message */
} diag_bufs_t;

static void diagnose_timeout(_kernel_oserror *out_err, const char *what,
                              int is_bin, unsigned int qba, unsigned int qea,
                              const diag_bufs_t *bufs)
{
    io_mapping_t v3d;
    unsigned int sts = 0, cs = 0, ca = 0, ea = 0;
    unsigned int msk = 0, hmsk = 0, hsts2 = 0, bfc_now = 0, rfc_now = 0;
    unsigned int mmu_ctl = 0, hub_sts = 0, vio_id = 0, vio_addr = 0;
    unsigned int asb_m = 0, asb_s = 0;
    unsigned int pos = 0;
    int recover_attempted = 0, recover_ok = 0;

    if (!io_map(V3D_MB_BASE, &v3d))
    {
        sts = IO_WORD(v3d, V3D_CTL_INT_STS);
        msk = IO_WORD(v3d, V3D_CTL_INT_MSK_STS);
        hmsk = IO_WORD(v3d, V3D_HUB_INT_MSK_STS);
        bfc_now = IO_WORD(v3d, V3D_CLE_BFC);
        rfc_now = IO_WORD(v3d, V3D_CLE_RFC);
        mmu_ctl = IO_WORD(v3d, V3D_MMU_CTL);
        hub_sts = IO_WORD(v3d, V3D_HUB_INT_STS);
        hsts2 = hub_sts;
        /* Added 2026-09-28: first real reload-freeze repro that hit this
         * bounded timeout instead of a hard freeze showed CT0CA/CT0EA/
         * CT0QBA/CT0QEA all read 0xFFFFFFFF while CS/STS/MMU_CTL read a
         * clean 0 in the same io_map session - only the CLE's address/
         * queue registers looked unclocked, not the whole block.
         * v3d_ensure_clock_on (module.c) only clears this ASB gate when
         * the firmware's OWN clock-already-on check is false, so a
         * second/reload PowerOn skips the clear even if the gate itself
         * has independently ended up re-stopped. ASB_REQ_STOP set here
         * would confirm that theory directly. */
        asb_m = IO_WORD(v3d, ASB_V3D_M_CTRL);
        asb_s = IO_WORD(v3d, ASB_V3D_S_CTRL);
        if (mmu_ctl & V3D_MMU_CTL_ENABLE)
        {
            vio_id = IO_WORD(v3d, V3D_MMU_VIO_ID);
            vio_addr = IO_WORD(v3d, V3D_MMU_VIO_ADDR);
        }
        if (is_bin)
        {
            cs = IO_WORD(v3d, V3D_CLE_CT0CS);
            ca = IO_WORD(v3d, V3D_CLE_CT0CA);
            ea = IO_WORD(v3d, V3D_CLE_CT0EA);
        }
        else
        {
            cs = IO_WORD(v3d, V3D_CLE_CT1CS);
            ca = IO_WORD(v3d, V3D_CLE_CT1CA);
            ea = IO_WORD(v3d, V3D_CLE_CT1EA);
        }

        if (cs & V3D_CT_CS_CTRUN)
        {
            unsigned int cs_reg = is_bin ? V3D_CLE_CT0CS : V3D_CLE_CT1CS;
            unsigned int spin;

            recover_attempted = 1;
            IO_WORD(v3d, cs_reg) = V3D_CT_CS_CTRUN; /* request stop - see comment above */
            for (spin = 0; spin < 100000u; spin++)
            {
                if (!(IO_WORD(v3d, cs_reg) & V3D_CT_CS_CTRUN))
                {
                    recover_ok = 1;
                    break;
                }
            }
        }

        io_unmap(&v3d);
    }

    append_str(out_err->errmess, &pos, "V3DDriver: timeout: ");
    append_str(out_err->errmess, &pos, what);
    append_str(out_err->errmess, &pos, " CS=");
    append_hex32(out_err->errmess, &pos, cs);
    append_str(out_err->errmess, &pos, (cs & V3D_CT_CS_CTRUN) ? "(RUN" : "(STOP");
    if (cs & V3D_CT_CS_CTERR) append_str(out_err->errmess, &pos, " ERR");
    append_str(out_err->errmess, &pos, ")");
    if (recover_attempted)
        append_str(out_err->errmess, &pos, recover_ok ? " REC=stopped" : " REC=timeout");
    /* Compact fixed-width "(XY)" form, not "(clear)"/"(STOP+ACK)" -
     * the first cut of this added ~50 bytes across both fields and
     * pushed a realistic (non-MMU) message to 258 bytes, past
     * ERRMESS_CAPACITY, silently truncating tSt= off the end (caught
     * on the very first real hardware hit of this exact code, 2026-09-
     * 28). X = 'S' if ASB_REQ_STOP set else '-', Y = 'A' if ASB_ACK
     * set else '-'. */
    append_str(out_err->errmess, &pos, " ASB=");
    append_str(out_err->errmess, &pos, (asb_m & ASB_REQ_STOP) ? "(S" : "(-");
    append_str(out_err->errmess, &pos, (asb_m & ASB_ACK) ? "A)" : "-)");
    append_str(out_err->errmess, &pos, (asb_s & ASB_REQ_STOP) ? "(S" : "(-");
    append_str(out_err->errmess, &pos, (asb_s & ASB_ACK) ? "A)" : "-)");
    append_str(out_err->errmess, &pos, " STS=");
    append_hex32(out_err->errmess, &pos, sts);
    if (sts & V3D_INT_OUTOMEM) append_str(out_err->errmess, &pos, "(OOM)");
    if (sts & V3D_INT_GMPV) append_str(out_err->errmess, &pos, "(GMPV)");
    /* Added for the reload-freeze round 8: did the HARDWARE finish the job (frame counter moved) while the
     * interrupt was never counted? MSK/HMK = which interrupts were enabled, HST = hub status. */
    append_str(out_err->errmess, &pos, " MSK=");
    append_hex32(out_err->errmess, &pos, msk);
    append_str(out_err->errmess, &pos, " HST=");
    append_hex32(out_err->errmess, &pos, hsts2);
    append_str(out_err->errmess, &pos, " HMK=");
    append_hex32(out_err->errmess, &pos, hmsk);
    append_str(out_err->errmess, &pos, " BFC=");
    append_dec(out_err->errmess, &pos, bufs->bfc0);
    append_str(out_err->errmess, &pos, ">");
    append_dec(out_err->errmess, &pos, bfc_now);
    append_str(out_err->errmess, &pos, " RFC=");
    append_dec(out_err->errmess, &pos, bufs->rfc0);
    append_str(out_err->errmess, &pos, ">");
    append_dec(out_err->errmess, &pos, rfc_now);
    append_str(out_err->errmess, &pos, " MMU=");
    append_dec(out_err->errmess, &pos, mmu_ctl & 1u);
    if (mmu_ctl & V3D_MMU_CTL_ENABLE)
    {
        append_str(out_err->errmess, &pos, " HUBSTS=");
        append_hex32(out_err->errmess, &pos, hub_sts);
        append_str(out_err->errmess, &pos, " VioID=");
        append_hex32(out_err->errmess, &pos, vio_id);
        append_str(out_err->errmess, &pos, " VioAddr=");
        append_hex32(out_err->errmess, &pos, vio_addr);
    }
    append_str(out_err->errmess, &pos, " CA=");
    append_hex32(out_err->errmess, &pos, ca);
    append_str(out_err->errmess, &pos, " EA=");
    append_hex32(out_err->errmess, &pos, ea);
    append_str(out_err->errmess, &pos, " QBA=");
    append_hex32(out_err->errmess, &pos, qba);
    append_str(out_err->errmess, &pos, " QEA=");
    append_hex32(out_err->errmess, &pos, qea);
    /* render_target dropped from this message (2026-09-28, freed room
     * for ASBM/ASBS above) - it's the render phase's own buffer, the
     * least relevant of the three to a BIN-phase timeout specifically;
     * still in diag_bufs_t and available to a future render-phase
     * diagnostic if needed. */
    out_err->errmess[pos] = '\0';
}

/*
 * Waits for the real interrupt handler (v3d_irq.c) to advance its own
 * bin/render-done counter past the value captured before submission.
 *
 * This used to also poll V3D_CLE_BFC/RFC directly as a hybrid fallback
 * (either signal satisfying it), deliberately, while the interrupt
 * path was still unproven - a subtly-wrong interrupt couldn't regress
 * reliability that way, it would just silently fall back to register
 * polling. Hardware-confirmed reliable under sustained repetition
 * (2026-09-26: StressTest.bas, 50/50 IRQ-driven completions on both
 * V3D_ClearTest and V3D_ClearToSprite, zero fallback) - the register
 * half added real complexity (a second, always-open V3D_MB_BASE
 * mapping inside this loop) for a safety net that never triggered
 * once trust was established, so it's gone now, not kept "just in
 * case": the interrupt path is what this driver actually relies on.
 */
static _kernel_oserror *v3d_poll_counter(volatile unsigned int *irq_counter,
                                          unsigned int irq_initial,
                                          _kernel_oserror *timeout_err,
                                          const char *what, int is_bin,
                                          unsigned int qba, unsigned int qea,
                                          const diag_bufs_t *bufs)
{
    unsigned int spin;

    for (spin = 0; spin < 5000000u; spin++)
    {
        if (*irq_counter != irq_initial)
            return NULL;
    }

    diagnose_timeout(timeout_err, what, is_bin, qba, qea, bufs);
    return timeout_err;
}

/*
 * CACHED SCRATCH ALLOCATION - claims each of the four scratch buffers
 * (cl/render_target/tile_alloc/tile_state) ONCE per distinct
 * `base_page`, then reuses them on every subsequent call, instead of
 * calling `pmp_alloc` fresh every time. This is a deliberate, narrow
 * exception to this file's own "hardware-proven M2/M3 path, don't
 * touch it" convention (see this file's top comment) - justified by
 * real evidence, not a style preference: `pmp_alloc`'s claim step
 * (OS_DynamicArea reason 21) unconditionally broadcasts
 * `Service_MemoryMoved`, a genuinely synchronous kernel service call
 * reaching into the Wimp module's own real bookkeeping handler for it
 * - confirmed to be the root cause of a hard machine freeze in the
 * GraphicsV-routed fill/copy paths (see cl_fill.c's own identical
 * mechanism and README's "Task Manager freeze" section) when this
 * broadcast happens from inside a live Wimp-dispatched call. This
 * file's own v3d_clear_run had the exact same four-calls-per-job
 * `pmp_alloc` pattern, unpatched until now - worth fixing given the
 * same evidence applies, and while investigating a SEPARATE, still-
 * unresolved freeze (module reload without an intervening reboot) that
 * happens to run through this exact code path.
 *
 * Unlike cl_fill.c/cl_copy.c, no "worst-case" sizing logic is needed
 * here: every one of this function's buffer sizes is a fixed compile-
 * time constant (CL_PAGE_COUNT, RENDER_TARGET_PAGE_COUNT or
 * SPRITE_RENDER_PAGE_COUNT depending on caller, TILE_ALLOC_PAGE_COUNT,
 * TILE_STATE_PAGE_COUNT - this file only ever does one fixed-size
 * 64x64 single-tile clear), so whatever size is requested the first
 * time at a given base_page is simply cached directly - defensively
 * falling back to an uncached pmp_alloc if a later call at the same
 * base_page ever asks for something bigger, exactly like the fill/copy
 * caches do, even though that should never actually happen given this
 * file's own fixed call pattern.
 */
#define MAX_CACHED_CLEAR_SCRATCH 2u

typedef struct
{
    unsigned int base_page;
    unsigned int cl_phys;           void *cl_log;
    unsigned int render_phys;       void *render_log;       unsigned int render_pages;
    unsigned int tile_alloc_phys;   void *tile_alloc_log;
    unsigned int tile_state_phys;   void *tile_state_log;
} clear_scratch_cache_t;

static clear_scratch_cache_t g_clear_scratch_cache[MAX_CACHED_CLEAR_SCRATCH];
static unsigned int g_clear_scratch_cache_count = 0;

static clear_scratch_cache_t *find_or_create_clear_scratch(pmp_area_t *pmp, unsigned int base_page,
                                                              unsigned int render_page_count,
                                                              _kernel_oserror **err_out)
{
    unsigned int i;
    clear_scratch_cache_t *slot;
    unsigned int page;

    *err_out = NULL;

    for (i = 0; i < g_clear_scratch_cache_count; i++)
        if (g_clear_scratch_cache[i].base_page == base_page)
            return &g_clear_scratch_cache[i];

    if (g_clear_scratch_cache_count >= MAX_CACHED_CLEAR_SCRATCH)
        return NULL;

    slot = &g_clear_scratch_cache[g_clear_scratch_cache_count];
    page = base_page;

    *err_out = pmp_alloc(pmp, page, CL_PAGE_COUNT, &slot->cl_phys, &slot->cl_log);
    if (*err_out) return NULL;
    page += CL_PAGE_COUNT;

    slot->render_pages = render_page_count;
    *err_out = pmp_alloc(pmp, page, render_page_count, &slot->render_phys, &slot->render_log);
    if (*err_out) return NULL;
    page += render_page_count;

    *err_out = pmp_alloc(pmp, page, TILE_ALLOC_PAGE_COUNT, &slot->tile_alloc_phys, &slot->tile_alloc_log);
    if (*err_out) return NULL;
    page += TILE_ALLOC_PAGE_COUNT;

    *err_out = pmp_alloc(pmp, page, TILE_STATE_PAGE_COUNT, &slot->tile_state_phys, &slot->tile_state_log);
    if (*err_out) return NULL;

    slot->base_page = base_page;
    g_clear_scratch_cache_count++;
    return slot;
}

/*
 * Shared by v3d_clear_test and v3d_clear_to_sprite. `render_page_count`
 * sizes the render-target buffer (callers needing extra room before
 * the pixel data - e.g. a sprite header - pass more pages);
 * `render_header_bytes` is how far into that buffer the GPU's actual
 * pixel store target starts, letting a caller prefix the buffer with
 * its own data (written through *render_log_out, which always points
 * at the very start of the buffer, not the pixel data) without it
 * being clobbered by the GPU.
 */
static _kernel_oserror *v3d_clear_run(pmp_area_t *pmp, unsigned int base_page,
                                       unsigned int clear_color_rgba,
                                       unsigned int render_page_count,
                                       unsigned int render_header_bytes,
                                       unsigned int *render_target_phys_out,
                                       void **render_target_log_out,
                                       unsigned int *readback_pixel_out)
{
    unsigned int cl_phys, render_phys, tile_alloc_phys, tile_state_phys;
    void *cl_log, *render_log, *tile_alloc_log, *tile_state_log;
    _kernel_oserror *err;
    cl_buf_t bcl, rcl;
    unsigned int bcl_start, bcl_end, rcl_start, rcl_end;
    unsigned int pixel_phys;
    io_mapping_t v3d;
    unsigned int last_bin_irq, last_render_irq;
    unsigned int page = base_page;
    diag_bufs_t diag_bufs;

    v3d_beacon(B_CT_ENTER);
    {
        clear_scratch_cache_t *cache = find_or_create_clear_scratch(pmp, base_page,
                                                                       render_page_count, &err);
        if (err) return err;

        if (cache)
        {
            cl_phys = cache->cl_phys; cl_log = cache->cl_log;
            render_phys = cache->render_phys; render_log = cache->render_log;
            tile_alloc_phys = cache->tile_alloc_phys; tile_alloc_log = cache->tile_alloc_log;
            tile_state_phys = cache->tile_state_phys; tile_state_log = cache->tile_state_log;
        }
        else
        {
            /* Bigger than cached (shouldn't happen given this file's
             * fixed call pattern) or the cache table is full - fall
             * back to the original uncached sequence for this call. */
            err = pmp_alloc(pmp, page, CL_PAGE_COUNT, &cl_phys, &cl_log);
            if (err) return err;
            page += CL_PAGE_COUNT;

            err = pmp_alloc(pmp, page, render_page_count, &render_phys, &render_log);
            if (err) return err;
            page += render_page_count;

            err = pmp_alloc(pmp, page, TILE_ALLOC_PAGE_COUNT, &tile_alloc_phys, &tile_alloc_log);
            if (err) return err;
            page += TILE_ALLOC_PAGE_COUNT;

            err = pmp_alloc(pmp, page, TILE_STATE_PAGE_COUNT, &tile_state_phys, &tile_state_log);
            if (err) return err;
        }
    }
    pixel_phys = render_phys + render_header_bytes;

    /*
     * Zero the tile-alloc and tile-state scratch buffers before the
     * binner ever touches them. The bare-metal reference never does
     * this - its memory comes straight from a fresh boot, pristine
     * zero from power-on. RISC OS instead hands out recycled physical
     * pages via OS_Memory 12, which can carry arbitrary leftover
     * content from whatever last used them. If the binner's own
     * bookkeeping in this scratch space is sensitive to its initial
     * content, that would explain a hang whose presence depends on
     * exactly which physical pages happen to get allocated - which
     * fits: this was never reproducible with V3D_ClearTest's page
     * range, only V3D_ClearToSprite's.
     */
    {
        unsigned int i;
        volatile unsigned int *p;

        p = (volatile unsigned int *) tile_alloc_log;
        for (i = 0; i < (TILE_ALLOC_PAGE_COUNT * PAGE_SIZE) / 4u; i++)
            p[i] = 0;

        p = (volatile unsigned int *) tile_state_log;
        for (i = 0; i < (TILE_STATE_PAGE_COUNT * PAGE_SIZE) / 4u; i++)
            p[i] = 0;
    }
    v3d_beacon(B_CT_ZEROED);

    diag_bufs.cl = cl_phys;
    diag_bufs.render_target = render_phys;
    diag_bufs.tile_alloc = tile_alloc_phys;
    diag_bufs.tile_state = tile_state_phys;
    diag_bufs.bfc0 = 0;
    diag_bufs.rfc0 = 0;

    /* --- BCL: empty bin (no primitives - see the reference's full
     * always-present state-setup block below, added after hardware
     * confirmed a bare TileBinningModeCfg+OP_START_TILE_BINNING+
     * OP_FLUSH left binning never completing; order and values copied
     * verbatim from the reference's gl_rainbow_triangle.cpp) --- */
    cl_init(&bcl, cl_log, cl_phys, PAGE_SIZE);
    bcl_start = cl_paddr(&bcl);
    emit_tile_binning_mode_cfg(&bcl, CL_CLEAR_TEST_WIDTH, CL_CLEAR_TEST_HEIGHT);
    emit_flush_vcd_cache(&bcl);
    emit_occlusion_query_counter(&bcl, 0);
    emit_start_tile_binning(&bcl);

    emit_clip_window(&bcl, 0, 0, CL_CLEAR_TEST_WIDTH, CL_CLEAR_TEST_HEIGHT);
    emit_cfg_bits(&bcl, 1, 1, 1, 0, 0, 0, 0, 7, 0, 0, 1, 0, 0, 0, 0);
    emit_point_size(&bcl, 1.0f);
    emit_line_width(&bcl, 1.0f);
    emit_clipper_xy_scaling(&bcl, (CL_CLEAR_TEST_WIDTH / 2) * 256.0f,
                             (CL_CLEAR_TEST_HEIGHT / 2) * -256.0f);
    emit_clipper_z_scale_and_offset(&bcl, 0.5f, 0.5f);
    emit_clipper_z_min_max(&bcl, 0.0f, 1.0f);
    emit_viewport_offset(&bcl, CL_CLEAR_TEST_WIDTH / 2, CL_CLEAR_TEST_HEIGHT / 2, 0, 0);
    emit_color_write_masks(&bcl, 0);
    emit_blend_constant_color(&bcl, 0, 0, 0, 0);
    emit_zero_all_flat_shade_flags(&bcl);
    emit_zero_all_non_perspective_flags(&bcl);
    emit_zero_all_centroid_flags(&bcl);
    emit_transform_feedback_specs(&bcl, 0, 0);
    emit_occlusion_query_counter(&bcl, 0);
    emit_sample_state(&bcl, 0xF, 1.0f);
    emit_vcm_cache_size(&bcl, 4, 4);

    /* No GlShaderState / VertexArrayPrims - we draw nothing. */
    emit_flush(&bcl);
    bcl_end = cl_paddr(&bcl);

    /* --- RCL: clear the single tile, store it, done --- */
    cl_init(&rcl, (unsigned char *) cl_log + bcl.offset,
            cl_phys + bcl.offset, PAGE_SIZE - bcl.offset);
    rcl_start = cl_paddr(&rcl);
    emit_tile_rendering_mode_cfg_common(&rcl, CL_CLEAR_TEST_WIDTH,
                                         CL_CLEAR_TEST_HEIGHT, 2);
    emit_tile_rendering_mode_cfg_clear_colors(&rcl, 0, clear_color_rgba, 0);
    emit_tile_rendering_mode_cfg_color_rt0(&rcl, 0, 2, 0);
    emit_tile_rendering_mode_cfg_zs_clear(&rcl, 0, 1.0f);
    emit_tile_list_initial_block_size(&rcl, 0, 1);
    emit_multicore_tile_list_set_base(&rcl, tile_alloc_phys);
    emit_multicore_supertile_cfg(&rcl, 1, 1, 1, 1, 1, 1, 1, 0, 0);

    emit_tile_coordinates(&rcl, 0, 0);
    emit_end_of_loads(&rcl);
    emit_store_tile_buffer_none(&rcl);
    emit_clear_tile_buffers(&rcl);
    emit_end_of_tile_marker(&rcl);

    /* dummy store: workaround a race condition, per the reference's own
     * comment at the equivalent point in gl_rainbow_triangle.cpp */
    emit_tile_coordinates(&rcl, 0, 0);
    emit_end_of_loads(&rcl);
    emit_store_tile_buffer_rt0(&rcl, 27 /* RGBA8 */,
                                CL_CLEAR_TEST_WIDTH * 4, pixel_phys);
    emit_end_of_tile_marker(&rcl);

    /* No SupertileCoordinates / StartAddressOfGenericTileList here -
     * see the "not verified" note above emit_supertile_coordinates.
     * Both TileCoordinates blocks above already did our real work
     * (clear + store) inline; we have no supertiles to walk. */
    emit_end_of_rendering(&rcl);
    rcl_end = cl_paddr(&rcl);
    v3d_beacon(B_CT_BUILT);

    /* --- kick off binning (short-held mapping - see v3d_poll_counter's
     * comment for why long-held ones are dangerous) --- */
    LEG_PRE_KICK();
    err = LEG_MAP(&v3d);
    if (err) return err;
    v3d_beacon(B_CT_MAP1);
    diag_bufs.bfc0 = IO_WORD(v3d, V3D_CLE_BFC);
    diag_bufs.rfc0 = IO_WORD(v3d, V3D_CLE_RFC);

    /* Clear out any overflow allocation left attached to a previous
     * job - see V3D_PTB_BPOS's comment in v3d_regs.h. Real driver does
     * this before anything else in the bin submission, cache flush
     * included. */
    IO_WORD(v3d, V3D_PTB_BPOS) = 0;

    v3d_flush_caches(&v3d);
    last_bin_irq = g_v3d_irq_bin_done_count;

    /* Matches the real Linux v3d driver's v3d_bin_job_run() exactly -
     * see the removed-code comment above. No CS/CA read, no restart
     * trigger: writing QEA is what starts the job, every time. */
    IO_WORD(v3d, V3D_CLE_CT0QMA) = tile_alloc_phys;
    IO_WORD(v3d, V3D_CLE_CT0QMS) = TILE_ALLOC_PAGE_COUNT * PAGE_SIZE;
    IO_WORD(v3d, V3D_CLE_CT0QTS) = tile_state_phys | V3D_CLE_CT0QTS_ENABLE;
    IO_WORD(v3d, V3D_CLE_CT0QBA) = bcl_start;
    IO_WORD(v3d, V3D_CLE_CT0QEA) = bcl_end; /* kicks off binning */
    v3d_beacon(B_CT_KICK_BIN);

    LEG_UNMAP(&v3d);

    {
        static _kernel_oserror err_bin_timeout = { 7, "" };
        err = v3d_poll_counter(&g_v3d_irq_bin_done_count, last_bin_irq,
                                &err_bin_timeout,
                                "BFC (binning)", 1, bcl_start, bcl_end,
                                &diag_bufs);
        if (err)
            return err;
    }
    v3d_beacon(B_CT_BIN_DONE);

    /* --- kick off rendering --- */
    err = LEG_MAP(&v3d);
    if (err) return err;
    v3d_beacon(B_CT_MAP2);

    v3d_flush_caches(&v3d);
    last_render_irq = g_v3d_irq_render_done_count;

    IO_WORD(v3d, V3D_CLE_CT1QBA) = rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = rcl_end; /* kicks off rendering - see thread 0's comment */
    v3d_beacon(B_CT_KICK_RENDER);

    LEG_UNMAP(&v3d);

    {
        static _kernel_oserror err_render_timeout = { 8, "" };
        err = v3d_poll_counter(&g_v3d_irq_render_done_count, last_render_irq,
                                &err_render_timeout,
                                "RFC (render)", 0, rcl_start, rcl_end,
                                &diag_bufs);
        if (err)
            return err;
    }
    v3d_beacon(B_CT_RENDER_DONE);

    *render_target_phys_out = render_phys;
    *render_target_log_out = render_log;
    *readback_pixel_out =
        *(volatile unsigned int *) ((unsigned char *) render_log + render_header_bytes);
    return NULL;
}

/*
 * 16bpp screen-mode-support Phase 2 diagnostic (2026-09-30): an
 * isolated, throwaway test, NOT meant to become permanent - determines
 * empirically which V3D render-target format code (candidates:
 * ABGR1555=4, RGBA5551=45, both plausible matches for RISC OS's real
 * RGB555 16bpp screen format per the real PRM Video chapter - R bits
 * 0-4, G bits 5-9, B bits 10-14, bit 15 reserved) actually matches
 * RISC OS's own bit layout, rather than guessing and risking exactly
 * the kind of silent channel swap this project already hit once (the
 * Task Manager beacon diagnostic's R/B mixup).
 *
 * Deliberately a near-verbatim copy of v3d_clear_run's own proven
 * structure (this file's "hardware-proven M2/M3 path, don't touch it"
 * convention means a new function here, not a modification of that
 * one) with exactly four things changed: format_code is a parameter
 * instead of the hardcoded 27 (RGBA8), clear_color16 is passed as a
 * 16-bit value (zero-extended into the clear-colours packet's 32-bit
 * color_low field - that field is already wide enough to hold a
 * narrower value untouched), the store stride is 2 bytes/pixel instead
 * of 4, and the readback reads a 16-bit halfword instead of a 32-bit
 * word. Everything else - INCLUDING the internal tile-buffer rendering
 * config (bpp0=0/type0=2, left unchanged) - stays identical to the
 * proven 32bpp path. bpp0/type0 configure internal tile-buffer
 * precision, a separate concept from the final stored format (which
 * output_format alone controls) - left alone here to keep this test
 * isolated to exactly the one question it exists to answer, not two
 * at once.
 *
 * No sprite-header prefix (unlike v3d_clear_run's general form) and
 * its own dedicated scratch pages via plain, uncached pmp_alloc calls
 * (a one-off manual diagnostic, not a hot GraphicsV-routed path, so
 * the scratch-caching mechanism the real freeze fixes needed doesn't
 * apply here) - callers must pass a base_page well clear of
 * CLEAR_TEST_BASE_PAGE/CLEAR_SPRITE_BASE_PAGE's own ranges.
 */
_kernel_oserror *v3d_16bpp_format_test(pmp_area_t *pmp, unsigned int base_page,
                                        unsigned int format_code,
                                        unsigned int clear_color16,
                                        unsigned int *render_target_phys_out,
                                        unsigned int *readback_pixel_out)
{
    unsigned int cl_phys, render_phys, tile_alloc_phys, tile_state_phys;
    void *cl_log, *render_log, *tile_alloc_log, *tile_state_log;
    _kernel_oserror *err;
    cl_buf_t bcl, rcl;
    unsigned int bcl_start, bcl_end, rcl_start, rcl_end;
    io_mapping_t v3d;
    unsigned int last_bin_irq, last_render_irq;
    unsigned int page = base_page;
    diag_bufs_t diag_bufs;

    err = pmp_alloc(pmp, page, CL_PAGE_COUNT, &cl_phys, &cl_log);
    if (err) return err;
    page += CL_PAGE_COUNT;

    err = pmp_alloc(pmp, page, RENDER_TARGET_PAGE_COUNT, &render_phys, &render_log);
    if (err) return err;
    page += RENDER_TARGET_PAGE_COUNT;

    err = pmp_alloc(pmp, page, TILE_ALLOC_PAGE_COUNT, &tile_alloc_phys, &tile_alloc_log);
    if (err) return err;
    page += TILE_ALLOC_PAGE_COUNT;

    err = pmp_alloc(pmp, page, TILE_STATE_PAGE_COUNT, &tile_state_phys, &tile_state_log);
    if (err) return err;

    {
        unsigned int i;
        volatile unsigned int *p;

        p = (volatile unsigned int *) tile_alloc_log;
        for (i = 0; i < (TILE_ALLOC_PAGE_COUNT * PAGE_SIZE) / 4u; i++)
            p[i] = 0;

        p = (volatile unsigned int *) tile_state_log;
        for (i = 0; i < (TILE_STATE_PAGE_COUNT * PAGE_SIZE) / 4u; i++)
            p[i] = 0;
    }

    diag_bufs.cl = cl_phys;
    diag_bufs.render_target = render_phys;
    diag_bufs.tile_alloc = tile_alloc_phys;
    diag_bufs.tile_state = tile_state_phys;
    diag_bufs.bfc0 = 0;
    diag_bufs.rfc0 = 0;

    cl_init(&bcl, cl_log, cl_phys, PAGE_SIZE);
    bcl_start = cl_paddr(&bcl);
    emit_tile_binning_mode_cfg(&bcl, CL_CLEAR_TEST_WIDTH, CL_CLEAR_TEST_HEIGHT);
    emit_flush_vcd_cache(&bcl);
    emit_occlusion_query_counter(&bcl, 0);
    emit_start_tile_binning(&bcl);

    emit_clip_window(&bcl, 0, 0, CL_CLEAR_TEST_WIDTH, CL_CLEAR_TEST_HEIGHT);
    emit_cfg_bits(&bcl, 1, 1, 1, 0, 0, 0, 0, 7, 0, 0, 1, 0, 0, 0, 0);
    emit_point_size(&bcl, 1.0f);
    emit_line_width(&bcl, 1.0f);
    emit_clipper_xy_scaling(&bcl, (CL_CLEAR_TEST_WIDTH / 2) * 256.0f,
                             (CL_CLEAR_TEST_HEIGHT / 2) * -256.0f);
    emit_clipper_z_scale_and_offset(&bcl, 0.5f, 0.5f);
    emit_clipper_z_min_max(&bcl, 0.0f, 1.0f);
    emit_viewport_offset(&bcl, CL_CLEAR_TEST_WIDTH / 2, CL_CLEAR_TEST_HEIGHT / 2, 0, 0);
    emit_color_write_masks(&bcl, 0);
    emit_blend_constant_color(&bcl, 0, 0, 0, 0);
    emit_zero_all_flat_shade_flags(&bcl);
    emit_zero_all_non_perspective_flags(&bcl);
    emit_zero_all_centroid_flags(&bcl);
    emit_transform_feedback_specs(&bcl, 0, 0);
    emit_occlusion_query_counter(&bcl, 0);
    emit_sample_state(&bcl, 0xF, 1.0f);
    emit_vcm_cache_size(&bcl, 4, 4);

    emit_flush(&bcl);
    bcl_end = cl_paddr(&bcl);

    cl_init(&rcl, (unsigned char *) cl_log + bcl.offset,
            cl_phys + bcl.offset, PAGE_SIZE - bcl.offset);
    rcl_start = cl_paddr(&rcl);
    emit_tile_rendering_mode_cfg_common(&rcl, CL_CLEAR_TEST_WIDTH,
                                         CL_CLEAR_TEST_HEIGHT, 2);
    emit_tile_rendering_mode_cfg_clear_colors(&rcl, 0, clear_color16, 0);
    emit_tile_rendering_mode_cfg_color_rt0(&rcl, 0, 2, 0);
    emit_tile_rendering_mode_cfg_zs_clear(&rcl, 0, 1.0f);
    emit_tile_list_initial_block_size(&rcl, 0, 1);
    emit_multicore_tile_list_set_base(&rcl, tile_alloc_phys);
    emit_multicore_supertile_cfg(&rcl, 1, 1, 1, 1, 1, 1, 1, 0, 0);

    emit_tile_coordinates(&rcl, 0, 0);
    emit_end_of_loads(&rcl);
    emit_store_tile_buffer_none(&rcl);
    emit_clear_tile_buffers(&rcl);
    emit_end_of_tile_marker(&rcl);

    emit_tile_coordinates(&rcl, 0, 0);
    emit_end_of_loads(&rcl);
    emit_store_tile_buffer_rt0(&rcl, format_code,
                                CL_CLEAR_TEST_WIDTH * 2, render_phys);
    emit_end_of_tile_marker(&rcl);

    emit_end_of_rendering(&rcl);
    rcl_end = cl_paddr(&rcl);

    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    IO_WORD(v3d, V3D_PTB_BPOS) = 0;
    v3d_flush_caches(&v3d);
    last_bin_irq = g_v3d_irq_bin_done_count;

    IO_WORD(v3d, V3D_CLE_CT0QMA) = tile_alloc_phys;
    IO_WORD(v3d, V3D_CLE_CT0QMS) = TILE_ALLOC_PAGE_COUNT * PAGE_SIZE;
    IO_WORD(v3d, V3D_CLE_CT0QTS) = tile_state_phys | V3D_CLE_CT0QTS_ENABLE;
    IO_WORD(v3d, V3D_CLE_CT0QBA) = bcl_start;
    IO_WORD(v3d, V3D_CLE_CT0QEA) = bcl_end;

    io_unmap(&v3d);

    {
        static _kernel_oserror err_bin_timeout = { 7, "" };
        err = v3d_poll_counter(&g_v3d_irq_bin_done_count, last_bin_irq,
                                &err_bin_timeout,
                                "BFC (binning)", 1, bcl_start, bcl_end,
                                &diag_bufs);
        if (err)
            return err;
    }

    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    v3d_flush_caches(&v3d);
    last_render_irq = g_v3d_irq_render_done_count;

    IO_WORD(v3d, V3D_CLE_CT1QBA) = rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = rcl_end;

    io_unmap(&v3d);

    {
        static _kernel_oserror err_render_timeout = { 8, "" };
        err = v3d_poll_counter(&g_v3d_irq_render_done_count, last_render_irq,
                                &err_render_timeout,
                                "RFC (render)", 0, rcl_start, rcl_end,
                                &diag_bufs);
        if (err)
            return err;
    }

    *render_target_phys_out = render_phys;
    *readback_pixel_out = (unsigned int) *(volatile unsigned short *) render_log;
    return NULL;
}

_kernel_oserror *v3d_clear_test(pmp_area_t *pmp, unsigned int base_page,
                                 unsigned int clear_color_rgba,
                                 unsigned int *render_target_phys_out,
                                 unsigned int *readback_pixel_out)
{
    void *render_log;
    return v3d_clear_run(pmp, base_page, clear_color_rgba,
                          RENDER_TARGET_PAGE_COUNT, 0,
                          render_target_phys_out, &render_log,
                          readback_pixel_out);
}

_kernel_oserror *v3d_clear_to_sprite(pmp_area_t *pmp, unsigned int base_page,
                                      unsigned int clear_color_rgba,
                                      unsigned int *sprite_area_phys_out,
                                      void **sprite_area_log_out,
                                      unsigned int *readback_pixel_out)
{
    _kernel_oserror *err = v3d_clear_run(
        pmp, base_page, clear_color_rgba,
        SPRITE_RENDER_PAGE_COUNT, SPRITE_HEADER_BYTES,
        sprite_area_phys_out, sprite_area_log_out, readback_pixel_out);
    if (err)
        return err;

    /* Headers only - the GPU already wrote the pixel data (at
     * *sprite_area_log_out + SPRITE_HEADER_BYTES) as part of
     * v3d_clear_run above. Written after, but there's no race: CPU and
     * GPU touch disjoint byte ranges of the same buffer. */
    sprite_write_header(*sprite_area_log_out, CL_CLEAR_TEST_WIDTH,
                         CL_CLEAR_TEST_HEIGHT, "v3dtest");
    return NULL;
}
