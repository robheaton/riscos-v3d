/*
 * module.c - RISC OS module skeleton for milestone M0/M1 of an
 * independent V3D (Pi 4 / CM4 GPU) driver: power on the block, read
 * back its IDENT registers, and expose that over two SWIs.
 *
 * This is a SKETCH to validate the approach end-to-end on real hardware
 * before committing to anything bigger (control lists, shaders, Mesa).
 * It intentionally does NOT touch the CLE/render path yet.
 *
 * Build: GCCSDK cross-toolchain + cmhg (see ../Makefile).
 *
 * v2: replaced the earlier HAL_IOAddress() guess with OS_Memory 14/15,
 * the documented public API for temporarily mapping device physical
 * memory (see io_map.h). Each access now maps its megabyte window, does
 * its reads/writes, and releases it immediately - nothing is cached
 * across calls, per the PRM's explicit warning against long-life
 * mappings from this shared window.
 */

#include <stddef.h>
#include "kernel.h"
#include "swis.h"
#include "v3d_regs.h"
#include "io_map.h"
#include "phys_buffer.h"
#include "cl_clear.h"
#include "memsplit.h"
#include "v3d_clock.h"
#include "v3d_irq.h"
#include "screen_info.h"
#include "cl_fill.h"
#include "cl_copy.h"
#include "gfxv_accel.h"
#include "V3DDriver.h" /* cmunge-generated: SWI numbers + entry-point
                         * prototypes. Included so a signature mismatch
                         * (like the module_final one below) is a compile
                         * error, not a silent ABI bug. */

static int g_powered_on = 0;
static unsigned int g_v3d_clock_rate = 0; /* set once, on the first real
                                            * V3D_PowerOn - see its comment */

/* M2 in progress: physical buffer infrastructure for control lists.
 * Created lazily on first use rather than in module_init, so a module
 * load with no buffer use touches no PMP state at all. Shared by every
 * SWI that needs a physical buffer - each picks its own non-overlapping
 * page range within it (see the page_index comments at each call site). */
static pmp_area_t g_pmp;
static int g_pmp_created = 0;

static _kernel_oserror *ensure_pmp(void)
{
    _kernel_oserror *err;

    if (g_pmp_created)
        return NULL;

    /* 64 pages (256KB) covers V3D_TestBuffer's 1 page plus
     * v3d_clear_test's 9 pages with plenty of headroom to grow. */
    err = pmp_create("V3DBuf", 1024u * 1024u, 64, &g_pmp);
    if (err)
        return err;

    g_pmp_created = 1;
    return NULL;
}

/* Uses PMP page 0. */
static _kernel_oserror *v3d_test_buffer(unsigned int *phys_addr_out,
                                         unsigned int *readback_out,
                                         unsigned int *logical_addr_out)
{
    _kernel_oserror *err;
    void *log_ptr;
    unsigned int phys_addr;
    volatile unsigned int *word;

    err = ensure_pmp();
    if (err)
        return err;

    err = pmp_alloc(&g_pmp, 0, 1, &phys_addr, &log_ptr);
    if (err)
        return err;

    word = (volatile unsigned int *) log_ptr;
    *word = 0xDEADBEEFu;

    *phys_addr_out = phys_addr;
    *readback_out = *word;
    *logical_addr_out = (unsigned int) log_ptr;
    return NULL;
}

/* Uses PMP pages 16-24 (9 pages) - well clear of V3D_TestBuffer's page 0. */
#define CLEAR_TEST_BASE_PAGE 16u

/* Uses PMP pages 32-41 (10 pages) - well clear of ClearTest's range. */
#define CLEAR_SPRITE_BASE_PAGE 32u

/*
 * Separate, much bigger PMP area for V3D_FillTest (step 2 of the
 * planned GraphicsV Render accelerator - see README.md). The shared
 * g_pmp above is sized for tiny fixed-64x64 test buffers (1MB/64 pages
 * total); a multi-tile fill needs room to grow up to full-screen size
 * (1920x1088 padded RGBA8 alone is ~8MB - see cl_fill.c's sizing
 * comments), so this gets its own 16MB logical window instead of
 * enlarging g_pmp and risking the already-proven allocator/DA config
 * every other SWI here depends on. Created lazily, same as g_pmp.
 */
static pmp_area_t g_pmp_fill;
static int g_pmp_fill_created = 0;

/*
 * Base-page layout within g_pmp_fill: one reservation per SWI/accel
 * path, each a full PMP_FILL_BLOCK_PAGES-page block, so no two can
 * ever overlap - trivially verifiable (every reservation is just
 * `block_index * PMP_FILL_BLOCK_PAGES`), rather than requiring hand-
 * checked arithmetic against each path's own precise worst-case need.
 *
 * PMP_FILL_BLOCK_PAGES=8192 (32MB) was picked as roughly double the
 * single biggest worst-case need of any one path (~4137 pages - two
 * PMP_MAX_PAGES_PER_CALL-sized buffers plus a small scratch, e.g.
 * V3D_CopyTest's source+dest or V3D_OverlapCopyTest's bounding buffer
 * + its own scratch-buffer pass), leaving comfortable headroom without
 * needing to size each block individually.
 *
 * RAISED (2026-09-26, alongside PMP_MAX_PAGES_PER_CALL): before this,
 * SCREEN_COPY_BASE_PAGE hosted BOTH the tiny direct-copy scratch
 * (~41 pages) AND the overlap-copy scratch (now up to ~2089 pages) at
 * the SAME base, spilling straight into the next reservation 200
 * pages later - found by inspection while raising that cap, before it
 * became a real hardware bug. This uniform-block scheme is deliberately
 * harder to get subtly wrong the same way again.
 */
#define PMP_FILL_BLOCK_PAGES 8192u
#define PMP_FILL_BLOCK_COUNT 6u

static _kernel_oserror *ensure_pmp_fill(void)
{
    _kernel_oserror *err;

    if (g_pmp_fill_created)
        return NULL;

    /* PMP logical-window reservation is cheap (no real memory
     * committed until pages are actually claimed via pmp_alloc), so
     * there's no cost to sizing this generously. */
    err = pmp_create("V3DFill", PMP_FILL_BLOCK_COUNT * PMP_FILL_BLOCK_PAGES
                      * PAGE_SIZE, PMP_FILL_BLOCK_COUNT * PMP_FILL_BLOCK_PAGES,
                      &g_pmp_fill);
    if (err)
        return err;

    g_pmp_fill_created = 1;
    return NULL;
}

#define FILL_TEST_BASE_PAGE            (0u * PMP_FILL_BLOCK_PAGES) /* V3D_FillTest */
#define SCREEN_FILL_BASE_PAGE          (1u * PMP_FILL_BLOCK_PAGES) /* V3D_ScreenFillTest / accel fill */
#define COPY_TEST_BASE_PAGE            (2u * PMP_FILL_BLOCK_PAGES) /* V3D_CopyTest */
#define SCREEN_COPY_BASE_PAGE          (3u * PMP_FILL_BLOCK_PAGES) /* V3D_ScreenCopyTest / accel direct copy */
#define OVERLAP_COPY_TEST_BASE_PAGE    (4u * PMP_FILL_BLOCK_PAGES) /* V3D_OverlapCopyTest */
#define SCREEN_COPY_OVERLAP_BASE_PAGE  (5u * PMP_FILL_BLOCK_PAGES) /* V3D_ScreenCopyOverlapTest / accel overlap copy */

/*
 * Step 3 of the planned GraphicsV Render accelerator: caches the
 * screen's confirmed physical framestore base (see
 * v3d_get_framestore_phys's own comment for why this is
 * checked for real, not assumed) plus the mode info needed to target
 * it, computed once and reused - re-verifying contiguity across the
 * whole screen on every single fill call would be wasteful, and the
 * mode isn't expected to change without a module reload anyway.
 */
static int g_screen_framestore_verified = 0;
static v3d_screen_info_t g_screen_info;
static unsigned int g_screen_phys_base = 0;

static _kernel_oserror *ensure_screen_framestore(void)
{
    _kernel_oserror *err;

    if (g_screen_framestore_verified)
        return NULL;

    err = v3d_read_screen_info(&g_screen_info);
    if (err)
        return err;

    if (g_screen_info.log2_bpp != 5u)
    {
        static _kernel_oserror err_bad_bpp = {
            16, "V3DDriver: V3D_ScreenFillTest only supports 32bpp modes"
        };
        return &err_bad_bpp;
    }

    err = v3d_get_framestore_phys(&g_screen_info, &g_screen_phys_base);
    if (err)
        return err;

    g_screen_framestore_verified = 1;
    return NULL;
}

/*
 * Power-on sequence, transcribed from the MIT-licensed bare-metal
 * reference (pm.cpp) - direct PM_GRAFX reset deassert + ASB master/slave
 * gate release. No firmware mailbox call required for this step.
 *
 * HISTORY: an earlier version of this function explicitly asserted
 * reset then deasserted it on *every* call (diverging from the
 * reference, which only ever sets PM_V3DRSTN once, correct for a chip
 * fresh from silicon reset), reasoning that repeated calls needed a
 * genuine pulse each time to recover a CLE thread that had gotten
 * wedged. That reasoning was wrong, and expensively so: hardware
 * testing (2026-09-26) spent nineteen separate attempts investigating
 * a "second GPU submission in a session always hangs" bug - address
 * ranges, clock re-enable idempotency, restart/kick logic (five
 * variants, then removed entirely to match the real Linux v3d
 * driver), V3D_PTB_BPOS - before finding the actual cause: calling
 * V3D_PowerOn a *second* time, re-pulsing a hardware reset on an
 * already-warm, already-clocked, already-working V3D core, is itself
 * what breaks it. A minimal test confirmed it directly: PowerOn once,
 * then two separate ClearTest/ClearToSprite submissions with no
 * second PowerOn in between - both succeeded, byte-exact, on entirely
 * different physical addresses.
 *
 * So: real callers should only need to call this once per boot
 * anyway, but defensively calling it again (a completely reasonable
 * thing for real code to do) must not silently break the GPU. Made
 * idempotent via g_powered_on - but that guard is per-*module-
 * instance* C state, and a real hardware test (2026-09-26) found it
 * doesn't survive a plain module reload (*RMKill then *RMLoad): a
 * freshly-loaded instance's g_powered_on is 0 again even though the
 * underlying V3D
 * core is still warm from the previous instance's use, so its first
 * V3D_PowerOn call reset-pulses an already-working core all over
 * again - the machine froze running real GPU work right after such a
 * reload, without an intervening full reboot. g_powered_on can only
 * ever answer "did *this instance* power on", not "does the hardware
 * actually need it" - the fix is to ask the hardware directly instead:
 * v3d_ensure_clock_on() now reports whether the firmware clock was
 * *already* on before this call touched anything, which is real,
 * instance-independent hardware truth. If so, skip the reset/gate
 * sequence entirely, regardless of this instance's own history.
 */
static _kernel_oserror *v3d_power_on(unsigned int *clock_rate_out)
{
    io_mapping_t pm, v3d;
    _kernel_oserror *err;
    unsigned int val, clock_rate;
    int clock_already_on;
    volatile unsigned int spin; /* volatile: the reset-pulse delay loop
                                  * below has no other side effect, and
                                  * -O2 is entitled to delete a plain
                                  * empty counting loop entirely */

    if (g_powered_on)
    {
        *clock_rate_out = g_v3d_clock_rate;
        return NULL;
    }

    /* --- firmware clock gate: confirmed root cause of every CLE hang
     * logged so far (2026-09-26 hardware test: V3D_ClockInfo reported
     * state=0, clock gated off) - see v3d_clock.h. Must happen before
     * anything else touches V3D's own registers, not just before the
     * CLE is used: a gated block can still be read/written (explaining
     * why ReadIdent/TestBuffer always worked regardless), so this was
     * never going to show up as a register-access failure. */
    err = v3d_ensure_clock_on(&clock_rate, &clock_already_on);
    if (err)
        return err;

    if (clock_already_on)
    {
        /* Real hardware evidence this core was already through a
         * complete, correct power-on before - whether by this
         * instance or an earlier one. Re-pulsing reset now would be
         * exactly the confirmed hang/freeze cause described above. */
        g_powered_on = 1;
        g_v3d_clock_rate = clock_rate;
        *clock_rate_out = clock_rate;
        return NULL;
    }

    /* --- PM_GRAFX: assert then deassert V3D reset (real pulse) --- */
    err = io_map(PM_MB_BASE, &pm);
    if (err)
        return err;

    val = IO_WORD(pm, PM_GRAFX);
    IO_WORD(pm, PM_GRAFX) = PM_PASSWORD | (val & ~PM_V3DRSTN); /* assert reset */

    for (spin = 0; spin < 100000u; spin++)
        ; /* crude delay - no documented minimum pulse width to go on */

    val = IO_WORD(pm, PM_GRAFX);
    IO_WORD(pm, PM_GRAFX) = PM_PASSWORD | val | PM_V3DRSTN; /* deassert reset */

    io_unmap(&pm);

    /* --- ASB master/slave gates (same megabyte as V3D_HUB/CORE0) --- */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err)
        return err;

    IO_WORD(v3d, ASB_V3D_M_CTRL) = IO_WORD(v3d, ASB_V3D_M_CTRL) & ~ASB_REQ_STOP;
    for (spin = 0; IO_WORD(v3d, ASB_V3D_M_CTRL) & ASB_ACK; spin++)
    {
        if (spin > 1000000)
        {
            static _kernel_oserror err_asb_m = {
                2, "V3DDriver: timed out waiting for ASB master gate ACK "
                   "to clear during V3D power-on"
            };
            io_unmap(&v3d);
            return &err_asb_m;
        }
    }

    IO_WORD(v3d, ASB_V3D_S_CTRL) = IO_WORD(v3d, ASB_V3D_S_CTRL) & ~ASB_REQ_STOP;
    for (spin = 0; IO_WORD(v3d, ASB_V3D_S_CTRL) & ASB_ACK; spin++)
    {
        if (spin > 1000000)
        {
            static _kernel_oserror err_asb_s = {
                3, "V3DDriver: timed out waiting for ASB slave gate ACK "
                   "to clear during V3D power-on"
            };
            io_unmap(&v3d);
            return &err_asb_s;
        }
    }

    io_unmap(&v3d);

    g_powered_on = 1;
    g_v3d_clock_rate = clock_rate;
    *clock_rate_out = clock_rate;
    return NULL;
}

/*
 * Step 4 of the planned GraphicsV Render accelerator (see README.md
 * and gfxv_accel.c): the one function that knows how to actually
 * perform an accelerated fill using this driver's existing PMP/IRQ/
 * screen-framestore infrastructure, exposed for gfxv_accel.c's vector
 * handler to call. Takes GraphicsV's OWN inclusive (left, bottom,
 * right, top) rectangle - 0,0 is bottom-left, positive Y is up, per
 * GraphicsV 13 2's own parameter block doc - and converts to this
 * driver's internal top-down-from-ScreenStart convention (confirmed by
 * step 3's hardware test: increasing y moves away from the top) before
 * calling v3d_fill_screen. This is the one place that conversion
 * happens - gfxv_accel.c deliberately knows nothing about it.
 *
 * Unlike V3D_ScreenFillTest (which requires the caller to have already
 * called V3D_PowerOn), this self-initialises everything it needs -
 * a real desktop redraw calling into GraphicsV won't have called any
 * of this module's own test SWIs first.
 */
_kernel_oserror *v3d_accel_render_fill(unsigned int left, unsigned int bottom,
                                         unsigned int right, unsigned int top,
                                         unsigned int colour_rgba)
{
    unsigned int clock_rate;
    unsigned int width, height, y_top_down;
    unsigned int tiles_x, tiles_y;
    _kernel_oserror *err;

    err = v3d_power_on(&clock_rate);
    if (err)
        return err;

    err = ensure_screen_framestore();
    if (err)
        return err;

    err = ensure_pmp_fill();
    if (err)
        return err;

    err = v3d_irq_ensure_ready();
    if (err)
        return err;

    width = right - left + 1u;
    height = top - bottom + 1u;
    y_top_down = g_screen_info.ywind_limit - top;

    return v3d_fill_screen(&g_pmp_fill, SCREEN_FILL_BASE_PAGE,
                             left, y_top_down, width, height, colour_rgba,
                             g_screen_phys_base, g_screen_info.line_length,
                             &tiles_x, &tiles_y);
}

/*
 * Step 5's equivalent of v3d_accel_render_fill, for op 1 (copy) - see
 * gfxv_accel.c's handle_render_copy. Same self-initialisation, same
 * GraphicsV-bottom-up-to-internal-top-down coordinate conversion,
 * applied independently to both rectangles (each has its own bottom
 * edge, so each needs its own flip - they are NOT assumed to share a
 * common y-origin beyond both being on the same screen). Non-
 * overlapping only - the caller (gfxv_accel.c's handle_render_copy)
 * must have already checked that itself; this function does not
 * re-check, same division of responsibility as v3d_accel_render_fill
 * not re-validating its own caller's ECF decoding.
 */
_kernel_oserror *v3d_accel_render_copy(unsigned int src_left, unsigned int src_bottom,
                                         unsigned int dest_left, unsigned int dest_bottom,
                                         unsigned int width, unsigned int height)
{
    unsigned int clock_rate;
    unsigned int src_y_top_down, dest_y_top_down;
    unsigned int tiles_x, tiles_y;
    _kernel_oserror *err;

    err = v3d_power_on(&clock_rate);
    if (err)
        return err;

    err = ensure_screen_framestore();
    if (err)
        return err;

    err = ensure_pmp_fill();
    if (err)
        return err;

    err = v3d_irq_ensure_ready();
    if (err)
        return err;

    /* Each rectangle's own top edge, flipped independently - see this
     * function's own top comment. */
    src_y_top_down = g_screen_info.ywind_limit - (src_bottom + height - 1u);
    dest_y_top_down = g_screen_info.ywind_limit - (dest_bottom + height - 1u);

    return v3d_copy_screen(&g_pmp_fill, SCREEN_COPY_BASE_PAGE,
                             src_left, src_y_top_down, dest_left, dest_y_top_down,
                             width, height,
                             g_screen_phys_base, g_screen_info.line_length,
                             &tiles_x, &tiles_y);
}

/*
 * Overlap-safe twin of v3d_accel_render_copy - same self-
 * initialisation and coordinate-flip reasoning, but routes through
 * v3d_copy_screen_overlapping's scratch-buffer pass instead of a
 * direct single-pass copy (see cl_copy.h/.c for why a direct pass
 * isn't safe once source and destination overlap). Uses its own
 * SCREEN_COPY_OVERLAP_BASE_PAGE, not SCREEN_COPY_BASE_PAGE - the
 * scratch buffer this path needs (up to PMP_MAX_PAGES_PER_CALL pages)
 * is far bigger than the direct path's ~41-page scratch, so the two
 * need separate reservations (see the base-page layout comment above).
 */
_kernel_oserror *v3d_accel_render_copy_overlapping(unsigned int src_left, unsigned int src_bottom,
                                                     unsigned int dest_left, unsigned int dest_bottom,
                                                     unsigned int width, unsigned int height)
{
    unsigned int clock_rate;
    unsigned int src_y_top_down, dest_y_top_down;
    unsigned int tiles_x, tiles_y;
    _kernel_oserror *err;

    err = v3d_power_on(&clock_rate);
    if (err)
        return err;

    err = ensure_screen_framestore();
    if (err)
        return err;

    err = ensure_pmp_fill();
    if (err)
        return err;

    err = v3d_irq_ensure_ready();
    if (err)
        return err;

    src_y_top_down = g_screen_info.ywind_limit - (src_bottom + height - 1u);
    dest_y_top_down = g_screen_info.ywind_limit - (dest_bottom + height - 1u);

    return v3d_copy_screen_overlapping(&g_pmp_fill, SCREEN_COPY_OVERLAP_BASE_PAGE,
                             src_left, src_y_top_down, dest_left, dest_y_top_down,
                             width, height,
                             g_screen_phys_base, g_screen_info.line_length,
                             &tiles_x, &tiles_y);
}

static _kernel_oserror *v3d_read_ident(unsigned int *ident, unsigned int n)
{
    io_mapping_t v3d;
    _kernel_oserror *err;

    if (!g_powered_on)
    {
        static _kernel_oserror err_not_on = {
            4, "V3DDriver: V3D_ReadIdent called before V3D_PowerOn"
        };
        return &err_not_on;
    }

    /* V3D_HUB_IDENT0/1/2 and V3D_CTL_IDENT0 all sit in the same
     * megabyte window (V3D_MB_BASE) - one mapping covers all four. */
    err = io_map(V3D_MB_BASE, &v3d);
    if (err)
        return err;

    if (n > 0) ident[0] = IO_WORD(v3d, V3D_HUB_IDENT0);
    if (n > 1) ident[1] = IO_WORD(v3d, V3D_HUB_IDENT1);
    if (n > 2) ident[2] = IO_WORD(v3d, V3D_HUB_IDENT2);
    if (n > 3) ident[3] = IO_WORD(v3d, V3D_CTL_IDENT0);
    /* CT0CS/CT1CS added to check what a freshly-reset thread's state
     * actually is *before* any submission ever happens - v3d_restart_
     * thread's "skip restart if CTRUN already set" logic assumes a
     * fresh thread reads CTRUN=0, and that assumption has never
     * actually been checked in isolation like this. */
    if (n > 4) ident[4] = IO_WORD(v3d, V3D_CLE_CT0CS);
    if (n > 5) ident[5] = IO_WORD(v3d, V3D_CLE_CT1CS);

    io_unmap(&v3d);
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Module entry points (names must match the .cmhg file)            */
/* ---------------------------------------------------------------- */

_kernel_oserror *module_init(const char *cmd_tail, int podule_base, void *pw)
{
    (void) cmd_tail;
    (void) podule_base;

    /* Captured here, not discarded: v3d_irq_claim() needs this exact
     * value later - see v3d_irq_set_private_word's comment in
     * v3d_irq.h for why a raw hardware vector needs it passed
     * explicitly, unlike every other entry point into this module. */
    v3d_irq_set_private_word(pw);
    v3d_gfxv_set_private_word(pw);

    g_powered_on = 0;
    /* Deliberately doing nothing else here: no hardware is touched, and
     * no IO region is mapped, until a SWI actually asks for it. */
    return NULL;
}

_kernel_oserror *module_final(int fatal, int podule_base, void *pw)
{
    _kernel_oserror *irq_err, *gfxv_err, *pmp_err = NULL;

    (void) fatal;
    (void) podule_base;
    (void) pw;

    /* Must run unconditionally, even if IRQ handling was never used
     * this session - see v3d_irq_shutdown's own comment for why a
     * reloaded module leaving its interrupt vector claimed is a real
     * hazard, not just untidy. */
    irq_err = v3d_irq_shutdown();

    /* Same reasoning, a second system vector: see the README's
     * "GraphicsV Render accelerator" section for why this compounds
     * the existing, unresolved reload-without-reboot freeze risk - a
     * safe no-op if V3D_AccelStart was never called this session. */
    gfxv_err = v3d_gfxv_shutdown();

    /* Same reasoning, different resource: the PMP Dynamic Area is
     * system-wide OS state that outlives this module unless its
     * claimed physical pages are explicitly released first - see
     * pmp_release's own comment in phys_buffer.c. Release the whole
     * area's page range unconditionally (safe even for pages never
     * actually used - see pmp_release's own comment), not just
     * whichever sub-ranges this particular session happened to touch. */
    if (g_pmp_created)
        pmp_err = pmp_release(&g_pmp, 0, g_pmp.max_pages);

    if (g_pmp_fill_created)
    {
        _kernel_oserror *fill_err = pmp_release(&g_pmp_fill, 0, g_pmp_fill.max_pages);
        if (!pmp_err)
            pmp_err = fill_err;
    }

    if (irq_err) return irq_err;
    if (gfxv_err) return gfxv_err;
    return pmp_err;
}

/* ---------------------------------------------------------------- */
/* SWI handler - dispatch table order must match the .cmhg file     */
/* ---------------------------------------------------------------- */

enum
{
    SWI_V3D_PowerOn = 0,
    SWI_V3D_ReadIdent = 1,
    SWI_V3D_TestBuffer = 2,
    SWI_V3D_ClearTest = 3,
    SWI_V3D_ClearToSprite = 4,
    SWI_V3D_MemSplit = 5,
    SWI_V3D_ClockInfo = 6,
    SWI_V3D_IRQTest = 7,
    SWI_V3D_ScreenInfo = 8,
    SWI_V3D_FillTest = 9,
    SWI_V3D_ScreenFillTest = 10,
    SWI_V3D_AccelStart = 11,
    SWI_V3D_AccelStop = 12,
    SWI_V3D_AccelStats = 13,
    SWI_V3D_AccelFailInfo = 14,
    SWI_V3D_CopyTest = 15,
    SWI_V3D_ScreenCopyTest = 16,
    SWI_V3D_AccelCopyStats = 17,
    SWI_V3D_OverlapCopyTest = 18,
    SWI_V3D_ScreenCopyOverlapTest = 19,
};

_kernel_oserror *v3d_swi_handler(int swi_no, _kernel_swi_regs *r, void *pw)
{
    (void) pw;

    switch (swi_no)
    {
        case SWI_V3D_PowerOn:
        {
            unsigned int clock_rate;
            _kernel_oserror *err = v3d_power_on(&clock_rate);
            if (err)
                return err;
            r->r[0] = 1; /* success flag */
            r->r[1] = clock_rate; /* Hz, confirmed on from v3d_ensure_clock_on */
            return NULL;
        }

        case SWI_V3D_ReadIdent:
        {
            unsigned int ident[6] = { 0 };
            _kernel_oserror *err = v3d_read_ident(ident, 6);
            if (err)
                return err;
            r->r[0] = ident[0];
            r->r[1] = ident[1];
            r->r[2] = ident[2];
            r->r[3] = ident[3];
            r->r[4] = ident[4]; /* CT0CS - diagnostic, see v3d_read_ident */
            r->r[5] = ident[5]; /* CT1CS */
            return NULL;
        }

        case SWI_V3D_TestBuffer:
        {
            unsigned int phys_addr, readback, logical_addr;
            _kernel_oserror *err =
                v3d_test_buffer(&phys_addr, &readback, &logical_addr);
            if (err)
                return err;
            r->r[0] = phys_addr;
            r->r[1] = readback;
            r->r[2] = logical_addr;
            return NULL;
        }

        case SWI_V3D_ClearTest:
        {
            unsigned int phys_addr, readback;
            unsigned int bin_irq_before, render_irq_before;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    5, "V3DDriver: V3D_ClearTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            /* Captured before submission so R2/R3 report whether *this*
             * call's completion was actually seen by the interrupt
             * handler, not just cumulative totals from earlier calls -
             * temporary, stage-2b-only observability (see v3d_irq.h). */
            bin_irq_before = g_v3d_irq_bin_done_count;
            render_irq_before = g_v3d_irq_render_done_count;

            err = v3d_clear_test(&g_pmp, CLEAR_TEST_BASE_PAGE, r->r[0],
                                  &phys_addr, &readback);
            if (err)
                return err;

            r->r[0] = phys_addr;
            r->r[1] = readback;
            r->r[2] = g_v3d_irq_bin_done_count - bin_irq_before;
            r->r[3] = g_v3d_irq_render_done_count - render_irq_before;
            return NULL;
        }

        case SWI_V3D_ClearToSprite:
        {
            unsigned int phys_addr, readback;
            unsigned int bin_irq_before, render_irq_before;
            void *log_addr;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    9, "V3DDriver: V3D_ClearToSprite called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            /* See V3D_ClearTest's own comment - temporary, stage-2b-only
             * observability. Packed into one register here (unlike
             * ClearTest's separate R2/R3) since R0-R2 are already used
             * for the sprite area/readback/physical address. */
            bin_irq_before = g_v3d_irq_bin_done_count;
            render_irq_before = g_v3d_irq_render_done_count;

            err = v3d_clear_to_sprite(&g_pmp, CLEAR_SPRITE_BASE_PAGE, r->r[0],
                                       &phys_addr, &log_addr, &readback);
            if (err)
                return err;

            r->r[0] = (unsigned int) log_addr; /* sprite area - pass to OS_SpriteOp */
            r->r[1] = readback;
            r->r[2] = phys_addr;
            r->r[3] = ((g_v3d_irq_bin_done_count - bin_irq_before) != 0
                       && (g_v3d_irq_render_done_count - render_irq_before) != 0)
                      ? 1u : 0u; /* 1 = both completions were IRQ-driven */
            return NULL;
        }

        case SWI_V3D_MemSplit:
        {
            unsigned int arm_base, arm_size, vc_base, vc_size;
            _kernel_oserror *err = v3d_read_mem_split(&arm_base, &arm_size,
                                                        &vc_base, &vc_size);
            if (err)
                return err;

            r->r[0] = arm_base;
            r->r[1] = arm_size;
            r->r[2] = vc_base;
            r->r[3] = vc_size;
            return NULL;
        }

        case SWI_V3D_ClockInfo:
        {
            unsigned int state, rate, max_rate;
            _kernel_oserror *err = v3d_query_clock(&state, &rate, &max_rate);
            if (err)
                return err;

            r->r[0] = state;
            r->r[1] = rate;
            r->r[2] = max_rate;
            return NULL;
        }

        case SWI_V3D_IRQTest:
        {
            /* Real IRQ handling test - see v3d_irq.h. Claims the
             * vector, resolves+exercises the raw HAL_IRQClear call
             * (proves the address/static-base/branch sequence doesn't
             * crash, from safe non-interrupt context), then runs the
             * fully isolated live-fire cycle (unmask, HAL_IRQEnable,
             * genuinely wait, HAL_IRQDisable, re-mask - see
             * v3d_irq_test_live_cycle's own comment) - never submits
             * any GPU job, so any handler invocation at all during the
             * wait means something can trigger spuriously and must be
             * understood before real submission code ever unmasks for
             * real. Releases the vector again (this is a one-shot
             * test, not permanent wiring), and reports the claimed/
             * spurious counts observed *during the live-fire wait
             * specifically* - both should be 0. */
            _kernel_oserror *err, *cleanup_err;
            unsigned int live_claimed = 0, live_spurious = 0;

            g_v3d_irq_claimed_count = 0;
            g_v3d_irq_spurious_count = 0;

            err = v3d_irq_claim();
            if (err)
                return err; /* never claimed - nothing to release */

            if (!err) err = v3d_irq_resolve_clear();
            if (!err) err = v3d_irq_test_call_clear();
            if (!err) err = v3d_irq_test_live_cycle(&live_claimed, &live_spurious);

            /* Always release, even if something above failed - we did
             * successfully claim, so we must clean that up regardless
             * of what went wrong after. */
            cleanup_err = v3d_irq_release();
            if (!err)
                err = cleanup_err;

            if (err)
                return err;

            r->r[0] = live_claimed;
            r->r[1] = live_spurious;
            return NULL;
        }

        case SWI_V3D_ScreenInfo:
        {
            /* Read-only groundwork for the planned GraphicsV Render
             * accelerator (see README.md) - fetches the OS's own live
             * screen-mode state, nothing V3D-specific, no hardware
             * touched. Packed two values per register since there are
             * seven fields and only R0-R5 conventionally used for
             * output here; R4/R5 used to avoid clashing with the
             * driver's own dispatch (this SWI never touches r->r[6+]). */
            v3d_screen_info_t info;
            _kernel_oserror *err = v3d_read_screen_info(&info);
            if (err)
                return err;

            r->r[0] = info.mode_flags;
            r->r[1] = info.log2_bpp;
            r->r[2] = info.line_length;
            r->r[3] = info.xwind_limit;
            r->r[4] = info.ywind_limit;
            r->r[5] = info.screen_start;
            r->r[6] = info.display_start;
            r->r[7] = info.total_screen_size;
            return NULL;
        }

        case SWI_V3D_FillTest:
        {
            /* Step 2 of the planned GraphicsV Render accelerator - see
             * README.md and cl_fill.c. R0=width, R1=height, R2=colour
             * in; fills a multi-tile rectangle into a fresh test buffer
             * (not the real screen - that's step 3), returns physical
             * address (R0), row stride in bytes (R1), tiles_x (R2),
             * tiles_y (R3), and the *logical* address (R4) so a BASIC
             * test script can sample pixels directly with `!(addr)`. */
            unsigned int target_phys, stride, tiles_x, tiles_y;
            void *target_log;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    13, "V3DDriver: V3D_FillTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_fill_test(&g_pmp_fill, 0, (unsigned int) r->r[0],
                                  (unsigned int) r->r[1], (unsigned int) r->r[2],
                                  &target_phys, &target_log, &stride,
                                  &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = target_phys;
            r->r[1] = stride;
            r->r[2] = tiles_x;
            r->r[3] = tiles_y;
            r->r[4] = (unsigned int) target_log;
            return NULL;
        }

        case SWI_V3D_ScreenFillTest:
        {
            /* Step 3 of the planned GraphicsV Render accelerator - see
             * README.md and cl_fill.c's v3d_fill_screen. R0=x, R1=y,
             * R2=width, R3=height, R4=colour in. Writes DIRECTLY into
             * the real, live screen framestore - unlike every other
             * test SWI in this module, this one has a visible effect
             * on the real desktop the moment it succeeds (the Wimp may
             * also immediately redraw over it - that's expected, not a
             * bug, see README). Returns tiles_x (R0), tiles_y (R1). */
            unsigned int x = (unsigned int) r->r[0];
            unsigned int y = (unsigned int) r->r[1];
            unsigned int width = (unsigned int) r->r[2];
            unsigned int height = (unsigned int) r->r[3];
            unsigned int colour = (unsigned int) r->r[4];
            unsigned int tiles_x, tiles_y;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    17, "V3DDriver: V3D_ScreenFillTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_screen_framestore();
            if (err)
                return err;

            if (width == 0u || height == 0u ||
                x + width > g_screen_info.xwind_limit + 1u ||
                y + height > g_screen_info.ywind_limit + 1u)
            {
                static _kernel_oserror err_out_of_bounds = {
                    18, "V3DDriver: V3D_ScreenFillTest rectangle is outside "
                        "the current screen mode's bounds"
                };
                return &err_out_of_bounds;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_fill_screen(&g_pmp_fill, SCREEN_FILL_BASE_PAGE,
                                    x, y, width, height, colour,
                                    g_screen_phys_base, g_screen_info.line_length,
                                    &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = tiles_x;
            r->r[1] = tiles_y;
            return NULL;
        }

        case SWI_V3D_AccelStart:
            /* Step 4 of the planned GraphicsV Render accelerator - see
             * README.md and gfxv_accel.c. Deliberately explicit opt-in,
             * not auto-claimed on module load - see gfxv_accel.h's top
             * comment for why. No arguments, no return values. */
            return v3d_gfxv_start();

        case SWI_V3D_AccelStop:
            return v3d_gfxv_stop();

        case SWI_V3D_AccelStats:
            /* Real instrumentation for the accelerator now sitting in
             * the live Wimp/VDU graphics pipeline - see gfxv_accel.c.
             * Returns total handler invocations (R0), how many were
             * reason 13/Render (R1), how many were op 2/fill (R2), how
             * many had a genuine solid ECF (R3), how many were actually
             * accelerated (R4), and how many attempted-but-failed (R5). */
            r->r[0] = g_gfxv_call_count;
            r->r[1] = g_gfxv_render_count;
            r->r[2] = g_gfxv_fill_op_count;
            r->r[3] = g_gfxv_solid_count;
            r->r[4] = g_gfxv_claimed_count;
            r->r[5] = g_gfxv_error_count;
            return NULL;

        case SWI_V3D_AccelFailInfo:
            /* Failure diagnostics for a first live-desktop run that
             * showed a high accelerated-fill failure rate - see
             * gfxv_accel.c's own comment. Last failure's error number
             * (R0) and exact rectangle (R1-R4: left/bottom/right/top),
             * plus min/max pixel area seen among failures (R5/R6) and
             * successes (R7/R8) - 0xFFFFFFFF for a min means "none seen". */
            r->r[0] = g_gfxv_last_fail_errnum;
            r->r[1] = g_gfxv_last_fail_left;
            r->r[2] = g_gfxv_last_fail_bottom;
            r->r[3] = g_gfxv_last_fail_right;
            r->r[4] = g_gfxv_last_fail_top;
            r->r[5] = g_gfxv_fail_min_area;
            r->r[6] = g_gfxv_fail_max_area;
            r->r[7] = g_gfxv_success_min_area;
            r->r[8] = g_gfxv_success_max_area;
            return NULL;

        case SWI_V3D_CopyTest:
        {
            /* First step of op 1 (copy) - see README.md and
             * c/cl_copy.c. R0=width, R1=height in; copies from a
             * synthetic source buffer (filled with a per-pixel test
             * pattern) to a synthetic dest buffer (not the real screen
             * yet). Returns the source logical address (R0), dest
             * logical address (R1), shared stride in bytes (R2),
             * tiles_x (R3), tiles_y (R4) - a test script samples both
             * buffers directly to confirm the copy matched pixel for
             * pixel. Requires V3D_PowerOn. */
            void *src_log, *dest_log;
            unsigned int stride, tiles_x, tiles_y;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    22, "V3DDriver: V3D_CopyTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_copy_test(&g_pmp_fill, COPY_TEST_BASE_PAGE,
                                  (unsigned int) r->r[0], (unsigned int) r->r[1],
                                  &src_log, &dest_log, &stride, &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = (unsigned int) src_log;
            r->r[1] = (unsigned int) dest_log;
            r->r[2] = stride;
            r->r[3] = tiles_x;
            r->r[4] = tiles_y;
            return NULL;
        }

        case SWI_V3D_ScreenCopyTest:
        {
            /* Second step of op 1 (copy) - see README.md and
             * c/cl_copy.c's v3d_copy_screen. R0=src_x, R1=src_y,
             * R2=dest_x, R3=dest_y, R4=width, R5=height in. Writes
             * DIRECTLY into the real, live screen framestore, same
             * caution as V3D_ScreenFillTest. Non-overlapping only -
             * refuses with a clear error if the source and dest
             * rectangles overlap at all, same fail-safe philosophy as
             * everywhere else in this module (see cl_copy.h's own
             * comment on why overlap isn't handled yet). Returns
             * tiles_x (R0), tiles_y (R1). */
            unsigned int src_x = (unsigned int) r->r[0];
            unsigned int src_y = (unsigned int) r->r[1];
            unsigned int dest_x = (unsigned int) r->r[2];
            unsigned int dest_y = (unsigned int) r->r[3];
            unsigned int width = (unsigned int) r->r[4];
            unsigned int height = (unsigned int) r->r[5];
            unsigned int tiles_x, tiles_y;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    23, "V3DDriver: V3D_ScreenCopyTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_screen_framestore();
            if (err)
                return err;

            if (width == 0u || height == 0u ||
                src_x + width > g_screen_info.xwind_limit + 1u ||
                src_y + height > g_screen_info.ywind_limit + 1u ||
                dest_x + width > g_screen_info.xwind_limit + 1u ||
                dest_y + height > g_screen_info.ywind_limit + 1u)
            {
                static _kernel_oserror err_out_of_bounds = {
                    24, "V3DDriver: V3D_ScreenCopyTest rectangle is outside "
                        "the current screen mode's bounds"
                };
                return &err_out_of_bounds;
            }

            /* AABB overlap test: NOT (one rect is entirely to one
             * side of the other on either axis). */
            if (src_x < dest_x + width && dest_x < src_x + width &&
                src_y < dest_y + height && dest_y < src_y + height)
            {
                static _kernel_oserror err_overlap = {
                    25, "V3DDriver: V3D_ScreenCopyTest source and "
                        "destination rectangles overlap - not supported yet"
                };
                return &err_overlap;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_copy_screen(&g_pmp_fill, SCREEN_COPY_BASE_PAGE,
                                    src_x, src_y, dest_x, dest_y, width, height,
                                    g_screen_phys_base, g_screen_info.line_length,
                                    &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = tiles_x;
            r->r[1] = tiles_y;
            return NULL;
        }

        case SWI_V3D_AccelCopyStats:
            /* Op 1 (copy) instrumentation - see gfxv_accel.c's own
             * comment. Total op-1 calls seen (R0), how many were
             * overlapping (R1, now accelerated via the scratch-buffer
             * path rather than always skipped), how many
             * non-overlapping copies were accelerated directly (R2),
             * how many of those failed (R3), how many overlapping
             * copies were accelerated via the scratch-buffer path (R4),
             * how many of those failed or were too large (R5), and the
             * last failure's error number from either path (R6). */
            r->r[0] = g_gfxv_copy_op_count;
            r->r[1] = g_gfxv_copy_overlap_count;
            r->r[2] = g_gfxv_copy_claimed_count;
            r->r[3] = g_gfxv_copy_error_count;
            r->r[4] = g_gfxv_copy_overlap_claimed_count;
            r->r[5] = g_gfxv_copy_overlap_error_count;
            r->r[6] = g_gfxv_copy_last_fail_errnum;
            return NULL;

        case SWI_V3D_OverlapCopyTest:
        {
            /* Isolated test for the new overlap-safe copy path (see
             * README.md and c/cl_copy.c's v3d_copy_overlap_test) -
             * genuinely new territory (scratch-buffer two-pass copy),
             * tested against a synthetic buffer before ever trusting
             * it live. R0=width, R1=height, R2=shift_x, R3=shift_y in
             * - deliberately NOT required to be tile-aligned, since a
             * sub-tile shift is exactly the case a direct copy gets
             * wrong. Returns the buffer's logical address (R0), row
             * stride (R1), tiles_x (R2), tiles_y (R3) for a test
             * script to sample and verify pixel-for-pixel. */
            void *buf_log;
            unsigned int stride, tiles_x, tiles_y;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    26, "V3DDriver: V3D_OverlapCopyTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_copy_overlap_test(&g_pmp_fill, OVERLAP_COPY_TEST_BASE_PAGE,
                                          (unsigned int) r->r[0], (unsigned int) r->r[1],
                                          (unsigned int) r->r[2], (unsigned int) r->r[3],
                                          &buf_log, &stride, &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = (unsigned int) buf_log;
            r->r[1] = stride;
            r->r[2] = tiles_x;
            r->r[3] = tiles_y;
            return NULL;
        }

        case SWI_V3D_ScreenCopyOverlapTest:
        {
            /* Real-framestore counterpart to V3D_OverlapCopyTest -
             * same escalation discipline as every other real-screen
             * feature in this driver (synthetic buffer first, then
             * the real framestore, before ever trusting it live -
             * this one was already live-wired by the time this test
             * was added, given handle_render_copy already calls
             * v3d_copy_screen_overlapping; this closes that gap
             * retroactively). R0=src_x, R1=src_y, R2=dest_x, R3=dest_y,
             * R4=width, R5=height in - deliberately allowed to
             * overlap (unlike V3D_ScreenCopyTest, which refuses).
             * Writes DIRECTLY into the real, live screen framestore,
             * same caution as every other ScreenFillTest/ScreenCopyTest.
             * Returns tiles_x (R0), tiles_y (R1). */
            unsigned int src_x = (unsigned int) r->r[0];
            unsigned int src_y = (unsigned int) r->r[1];
            unsigned int dest_x = (unsigned int) r->r[2];
            unsigned int dest_y = (unsigned int) r->r[3];
            unsigned int width = (unsigned int) r->r[4];
            unsigned int height = (unsigned int) r->r[5];
            unsigned int tiles_x, tiles_y;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    27, "V3DDriver: V3D_ScreenCopyOverlapTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_screen_framestore();
            if (err)
                return err;

            if (width == 0u || height == 0u ||
                src_x + width > g_screen_info.xwind_limit + 1u ||
                src_y + height > g_screen_info.ywind_limit + 1u ||
                dest_x + width > g_screen_info.xwind_limit + 1u ||
                dest_y + height > g_screen_info.ywind_limit + 1u)
            {
                static _kernel_oserror err_out_of_bounds = {
                    24, "V3DDriver: V3D_ScreenCopyOverlapTest rectangle is "
                        "outside the current screen mode's bounds"
                };
                return &err_out_of_bounds;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_copy_screen_overlapping(&g_pmp_fill, SCREEN_COPY_OVERLAP_BASE_PAGE,
                                    src_x, src_y, dest_x, dest_y, width, height,
                                    g_screen_phys_base, g_screen_info.line_length,
                                    &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = tiles_x;
            r->r[1] = tiles_y;
            return NULL;
        }

        default:
            /* error_BAD_SWI is cmunge's own generated macro (see
             * V3DDriver.h) for exactly this case - the documented
             * correct return value for a SWI number out of range for
             * this module, not a made-up error. */
            return error_BAD_SWI;
    }
}
