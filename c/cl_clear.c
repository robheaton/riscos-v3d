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
} diag_bufs_t;

static void diagnose_timeout(_kernel_oserror *out_err, const char *what,
                              int is_bin, unsigned int qba, unsigned int qea,
                              const diag_bufs_t *bufs)
{
    io_mapping_t v3d;
    unsigned int sts = 0, cs = 0, ca = 0, ea = 0;
    unsigned int rb_qba = 0, rb_qea = 0;
    unsigned int mmu_ctl = 0, hub_sts = 0, vio_id = 0, vio_addr = 0;
    unsigned int pos = 0;
    int recover_attempted = 0, recover_ok = 0;

    if (!io_map(V3D_MB_BASE, &v3d))
    {
        sts = IO_WORD(v3d, V3D_CTL_INT_STS);
        mmu_ctl = IO_WORD(v3d, V3D_MMU_CTL);
        hub_sts = IO_WORD(v3d, V3D_HUB_INT_STS);
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
            rb_qba = IO_WORD(v3d, V3D_CLE_CT0QBA);
            rb_qea = IO_WORD(v3d, V3D_CLE_CT0QEA);
        }
        else
        {
            cs = IO_WORD(v3d, V3D_CLE_CT1CS);
            ca = IO_WORD(v3d, V3D_CLE_CT1CA);
            ea = IO_WORD(v3d, V3D_CLE_CT1EA);
            rb_qba = IO_WORD(v3d, V3D_CLE_CT1QBA);
            rb_qea = IO_WORD(v3d, V3D_CLE_CT1QEA);
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
    append_str(out_err->errmess, &pos, " STS=");
    append_hex32(out_err->errmess, &pos, sts);
    if (sts & V3D_INT_OUTOMEM) append_str(out_err->errmess, &pos, "(OOM)");
    if (sts & V3D_INT_GMPV) append_str(out_err->errmess, &pos, "(GMPV)");
    append_str(out_err->errmess, &pos, " MMU=");
    append_hex32(out_err->errmess, &pos, mmu_ctl & 1u);
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
    append_str(out_err->errmess, &pos, " QBA=w");
    append_hex32(out_err->errmess, &pos, qba);
    append_str(out_err->errmess, &pos, "/r");
    append_hex32(out_err->errmess, &pos, rb_qba);
    append_str(out_err->errmess, &pos, " QEA=w");
    append_hex32(out_err->errmess, &pos, qea);
    append_str(out_err->errmess, &pos, "/r");
    append_hex32(out_err->errmess, &pos, rb_qea);
    append_str(out_err->errmess, &pos, " tAl=");
    append_hex32(out_err->errmess, &pos, bufs->tile_alloc);
    append_str(out_err->errmess, &pos, " rTg=");
    append_hex32(out_err->errmess, &pos, bufs->render_target);
    append_str(out_err->errmess, &pos, " tSt=");
    append_hex32(out_err->errmess, &pos, bufs->tile_state);
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

    err = pmp_alloc(pmp, page, CL_PAGE_COUNT, &cl_phys, &cl_log);
    if (err) return err;
    page += CL_PAGE_COUNT;

    err = pmp_alloc(pmp, page, render_page_count, &render_phys, &render_log);
    if (err) return err;
    page += render_page_count;
    pixel_phys = render_phys + render_header_bytes;

    err = pmp_alloc(pmp, page, TILE_ALLOC_PAGE_COUNT, &tile_alloc_phys, &tile_alloc_log);
    if (err) return err;
    page += TILE_ALLOC_PAGE_COUNT;

    err = pmp_alloc(pmp, page, TILE_STATE_PAGE_COUNT, &tile_state_phys, &tile_state_log);
    if (err) return err;

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

    diag_bufs.cl = cl_phys;
    diag_bufs.render_target = render_phys;
    diag_bufs.tile_alloc = tile_alloc_phys;
    diag_bufs.tile_state = tile_state_phys;

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

    /* --- kick off binning (short-held mapping - see v3d_poll_counter's
     * comment for why long-held ones are dangerous) --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

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

    /* --- kick off rendering --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err) return err;

    v3d_flush_caches(&v3d);
    last_render_irq = g_v3d_irq_render_done_count;

    IO_WORD(v3d, V3D_CLE_CT1QBA) = rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = rcl_end; /* kicks off rendering - see thread 0's comment */

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
    *render_target_log_out = render_log;
    *readback_pixel_out =
        *(volatile unsigned int *) ((unsigned char *) render_log + render_header_bytes);
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
