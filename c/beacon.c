/*
 * beacon.c - see beacon.h. Only compiled into the diagnostic build (make beacons).
 */
#ifdef V3D_BEACONS

#include "beacon.h"
#include "kernel.h"

#define SWI_OS_ReadVduVariables 0x31

#define VDUVAR_LOG2_BPP      0x09
#define VDUVAR_LINE_LENGTH   0x06
#define VDUVAR_YWIND_LIMIT   0x0C
#define VDUVAR_DISPLAY_START 0x95

#define BAR_COLS    16u
#define BAR_ROWS    3u
#define CELL        22u   /* pixel pitch of one block                       */
#define BLOCK       20u   /* drawn size inside the cell                     */
#define MARGIN      12u   /* from the left and bottom edges of the screen   */
#define GROUP_GAP   10u   /* extra space after every fourth column, to count by */

static unsigned char *g_screen;      /* top-left pixel of the bar's area (NULL = not usable)  */
static unsigned int g_line;          /* bytes per pixel row                                   */
static unsigned int g_bytes_pp;      /* 2 or 4                                                */
static unsigned int g_current;       /* the step shown red (0 = none)                         */

/* Chosen at load time by v3d_beacon_configure; all off = the control build. */
static unsigned int g_draw = 0;      /* BDRAW: draw the bar                                   */
static unsigned int g_dsb = 0;       /* BDSB: barrier at every point                          */
static unsigned int g_zones = B_ZPOWER | B_ZCLEAR | B_ZLIFE;
unsigned int g_v3d_warm_detect = 0;
unsigned int g_v3d_legacy_perm = 0;
unsigned int g_v3d_legacy_dsb = 0;

#ifndef BEACON_DSB /* the host test (test/host/beacon_test.c) defines this away */
#define BEACON_DSB() __asm__ __volatile__(".word 0xF57FF04F" ::: "memory")
#endif
#define DSB() do { if (g_dsb) BEACON_DSB(); } while (0)

/* RISC OS PIXEL layouts (not the palette entry order &BBGGRR00): 32bpp is &00BBGGRR, 16bpp is %BBBBBGGGGGGRRRRR
 * (RGB565 - this hardware's real 16bpp mode). The first beacon builds used the palette order and showed cyan and
 * green instead of white and red; the positions were always right. */
static unsigned int colour(unsigned int which)
{
    /* 0 black, 1 grey outline, 2 white, 3 red */
    static const unsigned int c32[4] = { 0x00000000u, 0x00808080u, 0x00FFFFFFu, 0x000000FFu };
    static const unsigned int c16[4] = { 0x0000u, 0x8410u, 0xFFFFu, 0x001Fu };
    return g_bytes_pp == 4u ? c32[which] : c16[which];
}

static void put_pixel(unsigned int x, unsigned int y, unsigned int value)
{
    unsigned char *p = g_screen + y * g_line + x * g_bytes_pp;

    if (g_bytes_pp == 4u)
        *(volatile unsigned int *) p = value;
    else
        *(volatile unsigned short *) p = (unsigned short) value;
}

/* A block's pixels: a 2-pixel outline, and an inside of `fill`. */
static void draw_block(unsigned int step, unsigned int outline, unsigned int fill)
{
    unsigned int idx = step - 1u;
    unsigned int col = idx % BAR_COLS;
    unsigned int x0 = col * CELL + (col / 4u) * GROUP_GAP;
    unsigned int y0 = (idx / BAR_COLS) * CELL;
    unsigned int x, y;
    unsigned int o = colour(outline), f = colour(fill);

    for (y = 0; y < BLOCK; y++)
        for (x = 0; x < BLOCK; x++)
        {
            int edge = (x < 2u || y < 2u || x >= BLOCK - 2u || y >= BLOCK - 2u);
            put_pixel(x0 + x, y0 + y, edge ? o : f);
        }
}

void v3d_beacon_prepare(unsigned int zone)
{
    static const int vars[5] = { VDUVAR_LOG2_BPP, VDUVAR_LINE_LENGTH, VDUVAR_YWIND_LIMIT,
                                 VDUVAR_DISPLAY_START, -1 };
    static int results[4]; /* static: foreground only, and it keeps the host test's 32-bit pointer casts honest */
    _kernel_swi_regs regs;
    unsigned int log2bpp, rows, i;
    unsigned int bar_h = BAR_ROWS * CELL;

    g_screen = 0;
    g_current = 0;
    if (!g_draw || !(g_zones & zone))
        return;

    regs.r[0] = (int) vars;
    regs.r[1] = (int) results;
    if (_kernel_swi(SWI_OS_ReadVduVariables, &regs, &regs))
        return;

    log2bpp = (unsigned int) results[0];
    g_line = (unsigned int) results[1];
    rows = (unsigned int) results[2] + 1u;
    if (log2bpp != 4u && log2bpp != 5u)
        return; /* only the direct-colour depths this driver supports */
    if (!results[3] || rows < bar_h + 2u * MARGIN)
        return;

    g_bytes_pp = 1u << (log2bpp - 3u);
    g_screen = (unsigned char *) results[3] + (rows - bar_h - MARGIN) * g_line + MARGIN * g_bytes_pp;

    for (i = 1; i <= BAR_COLS * BAR_ROWS; i++)
        draw_block(i, 1u, 0u);
    DSB();
}

static unsigned int zone_of(unsigned int step)
{
    if (step >= B_CT_ENTER && step <= B_CT_RETURN)
        return B_ZCLEAR;
    if (step >= B_INIT_ENTER && step <= B_FINAL_DONE)
        return B_ZLIFE;
    return B_ZPOWER;
}

void v3d_beacon(unsigned int step)
{
    if (!(g_zones & zone_of(step)))
        return;

    if (g_draw && g_screen && step >= 1u && step <= BAR_COLS * BAR_ROWS)
    {
        if (g_current)
            draw_block(g_current, 1u, 2u); /* reached earlier: white */
        draw_block(step, 1u, 3u);          /* the latest: red */
        g_current = step;
    }
    DSB();
}

/* Case-insensitive substring search, as module.c's cmd_tail_has_word (target in upper case). */
static int has_word(const char *tail, const char *target)
{
    unsigned int i;

    if (!tail)
        return 0;
    for (; *tail; tail++)
    {
        for (i = 0; target[i]; i++)
        {
            char c = tail[i];
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

void v3d_beacon_configure(const char *cmd_tail)
{
    unsigned int z = 0;
    char line[100];
    unsigned int n = 0;
    _kernel_swi_regs regs;
    const char *s;

    g_draw = has_word(cmd_tail, "BDRAW") ? 1u : 0u;
    g_dsb = has_word(cmd_tail, "BDSB") ? 1u : 0u;
    if (has_word(cmd_tail, "BZPOWER")) z |= B_ZPOWER;
    if (has_word(cmd_tail, "BZCLEAR")) z |= B_ZCLEAR;
    if (has_word(cmd_tail, "BZLIFE")) z |= B_ZLIFE;
    g_zones = z ? z : (B_ZPOWER | B_ZCLEAR | B_ZLIFE);
    g_v3d_warm_detect = has_word(cmd_tail, "WARM") ? 1u : 0u;
    g_v3d_legacy_perm = has_word(cmd_tail, "LPERM") ? 1u : 0u;
    g_v3d_legacy_dsb = has_word(cmd_tail, "LDSB") ? 1u : 0u;

    /* one line saying what this load will do, so the tester can see which variant is running */
    s = "V3DDriver BEACONS variant: ";
    while (*s) line[n++] = *s++;
    s = g_draw ? "DRAW " : "nodraw ";
    while (*s) line[n++] = *s++;
    s = g_dsb ? "DSB " : "nodsb ";
    while (*s) line[n++] = *s++;
    s = "zones ";
    while (*s) line[n++] = *s++;
    if (g_zones & B_ZPOWER) { line[n++] = 'P'; }
    if (g_zones & B_ZCLEAR) { line[n++] = 'C'; }
    if (g_zones & B_ZLIFE)  { line[n++] = 'L'; }
    s = g_v3d_warm_detect ? " WARM" : " noWARM";
    while (*s) line[n++] = *s++;
    s = g_v3d_legacy_perm ? " LPERM" : " noLPERM";
    while (*s) line[n++] = *s++;
    s = g_v3d_legacy_dsb ? " LDSB" : " noLDSB";
    while (*s) line[n++] = *s++;
    line[n++] = 0;
    regs.r[0] = (int) line;
    (void) _kernel_swi(0x02 /* OS_Write0 */, &regs, &regs);
    (void) _kernel_swi(0x03 /* OS_NewLine */, &regs, &regs);
}

#endif /* V3D_BEACONS */
