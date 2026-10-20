/*
 * Host test (make hosttest): runs the REAL c/cl_copy.c (v3d_copy_screen and the one-pass ordered
 * v3d_copy_screen_overlapping, including run_copy's band splitting) and the real c/cl_common.c scratch
 * code against a MOCK v3d_job_run that parses the render control list the driver built and plays it
 * back on a model of the screen, tile by tile in the order the list gives:
 *
 *     tile (c, r):  LOAD  a full 64x64 block from  src  + row*stride + col*bpp   (not clipped)
 *                   STORE that block to              dest + ...                   CLIPPED to the job's frame
 *
 * (the model overlap_order_test.c uses, which the hardware tests confirmed). The result of every copy is
 * compared with a plain memmove of the same rectangle, over the WHOLE canvas, so a pixel written
 * outside the rectangle, a band moved to the wrong rows, a wrong stride or a wrong visiting order all
 * show up. Cases include scrolls up and down and sideways, both depths, tiny job limits that force many
 * bands, and full-screen scrolls at 1920x1200 and 3840x2160.
 * Built with cl_common.c compiled -Dv3d_job_run=v3d_job_run_real so the mock can take its place.
 */
#include "v3d_errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cl_common.h"
#include "cl_copy.h"
#include "cl_packets.h"
#include "v3d_irq.h"

volatile unsigned int g_v3d_irq_bin_done_count, g_v3d_irq_render_done_count;
volatile unsigned int g_v3d_irq_claimed_count, g_v3d_irq_spurious_count;
const io_mapping_t *v3d_irq_permanent_mapping(void) { return NULL; }
unsigned int v3d_read_centiseconds(void) { return 0; }
_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *a, _kernel_swi_regs *b)
{ static _kernel_oserror e = {1, "no swi on host"}; (void)swi;(void)a;(void)b; return &e; }

#define MOCK_PAGES 2200            /* room for the 8MB bounce buffer (2048 pages) and the scratch slot above it */
#define BACKING_BASE 0x10000000u
static unsigned char *backing;
_kernel_oserror *pmp_release(pmp_area_t *a, unsigned first, unsigned n)
{ v3d_scratch_note_release(a, first, n); return NULL; }
_kernel_oserror *pmp_alloc_policy(pmp_area_t *a, unsigned page, unsigned n, unsigned attr, unsigned *phys, void **log)
{ (void)a;(void)n;(void)attr; *phys = BACKING_BASE + page * 4096u; *log = backing + (size_t) page * 4096u; return NULL; }
_kernel_oserror *pmp_alloc(pmp_area_t *a, unsigned page, unsigned n, unsigned *phys, void **log)
{ return pmp_alloc_policy(a, page, n, 0, phys, log); }

/* ---- the model memory: the screen canvas at SCR_BASE, and the driver's own pages (the bounce buffer) ---- */
#define SCR_BASE 0x20000000u
static unsigned char *mem;            /* the canvas */
static size_t mem_bytes;
static unsigned int bpp;              /* bytes per pixel */
static unsigned long long njobs, max_tiles_job, nbad_range;

static unsigned char *addr_ptr(unsigned long long a, unsigned long long n)
{
    if (a >= SCR_BASE && a - SCR_BASE + n <= mem_bytes)
        return mem + (a - SCR_BASE);
    if (a >= BACKING_BASE && a - BACKING_BASE + n <= (unsigned long long) MOCK_PAGES * 4096u)
        return backing + (a - BACKING_BASE);
    return NULL;
}

_kernel_oserror *v3d_job_run(const v3d_job_t *job)
{
    unsigned char *p = backing + (job->rcl_start - BACKING_BASE);
    unsigned char *end = backing + (job->rcl_end - BACKING_BASE);
    unsigned int fw, fh, tcount = 0;
    static unsigned char block[64 * 64 * 4];

    if (job->overflow) { printf("JOB OVERFLOW FLAG\n"); exit(3); }
    njobs++;
    if (p[0] != 121) { printf("bad hdr0\n"); exit(2); }
    fw = p[2] | (p[3] << 8); fh = p[4] | (p[5] << 8);
    p += 9; p += 9; p += 9; p += 9; p += 2; p += 5;      /* the header packets, as in fillsim.c */
    if (p[0] != 122) { printf("bad hdr supertile\n"); exit(2); }
    p += 9;
    while (p < end)
    {
        unsigned int col, row, lstride, sstride, j;
        unsigned int laddr, saddr;

        if (p[0] == 13) { p++; break; }
        if (p[0] != 124) { printf("expected tile coords, got %u\n", p[0]); exit(2); }
        col = (p[1] | (p[2] << 8) | (p[3] << 16)) & 0xFFF; row = ((p[1] | (p[2] << 8) | (p[3] << 16)) >> 12) & 0xFFF;
        p += 4;
        if (p[0] != 30) { printf("expected load, got %u\n", p[0]); exit(2); }
        lstride = ((p[4] | (p[5] << 8) | (p[6] << 16)) >> 4) & 0xFFFFF;
        laddr = p[9] | (p[10] << 8) | (p[11] << 16) | ((unsigned) p[12] << 24);
        p += 13;
        if (p[0] != 26) { printf("expected end of loads\n"); exit(2); }
        p++;
        if (p[0] != 29) { printf("expected store, got %u\n", p[0]); exit(2); }
        sstride = ((p[4] | (p[5] << 8) | (p[6] << 16)) >> 4) & 0xFFFFF;
        saddr = p[9] | (p[10] << 8) | (p[11] << 16) | ((unsigned) p[12] << 24);
        p += 13;
        if (p[0] != 27) { printf("expected end of tile\n"); exit(2); }
        p++;
        tcount++;

        for (j = 0; j < 64; j++)                      /* LOAD: the whole 64x64 block, unclipped */
        {
            unsigned long long a = (unsigned long long) laddr + (unsigned long long) (row * 64 + j) * lstride
                                   + (unsigned long long) (col * 64) * bpp;
            unsigned char *src = addr_ptr(a, 64u * bpp);
            if (!src) { nbad_range++; memset(block + j * 64 * bpp, 0, 64u * bpp); continue; }
            memcpy(block + j * 64 * bpp, src, 64u * bpp);
        }
        for (j = 0; j < 64; j++)                      /* STORE: only what is inside the job's frame */
        {
            unsigned int yy = row * 64 + j, n;
            unsigned long long a;
            unsigned char *dst;
            if (yy >= fh) break;
            if (col * 64 >= fw) continue;
            n = fw - col * 64; if (n > 64) n = 64;
            a = (unsigned long long) saddr + (unsigned long long) yy * sstride + (unsigned long long) (col * 64) * bpp;
            dst = addr_ptr(a, (unsigned long long) n * bpp);
            if (!dst) { nbad_range++; continue; }
            memcpy(dst, block + j * 64 * bpp, (size_t) n * bpp);
        }
    }
    if (tcount > max_tiles_job) max_tiles_job = tcount;
    if (p != end) { printf("RCL parse did not end exactly\n"); exit(2); }
    return NULL;
}

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(unsigned n)
{ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned) (rng % n); }

static int verbose;
static unsigned long ncases, nrefused;

/* top-down pixel coordinates throughout, like the screen functions. overlapping: the ordered scroll
 * path (mode 1, or the two-pass bounce path when two_pass); otherwise the plain non-overlapping copy. */
static int run_case(const char *name, unsigned lw, unsigned bytes_pp, unsigned rows,
                    unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned w, unsigned h,
                    unsigned maxjob, int overlapping, int two_pass)
{
    pmp_area_t pmp = { 7, NULL, MOCK_PAGES };
    unsigned fmtc = bytes_pp == 4 ? 27 : 7;
    size_t n = (size_t) lw * bytes_pp * rows, i, rowb = (size_t) w * bytes_pp;
    unsigned char *ref, *tmp;
    unsigned tx = 0, ty = 0, y;
    _kernel_oserror *e;
    int ok;

    g_v3d_tune.max_job_tiles = maxjob;
    g_v3d_tune.overlap_mode = two_pass ? 0 : 1;
    v3d_scratch_reset_all();
    bpp = bytes_pp; mem_bytes = n;
    free(mem); mem = malloc(n);
    for (i = 0; i < n; i++) mem[i] = (unsigned char) ((i * 2654435761u) >> 11 ^ (i >> 9));
    ref = malloc(n); memcpy(ref, mem, n);
    tmp = malloc(rowb * h);
    for (y = 0; y < h; y++) memcpy(tmp + y * rowb, ref + ((size_t) (sy + y) * lw + sx) * bytes_pp, rowb);
    for (y = 0; y < h; y++) memcpy(ref + ((size_t) (dy + y) * lw + dx) * bytes_pp, tmp + y * rowb, rowb);
    free(tmp);
    njobs = max_tiles_job = nbad_range = 0;

    if (overlapping)
        e = v3d_copy_screen_overlapping(&pmp, 0, sx, sy, dx, dy, w, h, SCR_BASE, lw * bytes_pp, bytes_pp, fmtc, &tx, &ty);
    else
        e = v3d_copy_screen(&pmp, 0, sx, sy, dx, dy, w, h, SCR_BASE, lw * bytes_pp, bytes_pp, fmtc, &tx, &ty);
    if (e)
    {
        if (e->errnum == V3D_ERR(14) && two_pass) { nrefused++; free(ref); return 1; }  /* bounce buffer cap: refused by design */
        printf("%-40s ERR %d %s\n", name, e->errnum, e->errmess);
        free(ref);
        return 0;
    }
    ok = memcmp(mem, ref, n) == 0 && nbad_range == 0 && max_tiles_job <= 510 &&
         tx == (w + 63) / 64 && ty == (h + 63) / 64;
    if (!ok || verbose)
        printf("%-40s %s jobs=%llu maxTilesJob=%llu badRange=%llu\n", name, ok ? "OK  " : "FAIL",
               njobs, max_tiles_job, nbad_range);
    ncases++;
    free(ref);
    return ok;
}

int main(int argc, char **argv)
{
    int fails = 0;
    unsigned iter;
    static const unsigned maxes[] = { 510, 300, 100, 17, 8, 6, 3 };

    backing = calloc(MOCK_PAGES, 4096);
    verbose = argc > 1;
    (void) argv;

    /* full-screen scrolls at the sizes batching exists for, in all four directions (top-down y) */
    fails += !run_case("1920x1200 32 scroll up 21", 1920, 4, 1200 + 70, 0, 21, 0, 0, 1920, 1179, 510, 1, 0);
    fails += !run_case("1920x1200 32 scroll down 21", 1920, 4, 1200 + 70, 0, 0, 0, 21, 1920, 1179, 510, 1, 0);
    fails += !run_case("1920x1200 16 scroll up 21", 1920, 2, 1200 + 70, 0, 21, 0, 0, 1920, 1179, 510, 1, 0);
    fails += !run_case("1920x1200 16 scroll down 21", 1920, 2, 1200 + 70, 0, 0, 0, 21, 1920, 1179, 510, 1, 0);
    fails += !run_case("1920x1200 32 scroll right 37 down 5", 1920 + 64, 4, 1200 + 70, 0, 0, 37, 5, 1850, 1190, 510, 1, 0);
    fails += !run_case("1920x1200 32 scroll left 37 up 5", 1920 + 64, 4, 1200 + 70, 37, 5, 0, 0, 1850, 1190, 510, 1, 0);
    fails += !run_case("1920x1200 32 direct copy halves", 1920, 4, 1200 + 70, 0, 0, 0, 600, 1920, 600, 510, 0, 0);
    fails += !run_case("3840x2160 16 scroll up 21", 3840, 2, 2160 + 70, 0, 21, 0, 0, 3840, 2139, 510, 1, 0);
    fails += !run_case("3840x2160 16 scroll down 21", 3840, 2, 2160 + 70, 0, 0, 0, 21, 3840, 2139, 510, 1, 0);
    fails += !run_case("3840x2160 32 scroll down 21", 3840, 4, 2160 + 70, 0, 0, 0, 21, 3840, 2139, 510, 1, 0);

    /* random rectangles and shifts, with tiny job limits that force many bands */
    for (iter = 0; iter < 1500; iter++)
    {
        unsigned bytes_pp = (rnd(2) ? 4 : 2);
        unsigned w = 1 + rnd(500), h = 1 + rnd(500);
        unsigned m = maxes[rnd(7)];
        int overlapping = rnd(4) != 0;
        int two_pass = overlapping && rnd(5) == 0;
        unsigned sx = rnd(60), sy = rnd(60), dx, dy, lw, rows;
        char name[96];

        if (overlapping)
        {
            dx = (unsigned) ((int) sx + (int) rnd(161) - 80); dy = (unsigned) ((int) sy + (int) rnd(161) - 80);
            if ((int) dx < 0) dx = 0;
            if ((int) dy < 0) dy = 0;
            if (rnd(6) == 0) dx = sx;
            if (rnd(6) == 0) dy = sy;
            if (dx == sx && dy == sy) dx = sx + 1;
        }
        else
        {   /* non-overlapping: destination clear of the source */
            dx = sx + w + rnd(20); dy = sy;
            if (rnd(2)) { dx = sx; dy = sy + h + rnd(20); }
        }
        lw = (sx > dx ? sx : dx) + w + 64 + rnd(30);
        rows = (sy > dy ? sy : dy) + h + 64 + rnd(30);
        snprintf(name, sizeof name, "rnd%u %ux%u %u->%u,%u->%u m=%u %s%s", iter, w, h, sx, dx, sy, dy, m,
                 overlapping ? "O" : "-", two_pass ? "2" : "1");
        if (!run_case(name, lw, bytes_pp, rows, sx, sy, dx, dy, w, h, m, overlapping, two_pass))
        { fails++; if (fails > 10) break; }
    }
    g_v3d_tune.overlap_mode = 1; g_v3d_tune.max_job_tiles = 510;
    printf("copy banding: %lu copies (%lu refused as too wide for the job limit), %s (%d fails)\n",
           ncases, nrefused, fails ? "FAILURES" : "ALL OK", fails);
    return fails != 0;
}
