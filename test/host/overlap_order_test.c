/*
 * Host simulation: is a SINGLE-PASS overlapping copy through 64x64 tiles exactly
 * equal to memmove, if the tiles are visited in the right order?
 *
 * Model of what run_copy (c/cl_copy.c) asks the V3D to do, one tile at a time,
 * strictly in sequence (V3D 4.2 on the Pi 4 has one core, and our render control
 * list uses explicit TileCoordinates, so the order is exactly the emission order):
 *     tile (c,r): LOAD  the full 64x64 block at  src  + (64c, 64r)   (even where it
 *                       runs past the rectangle - the load is not clipped)
 *                 STORE that block to              dest + (64c, 64r)  CLIPPED to the
 *                       rectangle's width/height
 * Rows are in memory order (top-down), so "later" = higher address.
 *
 * The rule under test: for dest - src = (dx, dy),
 *     columns descending when dx > 0, ascending otherwise,
 *     rows    descending when dy > 0, ascending otherwise.
 * i.e. visit first the tiles that lie AHEAD in the direction of the shift, because
 * those are the ones whose source a store at this tile would overwrite.
 *
 * Also runs the FORWARD order (always ascending) for contrast: it must produce
 * failures, which is what the original (rejected) analysis in the README found.
 *
 *   make hosttest        or:  gcc -O2 -o overlap_order_test overlap_order_test.c && ./overlap_order_test 200000
 * (the argument is the number of random attempts; default 20000, about 8 s).
 * 200,000 attempts (148,059 usable cases, 74,210 overlapping) gave 0 ordered failures
 * and 45,151 forward-order failures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE 64
#define MEMW 700   /* pixels per memory row (stride) */
#define MEMH 700

static unsigned int mem[MEMH][MEMW];
static unsigned int ref[MEMH][MEMW];

static unsigned long long rng = 88172645463325252ull;
static unsigned int rnd(unsigned int n)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned int) (rng % n);
}

static int ceil_div(int a, int b) { return (a + b - 1) / b; }

/* modes: 0 = forward (always ascending), 1 = ordered by shift sign */
static void tile_copy(int sx, int sy, int dx, int dy, int w, int h, int mode)
{
    int nx = ceil_div(w, TILE), ny = ceil_div(h, TILE);
    int c, r, i, j, ci, ri;
    unsigned int buf[TILE][TILE];

    for (ri = 0; ri < ny; ri++)
    {
        r = (mode == 1 && dy > sy) ? ny - 1 - ri : ri;
        for (ci = 0; ci < nx; ci++)
        {
            c = (mode == 1 && dx > sx) ? nx - 1 - ci : ci;

            for (j = 0; j < TILE; j++)           /* LOAD: unclipped 64x64 */
                for (i = 0; i < TILE; i++)
                    buf[j][i] = mem[sy + 64 * r + j][sx + 64 * c + i];

            for (j = 0; j < TILE; j++)           /* STORE: clipped to the rectangle */
                for (i = 0; i < TILE; i++)
                    if (64 * c + i < w && 64 * r + j < h)
                        mem[dy + 64 * r + j][dx + 64 * c + i] = buf[j][i];
        }
    }
}

static void fill_memory(void)
{
    int x, y;
    for (y = 0; y < MEMH; y++)
        for (x = 0; x < MEMW; x++)
            mem[y][x] = (unsigned int) ((y << 16) | x) ^ 0x5A5A0000u;
}

int main(int argc, char **argv)
{
    long limit = argc > 1 ? atol(argv[1]) : 20000;
    long cases = 0, bad_ordered = 0, bad_forward = 0, overlapping = 0;
    long k;

    for (k = 0; k < limit; k++)
    {
        int w, h, sx, sy, dx, dy, x, y, mode;
        static const int edge[] = {1, 2, 3, 63, 64, 65, 127, 128, 129, 191, 192, 193};

        w = (rnd(2) ? edge[rnd(12)] : 1 + (int) rnd(200));
        h = (rnd(2) ? edge[rnd(12)] : 1 + (int) rnd(200));
        sx = 70 + (int) rnd(60); sy = 70 + (int) rnd(60);
        /* shifts both smaller and larger than a tile, any sign, one axis possibly 0 */
        dx = sx + (int) rnd(301) - 150;
        dy = sy + (int) rnd(301) - 150;
        if (rnd(6) == 0) dx = sx;
        if (rnd(6) == 0) dy = sy;
        if (dx == sx && dy == sy) dx = sx + 1;
        /* keep everything inside the (padded) memory */
        if (dx < 0 || dy < 0) continue;
        if (sx + w + TILE >= MEMW || sy + h + TILE >= MEMH ||
            dx + w + TILE >= MEMW || dy + h + TILE >= MEMH) continue;

        {
            int ov = sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h;
            overlapping += ov;
        }

        for (mode = 0; mode <= 1; mode++)
        {
            int bad = 0;
            fill_memory();
            memcpy(ref, mem, sizeof mem);
            /* reference: true memmove semantics (copy the source out first) */
            {
                static unsigned int tmp[MEMH][MEMW];
                for (y = 0; y < h; y++)
                    for (x = 0; x < w; x++)
                        tmp[y][x] = ref[sy + y][sx + x];
                for (y = 0; y < h; y++)
                    for (x = 0; x < w; x++)
                        ref[dy + y][dx + x] = tmp[y][x];
            }
            tile_copy(sx, sy, dx, dy, w, h, mode);
            for (y = 0; y < MEMH && !bad; y++)
                for (x = 0; x < MEMW; x++)
                    if (mem[y][x] != ref[y][x]) { bad = 1; break; }
            if (bad)
            {
                if (mode == 1)
                {
                    if (bad_ordered < 5)
                        printf("ORDERED FAILURE: %dx%d src(%d,%d) -> dest(%d,%d)\n", w, h, sx, sy, dx, dy);
                    bad_ordered++;
                }
                else
                    bad_forward++;
            }
        }
        cases++;
    }

    printf("%ld cases (%ld genuinely overlapping)\n", cases, overlapping);
    printf("ordered by shift sign : %ld failures\n", bad_ordered);
    printf("always-forward order  : %ld failures (expected: many - this is the case the old analysis rejected)\n", bad_forward);
    return bad_ordered != 0;
}
