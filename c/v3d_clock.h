/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * v3d_clock.h - query (and, once confirmed necessary, enable) the
 * firmware-managed V3D core clock via the property-tag mailbox.
 *
 * Nothing in this driver has ever asked the firmware to turn the V3D
 * clock on - V3D_PowerOn only pulses the PM_GRAFX reset and releases
 * the ASB gates, which is a different, lower-level thing than the
 * firmware's own clock-gating for the block. A GPU register block
 * with no clock running would still respond correctly to plain
 * reads/writes of static registers (explaining why IDENT reads and
 * simple buffer tests work) while never actually advancing a state
 * machine that depends on that clock ticking (explaining a CLE thread
 * that reports CTRUN but never advances BFC, with no error flags -
 * there's nothing malfunctioning, there's just no clock edge to do the
 * work). Diagnostic-only for now: this reports real numbers before
 * anything forces a clock state, per this project's own rule of
 * diagnosing before fixing.
 */
#ifndef V3D_CLOCK_H
#define V3D_CLOCK_H

#include "kernel.h"

/*
 * Queries the V3D clock's current state, rate, and max rate via
 * BCMSupport_SendTempPropertyBuffer (see bcmsupport.h) - three
 * separate property-tag calls (GetClockState/GetClockRate/
 * GetMaxClockRate), not a guess.
 */
_kernel_oserror *v3d_query_clock(unsigned int *state_out,
                                   unsigned int *rate_out,
                                   unsigned int *max_rate_out);

/*
 * Confirmed root cause (2026-09-26 hardware test): V3D_ClockInfo
 * reported state=0 (clock gated off by firmware) with rate=250000000
 * remembered but not applied - explaining every CLE hang logged so
 * far (registers read/write fine with no clock; a state machine that
 * needs that clock to advance never does, with no error flags, since
 * nothing is actually malfunctioning). Turns the clock on
 * (SetClockState) and, if the firmware has no rate remembered at all,
 * requests one (SetClockRate, using GetMaxClockRate's answer) - a
 * one-way "make sure it's on" call, not a toggle.
 *
 * `already_on_out` (optional, may be NULL) reports whether the clock
 * was *already* on before this call touched anything - real hardware
 * truth, unlike module.c's own g_powered_on, which is per-module-
 * instance C state that a module reload wipes clean even though the
 * underlying GPU core is still warm. v3d_power_on() uses this to
 * decide whether the PM_GRAFX reset pulse is safe to skip - reset-
 * pulsing an already-warm core is a confirmed real hang/freeze cause
 * (see v3d_power_on's own comment), and "was g_powered_on set in *this*
 * instance" is the wrong question to ask after a reload.
 */
_kernel_oserror *v3d_ensure_clock_on(unsigned int *rate_out, int *already_on_out);

/*
 * Sets the V3D clock rate in Hz (clamped to the firmware's maximum) and returns
 * the rate actually in force and that maximum. Foreground use only, GPU idle.
 */
_kernel_oserror *v3d_set_clock_rate(unsigned int hz, unsigned int *rate_out,
                                      unsigned int *max_rate_out);

#endif
