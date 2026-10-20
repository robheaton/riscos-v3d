/* SPDX-License-Identifier: GPL-2.0-or-later */
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
#include "cl_common.h"
#include "gfxv_accel.h"
#include "accel_api.h"
#include "screen_base.h"
#include "beacon.h"
#include "v3d_errors.h"
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

/* Uses PMP pages 48-56 (9 pages) - well clear of ClearToSprite's range
 * (ends at 41), comfortably inside g_pmp's 64-page limit. */
#define FORMAT16_TEST_BASE_PAGE 48u

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
 * PMP_FILL_BLOCK_PAGES=4608 was picked to comfortably exceed the
 * single biggest worst-case need of any one path (~4137 pages - two
 * PMP_MAX_PAGES_PER_CALL-sized buffers plus a small scratch, e.g.
 * V3D_CopyTest's source+dest or V3D_OverlapCopyTest's bounding buffer
 * + its own scratch-buffer pass) with ~470 pages of real margin,
 * without needing to size each block individually.
 *
 * RAISED (2026-09-26, alongside PMP_MAX_PAGES_PER_CALL): before this,
 * SCREEN_COPY_BASE_PAGE hosted BOTH the tiny direct-copy scratch
 * (~41 pages) AND the overlap-copy scratch (now up to ~2089 pages) at
 * the SAME base, spilling straight into the next reservation 200
 * pages later - found by inspection while raising that cap, before it
 * became a real hardware bug. This uniform-block scheme is deliberately
 * harder to get subtly wrong the same way again.
 *
 * SHRUNK BACK DOWN (2026-09-26, hardware-diagnosed): an initial version
 * used 8192-page blocks (49152 total) - real hardware testing then hit
 * "Bad page number" on every call using OVERLAP_COPY_TEST_BASE_PAGE
 * (32768 = 2^15) and every base after it, while every base below that
 * kept working (fills and direct copies both stayed at 100% success in
 * the same test run). Never confirmed which of two real explanations
 * is the actual cause - a 16-bit-signed-style internal limit on PMP
 * page indices, or OS_DynamicArea 0 silently clamping the requested
 * max_pages below what was asked for (pmp_create's own comment already
 * flags that `out->max_pages` can differ from the request; this driver
 * never actually checked it) - because the fix is the same either way:
 * stay comfortably under 32768 total. This shrink does that (6*4608 =
 * 27648) using each path's own real worst-case need instead of a
 * padded "roughly double" guess, rather than just picking a smaller
 * arbitrary number.
 */
#define PMP_FILL_BLOCK_PAGES 4608u
#define PMP_FILL_BLOCK_COUNT 6u
/* Small dedicated slot for V3D_CopyTest16bpp (2026-09-30, 16bpp
 * support Phase 3), NOT a 7th full PMP_FILL_BLOCK_PAGES block - this
 * is a tiny, isolated synthetic-buffer test, not a full-screen
 * accelerator path like the other six blocks, so it doesn't need to
 * match their much larger size.
 *
 * 64 was a real hardware-confirmed undercount, not a guess left
 * uncorrected: a first attempt at this value hit "Bad page number",
 * and the ensure_pmp_fill diagnostic added alongside it (below)
 * proved the OS had granted the full requested total - the actual
 * problem was this constant being too small, not any OS-level
 * clamping. cl_copy.c's own scratch-caching (find_or_create_copy_
 * scratch) always claims WORST-CASE-sized buffers (sized for
 * WORST_CASE_TILES_X*Y=510 tiles) regardless of how small the actual
 * request is, by design, so later differently-sized calls at the same
 * base_page can reuse them - for a 100x80 test that's 49 pages of
 * scratch alone, plus 16 for the two (tiny, exactly-sized) test
 * buffers = 65 minimum, computed precisely and confirmed by hand
 * after the fact, not estimated in advance. 100 gives real margin
 * above that confirmed number. */
#define COPY_TEST_16BPP_EXTRA_PAGES 100u
/* Small dedicated slot for V3D_FillTest16bpp (2026-09-30, 16bpp
 * support Phase 4), same reasoning as COPY_TEST_16BPP_EXTRA_PAGES
 * above - a tiny isolated synthetic-buffer test, not a full
 * PMP_FILL_BLOCK_PAGES block. Computed precisely, not estimated: for
 * a 100x80 test (2x2 tiles of 64, padded to 128x128) the render
 * buffer is 128*128*2 = 32768 bytes = 8 pages, and cl_fill.c's own
 * fill-scratch cache (find_or_create_fill_scratch) claims the SAME
 * worst-case 49 pages as cl_copy.c's copy scratch (identical
 * WORST_CASE_TILES_X/Y constants in both files) regardless of actual
 * request size - 8 + 49 = 57 minimum. 100 gives the same real margin
 * COPY_TEST_16BPP_EXTRA_PAGES already uses above. */
#define FILL_TEST_16BPP_EXTRA_PAGES 100u

/*
 * Minimal manual decimal formatting for the one diagnostic below that
 * needs it (page counts) - matches this codebase's existing "no
 * <string.h>, no libc formatting" convention (see module.c's own
 * cmd_tail_has_word, cl_clear.c's append_str/append_hex32 for hex).
 * Bounds-checked against errmess's real 252-byte size (kernel.h) even
 * though this specific message is always short and fixed-shape.
 */
static unsigned int append_str_bounded(char *dst, unsigned int pos, const char *s)
{
    while (*s && pos < 251u)
        dst[pos++] = *s++;
    return pos;
}

static unsigned int append_uint_decimal(char *dst, unsigned int pos, unsigned int v)
{
    char digits[10];
    unsigned int n = 0;

    if (v == 0u)
    {
        if (pos < 251u) dst[pos++] = '0';
        return pos;
    }
    while (v > 0u && n < 10u)
    {
        digits[n++] = (char) ('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0u && pos < 251u)
        dst[pos++] = digits[--n];
    return pos;
}

/* Set once creating the V3DFill area has failed, so a failing create is
 * not repeated (each attempt used to leave another Dynamic Area behind and
 * broadcast Service_MemoryMoved) from inside every later GraphicsV call. */
static int g_pmp_fill_failed = 0;

static _kernel_oserror *ensure_pmp_fill(void)
{
    static _kernel_oserror err_failed_earlier = {
        V3D_ERR(32), "V3DDriver: creating the V3DFill Dynamic Area failed earlier - "
            "acceleration unavailable until the module is reloaded"
    };
    _kernel_oserror *err;

    if (g_pmp_fill_created)
        return NULL;
    if (g_pmp_fill_failed)
        return &err_failed_earlier;

    /* PMP logical-window reservation is cheap (no real memory
     * committed until pages are actually claimed via pmp_alloc), so
     * there's no cost to sizing this generously. Total is the six
     * regular blocks plus the two small dedicated 16bpp-test slots
     * (see their own comments for why those aren't matching full
     * blocks). */
    {
        unsigned int requested_pages = PMP_FILL_BLOCK_COUNT * PMP_FILL_BLOCK_PAGES
                                        + COPY_TEST_16BPP_EXTRA_PAGES
                                        + FILL_TEST_16BPP_EXTRA_PAGES;

        err = pmp_create("V3DFill", requested_pages * PAGE_SIZE,
                          requested_pages, &g_pmp_fill);
        if (err)
        {
            g_pmp_fill_failed = 1;
            return err;
        }

        /* CHECKED, not assumed (2026-09-30) - this project's own history
         * already flagged this exact gap: OS_DynamicArea 0 can silently
         * grant FEWER pages than requested (pmp_create's own docs note
         * the returned value, regs.r[9]/g_pmp_fill.max_pages, can
         * differ), and until now nothing here ever compared it against
         * what was actually asked for - a previous session only found a
         * working total (27648) by trial and error after a "Bad page
         * number" failure, without confirming the REAL limit. Proved
         * useful immediately: when COPY_TEST_16BPP_EXTRA_PAGES was
         * first undersized, this check did NOT fire, directly proving
         * the OS had granted the full request and the real bug was
         * elsewhere (that constant's own value) - see its comment. */
        if (g_pmp_fill.max_pages < requested_pages)
        {
            /* Built dynamically, not a fixed static string - the whole
             * point of this diagnostic is to report the REAL numbers
             * rather than send us back to guessing. */
            static _kernel_oserror err_clamped;
            unsigned int pos = 0;

            err_clamped.errnum = V3D_ERR(31);
            pos = append_str_bounded(err_clamped.errmess, pos,
                                      "V3DDriver: V3DFill area granted only ");
            pos = append_uint_decimal(err_clamped.errmess, pos, g_pmp_fill.max_pages);
            pos = append_str_bounded(err_clamped.errmess, pos, " pages, requested ");
            pos = append_uint_decimal(err_clamped.errmess, pos, requested_pages);
            err_clamped.errmess[pos] = '\0';

            /* Do not leave the half-sized area behind (v0.06 did, and
             * created another on every later call). */
            (void) pmp_destroy(&g_pmp_fill);
            g_pmp_fill_failed = 1;
            return &err_clamped;
        }
    }

    g_pmp_fill_created = 1;
    return NULL;
}

#define FILL_TEST_BASE_PAGE            (0u * PMP_FILL_BLOCK_PAGES) /* V3D_FillTest */
#define SCREEN_FILL_BASE_PAGE          (1u * PMP_FILL_BLOCK_PAGES) /* V3D_ScreenFillTest / accel fill */
#define COPY_TEST_BASE_PAGE            (2u * PMP_FILL_BLOCK_PAGES) /* V3D_CopyTest */
#define SCREEN_COPY_BASE_PAGE          (3u * PMP_FILL_BLOCK_PAGES) /* V3D_ScreenCopyTest / accel direct copy */
#define OVERLAP_COPY_TEST_BASE_PAGE    (4u * PMP_FILL_BLOCK_PAGES) /* V3D_OverlapCopyTest */
#define SCREEN_COPY_OVERLAP_BASE_PAGE  (5u * PMP_FILL_BLOCK_PAGES) /* V3D_ScreenCopyOverlapTest / accel overlap copy */
#define COPY_TEST_16BPP_BASE_PAGE      (6u * PMP_FILL_BLOCK_PAGES) /* V3D_CopyTest16bpp */
#define FILL_TEST_16BPP_BASE_PAGE      (COPY_TEST_16BPP_BASE_PAGE + COPY_TEST_16BPP_EXTRA_PAGES) /* V3D_FillTest16bpp */

/*
 * Step 3 of the planned GraphicsV Render accelerator: caches the
 * screen's confirmed physical framestore base (see
 * v3d_get_framestore_phys's own comment for why this is
 * checked for real, not assumed) plus the mode info needed to target
 * it, computed once and reused - re-verifying contiguity across the
 * whole screen on every single fill call would be wasteful.
 *
 * UPDATE (2026-09-30): "the mode isn't expected to change without a
 * module reload anyway" (the original reasoning here) turned out to
 * be a real gap, not a safe assumption - a live screen mode switch
 * while this module stayed loaded would have silently kept targeting
 * the OLD geometry. v3d_service_handler (below) now clears
 * g_screen_framestore_verified on Service_ModeChange, so the next real
 * accelerated call re-derives everything from scratch instead of
 * trusting a stale cache - this flag is no longer "verify once for the
 * module's whole lifetime", just "verify once per mode".
 */
/*
 * v0.07: the cache is now also (a) invalidated from the GraphicsV vector
 * itself, (b) negative-caching for errors that cannot change without a
 * mode change, and (c) aware of which bank the VDU is WRITING to.
 *
 * (a) The kernel clears the screen for a new mode (ModeChangeSub's
 * "BL FF") BEFORE it issues Service_ModeChange, so invalidating on that
 * service alone is too late: the new mode's CLS arrived at the
 * accelerator with the OLD mode's stride, height and base. 1280x720 to
 * 1920x1080 wrapped the Y flip and aimed an 8MB fill at an address ~1.8MB
 * below the framebuffer. What DOES precede the CLS is GraphicsV 6
 * (SetDMAAddress) with R0=3 - the kernel tells the driver where VDU output
 * now goes - so gfxv_accel.c reports that here (v3d_accel_note_vrender),
 * and the cache is rebuilt from the already-updated VDU variables on the
 * next Render. Service_PreModeChange (&4D) invalidates as well, so nothing
 * between the two can be served from the old mode.
 *
 * (b) A mode this driver cannot handle (8bpp, say) used to cost an
 * OS_ReadVduVariables SWI on every single Render, forever. The error is
 * now remembered until the next invalidation.
 *
 * (c) Rendering was always aimed at the framestore base GraphicsV 9
 * reports. BCMVideo's own Render uses the address from GraphicsV 6 R0=3
 * instead - the physical address of the bank the VDU writes to - which
 * differs whenever a program double-buffers with OS_Byte 112/113.
 * Observing that call gives the real target. Until one has been seen, a
 * screen whose write bank and display bank differ is declined rather than
 * guessed at.
 *
 * (d) 0.12: the write address belongs to ONE framebuffer. It is forgotten at the start of
 * every mode change (v3d_accel_mode_changing: GraphicsV 2 and Service_PreModeChange), because
 * the display driver then allocates a new framebuffer, usually somewhere else. Without that,
 * a desktop mode change left the old address in force: every fill was declined (error 35)
 * until the next mode change - found on the hardware by ModeChangeTest, whose first probe
 * ran in a mode the desktop had been switched to after the module was loaded. See
 * screen_base.h, which holds the rule and is tested on the host.
 */
static int g_screen_framestore_verified = 0;
static _kernel_oserror *g_screen_cached_err = NULL; /* static blocks only */
static v3d_screen_info_t g_screen_info;
static unsigned int g_screen_phys_base = 0;
static unsigned int g_screen_fs_base = 0;
static unsigned int g_screen_fs_size = 0;
static v3d_vrender_t g_vrender = { 0, 0 };

/* Wrap-safe: is [start, start+len) inside 0..limit inclusive, len >= 1? */
static inline int span_ok(unsigned int start, unsigned int len, unsigned int limit)
{
    return len != 0u && start <= limit && len - 1u <= limit - start;
}

void v3d_accel_screen_invalidate(void)
{
    g_screen_framestore_verified = 0;
    g_screen_cached_err = NULL;
}

void v3d_accel_note_vrender(unsigned int phys)
{
    v3d_vrender_note(&g_vrender, phys);
    v3d_accel_screen_invalidate();
}

/* A mode change is starting (GraphicsV 2, Service_PreModeChange): the display driver is about
 * to allocate a new framebuffer, so a write address learned for the old one is wrong for it.
 * Forget it - the kernel's report for the new mode, if one comes, replaces it, and if none
 * comes the framestore base is used. See screen_base.h. */
void v3d_accel_mode_changing(void)
{
    v3d_vrender_forget(&g_vrender);
    v3d_accel_screen_invalidate();
}

static _kernel_oserror *ensure_screen_framestore(void)
{
    static _kernel_oserror err_bad_bpp = {
        V3D_ERR(16), "V3DDriver: only 32bpp and 16bpp screen modes are supported"
    };
    static _kernel_oserror err_bad_geom = {
        V3D_ERR(33), "V3DDriver: the screen mode's geometry is inconsistent "
            "(line length / height exceed the framestore) - declined"
    };
    static _kernel_oserror err_bank = {
        V3D_ERR(34), "V3DDriver: the VDU write bank differs from the display bank "
            "and its address has not been seen yet - declined"
    };
    static _kernel_oserror err_vrender = {
        V3D_ERR(35), "V3DDriver: the VDU write address lies outside the "
            "framestore - declined"
    };
    _kernel_oserror *err;
    unsigned int fs_base, fs_size, base;
    unsigned long long vis_bytes;

    if (g_screen_framestore_verified)
        return g_screen_cached_err;

    err = v3d_read_screen_info(&g_screen_info);
    if (err)
        return err; /* an OS error, possibly transient - not remembered */

    /* This function only verifies/caches GEOMETRY, which is equally valid
     * to compute for either depth. Whether a given operation is ready for
     * the active depth is for its caller to say (screen_bpp_params). */
    if (g_screen_info.log2_bpp != 5u && g_screen_info.log2_bpp != 4u)
    {
        g_screen_cached_err = &err_bad_bpp;
        g_screen_framestore_verified = 1;
        return g_screen_cached_err;
    }

    err = v3d_get_framestore_phys(&g_screen_info, &fs_base, &fs_size);
    if (err)
        return err;

    /* The rectangle checks in v3d_accel_render_* trust xwind_limit and
     * ywind_limit, so make sure those describe memory that exists. */
    vis_bytes = (unsigned long long) (g_screen_info.ywind_limit + 1u) *
                 (unsigned long long) g_screen_info.line_length;
    g_screen_fs_size = fs_size;
    if (((unsigned long long) (g_screen_info.xwind_limit + 1u) <<
          (g_screen_info.log2_bpp - 3u)) > g_screen_info.line_length ||
        vis_bytes > fs_size)
    {
        g_screen_cached_err = &err_bad_geom;
        g_screen_framestore_verified = 1;
        return g_screen_cached_err;
    }

    switch (v3d_choose_base(&g_vrender, fs_base, fs_size, vis_bytes,
                            g_screen_info.screen_start, g_screen_info.display_start, &base))
    {
        case V3D_BASE_ERR_VRENDER:
            g_screen_cached_err = &err_vrender;
            g_screen_framestore_verified = 1;
            return g_screen_cached_err;
        case V3D_BASE_ERR_BANK:
            g_screen_cached_err = &err_bank;
            g_screen_framestore_verified = 1;
            return g_screen_cached_err;
        default:
            break;
    }

    g_screen_fs_base = fs_base;
    g_screen_phys_base = base;
    g_screen_cached_err = NULL;
    g_screen_framestore_verified = 1;
    return NULL;
}

/*
 * Copy and fill's shared per-depth parameters (2026-09-30, 16bpp
 * support) - the two values every accelerated copy or fill needs
 * beyond plain geometry: bytes/pixel for address arithmetic
 * (x/y are PIXEL coordinates, not byte offsets) and the V3D
 * render-target format code for the Load/Store packets themselves.
 *
 * CORRECTED (2026-09-30, same day): this hardware's real 16bpp screen
 * mode is genuine RGB565 (5-6-5, all 16 bits used for colour), NOT the
 * RGB555 (5-5-5 + 1 reserved bit) originally assumed from the classic
 * RISC OS PRM sprite-format doc - confirmed directly from the real
 * Raspberry Pi BCMVideo HAL driver source
 * (RiscOS/Sources/Video/HWSupport/BCMVideo/s/GraphicsV: "16bpp is
 * RGB565, i.e. RRRRRGGGGGGBBBBB"), whose firmware framebuffer
 * (dispmanx) is hardwired to 565 - this is also why "32 thousand
 * colours" is greyed out/unavailable in the mode chooser on this
 * hardware, it was never actually offered. Format 7 (BGR565, the only
 * 565-shaped entry in the V3D render-target format enum) replaces the
 * original 45 (RGBA5551) here - see gfxv_accel.c's widen_565_to_8bit_
 * rgba for the matching ECF-decode fix. Still used for COPY as well as
 * FILL even though copy's correctness never actually depended on which
 * specific format was declared (a Load+Store round trip through the
 * SAME format code is bit-exact regardless of the format's real
 * semantic shape, already hardware-proven with format 45 via
 * CopyTest16bpp.bas's non-colour test pattern) - kept unified so
 * "what format is this screen" has one single source of truth, not two
 * that could silently drift apart. Only ever called after a successful
 * ensure_screen_framestore(), which already restricts
 * g_screen_info.log2_bpp to 4 or 5 - the error case here is a
 * defensive backstop, not expected to actually trigger.
 */
static _kernel_oserror *screen_bpp_params(unsigned int *bytes_per_pixel_out,
                                           unsigned int *format_code_out)
{
    if (g_screen_info.log2_bpp == 5u)
    {
        *bytes_per_pixel_out = 4u;
        *format_code_out = 27u; /* RGBA8 */
        return NULL;
    }
    if (g_screen_info.log2_bpp == 4u)
    {
        *bytes_per_pixel_out = 2u;
        *format_code_out = 7u; /* BGR565 - see this function's own comment */
        return NULL;
    }
    {
        static _kernel_oserror err_bad_bpp = {
            V3D_ERR(29), "V3DDriver: unsupported screen depth for accelerated copy/fill"
        };
        return &err_bad_bpp;
    }
}

/*
 * Service_ModeChange (&46) handler (2026-09-30) - see this file's
 * `service-call-handler` directive in V3DDriver.cmhg. Confirmed
 * against the real PRM (riscos.com/support/developers/prm/vdu.html,
 * fetched 2026-09-30), not guessed: fires AFTER a screen mode switch
 * has already completed (unlike Service_PreModeChange/&4D, fired
 * before - not used here, nothing to veto), and "you should not claim
 * this service call" - it's a pure notification, every registered
 * handler sees it regardless of what others do. Deliberately does
 * nothing V3D-hardware-side itself: just invalidates the cache
 * ensure_screen_framestore already knows how to safely rebuild on the
 * next real accelerated call, using whatever the NEW mode's geometry
 * turns out to be. cmunge's generated signature is plain `void`, NOT
 * `void *` - confirmed the hard way against a real trial build
 * (cmhg/V3DDriver.h) after the secondhand docs said otherwise; "never
 * claim" for this service call means simply doing nothing to R1, not
 * returning any particular value - there's no return-value-based
 * claim/pass-on convention here the way VECTOR_CLAIM/VECTOR_PASSON
 * works for vector-handlers.
 */
void v3d_service_handler(int service_number, _kernel_swi_regs *r, void *pw)
{
    (void) r;
    (void) pw;

    /* &46 Service_ModeChange (after the switch) and &4D
     * Service_PreModeChange (before it) - see the screen cache comment
     * above for why the vector hook, not these, is what protects the
     * mode-change CLS. */
    if (service_number == 0x4D)
        v3d_accel_mode_changing();    /* before: forget the old framebuffer's write address */
    else if (service_number == 0x46)
        v3d_accel_screen_invalidate(); /* after: rebuild from the new mode's variables */
}

/*
 * OS_ReadMonotonicTime (SWI &42) is a plain, standard, non-reentrant-
 * risk SWI - returns centiseconds since boot in R0. Used to track real
 * accelerated-call timing (see V3D_AccelTimingStats below).
 */
unsigned int v3d_read_centiseconds(void)
{
    _kernel_swi_regs regs;
    regs.r[0] = 0; /* OS_ReadMonotonicTime takes no input; set so the compiler can see nothing is read unset */
    if (_kernel_swi(0x42, &regs, &regs))
        return 0u; /* diagnostic only - never let this become a new
                     * source of failure */
    return (unsigned int) regs.r[0];
}

static volatile unsigned int g_max_call_duration_us = 0;

/*
 * Real accelerated-call timing, readable via V3D_AccelTimingStats.
 * `g_calls_this_window`/`g_window_start_us` track a simple 1-second
 * sliding window of accelerated-call counts; `g_peak_calls_per_second`
 * remembers the worst burst rate seen all session.
 *
 * v0.07: times come from v3d_time_us() - one register read of the 1MHz
 * system timer - instead of two OS_ReadMonotonicTime SWIs per call at
 * 10ms resolution (so "worst call" could never read below 10ms).
 */
static unsigned int g_window_start_us = 0;
static unsigned int g_calls_this_window = 0;
volatile unsigned int g_peak_calls_per_second = 0;

static void record_call_duration(unsigned int t_start)
{
    unsigned int t_end = v3d_time_us();
    unsigned int elapsed = t_end - t_start; /* unsigned wraparound is
                                              * harmless here - worst
                                              * case one spuriously huge
                                              * reading on the rare
                                              * wrap, self-corrects next
                                              * call */
    if (elapsed > g_max_call_duration_us)
        g_max_call_duration_us = elapsed;

    if (t_end - g_window_start_us >= 1000000u) /* new 1-second window */
    {
        g_window_start_us = t_end;
        g_calls_this_window = 0;
    }
    g_calls_this_window++;
    if (g_calls_this_window > g_peak_calls_per_second)
        g_peak_calls_per_second = g_calls_this_window;
}

/*
 * Is the V3D ALREADY powered, out of reset and bridged - judged from the hardware itself?
 *
 * Found on real hardware 2026-10-09 (ReloadProbe photos): the firmware's GET_CLOCK_STATE for the V3D clock reads 0
 * ("off") even while the V3D is running happily - after a complete first power-on, and after fifty jobs. So the
 * "firmware clock already on" test that v3d_power_on used to choose between the warm and the cold path was
 * ALWAYS false: every first PowerOn of every module instance took the COLD path, which pulses PM_GRAFX reset on the
 * V3D with its ASB bus gates OPEN (Linux stops the slave and then the master gate BEFORE asserting reset; the cold
 * path never does, because on a fresh boot they are already stopped). On a reloaded module that left the slave
 * bridge FULL and the core ignoring writes: a soft timeout at the first job, or a bus stall - the "reload freeze".
 *
 * The three things a genuinely powered V3D shows, none of which needs the firmware's opinion:
 *   - PM_GRAFX has V3DRSTN set (reset released; a fresh boot reads reset HELD),
 *   - neither ASB gate has REQ_STOP or ACK set (a fresh boot reads both stopped),
 *   - the core's IDENT0 reads the V3D magic 'V','3','D' (a powered-down V3D answers 0xDEADBEEF).
 * Each is checked in that order and every read is of a block known to answer without hanging in that state.
 */
static int v3d_hw_is_up(void)
{
    io_mapping_t m;
    unsigned int grafx, asb_m, asb_s, ident;

    if (io_map(PM_MB_BASE, &m))
        return 0;
    grafx = IO_WORD(m, PM_GRAFX);
    io_unmap(&m);
    if (!(grafx & PM_V3DRSTN))
        return 0;

    if (io_map(V3D_MB_BASE, &m))
        return 0;
    asb_m = IO_WORD(m, ASB_V3D_M_CTRL);
    asb_s = IO_WORD(m, ASB_V3D_S_CTRL);
    if ((asb_m | asb_s) & (ASB_REQ_STOP | ASB_ACK))
    {
        io_unmap(&m);
        return 0;
    }
    ident = IO_WORD(m, V3D_CTL_IDENT0);
    io_unmap(&m);
    return (ident & 0x00FFFFFFu) == 0x00443356u;
}

#ifndef V3D_BEACONS
#define g_v3d_warm_detect 1u /* the diagnostic build switches this with the load word WARM */
#define g_v3d_cold_unsafe 0u /* the diagnostic build's load word OLDCOLD skips v3d_stop_bridge, to reproduce the old bug */
#endif

/*
 * Before the cold path asserts reset, make sure the ASB bus gates are STOPPED - slave first, then master, each with
 * the PM password and a wait for its ACK - exactly as Linux's bcm2835_asb_power_off does before it asserts reset
 * (drivers/pmdomain/bcm/bcm2835-power.c). On a fresh boot both gates are already stopped (REQ_STOP and ACK set), so
 * this does nothing and the proven power-on sequence is unchanged. It matters when the cold path runs on a V3D that
 * is up behind OPEN gates - which v3d_power_on only reaches now if v3d_hw_is_up() did not recognise it (an
 * unexpected ident, a half-powered state): resetting a live V3D behind an open bridge is what jammed the slave
 * bridge in the reload freeze. If a gate will not stop, nothing is reset and the user is told to reboot.
 */
static _kernel_oserror *v3d_stop_bridge(void)
{
    static _kernel_oserror err_no_stop = {
        V3D_ERR(52), "V3DDriver: a V3D bus gate did not stop, so the V3D is not being reset - reboot"
    };
    io_mapping_t m;
    _kernel_oserror *err;
    unsigned int spin, reg;
    static const unsigned int order[2] = { ASB_V3D_S_CTRL, ASB_V3D_M_CTRL }; /* slave, then master */
    unsigned int i;

    if (g_v3d_cold_unsafe)
        return NULL;

    err = io_map(V3D_MB_BASE, &m);
    if (err)
        return err;

    if ((IO_WORD(m, ASB_V3D_S_CTRL) & ASB_REQ_STOP) && (IO_WORD(m, ASB_V3D_M_CTRL) & ASB_REQ_STOP))
    {
        io_unmap(&m);
        return NULL; /* already stopped: a fresh boot */
    }

    for (i = 0; i < 2u; i++)
    {
        reg = order[i];
        IO_WORD(m, reg) = PM_PASSWORD | (IO_WORD(m, reg) | ASB_REQ_STOP);
        for (spin = 0; !(IO_WORD(m, reg) & ASB_ACK); spin++)
        {
            if (spin > 100000u)
            {
                /* put back what was stopped, newest first, and refuse */
                unsigned int j;
                for (j = i + 1u; j-- > 0u; )
                    IO_WORD(m, order[j]) = PM_PASSWORD | (IO_WORD(m, order[j]) & ~ASB_REQ_STOP);
                io_unmap(&m);
                return &err_no_stop;
            }
        }
    }

    io_unmap(&m);
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
 * actually need it" - so the hardware has to be asked.
 *
 * THE FIRST ATTEMPT AT THAT ASKED THE WRONG THING (found 2026-10-09).
 * It used v3d_ensure_clock_on()'s report of whether the FIRMWARE's clock
 * flag was already on, believing that was "real, instance-independent
 * hardware truth". It is not: on a Pi 4 the firmware reports the V3D
 * clock as OFF (state 0) all the time - on a fresh boot, and equally
 * while the V3D is up and running its jobs - so that test was never
 * true, the warm path never ran, and every first PowerOn of every module
 * instance reset-pulsed a running V3D with its bus gates open. That is
 * the reload freeze. v3d_hw_is_up() (above) now asks the registers
 * themselves, and if the V3D is already up there is no power-on to do.
 * See README.md, "The reload freeze: root-caused and fixed".
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

    v3d_beacon(B_PO_ENTER);

    /* --- already up? Ask the hardware, not the firmware flag (see v3d_hw_is_up). If so there is nothing to
     * power on, and above all no reset pulse: report the clock rate the firmware gives and carry on. */
    if (g_v3d_warm_detect && v3d_hw_is_up())
    {
        unsigned int st, rate, mx;

        err = v3d_query_clock(&st, &rate, &mx);
        if (err)
            return err;
        if (!rate)
            rate = 250000000u; /* the firmware's default; only reported, never programmed */
        g_powered_on = 1;
        g_v3d_clock_rate = rate;
        *clock_rate_out = rate;
        v3d_beacon(B_PO_RETURN);
        return NULL;
    }

    /* --- firmware clock gate: confirmed root cause of every CLE hang
     * logged so far (2026-09-26 hardware test: V3D_ClockInfo reported
     * state=0, clock gated off) - see v3d_clock.h. Must happen before
     * anything else touches V3D's own registers, not just before the
     * CLE is used: a gated block can still be read/written (explaining
     * why ReadIdent/TestBuffer always worked regardless), so this was
     * never going to show up as a register-access failure. */
    v3d_beacon(B_PO_CLK_BEFORE);
    err = v3d_ensure_clock_on(&clock_rate, &clock_already_on);
    if (err)
        return err;
    v3d_beacon(B_PO_CLK_AFTER);

    if (clock_already_on)
    {
        /* The firmware's "clock on" flag is only a REQUEST state, not proof
         * that reset has been released and the bus gates are open (Linux's
         * clk-raspberrypi notes it "doesn't actually power off the clock",
         * and a PowerOn that failed part-way leaves the clock on with the
         * gates still shut). Trusting it blindly meant the next call would
         * drive the CLE through closed gates. So look at the gates
         * themselves: after a complete power-on both ASB controls read
         * REQ_STOP=0 and ACK=0 (that is exactly what the sequence below
         * waits for). If they do not, refuse - re-running the reset pulse
         * on a core in an unknown state is the thing that froze machines
         * before, so a clear error and a reboot is the safe answer. */
        io_mapping_t chk;
        unsigned int asb_m, asb_s;
        static _kernel_oserror err_asb_closed = {
            V3D_ERR(45), "V3DDriver: the V3D clock is on but its bus gates are not "
                "open (a previous power-on did not complete) - reboot"
        };

        v3d_beacon(B_PO_MAP_BEFORE);
        err = io_map(V3D_MB_BASE, &chk);
        if (err)
            return err;
        v3d_beacon(B_PO_MAP_AFTER);
        asb_m = IO_WORD(chk, ASB_V3D_M_CTRL);
        v3d_beacon(B_PO_ASBM_READ);
        asb_s = IO_WORD(chk, ASB_V3D_S_CTRL);
        v3d_beacon(B_PO_ASBS_READ);
        io_unmap(&chk);
        v3d_beacon(B_PO_UNMAP_AFTER);

        if ((asb_m | asb_s) & (ASB_REQ_STOP | ASB_ACK))
            return &err_asb_closed;

        g_powered_on = 1;
        g_v3d_clock_rate = clock_rate;
        *clock_rate_out = clock_rate;
        v3d_beacon(B_PO_RETURN);
        return NULL;
    }

    /* --- PM_GRAFX: assert then deassert V3D reset (real pulse) --- */
    err = v3d_stop_bridge();
    if (err)
        return err;

    v3d_beacon(B_PO_RESET_BEFORE);
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
    v3d_beacon(B_PO_RESET_AFTER);

    /* --- ASB master/slave gates (same megabyte as V3D_HUB/CORE0) --- */
    v3d_beacon(B_PO_ASB_BEFORE);
    err = io_map(V3D_MB_BASE, &v3d);
    if (err)
        return err;

    IO_WORD(v3d, ASB_V3D_M_CTRL) = IO_WORD(v3d, ASB_V3D_M_CTRL) & ~ASB_REQ_STOP;
    for (spin = 0; IO_WORD(v3d, ASB_V3D_M_CTRL) & ASB_ACK; spin++)
    {
        if (spin > 1000000)
        {
            static _kernel_oserror err_asb_m = {
                V3D_ERR(2), "V3DDriver: timed out waiting for ASB master gate ACK "
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
                V3D_ERR(3), "V3DDriver: timed out waiting for ASB slave gate ACK "
                   "to clear during V3D power-on"
            };
            io_unmap(&v3d);
            return &err_asb_s;
        }
    }

    io_unmap(&v3d);
    v3d_beacon(B_PO_ASB_AFTER);

    g_powered_on = 1;
    g_v3d_clock_rate = clock_rate;
    *clock_rate_out = clock_rate;
    v3d_beacon(B_PO_RETURN);
    return NULL;
}

/*
 * Accessor (2026-09-30, 16bpp fill support) letting gfxv_accel.c learn
 * the screen's active depth BEFORE it can even decode a GraphicsV
 * Render fill's ECF block - log2_bpp==4 packs two sub-pixels per
 * 32-bit OR/EOR word, log2_bpp==5 packs exactly one (see
 * gfxv_accel.c's ecf_decode_striped). v3d_accel_render_fill/
 * _striped_fill already handle either depth internally via
 * screen_bpp_params, but that's only reachable AFTER the ECF has
 * already been decoded into a colour, so the depth is needed one step
 * earlier than that. Self-initialises via ensure_screen_framestore
 * exactly like every other accel entry point - safe to call before
 * V3D_PowerOn or any other setup.
 */
_kernel_oserror *v3d_accel_get_screen_log2_bpp(unsigned int *log2_bpp_out)
{
    _kernel_oserror *err = ensure_screen_framestore();
    if (err)
        return err;

    *log2_bpp_out = g_screen_info.log2_bpp;
    return NULL;
}

/*
 * Everything an accelerated operation needs in place before it can build a
 * job, in the order it is needed. Shared by all four entry points below
 * (they used to carry five copy-pasted calls each).
 */
static _kernel_oserror *accel_prepare(unsigned int *bytes_per_pixel,
                                       unsigned int *format_code)
{
    unsigned int clock_rate;
    _kernel_oserror *err;

    err = v3d_gpu_check();
    if (err)
        return err;

    err = v3d_power_on(&clock_rate);
    if (err)
        return err;

    err = ensure_screen_framestore();
    if (err)
        return err;

    err = screen_bpp_params(bytes_per_pixel, format_code);
    if (err)
        return err;

    err = ensure_pmp_fill();
    if (err)
        return err;

    return v3d_irq_ensure_ready();
}

static _kernel_oserror err_rect_range = {
    V3D_ERR(43), "V3DDriver: rectangle lies outside the screen - declined"
};

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
    unsigned int width, height, y_top_down;
    unsigned int tiles_x, tiles_y;
    unsigned int bytes_per_pixel, format_code;
    unsigned int t_start;
    _kernel_oserror *err;

    err = accel_prepare(&bytes_per_pixel, &format_code);
    if (err)
        return err;

    /* Validated here, against the geometry this module itself verified -
     * not left to the kernel's own clipping. v0.06 trusted its caller
     * completely, and the mode-change CLS showed that a rectangle which
     * does not belong to the cached mode turns into an arbitrary-address
     * write (the Y flip below wraps). */
    if (right < left || top < bottom)
        return &err_rect_range;
    width = right - left + 1u;
    height = top - bottom + 1u;
    if (!span_ok(left, width, g_screen_info.xwind_limit) ||
        !span_ok(bottom, height, g_screen_info.ywind_limit))
        return &err_rect_range;

    y_top_down = g_screen_info.ywind_limit - top;

    t_start = v3d_time_us();
    err = v3d_fill_screen(&g_pmp_fill, SCREEN_FILL_BASE_PAGE,
                             left, y_top_down, width, height, colour_rgba,
                             g_screen_phys_base, g_screen_info.line_length,
                             bytes_per_pixel, format_code,
                             &tiles_x, &tiles_y);
    if (!err)
        record_call_duration(t_start);
    return err;
}

/*
 * Dithered/patterned twin of v3d_accel_render_fill (see README's
 * "Dithered/patterned fills" section and gfxv_accel.c's
 * ecf_decode_striped) - same self-initialisation and coordinate-flip
 * as the solid case, but delegates to v3d_striped_fill_raw instead of
 * v3d_fill_screen, computing the rectangle's own row-0 physical address
 * the same way v3d_fill_screen does internally (screen_phys_base +
 * y_top_down*stride + left*4) since v3d_striped_fill_raw needs that
 * address directly, not an x/y pair.
 *
 * `row_phase` is passed as `y_top_down % 8u` - i.e. the rectangle's
 * absolute on-screen top-down row number, not anything rectangle-
 * relative - so the pattern stays anchored to the SCREEN as content
 * moves/redraws, matching RISC OS's own dither-pattern convention (see
 * cl_fill.h's v3d_striped_fill_raw comment for why this matters).
 *
 * Shares SCREEN_FILL_BASE_PAGE with the solid-fill path, not a
 * dedicated base page: both paths' scratch buffers are used strictly
 * sequentially (a real GraphicsV Render call always runs to completion,
 * SVC/foreground-only, before any other could start - see gfxv_accel.h),
 * so there is no real concurrent use to guard against, and the solid
 * path's own scratch is already sized for a full-screen rectangle,
 * comfortably larger than any single stripe's fill needs.
 */
_kernel_oserror *v3d_accel_render_striped_fill(unsigned int left, unsigned int bottom,
                                                 unsigned int right, unsigned int top,
                                                 const unsigned int colours[8])
{
    unsigned int width, height, y_top_down;
    unsigned int target_phys_row0;
    unsigned int bytes_per_pixel, format_code;
    unsigned int t_start;
    _kernel_oserror *err;

    err = accel_prepare(&bytes_per_pixel, &format_code);
    if (err)
        return err;

    if (right < left || top < bottom)
        return &err_rect_range;
    width = right - left + 1u;
    height = top - bottom + 1u;
    if (!span_ok(left, width, g_screen_info.xwind_limit) ||
        !span_ok(bottom, height, g_screen_info.ywind_limit))
        return &err_rect_range;

    y_top_down = g_screen_info.ywind_limit - top;

    target_phys_row0 = g_screen_phys_base + y_top_down * g_screen_info.line_length
                        + left * bytes_per_pixel;

    t_start = v3d_time_us();
    err = v3d_striped_fill_raw(&g_pmp_fill, SCREEN_FILL_BASE_PAGE,
                                  width, height, colours, y_top_down % 8u,
                                  target_phys_row0, g_screen_info.line_length,
                                  format_code);
    if (!err)
        record_call_duration(t_start);
    return err;
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
    unsigned int src_y_top_down, dest_y_top_down;
    unsigned int tiles_x, tiles_y;
    unsigned int bytes_per_pixel, format_code;
    unsigned int t_start;
    _kernel_oserror *err;

    err = accel_prepare(&bytes_per_pixel, &format_code);
    if (err)
        return err;

    if (!span_ok(src_left, width, g_screen_info.xwind_limit) ||
        !span_ok(dest_left, width, g_screen_info.xwind_limit) ||
        !span_ok(src_bottom, height, g_screen_info.ywind_limit) ||
        !span_ok(dest_bottom, height, g_screen_info.ywind_limit))
        return &err_rect_range;

    /* Each rectangle's own top edge, flipped independently - see this
     * function's own top comment. */
    src_y_top_down = g_screen_info.ywind_limit - (src_bottom + height - 1u);
    dest_y_top_down = g_screen_info.ywind_limit - (dest_bottom + height - 1u);

    t_start = v3d_time_us();
    err = v3d_copy_screen(&g_pmp_fill, SCREEN_COPY_BASE_PAGE,
                             src_left, src_y_top_down, dest_left, dest_y_top_down,
                             width, height,
                             g_screen_phys_base, g_screen_info.line_length,
                             bytes_per_pixel, format_code,
                             &tiles_x, &tiles_y);
    if (!err)
        record_call_duration(t_start);
    return err;
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
    unsigned int src_y_top_down, dest_y_top_down;
    unsigned int tiles_x, tiles_y;
    unsigned int bytes_per_pixel, format_code;
    unsigned int t_start;
    _kernel_oserror *err;

    err = accel_prepare(&bytes_per_pixel, &format_code);
    if (err)
        return err;

    if (!span_ok(src_left, width, g_screen_info.xwind_limit) ||
        !span_ok(dest_left, width, g_screen_info.xwind_limit) ||
        !span_ok(src_bottom, height, g_screen_info.ywind_limit) ||
        !span_ok(dest_bottom, height, g_screen_info.ywind_limit))
        return &err_rect_range;

    src_y_top_down = g_screen_info.ywind_limit - (src_bottom + height - 1u);
    dest_y_top_down = g_screen_info.ywind_limit - (dest_bottom + height - 1u);

    t_start = v3d_time_us();
    err = v3d_copy_screen_overlapping(&g_pmp_fill, SCREEN_COPY_OVERLAP_BASE_PAGE,
                             src_left, src_y_top_down, dest_left, dest_y_top_down,
                             width, height,
                             g_screen_phys_base, g_screen_info.line_length,
                             bytes_per_pixel, format_code,
                             &tiles_x, &tiles_y);
    if (!err)
        record_call_duration(t_start);
    return err;
}

static _kernel_oserror *v3d_read_ident(unsigned int *ident, unsigned int n)
{
    io_mapping_t v3d;
    _kernel_oserror *err;

    if (!g_powered_on)
    {
        static _kernel_oserror err_not_on = {
            V3D_ERR(4), "V3DDriver: V3D_ReadIdent called before V3D_PowerOn"
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

/*
 * Case-insensitive substring search for a fixed word in the module's
 * own *RMLoad command tail - deliberately not using strstr/strcasestr
 * (avoids depending on anything from string.h this codebase doesn't
 * already use elsewhere) and deliberately simple: this only needs to
 * recognise one fixed word at a time, not general command-line parsing.
 */
static int cmd_tail_has_word(const char *cmd_tail, const char *target)
{
    unsigned int i;

    if (!cmd_tail)
        return 0;

    for (; *cmd_tail; cmd_tail++)
    {
        for (i = 0; target[i]; i++)
        {
            char c = cmd_tail[i];
            if (c >= 'a' && c <= 'z')
                c = (char) (c - 'a' + 'A');
            if (c != target[i])
                break;
        }
        if (!target[i])
            return 1;
    }
    return 0;
}

/*
 * Diagnostic added 2026-09-28: Rob recalled a past session where a fix
 * placed in module_final appeared not to take effect, raising the
 * question of whether *RMKill genuinely invokes this entry point at
 * all (as opposed to running it but with insufficient cleanup code
 * inside it - the three separate, confirmed, already-fixed bugs here
 * historically were all the latter, not this). Nothing in git history
 * or memory confirms the entry point itself was ever skipped, but it's
 * cheap and completely safe to check directly (OS_SetVarVal, no V3D
 * hardware touched at all) rather than trust either recollection.
 * Called from module_init too, as a control: module_init is already
 * proven to run reliably (auto-start, every SWI, this whole project)
 * on every load, so if V3DDriver$InitRan gets set but
 * V3DDriver$FinalRan never does, that isolates the problem to
 * something specific about module_final's context rather than a bug
 * in this diagnostic itself. Test: *Unset the relevant variable(s),
 * reload/kill the module, then *Show V3DDriver$*Ran* - "yes" confirms
 * that entry point ran, no output confirms it didn't.
 */
static _kernel_oserror *set_ran_marker(const char *var_name)
{
    _kernel_swi_regs regs;
    regs.r[0] = (int) var_name;
    regs.r[1] = (int) "yes";
    regs.r[2] = 3; /* strlen("yes") */
    regs.r[3] = 0;
    regs.r[4] = 0; /* VarType_String */
    /* 0x24, NOT 0x20 - 0x20 is OS_Release (vector release), confirmed
     * the hard way: an earlier build using 0x20 by mistake (misremembered,
     * never actually checked against the real SWI list) surfaced as
     * "Bad vector release" the first time its error was actually
     * looked at, instead of silently discarded - see the riscos.com
     * numeric SWI list (OS_Release=0x20, OS_ReadUnsigned=0x21,
     * OS_GenerateEvent=0x22, OS_ReadVarVal=0x23, OS_SetVarVal=0x24),
     * fetched 2026-09-29, not trusted from memory a second time. */
    return _kernel_swi(0x24 /* OS_SetVarVal */, &regs, &regs);
}

_kernel_oserror *module_init(const char *cmd_tail, int podule_base, void *pw)
{
    (void) podule_base;

    v3d_beacon_configure(cmd_tail);
    v3d_beacon_prepare(B_ZLIFE);
    v3d_beacon(B_INIT_ENTER);

    /* V3DDriver$InitRan/$FinalRan (see set_ran_marker): confirmed on
     * real hardware 2026-09-29 that module_final DOES run on *RMKill -
     * both markers appeared as expected. Discarded here (like
     * module_final's own call) now that it's proven correct - a
     * diagnostic marker must never be able to block the module from
     * loading. Kept permanently, not ripped out: cheap, harmless, and
     * this exact question ("does an entry point actually run") is
     * worth being able to re-check instantly in a future session
     * without rebuilding anything. */
    (void) set_ran_marker("V3DDriver$InitRan");

    /* Defensive crash-recovery cleanup (Rob's own idea, 2026-09-29) -
     * see pmp_cleanup_stale's own comment in phys_buffer.c for the full
     * reasoning, including why it deliberately does NOT try to remove
     * the stale Dynamic Area itself, only release its pages. Must run
     * before this instance's own pmp_create calls (both lazy, on first
     * real use) - doing it unconditionally here, not lazily alongside
     * them, means it happens exactly once per load regardless of
     * whether this instance ever ends up using PMP itself. Pure OS-
     * level Dynamic Area bookkeeping, no V3D hardware touched - doesn't
     * conflict with this function's existing "stays lazy about the
     * GPU/PMP/IRQ machinery" principle below, that's specifically about
     * V3D hardware access, not general OS resource hygiene. */
    (void) pmp_cleanup_stale("V3DBuf");
    (void) pmp_cleanup_stale("V3DFill");

    /* Captured here, not discarded: v3d_irq_claim() needs this exact
     * value later - see v3d_irq_set_private_word's comment in
     * v3d_irq.h for why a raw hardware vector needs it passed
     * explicitly, unlike every other entry point into this module. */
    v3d_irq_set_private_word(pw);
    v3d_gfxv_set_private_word(pw);

    g_powered_on = 0;

    /*
     * Auto-start: claims GraphicsV immediately on load, so a plain
     * *RMLoad is sufficient for live desktop acceleration - no manual
     * V3D_AccelStart needed. A hard machine freeze under this (Task
     * Manager specifically) was found and fixed (see README's "Task
     * Manager freeze" section - a pmp_alloc/Service_MemoryMoved
     * reentrancy issue, now resolved by caching scratch page claims
     * instead of re-claiming them on every call) before this default
     * was restored.
     *
     * Opt-out: "NoAutoStart" on the command tail
     * (*RMLoad V3DDriver,ffa NoAutoStart) skips the automatic claim,
     * leaving the module in manual opt-in mode (V3D_AccelStart still
     * works normally) - useful for isolating this module during
     * testing.
     *
     * Deliberately does NOT power on the GPU/PMP/IRQ machinery here -
     * that stays lazy, deferred to the first real accelerated
     * GraphicsV call (v3d_accel_render_fill/copy already do this).
     *
     * A failure to claim (e.g. OS_Claim itself erroring) does NOT fail
     * module_init - the module still loads and its test SWIs still
     * work either way; only the automatic acceleration is unavailable,
     * recoverable later via a manual V3D_AccelStart.
     */
    if (!cmd_tail_has_word(cmd_tail, "NOAUTOSTART"))
        (void) v3d_gfxv_start();

    /* Deliberately doing nothing else here: no other hardware is
     * touched, and no IO region is mapped, until a SWI or a real
     * GraphicsV call actually asks for it. */
    v3d_beacon(B_INIT_DONE);
    return NULL;
}

/*
 * Is the GPU safe to unload under? Idle means both CLE threads stopped.
 * Waits briefly for a job that is still draining. Never touches the
 * hardware if this instance never powered it on.
 */
static int gpu_is_idle(void)
{
    io_mapping_t v3d;
    unsigned int t0, cs0, cs1;
    int idle = 0;

    if (!g_powered_on)
        return 1;
    if (g_v3d_wedged)
        return 0; /* a job timed out and may still be running */
    if (v3d_regs_map(&v3d))
        return 0;

    t0 = v3d_time_us();
    do
    {
        cs0 = IO_WORD(v3d, V3D_CLE_CT0CS);
        cs1 = IO_WORD(v3d, V3D_CLE_CT1CS);
        if (!((cs0 | cs1) & V3D_CT_CS_CTRUN))
        {
            idle = 1;
            break;
        }
    } while (v3d_time_us() - t0 < 100000u); /* 100ms */

    v3d_regs_unmap(&v3d);
    return idle;
}

/*
 * Order matters, and v0.06 had it backwards (IRQ off, THEN release the
 * vector that feeds the IRQ-dependent accelerator, then free the pages):
 *
 *   1. Make sure the GPU is idle BEFORE tearing anything down. A job that
 *      is still running owns our scratch pages; freeing them under it
 *      lets the GPU write into memory that now belongs to someone else.
 *      If it is not idle, refuse to unload (the module stays fully
 *      working) - unless `fatal`, when the machine is going down anyway.
 *   2. Release GraphicsV, so no new Render can arrive.
 *   3. Shut the interrupt down (masks, clears pending, releases).
 *   4. Remove the Dynamic Areas outright (pmp_destroy) - v0.06 only
 *      released their pages and left two zombie areas per load, with a
 *      resize-handler pointer into the RMA being freed. If the GPU might
 *      still be running (fatal unload with a busy GPU) the pages are LEFT
 *      ALLOCATED instead: a leak is harmless, the GPU writing into freed
 *      memory is not.
 *
 * Always returns NULL once it has started tearing down: returning an error
 * from a finalisation that has already released things leaves the module
 * loaded but half dismantled, which is worse than either outcome.
 */
_kernel_oserror *module_final(int fatal, int podule_base, void *pw)
{
    static _kernel_oserror err_gpu_busy = {
        V3D_ERR(48), "V3DDriver: the GPU is still running a job (or timed out and "
            "may still be) - not unloading. Reboot to clear it"
    };
    int idle;

    (void) podule_base;
    (void) pw;

    v3d_beacon_prepare(B_ZLIFE);
    v3d_beacon(B_FINAL_ENTER);
    (void) set_ran_marker("V3DDriver$FinalRan");

    idle = gpu_is_idle();
    v3d_beacon(B_FINAL_IDLE);
    if (!idle && !fatal)
        return &err_gpu_busy;

    (void) v3d_gfxv_shutdown();
    v3d_beacon(B_FINAL_GFXV);
    (void) v3d_irq_shutdown();
    v3d_beacon(B_FINAL_IRQ);

    if (idle)
    {
        if (g_pmp_created)
            (void) pmp_destroy(&g_pmp);
        if (g_pmp_fill_created)
            (void) pmp_destroy(&g_pmp_fill);
    }
    v3d_beacon(B_FINAL_PMP);

    g_pmp_created = 0;
    g_pmp_fill_created = 0;
    g_powered_on = 0;
    v3d_beacon(B_FINAL_DONE);
    return NULL;
}

/*
 * V3D_AccelTune (R0 = setting, R1 = new value or -1 to just read) - the
 * run-time switches (see cl_common.h for what they do). Returns the setting's
 * value from before the call in R0. Settings:
 *   0 zero_mode (0-1)       4 scratch_attr (0-1, applies after a reset)
 *   7 overlap_mode (0-1)
 *   5 reset scratch: releases the cached scratch pages, then claims the
 *     three production slots again right here in foreground code (never
 *     lazily inside a GraphicsV call). Returns 0 on success.
 *   6 re-arm after a GPU timeout. Returns the previous latch state.
 *   8 V3D clock in MHz: sets it (R1 = MHz, clamped to the firmware's maximum)
 *     or reads it (R1 = -1). Returns R0 = the rate now in force in MHz, R1 =
 *     the firmware's maximum in MHz. Foreground only, never during a job.
 *   9 max_job_tiles (1-510, -1 reads): the most 64x64 tiles one GPU job may hold. A
 *     fill or copy of more tiles is cut into bands of whole tile rows that fit (v3d_band_height
 *     in cl_common.c), at least one row per band, so 1 means "one row of tiles per job".
 *     Lowering it is how banding is tested on a screen that never needs it.
 * Defaults (v0.10): zero_mode 1, scratch_attr 1, overlap_mode 1, max_job_tiles 510, clock left as the
 * firmware has it. Setting 0, 4 and 7 to 0 and resetting (key 5) restores the
 * v0.06 behaviour exactly. Settings 1-3 (cl_mode, flush_mode, render_only), which
 * existed in v0.07-v0.09, were removed in v0.10: they saved nothing.
 */
static _kernel_oserror *tune_reset_scratch(void)
{
    static _kernel_oserror err_wedged_reset = {
        V3D_ERR(46), "V3DDriver: the GPU is switched off after a timeout - re-arm "
            "(V3D_AccelTune 6) only if you are sure it is idle"
    };
    unsigned int clock_rate, phys;
    void *log;
    v3d_scratch_t *slot;
    _kernel_oserror *err;

    if (g_v3d_wedged)
        return &err_wedged_reset;

    v3d_scratch_reset_all();

    err = v3d_power_on(&clock_rate);
    if (err)
        return err;
    err = ensure_pmp_fill();
    if (err)
        return err;
    err = v3d_irq_ensure_ready();
    if (err)
        return err;

    err = v3d_scratch_get(&g_pmp_fill, SCREEN_FILL_BASE_PAGE, 1u, &slot);
    if (err)
        return err;
    err = v3d_scratch_get(&g_pmp_fill, SCREEN_COPY_BASE_PAGE, 1u, &slot);
    if (err)
        return err;
    err = v3d_scratch_get(&g_pmp_fill,
                           SCREEN_COPY_OVERLAP_BASE_PAGE + PMP_MAX_PAGES_PER_CALL,
                           1u, &slot);
    if (err)
        return err;

    /* The 8MB bounce buffer is only for two-pass scrolls (overlap_mode 0); one-pass
     * scrolls (the default) never touch it, so do not tie up 8MB for nothing. It is
     * allowed to fail (fragmented memory) - the two-pass path then simply declines. */
    if (g_v3d_tune.overlap_mode == 0u)
        (void) v3d_overlap_buffer_get(&g_pmp_fill, SCREEN_COPY_OVERLAP_BASE_PAGE,
                                       1u, &phys, &log);
    return NULL;
}

static _kernel_oserror *accel_tune(_kernel_swi_regs *r)
{
    static _kernel_oserror err_bad_setting = {
        V3D_ERR(47), "V3DDriver: V3D_AccelTune: unknown setting, or value out of range"
    };
    unsigned int key = (unsigned int) r->r[0];
    unsigned int val = (unsigned int) r->r[1];
    unsigned int *target = NULL;

    switch (key)
    {
        case 0: target = &g_v3d_tune.zero_mode;    break;
        case 4: target = &g_v3d_tune.scratch_attr; break;
        case 7: target = &g_v3d_tune.overlap_mode; break;

        case 5:
        {
            _kernel_oserror *err = tune_reset_scratch();
            if (err)
                return err;
            r->r[0] = 0;
            return NULL;
        }

        case 6:
            r->r[0] = g_v3d_wedged ? 1u : 0u;
            v3d_gpu_rearm();
            return NULL;

        case 9:
            /* Tiles in one GPU job (1-510, default 510): a bigger fill or copy is cut into
             * bands of that many tiles or fewer (v3d_band_height). Lowering it is how the
             * banding is tested on a screen that would never need it. */
            r->r[0] = g_v3d_tune.max_job_tiles;
            if (val != 0xFFFFFFFFu)
            {
                if (val < 1u || val > WORST_CASE_N_TILES)
                    return &err_bad_setting;
                g_v3d_tune.max_job_tiles = val;
            }
            return NULL;

        case 8:
        {
            unsigned int state, rate, max_rate, clock_rate;
            _kernel_oserror *err;

            err = v3d_gpu_check();
            if (err)
                return err;
            err = v3d_power_on(&clock_rate); /* makes sure the clock is on at all */
            if (err)
                return err;

            if (val != 0xFFFFFFFFu)
            {
                static _kernel_oserror err_not_idle = {
                    V3D_ERR(51), "V3DDriver: the V3D clock is only changed with the GPU idle"
                };

                /* 200 MHz and up: this is for raising the clock from the 250 MHz
                 * the firmware leaves, never for asking for a rate it does not run. */
                if (val < 200u || val > 1000u)
                    return &err_bad_setting;
                if (!gpu_is_idle())
                    return &err_not_idle;
                err = v3d_set_clock_rate(val * 1000000u, &rate, &max_rate);
                if (err)
                    return err;
                g_v3d_clock_rate = rate;
            }

            err = v3d_query_clock(&state, &rate, &max_rate);
            if (err)
                return err;
            r->r[0] = rate / 1000000u;
            r->r[1] = max_rate / 1000000u;
            return NULL;
        }

        default:
            return &err_bad_setting;
    }

    r->r[0] = *target;
    if (val != 0xFFFFFFFFu)
    {
        if (val > 1u)
            return &err_bad_setting;
        *target = val;
    }
    return NULL;
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
    SWI_V3D_AccelCopyLastError = 20,
    SWI_V3D_RegSnapshot = 21,
    SWI_V3D_StripedFillTest = 22,
    SWI_V3D_AccelStripedStats = 23,
    SWI_V3D_ScreenStripedFillTest = 24,
    SWI_V3D_AccelReentrantCount = 25,
    SWI_V3D_AccelTimingStats = 26,
    SWI_V3D_16bppFormatTest = 27,
    SWI_V3D_CopyTest16bpp = 28,
    SWI_V3D_FillTest16bpp = 29,
    SWI_V3D_AccelLimits = 30,
    SWI_V3D_AccelProfile = 31,
    SWI_V3D_AccelTune = 32,
};

_kernel_oserror *v3d_swi_handler(int swi_no, _kernel_swi_regs *r, void *pw)
{
    (void) pw;

#ifdef V3D_BEACONS
    /* only the SWIs on the reload-freeze path draw a bar: the polling tools must not flicker it */
    switch (swi_no)
    {
        case SWI_V3D_PowerOn:
        case SWI_V3D_ReadIdent:
        case SWI_V3D_ClockInfo:
            v3d_beacon_prepare(B_ZPOWER);
            v3d_beacon(B_SWI_ENTER);
            break;
        case SWI_V3D_ClearTest:
        case SWI_V3D_ClearToSprite:
            v3d_beacon_prepare(B_ZCLEAR);
            v3d_beacon(B_SWI_ENTER);
            break;
        default:
            break;
    }
#endif

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
            v3d_beacon(B_SWI_EXIT);
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
                    V3D_ERR(5), "V3DDriver: V3D_ClearTest called before V3D_PowerOn"
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

        case SWI_V3D_16bppFormatTest:
        {
            /* Phase 2 of 16bpp screen-mode support - see the full
             * reasoning on v3d_16bpp_format_test itself (cl_clear.c).
             * R0=format_code to test, R1=colour in as 8-bit-per-channel
             * RGBA (byte0=R/byte1=G/byte2=B - the clear-colour packet
             * always takes 8bpc input regardless of target depth, see
             * this same function's own confirmed finding). Returns
             * R0=phys addr, R1=raw 16-bit readback. This hardware's
             * real 16bpp mode is genuine RGB565 (confirmed via real
             * BCMVideo HAL source + test/Format565Test.bas on real
             * hardware, NOT the classic RGB555 "32 thousand colours"
             * mode this hardware's HAL driver doesn't even offer) -
             * format_code 7 (BGR565) is the confirmed-correct one
             * (R bits11-15, G bits5-10, B bits0-4); format 45
             * (RGBA5551) was this project's original, now-superseded
             * guess for a genuine RGB555-mode screen. */
            unsigned int phys_addr, readback;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    V3D_ERR(28), "V3DDriver: V3D_16bppFormatTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_16bpp_format_test(&g_pmp, FORMAT16_TEST_BASE_PAGE,
                                         (unsigned int) r->r[0], (unsigned int) r->r[1],
                                         &phys_addr, &readback);
            if (err)
                return err;

            r->r[0] = phys_addr;
            r->r[1] = readback;
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
                    V3D_ERR(9), "V3DDriver: V3D_ClearToSprite called before V3D_PowerOn"
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
            static _kernel_oserror err_irq_in_use = {
                V3D_ERR(44), "V3DDriver: V3D_IRQTest cannot run once the accelerator "
                    "has set up interrupts - it would switch them off for "
                    "good. Reboot first, or load with NoAutoStart"
            };

            if (v3d_irq_is_ready())
                return &err_irq_in_use;

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
                    V3D_ERR(13), "V3DDriver: V3D_FillTest called before V3D_PowerOn"
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
            unsigned int bytes_per_pixel, format_code;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    V3D_ERR(17), "V3DDriver: V3D_ScreenFillTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_screen_framestore();
            if (err)
                return err;

            err = screen_bpp_params(&bytes_per_pixel, &format_code);
            if (err)
                return err;

            if (!span_ok(x, width, g_screen_info.xwind_limit) ||
                !span_ok(y, height, g_screen_info.ywind_limit))
            {
                static _kernel_oserror err_out_of_bounds = {
                    V3D_ERR(18), "V3DDriver: V3D_ScreenFillTest rectangle is outside "
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
                                    bytes_per_pixel, format_code,
                                    &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = tiles_x;
            r->r[1] = tiles_y;
            return NULL;
        }

        case SWI_V3D_ScreenStripedFillTest:
        {
            /* Real-framestore counterpart of V3D_ScreenFillTest, for the
             * new dithered-fill mechanism (see README's "Dithered/
             * patterned fills" section and V3D_StripedFillTest, which
             * proved the mechanism against a synthetic buffer only).
             * Writes DIRECTLY into the real, live screen framestore -
             * same visible-effect caveat as V3D_ScreenFillTest (the Wimp
             * may redraw over it immediately - expected, not a bug).
             *
             * R0=x, R1=y, R2=width, R3=height, R4=row_phase, R5=pointer
             * to 8 colour words in - same reasoning as
             * V3D_StripedFillTest for passing colours via a pointer
             * rather than individual registers. `row_phase` is left for
             * the CALLER to choose here (unlike the live accelerator's
             * own screen-anchored convention in
             * v3d_accel_render_striped_fill) - this is a manual, one-
             * shot test, not real desktop traffic, so there's nothing
             * for the pattern to stay anchored against; passing 0
             * simply means "the rectangle's own top row is colours[0]".
             * No output registers - success is a rectangle visibly
             * appearing on screen with the expected repeating stripes,
             * same "did it visibly work" check as V3D_ScreenFillTest. */
            unsigned int x = (unsigned int) r->r[0];
            unsigned int y = (unsigned int) r->r[1];
            unsigned int width = (unsigned int) r->r[2];
            unsigned int height = (unsigned int) r->r[3];
            unsigned int row_phase = (unsigned int) r->r[4];
            const unsigned int *colours_in = (const unsigned int *) r->r[5];
            unsigned int target_phys_row0;
            unsigned int bytes_per_pixel, format_code;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    V3D_ERR(17), "V3DDriver: V3D_ScreenStripedFillTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_screen_framestore();
            if (err)
                return err;

            err = screen_bpp_params(&bytes_per_pixel, &format_code);
            if (err)
                return err;

            if (!span_ok(x, width, g_screen_info.xwind_limit) ||
                !span_ok(y, height, g_screen_info.ywind_limit))
            {
                static _kernel_oserror err_out_of_bounds = {
                    V3D_ERR(18), "V3DDriver: V3D_ScreenStripedFillTest rectangle is "
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

            target_phys_row0 = g_screen_phys_base + y * g_screen_info.line_length
                                + x * bytes_per_pixel;

            err = v3d_striped_fill_raw(&g_pmp_fill, SCREEN_FILL_BASE_PAGE,
                                         width, height, colours_in, row_phase,
                                         target_phys_row0, g_screen_info.line_length,
                                         format_code);
            if (err)
                return err;

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
                    V3D_ERR(22), "V3DDriver: V3D_CopyTest called before V3D_PowerOn"
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

        case SWI_V3D_CopyTest16bpp:
        {
            /* 16bpp screen-mode support, Phase 3 isolated diagnostic
             * (2026-09-30) - see cl_copy.c's v3d_copy_test_16bpp for
             * the full reasoning (proves the copy path's LOAD side at
             * 16-bit width before the real framestore is ever touched
             * at this depth). Same register contract as V3D_CopyTest:
             * R0=width, R1=height in; source/dest logical addresses
             * (R0/R1), shared stride in bytes (R2), tiles_x/tiles_y
             * (R3/R4) out. Requires V3D_PowerOn. */
            void *src_log, *dest_log;
            unsigned int stride, tiles_x, tiles_y;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    V3D_ERR(30), "V3DDriver: V3D_CopyTest16bpp called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_copy_test_16bpp(&g_pmp_fill, COPY_TEST_16BPP_BASE_PAGE,
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

        case SWI_V3D_FillTest16bpp:
        {
            /* 16bpp screen-mode support, Phase 4 isolated diagnostic
             * (2026-09-30) - see cl_fill.c's v3d_fill_test_16bpp for
             * the full reasoning (proves the fill/store path at
             * 16-bit width, in isolation, before the real framestore
             * is ever touched at this depth). Same register contract
             * as V3D_FillTest: R0=width, R1=height, R2=colour in
             * (8-bit-per-channel RGBA, same convention as 32bpp - see
             * v3d_fill_test_16bpp's own comment); physical address
             * (R0), row stride in bytes (R1), tiles_x (R2), tiles_y
             * (R3), logical address (R4) out. Requires V3D_PowerOn. */
            unsigned int target_phys, stride, tiles_x, tiles_y;
            void *target_log;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    V3D_ERR(32), "V3DDriver: V3D_FillTest16bpp called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            err = v3d_fill_test_16bpp(&g_pmp_fill, FILL_TEST_16BPP_BASE_PAGE,
                                  (unsigned int) r->r[0], (unsigned int) r->r[1],
                                  (unsigned int) r->r[2],
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
            unsigned int bytes_per_pixel, format_code;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    V3D_ERR(23), "V3DDriver: V3D_ScreenCopyTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_screen_framestore();
            if (err)
                return err;

            err = screen_bpp_params(&bytes_per_pixel, &format_code);
            if (err)
                return err;

            if (!span_ok(src_x, width, g_screen_info.xwind_limit) ||
                !span_ok(dest_x, width, g_screen_info.xwind_limit) ||
                !span_ok(src_y, height, g_screen_info.ywind_limit) ||
                !span_ok(dest_y, height, g_screen_info.ywind_limit))
            {
                static _kernel_oserror err_out_of_bounds = {
                    V3D_ERR(24), "V3DDriver: V3D_ScreenCopyTest rectangle is outside "
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
                    V3D_ERR(25), "V3DDriver: V3D_ScreenCopyTest source and "
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
                                    bytes_per_pixel, format_code,
                                    &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = tiles_x;
            r->r[1] = tiles_y;
            return NULL;
        }

        case SWI_V3D_AccelLimits:
        {
            /* Size limits below which a GraphicsV fill/copy is left
             * unclaimed so the CPU does it (see gfxv_accel.c's SIZE LIMITS
             * comment). In, each -1 = leave unchanged (so all -1 just
             * reads): R0 = min fill bytes, R1 = min fill rows, R2 = min
             * direct-copy bytes, R3 = min copy rows, R4 = min overlapping-
             * copy bytes (bytes = width*height*bytes-per-pixel). Setting
             * all five to 0 accelerates everything. Out: R0-R4 = the
             * values BEFORE this call, R5 = fills declined as too small,
             * R6 = copies declined as too small. */
            volatile unsigned int *limits[5];
            unsigned int i;

            limits[0] = &g_gfxv_min_fill_bytes;
            limits[1] = &g_gfxv_min_fill_rows;
            limits[2] = &g_gfxv_min_copy_bytes;
            limits[3] = &g_gfxv_min_copy_rows;
            limits[4] = &g_gfxv_min_overlap_bytes;

            for (i = 0; i < 5u; i++)
            {
                unsigned int previous = *limits[i];

                if (r->r[i] != -1)
                    *limits[i] = ((unsigned int) r->r[i]) & 0x7FFFFFFFu;
                r->r[i] = (int) previous;
            }
            r->r[5] = (int) g_gfxv_fill_skipped_count;
            r->r[6] = (int) g_gfxv_copy_skipped_count;
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
                    V3D_ERR(26), "V3DDriver: V3D_OverlapCopyTest called before V3D_PowerOn"
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
            unsigned int bytes_per_pixel, format_code;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    V3D_ERR(27), "V3DDriver: V3D_ScreenCopyOverlapTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_screen_framestore();
            if (err)
                return err;

            err = screen_bpp_params(&bytes_per_pixel, &format_code);
            if (err)
                return err;

            if (!span_ok(src_x, width, g_screen_info.xwind_limit) ||
                !span_ok(dest_x, width, g_screen_info.xwind_limit) ||
                !span_ok(src_y, height, g_screen_info.ywind_limit) ||
                !span_ok(dest_y, height, g_screen_info.ywind_limit))
            {
                static _kernel_oserror err_out_of_bounds = {
                    V3D_ERR(24), "V3DDriver: V3D_ScreenCopyOverlapTest rectangle is "
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
                                    bytes_per_pixel, format_code,
                                    &tiles_x, &tiles_y);
            if (err)
                return err;

            r->r[0] = tiles_x;
            r->r[1] = tiles_y;
            return NULL;
        }

        case SWI_V3D_AccelCopyLastError:
            /* Real OS error message text for the last copy failure
             * (either path) - see gfxv_accel.c's own comment on why a
             * bare error number wasn't enough. Returns the address of
             * a null-terminated string (R0) - read directly in BASIC
             * via `$addr%`. Empty string if no failure has occurred. */
            r->r[0] = (unsigned int) v3d_gfxv_copy_last_fail_msg();
            return NULL;

        case SWI_V3D_RegSnapshot:
        {
            /* Proactive hardware register dump for the still-unresolved
             * reload-without-reboot freeze (see README's "Known
             * limitations" and "Real IRQ handling" sections) - a HARD
             * MACHINE FREEZE, not a SWI timeout, meaning cl_clear.c's
             * own diagnose_timeout() never gets a chance to run: the
             * freeze happens before any bounded wait could time out.
             * Every fix attempted so far (four of them) targeted a
             * candidate resource leak; none of them were actual
             * instrumentation - nobody has seen real V3D register
             * state in the moments leading up to this specific freeze.
             * This SWI is meant to be called and its output printed
             * BEFORE each risky call in a reload-then-stress-test
             * script, so if the freeze happens again, whatever printed
             * last is real forensic data instead of another guess.
             *
             * Deliberately does NOT require V3D_PowerOn - plain
             * register reads have always worked regardless of clock/
             * power state (see M0/M1's own findings), and the whole
             * point is to observe state that might be abnormal
             * specifically because of that.
             *
             * Returns: V3D_CTL_INT_STS (R0), V3D_HUB_INT_STS (R1),
             * V3D_CLE_CT0CS (R2), V3D_CLE_CT1CS (R3), V3D_CLE_CT0CA
             * (R4), V3D_CLE_CT0EA (R5), V3D_CLE_CT1CA (R6), V3D_CLE_
             * CT1EA (R7).
             *
             * R8/R9 (ASB_V3D_M_CTRL/ASB_V3D_S_CTRL) added 2026-09-28
             * after a live reload-freeze repro FINALLY produced a
             * bounded diagnose_timeout() instead of a hard freeze:
             * CT0CS read STOP (never started), but CT0CA/CT0EA/CT0QBA/
             * CT0QEA all read 0xFFFFFFFF while CS/STS/MMU_CTL read a
             * clean 0 in the SAME io_map session - i.e. only the CLE's
             * address/queue registers looked like an unclocked-bus
             * read, not the whole V3D block. `v3d_ensure_clock_on`
             * (module.c) only clears the ASB master/slave gate
             * (ASB_V3D_M_CTRL/S_CTRL, ASB_REQ_STOP) when the FIRMWARE's
             * own clock_already_on check is false - meaning on a
             * second/reload PowerOn, where the firmware still reports
             * the clock on, that gate-clear is skipped entirely even
             * if the ASB gate itself has independently ended up
             * re-stopped. These two registers directly test that:
             * ASB_REQ_STOP set here at the moment of a timeout would
             * confirm it; ASB_ACK's state says whether the gate is
             * actually granted either way. */
            io_mapping_t v3d;
            _kernel_oserror *err;

            /* Reload-freeze round 8: three SELECTED read-only banks, so a freeze in one of them still leaves
             * the earlier banks' values on the screen. R0 = the selector on entry (a value an old caller's
             * leftover R0 will not have):
             *   0x51D00001  power side only: PM_GRAFX (R0), ASB master gate (R1), ASB slave gate (R2)
             *   0x51D00002  hub bank: HUB_IDENT0, HUB_INT_STS, HUB_INT_MSK_STS, MMU_CTL (R0-R3)
             *   0x51D00003  core bank: CTL_IDENT0, CTL_INT_STS, CTL_INT_MSK_STS, CLE_BFC, CLE_RFC, CT0CS,
             *               CT1CS, CT0CA, CT0EA, CT0QTS (R0-R9)
             * Anything else gives the original snapshot below. */
            {
                unsigned int sel = (unsigned int) r->r[0];

                if (sel == 0x51D00001u)
                {
                    io_mapping_t pm;

                    err = io_map(PM_MB_BASE, &pm);
                    if (err)
                        return err;
                    r->r[0] = IO_WORD(pm, PM_GRAFX);
                    io_unmap(&pm);
                    err = io_map(V3D_MB_BASE, &v3d);
                    if (err)
                        return err;
                    r->r[1] = IO_WORD(v3d, ASB_V3D_M_CTRL);
                    r->r[2] = IO_WORD(v3d, ASB_V3D_S_CTRL);
                    io_unmap(&v3d);
                    return NULL;
                }
                if (sel == 0x51D00002u || sel == 0x51D00003u)
                {
                    err = io_map(V3D_MB_BASE, &v3d);
                    if (err)
                        return err;
                    if (sel == 0x51D00002u)
                    {
                        r->r[0] = IO_WORD(v3d, V3D_HUB_IDENT0);
                        r->r[1] = IO_WORD(v3d, V3D_HUB_INT_STS);
                        r->r[2] = IO_WORD(v3d, V3D_HUB_INT_MSK_STS);
                        r->r[3] = IO_WORD(v3d, V3D_MMU_CTL);
                    }
                    else
                    {
                        r->r[0] = IO_WORD(v3d, V3D_CTL_IDENT0);
                        r->r[1] = IO_WORD(v3d, V3D_CTL_INT_STS);
                        r->r[2] = IO_WORD(v3d, V3D_CTL_INT_MSK_STS);
                        r->r[3] = IO_WORD(v3d, V3D_CLE_BFC);
                        r->r[4] = IO_WORD(v3d, V3D_CLE_RFC);
                        r->r[5] = IO_WORD(v3d, V3D_CLE_CT0CS);
                        r->r[6] = IO_WORD(v3d, V3D_CLE_CT1CS);
                        r->r[7] = IO_WORD(v3d, V3D_CLE_CT0CA);
                        r->r[8] = IO_WORD(v3d, V3D_CLE_CT0EA);
                        r->r[9] = IO_WORD(v3d, V3D_CLE_CT0QTS);
                    }
                    io_unmap(&v3d);
                    return NULL;
                }
            }

            err = io_map(V3D_MB_BASE, &v3d);
            if (err)
                return err;

            r->r[0] = IO_WORD(v3d, V3D_CTL_INT_STS);
            r->r[1] = IO_WORD(v3d, V3D_HUB_INT_STS);
            r->r[2] = IO_WORD(v3d, V3D_CLE_CT0CS);
            r->r[3] = IO_WORD(v3d, V3D_CLE_CT1CS);
            r->r[4] = IO_WORD(v3d, V3D_CLE_CT0CA);
            r->r[5] = IO_WORD(v3d, V3D_CLE_CT0EA);
            r->r[6] = IO_WORD(v3d, V3D_CLE_CT1CA);
            r->r[7] = IO_WORD(v3d, V3D_CLE_CT1EA);
            r->r[8] = IO_WORD(v3d, ASB_V3D_M_CTRL);
            r->r[9] = IO_WORD(v3d, ASB_V3D_S_CTRL);

            io_unmap(&v3d);
            return NULL;
        }

        case SWI_V3D_StripedFillTest:
        {
            /* Isolated test harness for the dithered-fill mechanism
             * (see README's "Dithered/patterned fills" section,
             * cl_fill.c's v3d_striped_fill_raw/v3d_striped_fill_test,
             * and gfxv_accel.c's ecf_decode_striped) - same "prove it
             * against a synthetic buffer before touching the real
             * screen" methodology as V3D_FillTest, deliberately kept
             * completely separate from the live GraphicsV handler.
             *
             * Takes 8 colours via a pointer, not individually in
             * registers - BASIC's SYS only usefully offers R0-R9, and
             * width/height/row_phase plus 8 colours already needs 11
             * values; this also mirrors how the real ECF block is
             * itself passed as a pointer, not inline register values.
             *
             * R0=width, R1=height, R2=row_phase, R3=pointer to 8
             * colour words in. Returns physical address (R0), row
             * stride in bytes (R1), and the *logical* address (R2) so
             * a BASIC test script can sample pixels directly. */
            unsigned int target_phys, stride;
            void *target_log;
            const unsigned int *colours_in;
            _kernel_oserror *err;

            if (!g_powered_on)
            {
                static _kernel_oserror err_not_on = {
                    V3D_ERR(13), "V3DDriver: V3D_StripedFillTest called before V3D_PowerOn"
                };
                return &err_not_on;
            }

            err = ensure_pmp_fill();
            if (err)
                return err;

            err = v3d_irq_ensure_ready();
            if (err)
                return err;

            colours_in = (const unsigned int *) r->r[3];

            err = v3d_striped_fill_test(&g_pmp_fill, 0, (unsigned int) r->r[0],
                                          (unsigned int) r->r[1], colours_in,
                                          (unsigned int) r->r[2],
                                          &target_phys, &target_log, &stride);
            if (err)
                return err;

            r->r[0] = target_phys;
            r->r[1] = stride;
            r->r[2] = (unsigned int) target_log;
            return NULL;
        }

        case SWI_V3D_AccelStripedStats:
            /* Real instrumentation for the dithered-fill path now
             * sitting in the live GraphicsV handler - see
             * gfxv_accel.c. Returns how many render-fill calls decoded
             * as a genuine (non-solid) striped pattern (R0), how many
             * were actually accelerated (R1), how many attempted-but-
             * failed (R2), and the last failure's OS error number
             * (R3). */
            r->r[0] = g_gfxv_striped_count;
            r->r[1] = g_gfxv_striped_claimed_count;
            r->r[2] = g_gfxv_striped_error_count;
            r->r[3] = g_gfxv_striped_last_fail_errnum;
            return NULL;

        case SWI_V3D_AccelReentrantCount:
            /* Non-zero means a second GraphicsV Render fill/copy call
             * landed while a previous one was still in flight and was
             * safely skipped (see gfxv_accel.c's g_gfxv_busy) - a real
             * safety net kept as a permanent, cheap diagnostic. */
            r->r[0] = g_gfxv_reentrant_count;
            return NULL;

        case SWI_V3D_AccelTimingStats:
            /* Real accelerated-call timing, cheap to keep permanently:
             * the worst single call's duration ever seen, in
             * centiseconds (R0), and the highest accelerated-call rate
             * seen in any 1-second window all session (R1). */
            r->r[0] = g_max_call_duration_us / 10000u; /* cs, as before */
            r->r[1] = g_peak_calls_per_second;
            r->r[2] = g_max_call_duration_us;      /* new in v0.07: us */
            r->r[3] = v3d_time_is_fine();          /* 1 = real us timer */
            return NULL;

        case SWI_V3D_AccelProfile:
        {
            /* v0.07: where the time of an accelerated job actually goes,
             * and the health of the GPU path. R0 = view:
             *   0 = fills, 1 = copies (each JOB, so an overlapping copy
             *       counts twice): R0 jobs, R1..R5 SUMS of microseconds
             *       (setup = start to binning kicked, bin, mid = binning
             *       done to render kicked, render, total), R6 worst single
             *       job. R1 on entry = 1 to zero the counters afterwards.
             *   2 = health: R0 wedged (1 = the fail-stop latch is set),
             *       R1 timeouts so far, R2 microsecond timer in use,
             *       R3 write-bank address seen, R4 that address,
             *       R5 base rendering uses, R6 framestore base,
             *       R7 lost interrupts rescued.
             *   3 = last timeout, part 1: R0 (which|kind<<8|valid<<16),
             *       R1 INT_STS, R2 INT_MSK, R3 CT0CS, R4 CT0CA, R5 CT0EA,
             *       R6 CT1CS, R7 CT1CA.
             *   4 = last timeout, part 2: R0 CT1EA, R1 BFC, R2 RFC,
             *       R3 BFC before the job, R4 RFC before, R5 bin IRQs,
             *       R6 render IRQs, R7 hub INT_STS.
             *   5 = zero both sets of counters.
             *   7 = interrupt accounting (never reset): R0 lost-interrupt rescues,
             *       R1 interrupts claimed, R2 empty interrupts, R3 bin interrupts,
             *       R4 render interrupts, R5 jobs started. Every job should raise
             *       exactly one bin and one render interrupt, so R5 - R3 and R5 - R4
             *       should be 0 when idle; more means interrupts really were lost. Only
             *       valid on a boot that has not used V3D_ClearTest/V3D_ClearToSprite,
             *       which raise the same interrupts without counting a job.
             *       R6 times the screen could not be used for a fill or copy, R7 the
             *       error number of the last such decline.
             *   9 = event trace (see gfxv_accel.h). R1 = -1 clears the log and arms it;
             *       R1 = 0..31 reads event R1: R0 = 1 | kind<<8 | outcome<<16 (0 = no such
             *       event), R1-R5 = a..e (fill: left, bottom, right, top, ECF pointer;
             *       copy: src left, src bottom, dest left, dest bottom, (w-1)|(h-1)<<16;
             *       SetMode: R0 in; SetDMAAddress: address, DAG), R6 = (errnum AND &FFFF) | seq<<16 (view 7 R7 has the whole number),
             *       R7 = events stored | armed<<16.
             *   8 = one recorded rescue, R1 on entry = 0..3: R0 = valid | which<<8
             *       (1 bin 2 render) | kind<<16 (0 fill 1 copy), R1 waited us,
             *       R2 INT_STS, R3 INT_MSK, R4 hub INT_STS, R5 thread CS,
             *       R6 frame-counter delta | late<<16, R7 (jobs-bin) | (jobs-render)<<16.
             *   6 = the screen the driver is working from: R0 error
             *       number of the last reason it declined the screen
             *       (0 = none), R1 address of that message (read it
             *       with $R1 in BASIC), R2 log2 bpp, R3 line length,
             *       R4 XWindLimit, R5 YWindLimit, R6 TotalScreenSize,
             *       R7 framestore size reported by GraphicsV 9. */
            unsigned int view = (unsigned int) r->r[0];
            unsigned int reset = (unsigned int) r->r[1];

            /* 10 = census of fill/copy work (0.12 rev f): R1 = -1 zeroes the census and the job-time sums and
             * starts the clock; otherwise R1 = kind (0 fill, 1 copy, 2 scroll), R2 = size bucket 0-9 (pixel area
             * up to 64, 256, 1K, 4K, 16K, 64K, 256K, 1M, 4M, above). Returns R0 taken by the GPU, R1 left to
             * the CPU, R2 kilopixels the GPU did, R3 kilopixels left to the CPU, R4 microseconds since the reset. */
            if (view == 10u)
            {
                if (reset == 0xFFFFFFFFu)
                {
                    v3d_census_reset();
                    v3d_prof_reset();
                    r->r[0] = r->r[1] = r->r[2] = r->r[3] = r->r[4] = 0;
                }
                else
                {
                    unsigned int out[5];

                    v3d_census_read(reset, (unsigned int) r->r[2], out);
                    r->r[0] = out[0];
                    r->r[1] = out[1];
                    r->r[2] = out[2];
                    r->r[3] = out[3];
                    r->r[4] = out[4];
                }
                return NULL;
            }

            if (view <= 1u)
            {
                v3d_prof_t *k = &g_v3d_prof[view];

                r->r[0] = k->n;
                r->r[1] = k->setup_us;
                r->r[2] = k->bin_us;
                r->r[3] = k->mid_us;
                r->r[4] = k->render_us;
                r->r[5] = k->total_us;
                r->r[6] = k->max_us;
                r->r[7] = 0;
                if (reset == 1u)
                {
                    k->n = k->setup_us = k->bin_us = k->mid_us = 0;
                    k->render_us = k->total_us = k->max_us = 0;
                }
                return NULL;
            }
            if (view == 2u)
            {
                r->r[0] = g_v3d_wedged ? 1u : 0u;
                r->r[1] = g_v3d_wedge_count;
                r->r[2] = v3d_time_is_fine();
                r->r[3] = g_vrender.valid;
                r->r[4] = g_vrender.phys;
                r->r[5] = g_screen_phys_base;
                r->r[6] = g_screen_fs_base;
                r->r[7] = g_v3d_irq_rescue_count;
                return NULL;
            }
            if (view == 3u)
            {
                const v3d_timeout_diag_t *d = &g_v3d_timeout_diag;

                r->r[0] = d->which | (d->kind << 8) | (d->valid << 16);
                r->r[1] = d->int_sts;
                r->r[2] = d->int_msk;
                r->r[3] = d->ct0cs;
                r->r[4] = d->ct0ca;
                r->r[5] = d->ct0ea;
                r->r[6] = d->ct1cs;
                r->r[7] = d->ct1ca;
                return NULL;
            }
            if (view == 4u)
            {
                const v3d_timeout_diag_t *d = &g_v3d_timeout_diag;

                r->r[0] = d->ct1ea;
                r->r[1] = d->bfc;
                r->r[2] = d->rfc;
                r->r[3] = d->bfc0;
                r->r[4] = d->rfc0;
                r->r[5] = d->irq_bin_count;
                r->r[6] = d->irq_render_count;
                r->r[7] = d->hub_sts;
                return NULL;
            }
            if (view == 5u)
            {
                v3d_prof_reset();
                return NULL;
            }
            if (view == 7u)
            {
                r->r[0] = g_v3d_irq_rescue_count;
                r->r[1] = g_v3d_irq_claimed_count;
                r->r[2] = g_v3d_irq_spurious_count;
                r->r[3] = g_v3d_irq_bin_done_count;
                r->r[4] = g_v3d_irq_render_done_count;
                r->r[5] = g_v3d_jobs_run;
                r->r[6] = g_gfxv_screen_decline_count;
                r->r[7] = g_gfxv_screen_decline_last_errnum;
                return NULL;
            }
            if (view == 9u)
            {
                const gfxv_trace_t *t;
                unsigned int stored, armed;

                if (reset == 0xFFFFFFFFu)
                {
                    v3d_gfxv_trace_arm();
                    return NULL;
                }
                t = v3d_gfxv_trace_get(reset, &stored, &armed);
                r->r[7] = stored | (armed << 16);
                if (t == NULL)
                {
                    r->r[0] = 0;
                    r->r[1] = r->r[2] = r->r[3] = r->r[4] = r->r[5] = r->r[6] = 0;
                    return NULL;
                }
                r->r[0] = 1u | (t->kind << 8) | (t->outcome << 16);
                r->r[1] = t->a;
                r->r[2] = t->b;
                r->r[3] = t->c;
                r->r[4] = t->d;
                r->r[5] = t->e;
                r->r[6] = (t->errnum & 0xFFFFu) | (t->seq << 16);
                return NULL;
            }
            if (view == 8u)
            {
                const v3d_rescue_diag_t *d;

                if (reset >= V3D_RESCUE_DIAG_MAX)
                    return error_BAD_SWI;
                d = &g_v3d_rescue_diag[reset];
                r->r[0] = d->valid | (d->which << 8) | (d->kind << 16);
                r->r[1] = d->waited_us;
                r->r[2] = d->int_sts;
                r->r[3] = d->int_msk;
                r->r[4] = d->hub_sts;
                r->r[5] = d->ct_cs;
                r->r[6] = (d->fc_delta & 0xFFFFu) | (d->late << 16);
                r->r[7] = (d->jobs_minus_bin & 0xFFFFu) | (d->jobs_minus_render << 16);
                return NULL;
            }
            if (view == 6u)
            {
                r->r[0] = g_screen_cached_err ? V3D_ERR_INDEX(g_screen_cached_err->errnum) : 0u;
                r->r[1] = g_screen_cached_err ? (unsigned int) g_screen_cached_err->errmess : 0u;
                r->r[2] = g_screen_info.log2_bpp;
                r->r[3] = g_screen_info.line_length;
                r->r[4] = g_screen_info.xwind_limit;
                r->r[5] = g_screen_info.ywind_limit;
                r->r[6] = g_screen_info.total_screen_size;
                r->r[7] = g_screen_fs_size;
                return NULL;
            }
            return error_BAD_SWI;
        }

        case SWI_V3D_AccelTune:
            return accel_tune(r);

        default:
            /* error_BAD_SWI is cmunge's own generated macro (see
             * V3DDriver.h) for exactly this case - the documented
             * correct return value for a SWI number out of range for
             * this module, not a made-up error. */
            return error_BAD_SWI;
    }
}
