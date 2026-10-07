/*
 * cl_common.c - see cl_common.h.
 */

#include <stddef.h>
#include "cl_common.h"
#include "cl_packets.h"
#include "v3d_regs.h"
#include "io_map.h"
#include "v3d_irq.h"

/* ---------------------------------------------------------------- */
/* Tunables, health, profile                                         */
/* ---------------------------------------------------------------- */

v3d_tune_t g_v3d_tune = { 1, 1, 1 };

volatile unsigned int g_v3d_wedged = 0;
unsigned int g_v3d_wedge_count = 0;
unsigned int g_v3d_irq_rescue_count = 0;
v3d_rescue_diag_t g_v3d_rescue_diag[V3D_RESCUE_DIAG_MAX];
unsigned int g_v3d_jobs_run = 0;
v3d_timeout_diag_t g_v3d_timeout_diag;

v3d_prof_t g_v3d_prof[2];

static _kernel_oserror err_wedged = {
    40, "V3DDriver: GPU acceleration is switched off because a GPU job timed "
        "out earlier - reboot, or use V3D_AccelTune 6 to switch it back on"
};

static _kernel_oserror err_bad_dims = {
    10, "V3DDriver: rectangle width/height must be non-zero and no larger "
        "than 16384"
};

static _kernel_oserror err_too_big = {
    14, "V3DDriver: rectangle too large for the cached scratch buffers "
        "(screens above 1920x1088 are not accelerated yet)"
};

static _kernel_oserror err_no_slot = {
    15, "V3DDriver: no free scratch slot (too many distinct PMP bases in "
        "use)"
};

static _kernel_oserror err_overflow = {
    42, "V3DDriver: internal error - a control list outgrew its buffer; "
        "job not submitted"
};

static _kernel_oserror err_timeout[2][2] = {
    {
        { 11, "V3DDriver: fill timed out waiting for binning" },
        { 12, "V3DDriver: fill timed out waiting for rendering" }
    },
    {
        { 11, "V3DDriver: copy timed out waiting for binning" },
        { 12, "V3DDriver: copy timed out waiting for rendering" }
    }
};

_kernel_oserror *v3d_gpu_check(void)
{
    return g_v3d_wedged ? &err_wedged : NULL;
}

void v3d_gpu_rearm(void)
{
    g_v3d_wedged = 0;
}

void v3d_prof_reset(void)
{
    unsigned int k;
    for (k = 0; k < 2u; k++)
    {
        g_v3d_prof[k].n = 0;
        g_v3d_prof[k].setup_us = 0;
        g_v3d_prof[k].bin_us = 0;
        g_v3d_prof[k].mid_us = 0;
        g_v3d_prof[k].render_us = 0;
        g_v3d_prof[k].total_us = 0;
        g_v3d_prof[k].max_us = 0;
    }
}

/* ---------------------------------------------------------------- */
/* Timing                                                            */
/* ---------------------------------------------------------------- */

extern unsigned int v3d_read_centiseconds(void);

#define ST_PHYS_BASE (PERIPHERAL_BASE + 0x3000u) /* BCM2711 system timer */

static volatile unsigned int *g_timer_base = NULL;
static unsigned int g_timer_state = 0; /* 0 untried, 1 fine, 2 unavailable */

static void timer_init(void)
{
    volatile unsigned int *base;
    unsigned int first, spin;

    g_timer_state = 2;

    if (io_map_permanent(ST_PHYS_BASE, 0x1000u, &base))
        return;

    /* Do not trust an address until the counter is seen to tick: one
     * tick per microsecond, so a few thousand idle iterations must move
     * it. If it does not, ST_CLO is not where we think it is. */
    first = base[1];
    for (spin = 0; spin < 200000u; spin++)
    {
        if (base[1] != first)
        {
            g_timer_base = base;
            g_timer_state = 1;
            return;
        }
    }
}

unsigned int v3d_time_us(void)
{
    if (g_timer_state == 0)
        timer_init();
    if (g_timer_state == 1)
        return g_timer_base[1];
    return v3d_read_centiseconds() * 10000u;
}

unsigned int v3d_time_is_fine(void)
{
    if (g_timer_state == 0)
        timer_init();
    return g_timer_state == 1 ? 1u : 0u;
}

/* ---------------------------------------------------------------- */
/* Scratch slots                                                     */
/* ---------------------------------------------------------------- */

#define MAX_JOB_SLOTS 10u
#define MAX_BIG_SLOTS 2u

typedef struct
{
    pmp_area_t *pmp;
    unsigned int base_page;
    unsigned int pages;
    unsigned int phys;
    void *log;
} big_slot_t;

static v3d_scratch_t g_slots[MAX_JOB_SLOTS];
static unsigned int g_slot_count = 0;

static big_slot_t g_big_slots[MAX_BIG_SLOTS];
static unsigned int g_big_count = 0;

/* Set when a slot could not be built, so a failing allocation is not
 * retried (and OS_DynamicArea does not broadcast Service_MemoryMoved)
 * from inside every later GraphicsV call. Cleared by V3D_AccelTune 5/6. */
static unsigned int g_scratch_failed = 0;
static _kernel_oserror g_scratch_fail_err;
static unsigned int g_big_failed = 0;
static _kernel_oserror g_big_fail_err;

static void remember_error(_kernel_oserror *dst, const _kernel_oserror *src)
{
    unsigned int i;

    dst->errnum = src->errnum;
    for (i = 0; i < sizeof(dst->errmess) - 1u && src->errmess[i]; i++)
        dst->errmess[i] = src->errmess[i];
    dst->errmess[i] = '\0';
}

static void scratch_sizes(unsigned int n_tiles, unsigned int *alloc_pages,
                           unsigned int *state_pages, unsigned int *rcl_pages)
{
    /* tile_alloc: align(N*64,4096) + 64*64*2, transcribed from the
     * reference's gl_rainbow_triangle.cpp - the second term is a literal
     * 8192 bytes, not scaled by tile count. tile_state: 256 bytes/tile on
     * V3D 4.x. RCL: ~53 bytes fixed + 40/tile for a fill (32 for a copy),
     * plus slack. */
    unsigned int alloc_bytes = ((n_tiles * TILE_SIZE + 4095u) & ~4095u) +
                                TILE_SIZE * TILE_SIZE * 2u;

    *alloc_pages = v3d_bytes_to_pages(alloc_bytes);
    *state_pages = v3d_bytes_to_pages(n_tiles * 256u);
    *rcl_pages = v3d_bytes_to_pages(64u + n_tiles * 40u + 64u);
}

unsigned int v3d_tile_alloc_bytes(unsigned int n_tiles)
{
    unsigned int a, st, r;

    scratch_sizes(n_tiles, &a, &st, &r);
    return a * PAGE_SIZE;
}

static void drop_job_slot(unsigned int i)
{
    g_slot_count--;
    if (i != g_slot_count)
        g_slots[i] = g_slots[g_slot_count];
}

static void drop_big_slot(unsigned int i)
{
    g_big_count--;
    if (i != g_big_count)
        g_big_slots[i] = g_big_slots[g_big_count];
}

void v3d_scratch_note_release(const pmp_area_t *area, unsigned int first_page,
                               unsigned int num_pages)
{
    unsigned int i;
    unsigned int last = first_page + num_pages; /* exclusive */

    i = 0;
    while (i < g_slot_count)
    {
        v3d_scratch_t *s = &g_slots[i];

        if (s->pmp && s->pmp->area_num == area->area_num &&
            s->base_page < last && first_page < s->base_page + s->total_pages)
            drop_job_slot(i); /* re-examine index i: it now holds another */
        else
            i++;
    }

    i = 0;
    while (i < g_big_count)
    {
        big_slot_t *g = &g_big_slots[i];

        if (g->pmp && g->pmp->area_num == area->area_num &&
            g->base_page < last && first_page < g->base_page + g->pages)
            drop_big_slot(i);
        else
            i++;
    }
}

_kernel_oserror *v3d_scratch_get(pmp_area_t *pmp, unsigned int base_page,
                                  unsigned int n_tiles, v3d_scratch_t **out)
{
    unsigned int need_a, need_s, need_r, cap_a, cap_s, cap_r;
    unsigned int i, page, policy;
    v3d_scratch_t *slot;
    v3d_scratch_t tmp;
    _kernel_oserror *err;

    scratch_sizes(n_tiles, &need_a, &need_s, &need_r);

    /* Cache hit: the slot is only usable if THIS request fits it. v0.06
     * returned it unconditionally, so after any small job had created the
     * slot a later job with more than ~512 tiles wrote past it. */
    for (i = 0; i < g_slot_count; i++)
    {
        slot = &g_slots[i];
        if (slot->base_page == base_page && slot->pmp->area_num == pmp->area_num)
        {
            if (need_a > slot->tile_alloc_pages ||
                need_s > slot->tile_state_pages ||
                need_r > slot->rcl_pages)
                return &err_too_big;
            *out = slot;
            return NULL;
        }
    }

    if (g_scratch_failed)
        return &g_scratch_fail_err;

    scratch_sizes(WORST_CASE_N_TILES, &cap_a, &cap_s, &cap_r);
    if (need_a > cap_a || need_s > cap_s || need_r > cap_r)
        return &err_too_big;

    if (g_slot_count >= MAX_JOB_SLOTS)
        return &err_no_slot;

    /* Built in a local, registered only when complete: pmp_alloc_policy()
     * releases its range first, which calls v3d_scratch_note_release() and
     * can drop (and so re-order) slots already in the table. */
    tmp.pmp = pmp;
    tmp.base_page = base_page;
    tmp.total_pages = 1u + cap_r + cap_a + cap_s;
    tmp.rcl_pages = cap_r;
    tmp.tile_alloc_pages = cap_a;
    tmp.tile_state_pages = cap_s;
    tmp.attr = g_v3d_tune.scratch_attr;
    policy = tmp.attr ? PMP_ATTR_NC_BUFFERABLE : PMP_ATTR_STRONGLY_ORDERED;

    page = base_page;
    err = pmp_alloc_policy(pmp, page, 1u, policy, &tmp.cl_phys, &tmp.cl_log);
    page += 1u;
    if (!err)
    {
        err = pmp_alloc_policy(pmp, page, cap_r, policy, &tmp.rcl_phys, &tmp.rcl_log);
        page += cap_r;
    }
    if (!err)
    {
        err = pmp_alloc_policy(pmp, page, cap_a, policy, &tmp.tile_alloc_phys, &tmp.tile_alloc_log);
        page += cap_a;
    }
    if (!err)
        err = pmp_alloc_policy(pmp, page, cap_s, policy, &tmp.tile_state_phys, &tmp.tile_state_log);

    if (err)
    {
        /* Give back whatever was claimed and remember the failure, so the
         * next GraphicsV call does not repeat it. */
        remember_error(&g_scratch_fail_err, err);
        g_scratch_failed = 1;
        (void) pmp_release(pmp, base_page, tmp.total_pages);
        return &g_scratch_fail_err;
    }

    /* The table may have been compacted by the releases above, so check
     * there is still room and register at the CURRENT end. */
    if (g_slot_count >= MAX_JOB_SLOTS)
        return &err_no_slot;
    g_slots[g_slot_count] = tmp;
    *out = &g_slots[g_slot_count];
    g_slot_count++;
    return NULL;
}

_kernel_oserror *v3d_overlap_buffer_get(pmp_area_t *pmp, unsigned int base_page,
                                         unsigned int pages_needed,
                                         unsigned int *phys_out, void **log_out)
{
    unsigned int i;
    big_slot_t *g;
    big_slot_t tmp;
    _kernel_oserror *err;

    for (i = 0; i < g_big_count; i++)
    {
        g = &g_big_slots[i];
        if (g->base_page == base_page && g->pmp->area_num == pmp->area_num)
        {
            if (pages_needed > g->pages)
                return &err_too_big;
            *phys_out = g->phys;
            *log_out = g->log;
            return NULL;
        }
    }

    if (g_big_failed)
        return &g_big_fail_err;

    if (g_big_count >= MAX_BIG_SLOTS)
        return &err_no_slot;

    if (pages_needed > PMP_MAX_PAGES_PER_CALL)
        return &err_too_big;

    tmp.pmp = pmp;
    tmp.base_page = base_page;
    tmp.pages = PMP_MAX_PAGES_PER_CALL;

    err = pmp_alloc_policy(pmp, base_page, PMP_MAX_PAGES_PER_CALL,
                            PMP_ATTR_STRONGLY_ORDERED, &tmp.phys, &tmp.log);
    if (err)
    {
        remember_error(&g_big_fail_err, err);
        g_big_failed = 1;
        (void) pmp_release(pmp, base_page, PMP_MAX_PAGES_PER_CALL);
        return &g_big_fail_err;
    }

    if (g_big_count >= MAX_BIG_SLOTS)
        return &err_no_slot;
    g_big_slots[g_big_count] = tmp;
    g_big_count++;
    *phys_out = tmp.phys;
    *log_out = tmp.log;
    return NULL;
}

void v3d_scratch_reset_all(void)
{
    /* pmp_release() calls v3d_scratch_note_release() first, which removes
     * the matching slot, so each pass shrinks the table by one. The
     * "count unchanged" guard stops a failed release looping forever. */
    while (g_slot_count)
    {
        unsigned int before = g_slot_count;
        v3d_scratch_t s = g_slots[g_slot_count - 1u];

        (void) pmp_release(s.pmp, s.base_page, s.total_pages);
        if (g_slot_count == before)
            g_slot_count--;
    }

    while (g_big_count)
    {
        unsigned int before = g_big_count;
        big_slot_t g = g_big_slots[g_big_count - 1u];

        (void) pmp_release(g.pmp, g.base_page, g.pages);
        if (g_big_count == before)
            g_big_count--;
    }

    g_scratch_failed = 0;
    g_big_failed = 0;
}

static void zero_words(void *p, unsigned int bytes)
{
    volatile unsigned int *w = (volatile unsigned int *) p;
    unsigned int n = bytes >> 2;

    while (n >= 8u)
    {
        w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 0;
        w[4] = 0; w[5] = 0; w[6] = 0; w[7] = 0;
        w += 8;
        n -= 8u;
    }
    while (n--)
        *w++ = 0;
}

void v3d_scratch_zero(v3d_scratch_t *s, unsigned int n_tiles)
{
    unsigned int alloc_bytes, state_bytes;

    if (g_v3d_tune.zero_mode == 1u)
    {
        alloc_bytes = n_tiles * TILE_SIZE;
        state_bytes = n_tiles * 256u;
    }
    else
    {
        unsigned int a, st, r;

        scratch_sizes(n_tiles, &a, &st, &r);
        alloc_bytes = a * PAGE_SIZE;
        state_bytes = st * PAGE_SIZE;
    }

    zero_words(s->tile_alloc_log, alloc_bytes);
    zero_words(s->tile_state_log, state_bytes);
}

/* ---------------------------------------------------------------- */
/* Building                                                          */
/* ---------------------------------------------------------------- */

_kernel_oserror *v3d_check_dims(unsigned int width, unsigned int height)
{
    if (width == 0u || height == 0u || width > 16384u || height > 16384u)
        return &err_bad_dims;
    return NULL;
}

/*
 * Picks the smallest supertile_w/supertile_h (starting at 1x1, growing
 * whichever dimension is currently smaller) that keeps the resulting
 * supertile-grid cell count under 256 - transcribed from the reference's
 * getSuperTiles() (gl_rainbow_triangle.cpp ~49-69), not derived: the
 * "grow whichever is smaller" tie-break and the exact <256 threshold are
 * real choices from that source, not obvious ones.
 */
void v3d_get_super_tiles(unsigned int tiles_x, unsigned int tiles_y,
                          unsigned int *supertile_w, unsigned int *supertile_h,
                          unsigned int *frame_w_supertile,
                          unsigned int *frame_h_supertile)
{
    unsigned int sw = 1u, sh = 1u, fw, fh;

    for (;;)
    {
        fw = v3d_div_round_up(tiles_x, sw);
        fh = v3d_div_round_up(tiles_y, sh);
        if (fw * fh < 256u)
            break;
        if (sw < sh)
            sw++;
        else
            sh++;
    }

    *supertile_w = sw;
    *supertile_h = sh;
    *frame_w_supertile = fw;
    *frame_h_supertile = fh;
}

void v3d_build_bcl(cl_buf_t *bcl, unsigned int width, unsigned int height)
{
    emit_tile_binning_mode_cfg(bcl, width, height);
    emit_flush_vcd_cache(bcl);
    emit_occlusion_query_counter(bcl, 0);
    emit_start_tile_binning(bcl);

    emit_clip_window(bcl, 0, 0, width, height);
    emit_cfg_bits(bcl, 1, 1, 1, 0, 0, 0, 0, 7, 0, 0, 1, 0, 0, 0, 0);
    emit_point_size(bcl, 1.0f);
    emit_line_width(bcl, 1.0f);
    emit_clipper_xy_scaling_wh(bcl, width, height);
    emit_clipper_z_scale_and_offset(bcl, 0.5f, 0.5f);
    emit_clipper_z_min_max(bcl, 0.0f, 1.0f);
    emit_viewport_offset_whole(bcl, width / 2u, height / 2u);
    emit_color_write_masks(bcl, 0);
    emit_blend_constant_color(bcl, 0, 0, 0, 0);
    emit_zero_all_flat_shade_flags(bcl);
    emit_zero_all_non_perspective_flags(bcl);
    emit_zero_all_centroid_flags(bcl);
    emit_transform_feedback_specs(bcl, 0, 0);
    emit_occlusion_query_counter(bcl, 0);
    emit_sample_state(bcl, 0xF, 1.0f);
    emit_vcm_cache_size(bcl, 4, 4);
    emit_flush(bcl);
}

void v3d_build_rcl_header(cl_buf_t *rcl, unsigned int width, unsigned int height,
                           unsigned int clear_color_rgba,
                           unsigned int tile_alloc_phys,
                           unsigned int tiles_x, unsigned int tiles_y)
{
    unsigned int supertile_w, supertile_h, frame_w_supertile, frame_h_supertile;

    v3d_get_super_tiles(tiles_x, tiles_y, &supertile_w, &supertile_h,
                         &frame_w_supertile, &frame_h_supertile);

    emit_tile_rendering_mode_cfg_common(rcl, width, height, 2);
    emit_tile_rendering_mode_cfg_clear_colors(rcl, 0, clear_color_rgba, 0);
    emit_tile_rendering_mode_cfg_color_rt0(rcl, 0, 2, 0);
    emit_tile_rendering_mode_cfg_zs_clear(rcl, 0, 1.0f);
    emit_tile_list_initial_block_size(rcl, 0, 1);
    emit_multicore_tile_list_set_base(rcl, tile_alloc_phys);
    emit_multicore_supertile_cfg(rcl, supertile_w, supertile_h,
                                   frame_w_supertile, frame_h_supertile,
                                   tiles_x, tiles_y, 1, 0, 0);
}

/* ---------------------------------------------------------------- */
/* Running                                                           */
/* ---------------------------------------------------------------- */

/*
 * COMPLETION WAIT.
 *
 * The interrupt handler bumps a counter when the job finishes; this waits
 * for it to move. Bounded by real time (the 1MHz timer), NOT by a spin
 * count: v0.06 had a 5,000,000-iteration cap that ended the wait after
 * only ~10-25ms whatever the "300ms" comment said, which a large job on a
 * big screen could legitimately exceed.
 *
 * The clock is looked at every 256 spins (one register read, not a SWI),
 * the first sample being the reference point.
 *
 * LOST-INTERRUPT RESCUE: if no interrupt has arrived after RESCUE_AFTER_US
 * but the CLE's own frame counter has advanced and its thread has stopped,
 * the job IS finished and only the interrupt went missing - carry on, and
 * count it (V3D_AccelProfile reports it) rather than fail. This is
 * independent evidence from a different register, so it cannot make a job
 * look done that is not.
 */
#define TIMEOUT_US      300000u
#define RESCUE_AFTER_US   3000u
#define SPIN_SAFETY_CAP 500000000u /* if the clock itself is dead */

static int rescue_possible(unsigned int which, unsigned int fc_initial)
{
    io_mapping_t v3d;
    unsigned int fc, cs;

    if (v3d_regs_map(&v3d))
        return 0;
    fc = IO_WORD(v3d, which ? V3D_CLE_RFC : V3D_CLE_BFC);
    cs = IO_WORD(v3d, which ? V3D_CLE_CT1CS : V3D_CLE_CT0CS);
    v3d_regs_unmap(&v3d);

    return fc != fc_initial && !(cs & V3D_CT_CS_CTRUN);
}

/* Records what the hardware said at the moment a rescue was decided on (diagnostics
 * only - never changes behaviour). Returns the slot used, or V3D_RESCUE_DIAG_MAX if
 * the table is full. Reads registers only. */
static unsigned int note_rescue(unsigned int kind, unsigned int which,
                                 unsigned int waited, unsigned int fc_initial)
{
    unsigned int slot = g_v3d_irq_rescue_count;
    io_mapping_t v3d;
    v3d_rescue_diag_t *d;

    if (slot >= V3D_RESCUE_DIAG_MAX)
        return V3D_RESCUE_DIAG_MAX;

    d = &g_v3d_rescue_diag[slot];
    d->which = which + 1u;
    d->kind = kind;
    d->waited_us = waited;
    d->late = 0;
    d->jobs_minus_bin = g_v3d_jobs_run - g_v3d_irq_bin_done_count;
    d->jobs_minus_render = g_v3d_jobs_run - g_v3d_irq_render_done_count;

    if (!v3d_regs_map(&v3d))
    {
        d->int_sts = IO_WORD(v3d, V3D_CTL_INT_STS);
        d->int_msk = IO_WORD(v3d, V3D_CTL_INT_MSK_STS);
        d->hub_sts = IO_WORD(v3d, V3D_HUB_INT_STS);
        d->ct_cs = IO_WORD(v3d, which ? V3D_CLE_CT1CS : V3D_CLE_CT0CS);
        d->fc_delta = IO_WORD(v3d, which ? V3D_CLE_RFC : V3D_CLE_BFC) - fc_initial;
        v3d_regs_unmap(&v3d);
    }
    d->valid = 1;
    return slot;
}

static _kernel_oserror *note_timeout(unsigned int kind, unsigned int which,
                                      unsigned int bfc0, unsigned int rfc0)
{
    io_mapping_t v3d;
    v3d_timeout_diag_t *d = &g_v3d_timeout_diag;

    /* Latch first: nothing below may be allowed to un-do the fail-stop. */
    g_v3d_wedged = 1;
    g_v3d_wedge_count++;

    d->valid = 1;
    d->which = which + 1u;
    d->kind = kind;
    d->bfc0 = bfc0;
    d->rfc0 = rfc0;
    d->irq_bin_count = g_v3d_irq_bin_done_count;
    d->irq_render_count = g_v3d_irq_render_done_count;
    d->t_us = v3d_time_us();

    /* Forensics for the next hardware report: separates "the interrupt was
     * lost" (frame counter moved, thread stopped) from "the CLE is stuck"
     * (counter unmoved, CA short of EA). Reads only - nothing here touches
     * the running job. */
    if (!v3d_regs_map(&v3d))
    {
        d->int_sts = IO_WORD(v3d, V3D_CTL_INT_STS);
        d->int_msk = IO_WORD(v3d, V3D_CTL_INT_MSK_STS);
        d->hub_sts = IO_WORD(v3d, V3D_HUB_INT_STS);
        d->ct0cs = IO_WORD(v3d, V3D_CLE_CT0CS);
        d->ct0ca = IO_WORD(v3d, V3D_CLE_CT0CA);
        d->ct0ea = IO_WORD(v3d, V3D_CLE_CT0EA);
        d->ct1cs = IO_WORD(v3d, V3D_CLE_CT1CS);
        d->ct1ca = IO_WORD(v3d, V3D_CLE_CT1CA);
        d->ct1ea = IO_WORD(v3d, V3D_CLE_CT1EA);
        d->bfc = IO_WORD(v3d, V3D_CLE_BFC);
        d->rfc = IO_WORD(v3d, V3D_CLE_RFC);
        v3d_regs_unmap(&v3d);
    }

    return &err_timeout[kind][which];
}

static _kernel_oserror *wait_complete(unsigned int kind, unsigned int which,
                                       unsigned int irq_initial,
                                       unsigned int fc_initial,
                                       unsigned int bfc0, unsigned int rfc0)
{
    volatile unsigned int *counter = which ? &g_v3d_irq_render_done_count
                                           : &g_v3d_irq_bin_done_count;
    unsigned int spin = 0u, t_first = 0u;

    for (;;)
    {
        if (*counter != irq_initial)
        {
            V3D_DMB();
            return NULL;
        }

        if ((++spin & 0xFFu) == 0u)
        {
            unsigned int now = v3d_time_us();

            if (spin == 0x100u)
                t_first = now;
            else
            {
                unsigned int waited = now - t_first;

                if (waited >= RESCUE_AFTER_US && rescue_possible(which, fc_initial))
                {
                    unsigned int absorb;
                    unsigned int slot = note_rescue(kind, which, waited, fc_initial);

                    /* If the interrupt is merely late rather than lost, let
                     * it land now: counted after this wait it would
                     * otherwise be mistaken for the NEXT job finishing.
                     * Bounded, so a really lost one costs at most a few
                     * hundred microseconds, once. */
                    for (absorb = 0; absorb < 100000u && *counter == irq_initial; absorb++)
                        ;
                    if (slot < V3D_RESCUE_DIAG_MAX)
                        g_v3d_rescue_diag[slot].late = (*counter != irq_initial) ? 1u : 0u;
                    g_v3d_irq_rescue_count++;
                    V3D_DMB();
                    return NULL;
                }
                if (waited >= TIMEOUT_US || spin >= SPIN_SAFETY_CAP)
                    return note_timeout(kind, which, bfc0, rfc0);
            }
        }
    }
}

_kernel_oserror *v3d_job_run(const v3d_job_t *job)
{
    io_mapping_t v3d;
    _kernel_oserror *err;
    unsigned int last_bin_irq = 0, last_render_irq;
    unsigned int bfc0, rfc0;
    unsigned int t_kick_bin, t_bin_done, t_kick_render, t_done;
    v3d_prof_t *pk;

    if (g_v3d_wedged)
        return &err_wedged;
    if (job->overflow)
        return &err_overflow;

    err = v3d_regs_map(&v3d);
    if (err)
        return err;

    g_v3d_jobs_run++;

    /* Every CPU write this job depends on - control lists, zeroed tile
     * scratch, and any earlier CPU drawing into the screen that is still
     * sitting in the write buffer - must be complete before the GPU is
     * told to start. See V3D_DSB's comment in cl_common.h. */
    V3D_DSB();

    bfc0 = IO_WORD(v3d, V3D_CLE_BFC);
    rfc0 = IO_WORD(v3d, V3D_CLE_RFC);
    t_kick_bin = v3d_time_us();

    IO_WORD(v3d, V3D_PTB_BPOS) = 0;
    v3d_flush_caches(&v3d);
    last_bin_irq = g_v3d_irq_bin_done_count;

    IO_WORD(v3d, V3D_CLE_CT0QMA) = job->tile_alloc_phys;
    IO_WORD(v3d, V3D_CLE_CT0QMS) = job->tile_alloc_bytes;
    IO_WORD(v3d, V3D_CLE_CT0QTS) = job->tile_state_phys | V3D_CLE_CT0QTS_ENABLE;
    IO_WORD(v3d, V3D_CLE_CT0QBA) = job->bcl_start;
    IO_WORD(v3d, V3D_CLE_CT0QEA) = job->bcl_end;

    v3d_regs_unmap(&v3d);

    err = wait_complete(job->kind, 0u, last_bin_irq, bfc0, bfc0, rfc0);
    if (err)
        return err;
    t_bin_done = v3d_time_us();

    err = v3d_regs_map(&v3d);
    if (err)
        return err;

    v3d_flush_caches(&v3d);

    t_kick_render = v3d_time_us();
    last_render_irq = g_v3d_irq_render_done_count;
    rfc0 = IO_WORD(v3d, V3D_CLE_RFC);

    IO_WORD(v3d, V3D_CLE_CT1QBA) = job->rcl_start;
    IO_WORD(v3d, V3D_CLE_CT1QEA) = job->rcl_end;

    v3d_regs_unmap(&v3d);

    err = wait_complete(job->kind, 1u, last_render_irq, rfc0, bfc0, rfc0);
    if (err)
        return err;
    t_done = v3d_time_us();

    pk = &g_v3d_prof[job->kind];
    pk->n++;
    pk->setup_us += t_kick_bin - job->t_start_us;
    pk->bin_us += t_bin_done - t_kick_bin;
    pk->mid_us += t_kick_render - t_bin_done;
    pk->render_us += t_done - t_kick_render;
    pk->total_us += t_done - job->t_start_us;
    if (t_done - job->t_start_us > pk->max_us)
        pk->max_us = t_done - job->t_start_us;

    return NULL;
}
