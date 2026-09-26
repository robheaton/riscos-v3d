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

#endif
