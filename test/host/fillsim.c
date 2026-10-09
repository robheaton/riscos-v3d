/*
 * Host test (make hosttest): runs the REAL c/cl_fill.c (run_fill with its band splitting, the solid
 * and the striped/dithered paths) and the real c/cl_common.c scratch code against a MOCK v3d_job_run
 * that parses the render control list the driver built and "renders" it with the model
 * address = tile base + y*stride + x*bytes_per_pixel, 32-bit wrapped like the hardware. For every
 * rectangle it checks that each pixel of the rectangle is stored exactly once, none outside it, with the
 * right colour (striped: the right row phase), that no job holds more than 510 tiles and that the tile
 * counts reported are right - including 4K, 16384x16384 and tiny job limits that force many bands.
 * (Whether the real hardware adds the per-tile offsets as modelled was confirmed by the earlier
 * hardware tests; this checks everything the band code does around it.)
 * Built with cl_common.c compiled -Dv3d_job_run=v3d_job_run_real so the mock can take its place.
 * First written as a scratch simulation by a reviewer of the 0.12 change.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cl_common.h"
#include "cl_fill.h"
#include "cl_packets.h"
#include "v3d_irq.h"

volatile unsigned int g_v3d_irq_bin_done_count, g_v3d_irq_render_done_count;
volatile unsigned int g_v3d_irq_claimed_count, g_v3d_irq_spurious_count;
const io_mapping_t *v3d_irq_permanent_mapping(void) { return NULL; }
unsigned int v3d_read_centiseconds(void) { return 0; }
_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *a, _kernel_swi_regs *b)
{ static _kernel_oserror e = {1, "no swi on host"}; (void)swi;(void)a;(void)b; return &e; }

#define MOCK_PAGES 400
static unsigned char *backing;
_kernel_oserror *pmp_release(pmp_area_t *a, unsigned first, unsigned n)
{ v3d_scratch_note_release(a, first, n); return NULL; }
_kernel_oserror *pmp_alloc_policy(pmp_area_t *a, unsigned page, unsigned n, unsigned attr, unsigned *phys, void **log)
{ (void)a;(void)n;(void)attr; *phys = 0x10000000u + page * 4096u; *log = backing + (size_t) page * 4096u; return NULL; }
_kernel_oserror *pmp_alloc(pmp_area_t *a, unsigned page, unsigned n, unsigned *phys, void **log)
{ return pmp_alloc_policy(a, page, n, 0, phys, log); }

/* ---- the "screen" ---- */
static unsigned int SCR_BASE = 0x20000000u;
static unsigned int line_len, bpp;              /* real line length (bytes) and bytes per pixel */
static unsigned int rx0, ry0, rw, rh;           /* rectangle: x, y(top-down), w, h in pixels */
static unsigned int phase0;                     /* colour index of rectangle row 0 */
static const unsigned int *cols8;               /* striped colours or NULL */
static unsigned int solid_col;
static unsigned char *seen;                     /* bitmap rows*pixels-per-line */
static unsigned long long nseen, nbad_outside, nbad_dup, nbad_col, nbad_addr;
static unsigned long long ntiles_total, njobs, max_tiles_job;
static unsigned int canvas_rows, canvas_cols;
static int verbose;
static unsigned long ncases;

static unsigned int exp_colour(unsigned r)      /* r = row relative to rectangle */
{ return cols8 ? cols8[(phase0 + r) % 8u] : solid_col; }

_kernel_oserror *v3d_job_run(const v3d_job_t *job)
{
    unsigned char *p = backing + (job->rcl_start - 0x10000000u);
    unsigned char *end = backing + (job->rcl_end - 0x10000000u);
    unsigned int fw, fh, col = 0, row = 0, clear = 0, tcount = 0;
    unsigned int fmt, stride, addr, ib;
    unsigned int i, j;
    int stored_rt0;

    if (job->overflow) { printf("JOB OVERFLOW FLAG\n"); exit(3); }
    njobs++;
    /* header */
    if (p[0] != 121) { printf("bad hdr0\n"); exit(2); }
    fw = p[2] | (p[3] << 8); fh = p[4] | (p[5] << 8);
    p += 9;
    if (p[0] != 121 || (p[1] & 0xF) != 3) { printf("bad hdr1\n"); exit(2); }
    clear = p[2] | (p[3] << 8) | (p[4] << 16) | ((unsigned) p[5] << 24);
    p += 9; p += 9; p += 9; p += 2; p += 5;
    if (p[0] != 122) { printf("bad hdr supertile\n"); exit(2); }
    p += 9;
    while (p < end)
    {
        if (p[0] == 13) { p++; break; }
        if (p[0] != 124) { printf("expected tile coords, got %u at %ld\n", p[0], (long)(p - (backing + (job->rcl_start - 0x10000000u)))); exit(2); }
        col = (p[1] | (p[2] << 8) | (p[3] << 16)) & 0xFFF; row = ((p[1] | (p[2] << 8) | (p[3] << 16)) >> 12) & 0xFFF;
        p += 4;
        if (p[0] != 26) { printf("expected end of loads\n"); exit(2); }
        p++;
        stored_rt0 = 0;
        if (p[0] != 29) { printf("expected store\n"); exit(2); }
        if (p[1] == 0) stored_rt0 = 1;
        fmt = (p[2] | (p[3] << 8)) >> 4 & 0x3F; stride = ((p[4] | (p[5] << 8) | (p[6] << 16)) >> 4) & 0xFFFFF;
        addr = p[9] | (p[10] << 8) | (p[11] << 16) | ((unsigned) p[12] << 24);
        p += 13;
        if (p[0] == 25) p += 2;   /* clear */
        if (p[0] != 27) { printf("expected end of tile\n"); exit(2); }
        p++;
        tcount += stored_rt0;
        if (stored_rt0)
        {
            ib = (fmt == 27) ? 4u : 2u;
            if (ib != bpp) { printf("format/bpp mismatch\n"); exit(2); }
            for (j = 0; j < 64; j++)
            {
                unsigned long long yy = (unsigned long long) row * 64 + j;
                if (yy >= fh) break;
                for (i = 0; i < 64; i++)
                {
                    unsigned long long xx = (unsigned long long) col * 64 + i;
                    unsigned int a;
                    unsigned long long off;
                    unsigned int r, c;
                    if (xx >= fw) break;
                    a = addr + (unsigned int) (yy * stride + xx * ib);   /* 32-bit wrap like the HW */
                    off = (unsigned long long) a - SCR_BASE;
                    if ((unsigned long long) a < SCR_BASE) { nbad_addr++; continue; }
                    r = (unsigned int) (off / line_len); c = (unsigned int) ((off % line_len) / ib);
                    if (r >= canvas_rows || c >= canvas_cols) { nbad_outside++; continue; }
                    if (r < ry0 || r >= ry0 + rh || c < rx0 || c >= rx0 + rw) { nbad_outside++; continue; }
                    {
                        unsigned long long bit = (unsigned long long) r * canvas_cols + c;
                        if (seen[bit >> 3] & (1u << (bit & 7))) nbad_dup++;
                        else { seen[bit >> 3] |= (unsigned char) (1u << (bit & 7)); nseen++; }
                    }
                    if (exp_colour(r - ry0) != clear) nbad_col++;
                }
            }
        }
    }
    ntiles_total += tcount;
    if (tcount > max_tiles_job) max_tiles_job = tcount;
    if (p != end) { printf("RCL parse did not end exactly (p-end=%ld)\n", (long)(p - end)); exit(2); }
    return NULL;
}

static unsigned long long expected_pixels(void) { return (unsigned long long) rw * rh; }

static int run_case(const char *name, unsigned lw_px, unsigned bytes_pp, unsigned x, unsigned y,
                    unsigned w, unsigned h, int striped, unsigned phase, unsigned maxjob)
{
    pmp_area_t pmp = { 7, NULL, MOCK_PAGES };
    _kernel_oserror *e;
    unsigned int fmtc = bytes_pp == 4 ? 27 : 7;
    unsigned int tx, ty;
    static const unsigned int c8[8] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                        0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u };
    unsigned long long total_bits;
    unsigned int target;
    int ok;

    g_v3d_tune.max_job_tiles = maxjob;
    v3d_scratch_reset_all();
    line_len = lw_px * bytes_pp; bpp = bytes_pp;
    rx0 = x; ry0 = y; rw = w; rh = h; phase0 = striped ? phase : 0;
    cols8 = striped ? c8 : NULL; solid_col = 0xA5A5A5A5u;
    canvas_cols = lw_px; canvas_rows = y + h + 70;    /* some margin below to catch overspill */
    total_bits = (unsigned long long) canvas_rows * canvas_cols;
    free(seen); seen = calloc((size_t) (total_bits / 8 + 8), 1);
    nseen = nbad_outside = nbad_dup = nbad_col = nbad_addr = ntiles_total = njobs = max_tiles_job = 0;

    target = SCR_BASE + y * line_len + x * bytes_pp;
    if (striped)
        e = v3d_striped_fill_raw(&pmp, 0, w, h, c8, phase, target, line_len, fmtc);
    else
        e = v3d_fill_raw(&pmp, 0, w, h, solid_col, target, line_len, fmtc, &tx, &ty);
    if (e)
    {
        printf("%-34s ERR %d %s\n", name, e->errnum, e->errmess);
        return 0;
    }
    ok = (nseen == expected_pixels() && !nbad_outside && !nbad_dup && !nbad_col && !nbad_addr && max_tiles_job <= 510);
    if (!striped)
        ok = ok && tx == (w + 63) / 64 && ty == (h + 63) / 64;
    if (!ok || verbose)
        printf("%-34s %s jobs=%llu maxTilesJob=%llu seen=%llu/%llu outside=%llu dup=%llu col=%llu addr=%llu\n",
               name, ok ? "OK  " : "FAIL", njobs, max_tiles_job, nseen, expected_pixels(),
               nbad_outside, nbad_dup, nbad_col, nbad_addr);
    ncases++;
    return ok;
}

int main(int argc, char **argv)
{
    int fails = 0;
    unsigned iter;
    backing = calloc(MOCK_PAGES, 4096);
    verbose = argc > 1;
    (void) argv;

    /* hand-picked */
    fails += !run_case("1920x1080 32 solid", 1920, 4, 0, 0, 1920, 1080, 0, 0, 510);
    fails += !run_case("1920x1200 32 solid", 1920, 4, 0, 0, 1920, 1200, 0, 0, 510);
    fails += !run_case("1920x1200 16 solid", 1920, 2, 0, 0, 1920, 1200, 0, 0, 510);
    fails += !run_case("3840x2160 32 solid", 3840, 4, 0, 0, 3840, 2160, 0, 0, 510);
    fails += !run_case("3840x2160 16 solid", 3840, 2, 0, 0, 3840, 2160, 0, 0, 510);
    fails += !run_case("3840x2160 32 offset 3,5", 3840, 4, 3, 5, 3837, 2155, 0, 0, 510);
    fails += !run_case("3840x2160 32 striped ph0", 3840, 4, 0, 0, 3840, 2160, 1, 0, 510);
    fails += !run_case("3840x2160 32 striped ph5", 3840, 4, 0, 0, 3840, 2160, 1, 5, 510);
    fails += !run_case("3840x2160 16 striped ph3", 3840, 2, 0, 0, 3840, 2160, 1, 3, 510);
    fails += !run_case("4096x2160 32 striped", 4096, 4, 0, 0, 4096, 2160, 1, 7, 510);
    fails += !run_case("1x16384 32 (tall thin)", 64, 4, 3, 0, 1, 16384, 0, 0, 510);
    fails += !run_case("64x16384 32", 64, 4, 0, 0, 64, 16384, 0, 0, 510);
    fails += !run_case("65x16384 32", 65, 4, 0, 0, 65, 16384, 0, 0, 510);
    fails += !run_case("16384x16384 32", 16384, 4, 0, 0, 16384, 16384, 0, 0, 510);
    fails += !run_case("16384x16384 16", 16384, 2, 0, 0, 16384, 16384, 0, 0, 510);
    fails += !run_case("16384x2000 32 striped", 16384, 4, 0, 0, 16384, 2000, 1, 6, 510);
    fails += !run_case("16384x16384 32 striped", 16384, 4, 0, 0, 16384, 16384, 1, 2, 510);
    fails += !run_case("16384x16384 16 striped", 16384, 2, 0, 0, 16384, 16384, 1, 1, 510);

    /* random, with low limits to force banding on small rectangles */
    srand(12345);
    for (iter = 0; iter < 1500; iter++)
    {
        static const unsigned maxes[] = { 510, 300, 100, 17, 7, 4, 2, 1 };
        unsigned bytes_pp = (rand() & 1) ? 4 : 2;
        unsigned w = 1 + rand() % 700, h = 1 + rand() % 700;
        unsigned x = rand() % 40, y = rand() % 40;
        unsigned lw = x + w + rand() % 50;
        unsigned m = maxes[rand() % 8];
        int striped = rand() % 3 == 0;
        unsigned phase = rand() % 8;
        char name[64];
        unsigned tilesx = (w + 63) / 64;
        pmp_area_t dummy; (void) dummy;
        (void) tilesx;
        snprintf(name, sizeof name, "rnd%u %ux%u m=%u %s", iter, w, h, m, striped ? "S" : "-");
        {
            /* silence per-case output */
            FILE *save = stdout; (void) save;
        }
        if (!run_case(name, lw, bytes_pp, x, y, w, h, striped, phase, m)) { fails++; if (fails > 10) break; }
    }
    printf("fill banding: %lu rectangles, %s (%d fails)\n", ncases, fails ? "FAILURES" : "ALL OK", fails);
    return fails != 0;
}
