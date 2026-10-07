/*
 * cl_common.h - the parts of GPU job submission that cl_fill.c and
 * cl_copy.c used to each carry their own copy of: the scratch-buffer
 * cache, the completion wait, the control-list upload, the job kick
 * sequence, and the health/diagnostic state around them.
 *
 * Created 2026-10-04 (v0.07) after a full code review found the same
 * defects independently present in BOTH copies (a cached scratch slot
 * returned without checking the request fit it; a "300ms" circuit
 * breaker that was really a 10-25ms spin cap; no recovery after a
 * timeout; no DSB before the doorbell write). Fixing each twice was the
 * error-prone way to do it, so there is now exactly one of each, here.
 *
 * Everything in this file is on the hot path of every accelerated
 * GraphicsV call, so most of the design choices are about keeping that
 * path short AND about failing safe: any error leaves the GraphicsV
 * call unclaimed, so the kernel/BCMVideo draws it instead.
 */
#ifndef CL_COMMON_H
#define CL_COMMON_H

#include "kernel.h"
#include "phys_buffer.h"
#include "cl_builder.h"

#define TILE_SIZE 64u

/*
 * Memory barriers, emitted as raw words so this builds with whatever
 * -march the toolchain defaults to (verified 2026-10-04: both words
 * assemble to the same encodings under arm-unknown-riscos-as, and match
 * ARM DDI0406C A8.8.44/A8.8.54).
 *
 * V3D_DSB (DSB SY): CPU writes to the control lists, the tile scratch
 * and any earlier CPU drawing into the screen (write-buffered Normal
 * memory) must have COMPLETED before the doorbell write reaches the GPU,
 * or the GPU can read or overwrite stale data. Strongly-ordered stores
 * only order against other accesses to the same peripheral/block, not
 * against Normal memory, so ordering is not implied. This used to be
 * implicit when every job did OS_Memory 14/15 (each runs a barrier
 * sequence); the permanent register mapping introduced in v0.05 removed
 * it without anything replacing it.
 *
 * V3D_DMB (DMB SY): after the completion IRQ is seen, later CPU reads of
 * the screen must not be satisfied early from before the GPU finished.
 */
#define V3D_DSB() __asm__ __volatile__(".word 0xF57FF04F" ::: "memory")
#define V3D_DMB() __asm__ __volatile__(".word 0xF57FF05F" ::: "memory")

static inline unsigned int v3d_div_round_up(unsigned int a, unsigned int b)
{
    return (a + b - 1u) / b;
}

static inline unsigned int v3d_bytes_to_pages(unsigned int bytes)
{
    return v3d_div_round_up(bytes, PAGE_SIZE);
}

/* ---------------------------------------------------------------- */
/* Run-time tunables (V3D_AccelTune) - performance experiments       */
/* ---------------------------------------------------------------- */

/*
 * Three switches remain (v0.10). Each was measured on the real hardware
 * (test/TuneBench.bas, test/VerifySuite.bas, 2026-10-07) and the defaults are the
 * fastest combination, verified pixel-identical to the CPU's output in 450 of 450
 * random cases at both 32bpp and 16bpp:
 *
 *  zero_mode    1 (default) = zero only the bytes a request can touch
 *                   (n_tiles*64 of tile allocation, n_tiles*256 of tile state)
 *               0 = zero every page of the tile scratch each call (v0.06)
 *  scratch_attr 1 (default) = scratch pages Normal non-cacheable BUFFERABLE (word
 *                   stores merge in the write buffer; the DSB in v3d_job_run
 *                   drains it before the doorbell)
 *               0 = strongly-ordered, one bus transaction per store (v0.06)
 *                   Takes effect for scratch created after the next reset
 *                   (V3D_AccelTune 5).
 *  overlap_mode 1 (default) = scrolls in ONE GPU pass, tiles visited in an order
 *                   chosen from the sign of the shift (see cl_copy.c)
 *               0 = two passes through an 8MB bounce buffer (v0.06-v0.08)
 *
 * Together they took a tiny GPU job from 125 us to 8 us, and a scroll from two
 * jobs to one. Four other switches (building the control lists in cached memory,
 * skipping the second cache flush, never/once-only zeroing, and skipping the
 * binning job) were tried or analysed and removed: they saved nothing once these
 * three were set, or could only save ~2-3 us of the ~8 us left.
 */
typedef struct
{
    unsigned int zero_mode;
    unsigned int scratch_attr;
    unsigned int overlap_mode;
} v3d_tune_t;

extern v3d_tune_t g_v3d_tune;

/* ---------------------------------------------------------------- */
/* Health: the fail-stop latch                                       */
/* ---------------------------------------------------------------- */

/*
 * Set the first time a job times out. While set, EVERY accelerated
 * operation is refused immediately and the kernel's CPU/DMA path draws
 * instead. This is deliberately sticky: v0.06 retried straight away
 * after a timeout, re-writing the same control lists while the previous
 * job might still be running and letting that job's late completion
 * interrupt satisfy the NEW job's wait. A GPU that has already failed
 * once on this hardware is not something to keep poking.
 */
extern volatile unsigned int g_v3d_wedged;
extern unsigned int g_v3d_wedge_count;
extern unsigned int g_v3d_irq_rescue_count;

/*
 * Forensics for the lost-interrupt rescue (v0.11). The first hardware run of 0.10
 * (2026-10-07) rescued 3 jobs in ~2.3 million: the job had finished (frame counter
 * advanced, CLE thread stopped) but no completion interrupt had been counted 3 ms
 * later. Whether the interrupt was LOST, only LATE, or consumed without being counted
 * decides whether this is a hardware/firmware quirk to live with or a bug to fix (and
 * whether it is related to the unexplained reload freeze), so each of the first
 * V3D_RESCUE_DIAG_MAX rescues records what the registers said at that moment:
 *   int_sts/int_msk/hub_sts  - interrupt status / mask / hub status: a status bit still
 *                              SET means the interrupt was raised but never delivered;
 *                              clear means it was delivered (and counted, or eaten)
 *   ct_cs                    - that thread's control/status word (CTRUN etc.)
 *   fc_delta                 - how far the frame counter had moved since the kick
 *   late                     - 1 if the interrupt then arrived during the short
 *                              absorb wait, i.e. it was late rather than lost
 *   jobs_minus_bin/render    - jobs started minus bin/render interrupts counted: a job in
 *                              flight accounts for exactly 1, so >1 means earlier
 *                              interrupts were genuinely lost
 */
#define V3D_RESCUE_DIAG_MAX 4u
typedef struct
{
    unsigned int valid, which, kind, waited_us;
    unsigned int int_sts, int_msk, hub_sts, ct_cs;
    unsigned int fc_delta, late;
    unsigned int jobs_minus_bin, jobs_minus_render;
} v3d_rescue_diag_t;

extern v3d_rescue_diag_t g_v3d_rescue_diag[V3D_RESCUE_DIAG_MAX];
extern unsigned int g_v3d_jobs_run; /* accelerated jobs started since module load */

typedef struct
{
    unsigned int valid;       /* non-zero once a timeout has been captured */
    unsigned int which;       /* 1 = waiting for binning, 2 = for render */
    unsigned int kind;        /* 0 = fill, 1 = copy */
    unsigned int int_sts, int_msk, hub_sts;
    unsigned int ct0cs, ct0ca, ct0ea, ct1cs, ct1ca, ct1ea;
    unsigned int bfc, rfc, bfc0, rfc0;
    unsigned int irq_bin_count, irq_render_count;
    unsigned int t_us;
} v3d_timeout_diag_t;

extern v3d_timeout_diag_t g_v3d_timeout_diag;

/* Returns an error if the fail-stop latch is set, NULL otherwise. */
_kernel_oserror *v3d_gpu_check(void);

/* Re-arms after a timeout (V3D_AccelTune 6). */
void v3d_gpu_rearm(void);

/* ---------------------------------------------------------------- */
/* Timing                                                            */
/* ---------------------------------------------------------------- */

/*
 * Microsecond timestamp. Reads the BCM2711 system timer's free-running
 * 1MHz counter (ST_CLO, physical 0xFE003004) directly, which costs one
 * register read instead of the OS_ReadMonotonicTime SWI the driver used
 * before (and that only has 10ms resolution). If the timer cannot be
 * mapped it falls back to centiseconds*10000, so callers must only use
 * DIFFERENCES and must treat resolution as unknown.
 */
unsigned int v3d_time_us(void);

/* 1 when v3d_time_us is the real microsecond timer, 0 for the fallback. */
unsigned int v3d_time_is_fine(void);

/* ---------------------------------------------------------------- */
/* Profiling                                                         */
/* ---------------------------------------------------------------- */

#define V3D_JOB_FILL 0u
#define V3D_JOB_COPY 1u

typedef struct
{
    unsigned int n;         /* jobs recorded */
    unsigned int setup_us;  /* sum: job start -> binning kicked */
    unsigned int bin_us;    /* sum: binning kicked -> binning done */
    unsigned int mid_us;    /* sum: binning done -> render kicked */
    unsigned int render_us; /* sum: render kicked -> render done */
    unsigned int total_us;  /* sum: job start -> render done */
    unsigned int max_us;    /* worst single job total */
} v3d_prof_t;

extern v3d_prof_t g_v3d_prof[2];

void v3d_prof_reset(void);

/* ---------------------------------------------------------------- */
/* Scratch buffers                                                   */
/* ---------------------------------------------------------------- */

/*
 * Slot capacity: sized once, for a worst-case 30x17-tile rectangle
 * (1920x1088, the largest standard desktop mode). Anything bigger is
 * REFUSED, never handled by a per-call allocation: allocating inside a
 * GraphicsV call makes OS_DynamicArea broadcast Service_MemoryMoved from
 * inside a Wimp-dispatched call, which is what froze the machine under
 * Task Manager (README, "Task Manager freeze"). Larger screens are a
 * planned feature (size the slot from the mode), not a fallback.
 */
#define WORST_CASE_TILES_X 30u
#define WORST_CASE_TILES_Y 17u
#define WORST_CASE_N_TILES (WORST_CASE_TILES_X * WORST_CASE_TILES_Y)

/* Pages one slot claims at WORST_CASE_N_TILES: 1 CL + 6 RCL + 10
 * tile_alloc + 32 tile_state. Callers laying out PMP blocks must leave
 * at least this much room after each slot's base page. */
#define V3D_SLOT_PAGES 49u

typedef struct v3d_scratch
{
    pmp_area_t *pmp;
    unsigned int base_page;
    unsigned int total_pages;
    unsigned int cl_phys;         void *cl_log;
    unsigned int rcl_phys;        void *rcl_log;        unsigned int rcl_pages;
    unsigned int tile_alloc_phys; void *tile_alloc_log; unsigned int tile_alloc_pages;
    unsigned int tile_state_phys; void *tile_state_log; unsigned int tile_state_pages;
    unsigned int attr;
} v3d_scratch_t;

/*
 * Returns the scratch slot for (pmp, base_page), creating it on first
 * use, after checking that a request for `n_tiles` tiles actually FITS
 * it - on a cache hit as well as on creation. A request that does not
 * fit gets an error, never a smaller-than-needed slot.
 */
_kernel_oserror *v3d_scratch_get(pmp_area_t *pmp, unsigned int base_page,
                                  unsigned int n_tiles, v3d_scratch_t **out);

/*
 * The large pixel buffer the overlap-safe copy bounces through. Same
 * caching and invalidation rules as the job scratch above.
 */
_kernel_oserror *v3d_overlap_buffer_get(pmp_area_t *pmp, unsigned int base_page,
                                         unsigned int pages_needed,
                                         unsigned int *phys_out, void **log_out);

/*
 * Called by pmp_alloc/pmp_release for every page range they are about to
 * release: forgets any cached slot overlapping it, so a cache entry can
 * never outlive the pages behind it (the test SWIs re-use PMP ranges
 * with different sizes, which used to leave stale slots pointing at
 * pages the OS had already taken back).
 */
void v3d_scratch_note_release(const pmp_area_t *area, unsigned int first_page,
                               unsigned int num_pages);

/*
 * Releases every cached slot's pages and forgets them (V3D_AccelTune 5
 * and module finalisation). Only safe with the GPU idle.
 */
void v3d_scratch_reset_all(void);

/* The tile-allocation size the binner is told it may use for `n_tiles`
 * tiles (CT0QMS), in bytes, page-rounded. */
unsigned int v3d_tile_alloc_bytes(unsigned int n_tiles);

/* Zeroes the tile scratch for a job of `n_tiles` per g_v3d_tune.zero_mode. */
void v3d_scratch_zero(v3d_scratch_t *s, unsigned int n_tiles);

/* ---------------------------------------------------------------- */
/* Building and running a job                                        */
/* ---------------------------------------------------------------- */

/* Picks the supertile grid (see cl_fill.c's history for the source). */
void v3d_get_super_tiles(unsigned int tiles_x, unsigned int tiles_y,
                          unsigned int *supertile_w, unsigned int *supertile_h,
                          unsigned int *frame_w_supertile,
                          unsigned int *frame_h_supertile);

/* The binning control list: identical for every fill and copy. */
void v3d_build_bcl(cl_buf_t *bcl, unsigned int width, unsigned int height);

/* The render control list's fixed header. */
void v3d_build_rcl_header(cl_buf_t *rcl, unsigned int width, unsigned int height,
                           unsigned int clear_color_rgba,
                           unsigned int tile_alloc_phys,
                           unsigned int tiles_x, unsigned int tiles_y);

/* Rejects zero, and absurdly large, rectangles. */
_kernel_oserror *v3d_check_dims(unsigned int width, unsigned int height);

typedef struct
{
    unsigned int kind;              /* V3D_JOB_FILL / V3D_JOB_COPY */
    unsigned int t_start_us;        /* from v3d_time_us() at job start */
    unsigned int bcl_start, bcl_end;
    unsigned int rcl_start, rcl_end;
    unsigned int tile_alloc_phys, tile_alloc_bytes, tile_state_phys;
    int overflow;                   /* a control list outgrew its buffer */
} v3d_job_t;

/*
 * Kicks the binning job, waits, kicks the render job, waits. Handles the
 * barriers, the fail-stop latch, lost-interrupt rescue, timeouts and the
 * profile counters. The only place in the driver that writes the CLE's
 * submit registers for the accelerated paths.
 */
_kernel_oserror *v3d_job_run(const v3d_job_t *job);

#endif /* CL_COMMON_H */
