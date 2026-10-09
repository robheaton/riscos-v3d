/*
 * screen_base.h - which physical address the accelerator draws a screen at (0.12).
 *
 * Pure C with no kernel dependencies, so module.c and the host tests (test/host/) run the
 * SAME code. The inputs:
 *
 *   fs_base, fs_size   the framestore the display driver reports (GraphicsV 9): the
 *                      physical base and size of the whole allocation;
 *   vrender            the address the kernel last told the driver the VDU is writing
 *                      to (GraphicsV 6 with R0 = 3), if it has;
 *   screen_start /     the VDU variables ScreenStart and DisplayStart: the write bank
 *   display_start      and the displayed bank, equal unless a program is double buffering.
 *
 * The rule: use the write address when the kernel has reported one (it follows a program
 * that draws into the hidden bank); otherwise the framestore base, but only when the write
 * bank and the display bank are the same one.
 *
 * A reported write address belongs to ONE framebuffer. When the mode changes the display
 * driver allocates a new framebuffer (usually at a different address), and an address
 * learned for the old one is wrong for the new one: v0.12 first shipped without ever
 * forgetting it, and after a desktop mode change every fill was declined as "the write
 * address lies outside the framestore" (or, with an unlucky pair of modes, drawn at the
 * wrong place) until the next mode change. v3d_vrender_forget() is called at the start of
 * every mode change (GraphicsV 2, Service_PreModeChange); the kernel's own report for the
 * new mode, if it comes, replaces it.
 */
#ifndef SCREEN_BASE_H
#define SCREEN_BASE_H

typedef struct
{
    unsigned int valid;   /* the kernel has reported a write address since the last mode change */
    unsigned int phys;    /* ...and this is it */
} v3d_vrender_t;

static inline void v3d_vrender_note(v3d_vrender_t *v, unsigned int phys)
{
    v->phys = phys;
    v->valid = 1u;
}

static inline void v3d_vrender_forget(v3d_vrender_t *v)
{
    v->valid = 0u;
}

#define V3D_BASE_OK           0
#define V3D_BASE_ERR_VRENDER  1   /* the reported write address is outside the framestore */
#define V3D_BASE_ERR_BANK     2   /* write bank and display bank differ and no address is known */

/* vis_bytes = visible rows x line length: the write address must leave room for them. */
static inline int v3d_choose_base(const v3d_vrender_t *v,
                                  unsigned int fs_base, unsigned int fs_size,
                                  unsigned long long vis_bytes,
                                  unsigned int screen_start, unsigned int display_start,
                                  unsigned int *base_out)
{
    if (v->valid)
    {
        if (v->phys < fs_base ||
            (unsigned long long) (v->phys - fs_base) + vis_bytes > fs_size)
            return V3D_BASE_ERR_VRENDER;
        *base_out = v->phys;
        return V3D_BASE_OK;
    }
    if (screen_start != display_start)
        return V3D_BASE_ERR_BANK;
    *base_out = fs_base;
    return V3D_BASE_OK;
}

#endif /* SCREEN_BASE_H */
