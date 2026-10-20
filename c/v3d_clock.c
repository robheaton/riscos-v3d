/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * v3d_clock.c - see v3d_clock.h.
 *
 * Tag numbers and clock ID cross-checked against two independent real
 * sources (not guessed): the Random06457/rpi4-gpu-bare-metal-examples
 * reference project's mbox_message.hpp (GetClockState/SetClockState/
 * GetClockRate/SetClockRate/GetMaxClockRate, ClockID::V3D=5), and RISC
 * OS's own BCMSupport module source (hdr/BCMSupport, fetched from
 * gitlab.riscosopen.org/RiscOS/Sources/HWSupport/BCMSupport
 * 2026-09-24: ARM2VC_Tag_GetClockState=0x00030001 etc,
 * BaseClockID_V3D=5) - the two agree exactly.
 */

#include "v3d_clock.h"
#include "bcmsupport.h"
#include "beacon.h"
#include <stddef.h>
#include "v3d_errors.h"

#define TAG_GET_CLOCK_STATE    0x00030001u
#define TAG_SET_CLOCK_STATE    0x00038001u
#define TAG_GET_CLOCK_RATE     0x00030002u
#define TAG_SET_CLOCK_RATE     0x00038002u
#define TAG_GET_MAX_CLOCK_RATE 0x00030004u

#define CLOCK_ID_V3D 5u

/* [0] size [1] code [2] tag [3] value-buffer size [4] req/resp
 * indicator [5..7] up to 3 request/response words [8] end tag. A
 * value-buffer size of 12 covers every call here (SetClockRate's
 * 3-word request is the largest). */
static _kernel_oserror *tag_call(unsigned int tag, unsigned int a0,
                                   unsigned int a1, unsigned int a2,
                                   unsigned int *out0, unsigned int *out1)
{
    unsigned int req[9];
    unsigned int resp[9];
    _kernel_oserror *err;

    req[0] = sizeof(req);
    req[1] = 0;
    req[2] = tag;
    req[3] = 12;
    req[4] = 0;
    req[5] = a0;
    req[6] = a1;
    req[7] = a2;
    req[8] = 0;

#ifdef V3D_BEACONS
    /* before/after pair per kind of call, so the bar shows which mailbox request was in flight */
    v3d_beacon(tag == TAG_GET_CLOCK_STATE ? B_CK_STATE_BEFORE
             : tag == TAG_SET_CLOCK_STATE ? B_CK_SETSTATE_BEFORE
             : tag == TAG_GET_CLOCK_RATE  ? B_CK_RATE_BEFORE : B_CK_SETRATE_BEFORE);
#endif
    err = bcm_send_temp_property_buffer(req, resp);
    if (err)
        return err;
#ifdef V3D_BEACONS
    v3d_beacon(tag == TAG_GET_CLOCK_STATE ? B_CK_STATE_AFTER
             : tag == TAG_SET_CLOCK_STATE ? B_CK_SETSTATE_AFTER
             : tag == TAG_GET_CLOCK_RATE  ? B_CK_RATE_AFTER : B_CK_SETRATE_AFTER);
#endif

    if (out0) *out0 = resp[5];
    if (out1) *out1 = resp[6];
    return NULL;
}

_kernel_oserror *v3d_query_clock(unsigned int *state_out,
                                   unsigned int *rate_out,
                                   unsigned int *max_rate_out)
{
    unsigned int echo;
    _kernel_oserror *err;

    err = tag_call(TAG_GET_CLOCK_STATE, CLOCK_ID_V3D, 0, 0, &echo, state_out);
    if (err)
        return err;

    err = tag_call(TAG_GET_CLOCK_RATE, CLOCK_ID_V3D, 0, 0, &echo, rate_out);
    if (err)
        return err;

    return tag_call(TAG_GET_MAX_CLOCK_RATE, CLOCK_ID_V3D, 0, 0, &echo, max_rate_out);
}

_kernel_oserror *v3d_ensure_clock_on(unsigned int *rate_out, int *already_on_out)
{
    unsigned int echo, state, rate, max_rate;
    _kernel_oserror *err;

    /* Check before setting: a real hardware test (2026-09-26) found the
     * first V3D_PowerOn in a session works reliably, but a second one
     * - clock already confirmed on from the first call - reliably
     * hangs the CLE on a fresh, correctly-addressed submission (CS
     * shows CTRUN, BFC never advances, no error flags - not a stale-
     * state or address problem, both ruled out separately). The one
     * concrete difference between call 1 and call 2 is that call 2
     * re-issues SetClockState(on) to a clock that's already running.
     * If the firmware treats a redundant enable as a brief re-init of
     * an already-stable clock rather than a true no-op, a hardware
     * reset pulse landing moments later on a momentarily-disturbed
     * clock would produce exactly this symptom. Only ask the firmware
     * to change anything when the clock is actually confirmed off.
     *
     * CAUTION (2026-10-09, real hardware): the state this returns is NOT
     * a usable "is the V3D powered" test. On a Pi 4 the firmware reports
     * the V3D clock as OFF (state 0) on a fresh boot AND while the V3D is
     * up and running jobs, so *already_on_out is false in every case seen.
     * v3d_power_on therefore asks the hardware first (v3d_hw_is_up in
     * module.c) and only reaches this function for a genuinely cold V3D. */
    err = tag_call(TAG_GET_CLOCK_STATE, CLOCK_ID_V3D, 0, 0, &echo, &state);
    if (err)
        return err;

    if (already_on_out) *already_on_out = (state & 1u) ? 1 : 0;

    if (!(state & 1u))
    {
        err = tag_call(TAG_SET_CLOCK_STATE, CLOCK_ID_V3D, 1, 0, &echo, &state);
        if (err)
            return err;
    }

    err = tag_call(TAG_GET_CLOCK_RATE, CLOCK_ID_V3D, 0, 0, &echo, &rate);
    if (err)
        return err;

    if (!rate)
    {
        err = tag_call(TAG_GET_MAX_CLOCK_RATE, CLOCK_ID_V3D, 0, 0, &echo, &max_rate);
        if (err)
            return err;

        err = tag_call(TAG_SET_CLOCK_RATE, CLOCK_ID_V3D, max_rate, 0, &echo, &rate);
        if (err)
            return err;
    }

    if (rate_out) *rate_out = rate;
    return NULL;
}

/*
 * Asks the firmware for a new V3D clock rate (SetClockRate, clock 5), clamped
 * to the maximum the firmware reports, and reads back what it actually set.
 * The third word of the request is the firmware's "skip turbo" flag, 0 = allow
 * turbo, as v3d_ensure_clock_on above already uses it.
 *
 * Added v0.10 for a measured reason: the large-operation costs (about 3.4 us a
 * 64x64 tile for a fill, 6.3 us for a copy) match the V3D tile buffer moving
 * about four pixels per clock at the 250 MHz this driver leaves the clock at,
 * which would make them clock-bound, while the firmware reports a 500 MHz maximum
 * (the stock Pi 4 V3D rate). Only ever called from a foreground SWI with no job in
 * flight.
 */
_kernel_oserror *v3d_set_clock_rate(unsigned int hz, unsigned int *rate_out,
                                      unsigned int *max_rate_out)
{
    unsigned int echo, max_rate, rate;
    _kernel_oserror *err;

    err = tag_call(TAG_GET_MAX_CLOCK_RATE, CLOCK_ID_V3D, 0, 0, &echo, &max_rate);
    if (err)
        return err;

    /* A zero maximum means the firmware did not answer the tag; do not guess a
     * limit and ask for a rate on the strength of that. */
    if (!max_rate)
    {
        static _kernel_oserror err_no_max = {
            V3D_ERR(49), "V3DDriver: the firmware did not report a maximum V3D clock rate"
        };
        return &err_no_max;
    }
    if (hz > max_rate)
        hz = max_rate;

    err = tag_call(TAG_SET_CLOCK_RATE, CLOCK_ID_V3D, hz, 0, &echo, &rate);
    if (err)
        return err;

    err = tag_call(TAG_GET_CLOCK_RATE, CLOCK_ID_V3D, 0, 0, &echo, &rate);
    if (err)
        return err;

    if (!rate)
    {
        static _kernel_oserror err_no_rate = {
            V3D_ERR(50), "V3DDriver: the firmware did not report the V3D clock rate after "
                "the request"
        };
        return &err_no_rate;
    }

    if (rate_out) *rate_out = rate;
    if (max_rate_out) *max_rate_out = max_rate;
    return NULL;
}
