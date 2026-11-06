/*
 * Running lib/cairn_uplink's schedule against the real modem.
 *
 * cairn_uplink decides *what* to do and never does any of it: it takes
 * evidence, returns one action, and waits to be told the outcome. This file is
 * the other half — it gathers the evidence from the lifecycle, performs the
 * action, and reports back. Keeping them apart is what lets the scheduling
 * behaviour be tested on the host; putting the I/O in here keeps that true.
 *
 * Two rules it must not break, and how:
 *
 *   - **Capture is never delayed.** An LTE send runs only when parked, and the
 *     transfer's abort callback re-evaluates the sensors directly — so a rising
 *     supply rail or real motion stops an upload partway rather than after it.
 *   - **Nothing is deleted except through the receipt gate.** The transport
 *     owns that; this file only decides when it may run.
 *
 * (It used to arbitrate the 2.4 GHz radio between BLE and Wi-Fi slots too;
 * the Wi-Fi stack is deleted, and LTE has its own modem, so BLE stays up
 * through every send.)
 *
 * Why the abort check re-reads the sensors directly: a transfer blocks the
 * lifecycle tick for as long as it takes, so the fact queue is not being
 * drained and lc->last_accel_rms_mg is frozen at whatever it was when the send
 * began. Asking the sensor accessors is the only way to notice a trip starting
 * during an upload. The lifecycle already reads the supply rail this way, and
 * for the same reason.
 */

#ifndef CAIRN_UPLINK_RUNNER_H
#define CAIRN_UPLINK_RUNNER_H

#include <stdbool.h>
#include <stdint.h>

struct Lifecycle;

#if CAIRN_UPLINK_RUNNER

/* Initialise the schedule. Call once from lifecycle_begin. */
void uplink_runner_begin(void);

/*
 * One pass. Cheap when there is nothing to do, and blocking for as long as a
 * transfer takes when there is — bounded by the send limit and cut short by the
 * abort check.
 */
void uplink_runner_tick(struct Lifecycle *lc);

/* True while an LTE send is in progress, so standby is held off. */
bool uplink_runner_busy(void);

#else

static inline void uplink_runner_begin(void) {}
static inline void uplink_runner_tick(struct Lifecycle *lc) { (void)lc; }
static inline bool uplink_runner_busy(void) { return false; }

#endif /* CAIRN_UPLINK_RUNNER */

#endif /* CAIRN_UPLINK_RUNNER_H */
