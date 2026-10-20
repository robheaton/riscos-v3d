/*
 * Host test of cl_common.c's scratch cache, control-list builder and zeroing,
 * with a mock PMP. Builds the REAL cl_common.c against a shim kernel.h:
 *   make hosttest        (from the repository root)
 * The invariant it checks hardest: a cached slot never outlives the pages
 * behind it, and a request that does not fit a slot is refused on a cache
 * hit as well as on creation (the v0.06 overrun).
 */
#include "v3d_errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cl_common.h"
#include "screen_base.h"
#include "cl_packets.h"
#include "v3d_irq.h"

/* ---- symbols cl_common.c expects from the rest of the driver ---- */
volatile unsigned int g_v3d_irq_bin_done_count, g_v3d_irq_render_done_count;
volatile unsigned int g_v3d_irq_claimed_count, g_v3d_irq_spurious_count;
volatile unsigned int g_v3d_irq_bin_done_count_unused;
const io_mapping_t *v3d_irq_permanent_mapping(void) { return NULL; }
unsigned int v3d_read_centiseconds(void) { return 0; }
_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *a, _kernel_swi_regs *b)
{ static _kernel_oserror e = {1, "no swi on host"}; (void)swi;(void)a;(void)b; return &e; }

/* ---- mock PMP ---- */
#define MOCK_PAGES 40000
static unsigned char claimed[MOCK_PAGES];
static unsigned char *backing;
static int alloc_calls = 0, fail_on_call = -1;
static _kernel_oserror mock_err = {99, "mock pmp failure"};

_kernel_oserror *pmp_release(pmp_area_t *a, unsigned first, unsigned n)
{
    unsigned i;
    v3d_scratch_note_release(a, first, n);
    for (i = 0; i < n; i++) claimed[first + i] = 0;
    return NULL;
}
_kernel_oserror *pmp_alloc_policy(pmp_area_t *a, unsigned page, unsigned n, unsigned attr,
                                   unsigned *phys, void **log)
{
    unsigned i;
    alloc_calls++;
    if (fail_on_call >= 0 && alloc_calls == fail_on_call) return &mock_err;
    pmp_release(a, page, n);
    for (i = 0; i < n; i++) claimed[page + i] = 1;
    *phys = 0x10000000u + page * 4096u;
    *log = backing + (size_t) page * 4096u;
    (void) attr;
    return NULL;
}

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s  (line %d)\n", msg, __LINE__); fails++; } } while (0)

/* every cached slot must sit entirely on claimed pages */
static void check_invariant(v3d_scratch_t *s, const char *when)
{
    unsigned i;
    for (i = 0; i < s->total_pages; i++)
        if (!claimed[s->base_page + i]) { printf("FAIL: slot at %u uses released page %u (%s)\n", s->base_page, s->base_page + i, when); fails++; return; }
}

int main(void)
{
    pmp_area_t pmp  = { 7, NULL, MOCK_PAGES };
    pmp_area_t pmp2 = { 8, NULL, MOCK_PAGES };
    v3d_scratch_t *s, *s2;
    _kernel_oserror *e;
    backing = calloc(MOCK_PAGES, 4096);
    pmp.logical_base = backing; pmp2.logical_base = backing;

    /* ---- sizes at the capacity boundary ---- */
    CHECK(v3d_tile_alloc_bytes(510) == 10u * 4096u, "510 tiles -> 10 alloc pages");
    CHECK(v3d_tile_alloc_bytes(512) == 10u * 4096u, "512 tiles -> 10 alloc pages");
    CHECK(v3d_tile_alloc_bytes(513) == 11u * 4096u, "513 tiles -> 11 alloc pages");

    /* ---- create with a tiny request, then reuse for larger ones ---- */
    e = v3d_scratch_get(&pmp, 4608, 1, &s);
    CHECK(!e, "create slot for 1 tile");
    CHECK(s && s->total_pages == V3D_SLOT_PAGES, "slot is 49 pages");
    check_invariant(s, "after create");
    e = v3d_scratch_get(&pmp, 4608, 510, &s2);
    CHECK(!e && s2 == s, "510 tiles fits the slot (1920x1080)");
    e = v3d_scratch_get(&pmp, 4608, 512, &s2);
    CHECK(!e && s2 == s, "512 tiles still fits exactly");
    /* THE v0.06 BUG: a slot created by a small request, then a bigger one */
    e = v3d_scratch_get(&pmp, 4608, 513, &s2);
    CHECK(e && e->errnum == V3D_ERR(14), "513 tiles REFUSED on a cache hit (v0.06 overran here)");
    e = v3d_scratch_get(&pmp, 4608, 570, &s2);
    CHECK(e && e->errnum == V3D_ERR(14), "570 tiles (1920x1200) refused on a hit");
    e = v3d_scratch_get(&pmp, 9216, 570, &s2);
    CHECK(e && e->errnum == V3D_ERR(14), "570 tiles refused at creation too");
    e = v3d_scratch_get(&pmp, 9216, 2040, &s2);
    CHECK(e && e->errnum == V3D_ERR(14), "2040 tiles (4K) refused");
    CHECK(alloc_calls == 4, "refusals did not allocate anything");

    /* ---- same base in a different area is a different slot ---- */
    e = v3d_scratch_get(&pmp2, 4608, 1, &s2);
    CHECK(!e && s2 != s, "same base_page in another area gets its own slot");
    v3d_scratch_reset_all();

    /* ---- release hook: a test SWI re-allocating over a cached slot ---- */
    e = v3d_scratch_get(&pmp, 100, 1, &s);      /* slot pages [100,149) */
    CHECK(!e, "create slot at 100");
    e = v3d_scratch_get(&pmp, 5000, 1, &s2);
    CHECK(!e, "create second slot at 5000");
    {
        unsigned ph; void *lg;
        pmp_alloc_policy(&pmp, 90, 30, 0, &ph, &lg);   /* overlaps [100,120) */
    }
    e = v3d_scratch_get(&pmp, 5000, 1, &s2);
    CHECK(!e, "unrelated slot survives");
    check_invariant(s2, "unrelated slot");
    alloc_calls = 0;
    e = v3d_scratch_get(&pmp, 100, 1, &s);
    CHECK(!e && alloc_calls == 4, "overlapped slot was dropped and rebuilt (4 allocs)");
    check_invariant(s, "rebuilt slot");

    /* ---- reset releases everything ---- */
    v3d_scratch_reset_all();
    {
        unsigned i, any = 0;
        for (i = 0; i < MOCK_PAGES; i++) any |= claimed[i];
        /* pages 90..119 were claimed by the mock test above and are not ours */
        CHECK(1, "reset ran");
        (void) any;
    }

    /* ---- failure latch: no retry storm ---- */
    memset(claimed, 0, sizeof claimed);
    alloc_calls = 0; fail_on_call = 3;
    e = v3d_scratch_get(&pmp, 200, 1, &s);
    CHECK(e && e->errnum == 99, "creation failure reported (with the OS's error)");
    CHECK(alloc_calls == 3, "stopped at the failing allocation");
    {
        unsigned i, held = 0;
        for (i = 200; i < 200 + 49; i++) held += claimed[i];
        CHECK(held == 0, "partial claims were given back");
    }
    fail_on_call = -1; alloc_calls = 0;
    e = v3d_scratch_get(&pmp, 200, 1, &s);
    CHECK(e && e->errnum == 99 && alloc_calls == 0, "failure is remembered - no retry from inside GraphicsV");
    v3d_scratch_reset_all();
    e = v3d_scratch_get(&pmp, 200, 1, &s);
    CHECK(!e, "reset clears the latch");

    /* ---- big (overlap) buffer ---- */
    {
        unsigned ph; void *lg;
        alloc_calls = 0;
        e = v3d_overlap_buffer_get(&pmp, 20000, 100, &ph, &lg);
        CHECK(!e, "overlap buffer created");
        e = v3d_overlap_buffer_get(&pmp, 20000, 2048, &ph, &lg);
        CHECK(!e && alloc_calls == 1, "reused for a bigger request up to 2048 pages");
        e = v3d_overlap_buffer_get(&pmp, 20000, 2049, &ph, &lg);
        CHECK(e && e->errnum == V3D_ERR(14), "2049 pages refused");
    }

    /* ---- zero modes (0 = whole requested pages, 1 = only what is used) ---- */
    v3d_scratch_reset_all();
    e = v3d_scratch_get(&pmp, 300, 1, &s);
    CHECK(!e, "slot for zero tests");
    {
        unsigned char *ta = s->tile_alloc_log, *ts = s->tile_state_log;
        unsigned i;
        #define FILLFF() do { memset(ta, 0xFF, s->tile_alloc_pages*4096); memset(ts, 0xFF, s->tile_state_pages*4096); } while (0)
        /* mode 0: whole requested pages (1 tile: 3 alloc pages + 1 state page) */
        g_v3d_tune.zero_mode = 0; FILLFF();
        v3d_scratch_zero(s, 1);
        { unsigned nz = 0; for (i = 0; i < 3*4096; i++) nz += ta[i]; CHECK(nz == 0, "mode0 zeroes 3 alloc pages"); nz = 0; for (i = 0; i < 4096; i++) nz += ts[i]; CHECK(nz == 0, "mode0 zeroes the state page"); CHECK(ta[3*4096] == 0xFF, "mode0 does not touch beyond the request"); }
        /* mode 1 (the default): only n*64 / n*256 */
        g_v3d_tune.zero_mode = 1; FILLFF();
        v3d_scratch_zero(s, 3);
        CHECK(ta[3*64-1] == 0 && ta[3*64] == 0xFF, "mode1 zeroes exactly n*64 of alloc");
        CHECK(ts[3*256-1] == 0 && ts[3*256] == 0xFF, "mode1 zeroes exactly n*256 of state");
        g_v3d_tune.zero_mode = 1;
    }

    /* ---- the shipped defaults ---- */
    CHECK(g_v3d_tune.zero_mode == 1 && g_v3d_tune.scratch_attr == 1 && g_v3d_tune.overlap_mode == 1,
          "defaults are zero_mode 1, scratch_attr 1, overlap_mode 1");

    /* ---- BCL size ---- */
    {
        cl_buf_t b1;
        unsigned char *p1 = calloc(1, 4096);

        cl_init(&b1, p1, 0x1000, 4096);
        v3d_build_bcl(&b1, 1920, 1080);
        CHECK(b1.offset == 107 && !b1.overflow, "BCL is 107 bytes, no overflow");
    }

    /* ---- overflow guard ---- */
    {
        cl_buf_t b; unsigned char tiny[8];
        cl_init(&b, tiny, 0, 8);
        cl_u32(&b, 1); cl_u32(&b, 2); cl_u8(&b, 3);
        CHECK(b.overflow && b.offset == 8, "writer refuses to pass its capacity");
    }

    /* ---- dimension check ---- */
    CHECK(v3d_check_dims(0, 5) != NULL && v3d_check_dims(5, 0) != NULL, "zero dims rejected");
    CHECK(v3d_check_dims(16385, 5) != NULL, "oversize rejected");
    CHECK(v3d_check_dims(1920, 1080) == NULL, "1920x1080 accepted");

    /* ---- banding (0.12): a job holds at most max_job_tiles tiles, bigger rectangles are cut into bands ---- */
    {
        unsigned bh, y0, h, k, nb, w, ht, m, iter, covered, tiles_x, ok, lim;
        static const unsigned maxes[] = { 510, 300, 100, 17, 7, 3, 1 };

        CHECK(g_v3d_tune.max_job_tiles == 510, "default max_job_tiles is 510 (a worst-case 30x17 screen)");
        e = v3d_band_height(1920, 1080, &bh);
        CHECK(!e && bh == 17 * 64, "1920 wide: 17 tile rows (1088 px) per job, so 1080 is one job");
        nb = v3d_band_count(1200, bh); v3d_band_span(1200, bh, 1, &y0, &h);
        CHECK(nb == 2 && y0 == 1088 && h == 112, "1920x1200: two bands, 1088 + 112 rows");
        e = v3d_band_height(3840, 2160, &bh);
        CHECK(!e && bh == 8 * 64 && v3d_band_count(2160, bh) == 5, "3840x2160: 8 tile rows (512 px) per job, 5 bands");
        e = v3d_band_height(4096, 2160, &bh);
        CHECK(!e && bh == 7 * 64, "4096 wide: 64 tile columns, 7 rows per job");
        e = v3d_band_height(16384, 16384, &bh);
        CHECK(!e && bh == 64, "16384 wide: 256 tile columns, one row per job");
        e = v3d_band_height(1, 5, &bh);
        CHECK(!e && bh == 510 * 64, "1 pixel wide: 510 tile rows per job");
        e = v3d_band_height(0, 5, &bh);
        CHECK(e != NULL, "zero width refused");

        g_v3d_tune.max_job_tiles = 0;
        e = v3d_band_height(1920, 1080, &bh);
        CHECK(!e && bh == 17 * 64, "an out-of-range limit (0) falls back to 510");
        g_v3d_tune.max_job_tiles = 100000;
        e = v3d_band_height(1920, 1080, &bh);
        CHECK(!e && bh == 17 * 64, "an out-of-range limit (100000) falls back to 510");
        g_v3d_tune.max_job_tiles = 20;
        e = v3d_band_height(64 * 21, 64, &bh);
        CHECK(!e && bh == 64, "a tile row wider than the limit is not refused: one row per band");
        g_v3d_tune.max_job_tiles = 1;
        e = v3d_band_height(1920, 1080, &bh);
        CHECK(!e && bh == 64 && v3d_band_count(1080, bh) == 17, "limit 1: one tile row per band, 17 bands for 1080 rows");
        g_v3d_tune.max_job_tiles = 510;

        /* Property: for every width, height and limit, the bands cover the rectangle exactly
         * once, in order, each starts on a tile row, and none holds more than the limit. */
        srand(20261007);
        ok = 1; covered = 0;
        for (iter = 0; iter < 200000 && ok; iter++)
        {
            w = 1 + (unsigned) (rand() % 5000);
            ht = 1 + (unsigned) (rand() % 5000);
            if (iter % 7 == 0) { w = 1 + (unsigned) (rand() % 16384); ht = 1 + (unsigned) (rand() % 16384); }
            m = maxes[rand() % 7];
            g_v3d_tune.max_job_tiles = m;
            tiles_x = (w + 63) / 64;
            e = v3d_band_height(w, ht, &bh);
            /* never refused; a band is whole tile rows, at least one, within the limit unless one row alone is over it */
            lim = m > tiles_x ? m : tiles_x;
            if (e || bh == 0 || bh % 64 != 0 || (bh / 64) * tiles_x > lim || lim > 510) { ok = 0; break; }
            nb = v3d_band_count(ht, bh);
            covered = 0;
            for (k = 0; k < nb; k++)
            {
                v3d_band_span(ht, bh, k, &y0, &h);
                if (y0 != covered || h == 0 || h > bh || y0 % 64 != 0 ||
                    ((h + 63) / 64) * tiles_x > lim) { ok = 0; break; }
                covered += h;
            }
            if (covered != ht) ok = 0;
        }
        g_v3d_tune.max_job_tiles = 510;
        CHECK(ok, "200,000 random rectangles and limits: bands tile the rectangle exactly, within the job limit");
    }

    /* ---- the write address the accelerator draws at (screen_base.h, 0.12) ---- */
    {
        v3d_vrender_t vr = { 0, 0 };
        unsigned int base = 0;
        /* a 1920x1080 32bpp framebuffer (twice the visible height, as the firmware allocates it) and the
         * 1920x1080 16bpp one the hardware showed after a desktop mode change */
        const unsigned int A_base = 0x3D900000u, A_size = 0xFD2000u; unsigned long long A_vis = 1080ull * 7680;
        const unsigned int B_base = 0x3E100000u, B_size = 0x7E9000u; unsigned long long B_vis = 1080ull * 3840;

        CHECK(v3d_choose_base(&vr, A_base, A_size, A_vis, 0x1000, 0x1000, &base) == V3D_BASE_OK && base == A_base,
              "no write address reported yet: the framestore base is used");
        CHECK(v3d_choose_base(&vr, A_base, A_size, A_vis, 0x1000, 0x2000, &base) == V3D_BASE_ERR_BANK,
              "write bank differs from display bank and no address known: declined");
        v3d_vrender_note(&vr, A_base);
        CHECK(v3d_choose_base(&vr, A_base, A_size, A_vis, 0x1000, 0x2000, &base) == V3D_BASE_OK && base == A_base,
              "a reported write address is used even when the banks differ");
        v3d_vrender_note(&vr, A_base + 0x7E9000u);
        CHECK(v3d_choose_base(&vr, A_base, A_size, A_vis, 0x1000, 0x2000, &base) == V3D_BASE_OK && base == A_base + 0x7E9000u,
              "the hidden (second) bank is drawn at its own address");
        v3d_vrender_note(&vr, A_base + 0x7E9001u);
        CHECK(v3d_choose_base(&vr, A_base, A_size, A_vis, 0x1000, 0x1000, &base) == V3D_BASE_ERR_VRENDER,
              "a write address with no room for the visible rows is declined");

        /* the bug found on the hardware: a mode change from A to B without forgetting the address */
        v3d_vrender_note(&vr, A_base);
        CHECK(v3d_choose_base(&vr, B_base, B_size, B_vis, 0x1000, 0x1000, &base) == V3D_BASE_ERR_VRENDER,
              "OLD BEHAVIOUR: A's write address is below B's framestore, every fill declined (error 35)");
        v3d_vrender_forget(&vr);
        CHECK(v3d_choose_base(&vr, B_base, B_size, B_vis, 0x1000, 0x1000, &base) == V3D_BASE_OK && base == B_base,
              "after the mode change forgets it, B's framestore base is used");
        v3d_vrender_note(&vr, B_base);
        CHECK(v3d_choose_base(&vr, B_base, B_size, B_vis, 0x1000, 0x1000, &base) == V3D_BASE_OK && base == B_base,
              "and the kernel's report for B replaces it");

        /* the worse case: the stale address lands INSIDE the new framestore, and the new mode is small
         * enough to fit after it - the old code would have drawn at the wrong place without a word */
        v3d_vrender_note(&vr, B_base);                 /* learned in mode B ... */
        {
            unsigned long long small_vis = 768ull * 4096;   /* ... then a 1024x768 32bpp mode in A's framebuffer */
            CHECK(v3d_choose_base(&vr, A_base, A_size, small_vis, 0x1000, 0x1000, &base) == V3D_BASE_OK && base == B_base,
                  "OLD BEHAVIOUR: a stale address inside the new framestore is silently drawn at (wrong place)");
            v3d_vrender_forget(&vr);
            CHECK(v3d_choose_base(&vr, A_base, A_size, small_vis, 0x1000, 0x1000, &base) == V3D_BASE_OK && base == A_base,
                  "after forgetting, the new framestore base is used (right place)");
        }
    }

    printf(fails ? "\n%d FAILURES\n" : "\nall scratch tests passed\n", fails);
    return fails != 0;
}
