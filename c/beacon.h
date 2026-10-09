/*
 * beacon.h - screen beacons for localising a HARD FREEZE (compile-time option, off by default).
 *
 * When the machine freezes inside a SWI, every piece of software state dies with it - but the
 * display engine keeps scanning out the framebuffer, so whatever the CPU last STORED to the
 * screen is still there to photograph. Building with -DV3D_BEACONS (make beacons) turns
 * v3d_beacon(step) into a few plain stores that mark one numbered block of a bar at the
 * bottom-left of the screen:
 *
 *      the bar is 3 rows of 16 blocks (a wider gap after every 4th), step 1 = top-left, left to right, row by row;
 *      steps already reached are WHITE, the step reached MOST RECENTLY is RED, steps not reached
 *      are dark with a grey outline.
 *
 * After a freeze, the position of the red block (row r = 1..3, column c = 1..16) is the last step
 * the CPU reached: step = (r-1)*16 + c. The code that follows that step is what hung.
 *
 * v3d_beacon_prepare(zone) redraws an empty bar and must be called from FOREGROUND code (it makes one
 * OS_ReadVduVariables SWI to find the screen): the SWI handler, module_init and module_final call it
 * on entry. v3d_beacon() itself makes no SWI and takes no lock. Nothing is ever erased on the way
 * out - the bar of the most recent call stays on screen until the desktop repaints that area.
 *
 * In a normal build both calls compile to nothing, so the shipping module is unchanged.
 */
#ifndef BEACON_H
#define BEACON_H

/* The step numbers, so a photograph can be read without the source open. */
enum
{
    B_NONE = 0,
    /* V3D_PowerOn - the path that hangs after a reload */
    B_SWI_ENTER = 1,          /*  1 the V3D SWI handler was entered                        */
    B_PO_ENTER,               /*  2 v3d_power_on entered (and was not already powered on)  */
    B_PO_CLK_BEFORE,          /*  3 about to call v3d_ensure_clock_on                      */
    B_CK_STATE_BEFORE,        /*  4 mailbox: GET clock state - about to send               */
    B_CK_STATE_AFTER,         /*  5 ... reply received                                     */
    B_CK_SETSTATE_BEFORE,     /*  6 (only if the clock was off) SET clock state            */
    B_CK_SETSTATE_AFTER,      /*  7                                                        */
    B_CK_RATE_BEFORE,         /*  8 mailbox: GET clock rate                                */
    B_CK_RATE_AFTER,          /*  9                                                        */
    B_CK_SETRATE_BEFORE,      /* 10 (only if the rate was 0) GET max / SET rate            */
    B_CK_SETRATE_AFTER,       /* 11                                                        */
    B_PO_CLK_AFTER,           /* 12 v3d_ensure_clock_on returned                           */
    B_PO_MAP_BEFORE,          /* 13 clock already on: OS_Memory 14 map of the V3D megabyte */
    B_PO_MAP_AFTER,           /* 14                                                        */
    B_PO_ASBM_READ,           /* 15 ASB master control register read                       */
    B_PO_ASBS_READ,           /* 16 ASB slave control register read                        */
    B_PO_UNMAP_AFTER,         /* 17 OS_Memory 15 unmap done                                */
    B_PO_RETURN,              /* 18 v3d_power_on about to return success                   */
    B_PO_RESET_BEFORE,        /* 19 (clock was off) PM_GRAFX reset pulse                   */
    B_PO_RESET_AFTER,         /* 20                                                        */
    B_PO_ASB_BEFORE,          /* 21 (clock was off) ASB gate sequence                      */
    B_PO_ASB_AFTER,           /* 22                                                        */
    /* v3d_clear_run (V3D_ClearTest / V3D_ClearToSprite) */
    B_CT_ENTER,               /* 23                                                        */
    B_CT_ZEROED,              /* 24 tile alloc/state zeroed                                */
    B_CT_BUILT,               /* 25 bin and render control lists built                     */
    B_CT_MAP1,                /* 26 V3D mapped for the bin kick                            */
    B_CT_KICK_BIN,            /* 27 QBA/QEA written - bin job started                      */
    B_CT_BIN_DONE,            /* 28 bin completion seen                                    */
    B_CT_MAP2,                /* 29 V3D mapped for the render kick                         */
    B_CT_KICK_RENDER,         /* 30 render job started                                     */
    B_CT_RENDER_DONE,         /* 31 render completion seen                                 */
    B_CT_RETURN,              /* 32                                                        */
    /* module lifecycle */
    B_INIT_ENTER,             /* 33 module_init entered                                    */
    B_INIT_DONE,              /* 34                                                        */
    B_FINAL_ENTER,            /* 35 module_final entered                                   */
    B_FINAL_IDLE,             /* 36 GPU idle check done                                    */
    B_FINAL_GFXV,             /* 37 GraphicsV released                                     */
    B_FINAL_IRQ,              /* 38 interrupt shut down                                    */
    B_FINAL_PMP,              /* 39 Dynamic Areas destroyed                                */
    B_FINAL_DONE,             /* 40                                                        */
    /* the BCMSupport mailbox SWI lookup (first mailbox call of each module instance) */
    B_BCM_LOOKUP_BEFORE,      /* 41 OS_SWINumberFromString("BCMSupport_...") about to run  */
    B_BCM_LOOKUP_AFTER,       /* 42                                                        */
    B_SWI_EXIT,               /* 43 the handler is about to return to the caller           */
    B_LAST                    /* 44.. spare */
};

/* Which part of the code a step belongs to. The zones can be switched on and off independently at load time. */
#define B_ZPOWER 1u   /* steps 1-22 and 41-43: SWI entry, V3D_PowerOn, the mailbox clock calls          */
#define B_ZCLEAR 2u   /* steps 23-32: the legacy ClearTest / ClearToSprite submission                   */
#define B_ZLIFE  4u   /* steps 33-40: module_init / module_final                                        */

/*
 * One module, many behaviours (so the code layout is IDENTICAL across test variants): the *RMLoad command
 * tail picks what the beacon points actually do. Words (any case, may be mixed with NoAutoStart):
 *   BDRAW    draw the numbered bar on the screen at each point
 *   BDSB     execute a DSB barrier at each point
 *   BZPOWER / BZCLEAR / BZLIFE   restrict the above to those zones (none of the three given = all zones)
 * WARM turns on the real fix under test: v3d_power_on decides warm/cold from the hardware (PM_GRAFX, ASB gates, core
 * ident) instead of the firmware clock flag, so a reloaded module never reset-pulses a running V3D.
 * Two more words change the OLD test path itself (V3D_ClearTest / V3D_ClearToSprite), to make it submit jobs the way the
 * accelerated path does: LPERM (permanent register mapping instead of the per-job OS_Memory 14/15 window) and LDSB (one DSB
 * before the doorbell writes). They are independent of the beacon words above.
 * With neither BDRAW nor BDSB the points do nothing at all (they are still compiled in): the CONTROL.
 * The module prints one line at load saying what it will do.
 */
#ifdef V3D_BEACONS
void v3d_beacon_configure(const char *cmd_tail);
void v3d_beacon_prepare(unsigned int zone);
void v3d_beacon(unsigned int step);
extern unsigned int g_v3d_warm_detect;  /* WARM: v3d_power_on asks the hardware whether the V3D is already up (the fix) */
extern unsigned int g_v3d_legacy_perm; /* LPERM: legacy kicks use the permanent register mapping   */
extern unsigned int g_v3d_legacy_dsb;  /* LDSB: legacy submission executes a DSB before the doorbell */
#else
#define v3d_beacon_configure(tail) ((void) 0)
#define v3d_beacon_prepare(zone)   ((void) 0)
#define v3d_beacon(step)           ((void) 0)
#endif

#endif /* BEACON_H */
