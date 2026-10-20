/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * beacon_test.c - draws the beacon bar into a fake framebuffer on the host and writes PPM pictures, so
 * the layout and colours can be looked at before a diagnostic build goes anywhere near the hardware.
 * Also checks by assertion that: nothing is written outside the bar's rectangle, step N lands in block
 * N, exactly one block is red, and earlier steps stay white. Build/run: see `make beacontest`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <stdint.h>
#include "kernel.h"
#include "beacon.h"

static unsigned char *g_fb;
static int g_log2bpp = 5, g_line = 0, g_ylim = 1079;
static int g_swi_calls = 0;

_kernel_oserror *_kernel_swi(int no, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    int *res = (int *) (uintptr_t) (unsigned int) in->r[1];
    (void) out;
    g_swi_calls++;
    if (no != 0x31)
        return 0; /* OS_Write0 / OS_NewLine from configure: ignore */
    res[0] = g_log2bpp;
    res[1] = g_line;
    res[2] = g_ylim;
    res[3] = (int) (uintptr_t) g_fb;
    return 0;
}

static unsigned int px(int x, int y)
{
    int bpp = 1 << (g_log2bpp - 3);
    unsigned char *p = g_fb + (size_t) y * g_line + (size_t) x * bpp;
    return bpp == 4 ? *(unsigned int *) p : *(unsigned short *) p;
}

static void dump(const char *name, int w, int h)
{
    FILE *f = fopen(name, "wb");
    int x, y;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            unsigned int v = px(x, y), r, g, b;
            if (g_log2bpp == 5) { r = v & 255; g = (v >> 8) & 255; b = (v >> 16) & 255; }
            else { r = (v & 31) * 8; g = ((v >> 5) & 63) * 4; b = ((v >> 11) & 31) * 8; }
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    fclose(f);
}

static void run(int log2bpp, int w, int h, const char *tag)
{
    int bpp = 1 << (log2bpp - 3), step, x, y, red;
    unsigned int white, redv;
    size_t size;
    char name[64];

    g_log2bpp = log2bpp; g_line = w * bpp; g_ylim = h - 1;
    size = (size_t) g_line * h;
    g_fb = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(g_fb != MAP_FAILED);
    memset(g_fb, 0x55, size); /* a recognisable "desktop" so strays show up */

    v3d_beacon_configure("NoAutoStart bdraw bdsb");
    v3d_beacon_prepare(B_ZPOWER);
    for (step = 1; step <= 18; step++) v3d_beacon(step);

    white = log2bpp == 5 ? 0x00FFFFFFu : 0xFFFFu;
    redv  = log2bpp == 5 ? 0x000000FFu : 0x001Fu;

    /* the bar lives in x 12..12+352, y h-12-66..h-12 ; anything else must still be 0x55 filler */
    {
        int bx0 = 12, bx1 = 12 + 16 * 22 + 3 * 10, by1 = h - 12, by0 = by1 - 3 * 22;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
            {
                int inside = x >= bx0 && x < bx1 && y >= by0 && y < by1;
                if (!inside)
                {
                    unsigned int want = log2bpp == 5 ? 0x55555555u : 0x5555u;
                    if (px(x, y) != want) { printf("stray write at %d,%d\n", x, y); exit(1); }
                }
            }
        /* centre of each block: steps 1..17 white, 18 red, 19.. empty (black) */
        red = 0;
        for (step = 1; step <= 48; step++)
        {
            int idx = step - 1;
            unsigned int v = px(bx0 + (idx % 16) * 22 + ((idx % 16) / 4) * 10 + 10, by0 + (idx / 16) * 22 + 10);
            if (v == redv) red++;
            if (step < 18) assert(v == white);
            if (step == 18) assert(v == redv);
            if (step > 18) assert(v == 0);
        }
        assert(red == 1);
    }
    snprintf(name, sizeof name, "beacon_%s.ppm", tag);
    /* dump only the bottom-left corner region so the picture is small */
    {
        unsigned char *save = g_fb; int sline = g_line;
        g_fb = save + (size_t) (h - 120) * g_line;
        dump(name, 440, 120);
        g_fb = save; g_line = sline;
    }
    munmap(g_fb, size);
    printf("ok %s\n", tag);
}

/* Count the framebuffer words that differ from the filler after running a variant. */
static long touched(const char *tail, unsigned int zone, unsigned int first, unsigned int last, int *swis)
{
    size_t size, i;
    long n = 0;

    g_log2bpp = 5; g_line = 1920 * 4; g_ylim = 1079;
    size = (size_t) g_line * 1080;
    g_fb = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(g_fb != MAP_FAILED);
    memset(g_fb, 0x55, size);
    g_swi_calls = 0;
    v3d_beacon_configure(tail);
    g_swi_calls = 0; /* only count what prepare/beacon do from here */
    v3d_beacon_prepare(zone);
    for (i = first; i <= last; i++) v3d_beacon((unsigned int) i);
    *swis = g_swi_calls;
    for (i = 0; i < size; i++) if (g_fb[i] != 0x55) n++;
    munmap(g_fb, size);
    return n;
}

int main(void)
{
    int sw;

    run(5, 1920, 1080, "32bpp");
    run(4, 1920, 1200, "16bpp");

    /* the CONTROL (no switches): the points must do nothing at all - no stores, no SWIs */
    assert(touched("NoAutoStart", B_ZPOWER, 1, 22, &sw) == 0 && sw == 0);
    assert(touched("NoAutoStart", B_ZCLEAR, 23, 32, &sw) == 0 && sw == 0);
    assert(touched("BDSB", B_ZPOWER, 1, 22, &sw) == 0 && sw == 0);   /* barrier only: still no stores, no SWI */
    /* draw switch: stores happen, and exactly one SWI (OS_ReadVduVariables) per prepare */
    assert(touched("BDRAW", B_ZPOWER, 1, 22, &sw) > 0 && sw == 1);
    /* zones: with BZCLEAR only, power-zone steps and prepare draw nothing; clear-zone steps do */
    assert(touched("BDRAW BZCLEAR", B_ZPOWER, 1, 22, &sw) == 0 && sw == 0);
    assert(touched("BDRAW BZCLEAR", B_ZCLEAR, 23, 32, &sw) > 0 && sw == 1);
    assert(touched("BDRAW BZPOWER", B_ZCLEAR, 23, 32, &sw) == 0 && sw == 0);
    assert(touched("BDRAW BZLIFE", B_ZLIFE, 33, 40, &sw) > 0 && sw == 1);
    printf("ok variants (control draws nothing and makes no SWI; zones filter)\n");
    return 0;
}
