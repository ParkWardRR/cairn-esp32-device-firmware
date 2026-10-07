/*
 * The portable half: when to stand by, and when to come back.
 *
 * Split from the execution for the same reason cairn_ota is — these are the
 * decisions, and decisions are what host tests can reach. See cairn_power.h for
 * why this is not deep sleep.
 */

#include "cairn_power.h"

#include <stdio.h>

#include "cairn_format.h"
#include "cairn_engine.h"
#include "config.h"

bool cairn_power_should_standby(const cairn_power_evidence_t *e)
{
    return cairn_power_standby_blocker(e) == NULL;
}

/*
 * One blocker at a time, in the order that matters most.
 *
 * Conservative on purpose: anything resembling unfinished work keeps the device
 * awake. Staying up too long costs battery; sleeping too early costs a trip,
 * and those are not symmetrical.
 */
const char *cairn_power_standby_blocker(const cairn_power_evidence_t *e)
{
    if (e->bench_mode) {
        /* USB power only, no OBD rail: this is a bench session. Staying awake and keeping
         * the radio up is the point. Returned before trip_active because a bench "trip" is
         * a bench event, not a reason to stop reporting bench-ness. */
        return "USB bench power, no OBD rail";
    }
    if (e->trip_active) return "a trip is in progress";

    /*
     * An unsealed capture must be sealed before standby. Leaving one open for
     * days is survivable — boot recovery would find it — but it would sit
     * undeliverable the whole time, and the receipt-gated prune could never
     * reclaim the space.
     */
    if (e->capture_open) return "the open capture is not sealed yet";

    /*
     * Sealed bundles with no receipt exist only on this card. Standing by stops
     * the radio, so it stops the only process that can make them safe.
     */
    if (e->pending_bundles > 0 && e->link_online) {
        return "bundles are awaiting a receipt and the link is up";
    }

    if (e->idle_ms < cairn_engine_params()->standby_idle_ms) return "not idle long enough";

    /*
     * A supply already near the engine-on threshold means the engine is
     * probably running, whatever the accelerometer says.
     */
    if (e->battery_mv != CAIRN_U16_UNKNOWN &&
        e->battery_mv >= cairn_engine_params()->engine_on_mv) {
        return "supply voltage suggests the engine is running";
    }

    return NULL;
}

const char *cairn_wake_reason_name(cairn_wake_reason_t r)
{
    switch (r) {
    case CAIRN_WAKE_NONE:             return "NONE";
    case CAIRN_WAKE_MOTION:           return "MOTION";
    case CAIRN_WAKE_ENGINE_VOLTAGE:   return "ENGINE_VOLTAGE";
    case CAIRN_WAKE_PERIODIC_HEALTH:  return "PERIODIC_HEALTH";
    default:                          return "?";
    }
}

cairn_wake_reason_t cairn_power_should_wake(uint16_t accel_rms_mg,
                                            uint16_t battery_mv,
                                            uint32_t standby_ms,
                                            uint16_t motion_threshold_mg)
{
    /*
     * Voltage first. A vehicle starting lifts the rail well before it moves, so
     * this is the earlier and more reliable signal — and catching it early is
     * what lets the pre-roll cover the first seconds of the drive.
     */
    if (battery_mv != CAIRN_U16_UNKNOWN && battery_mv >= cairn_engine_params()->engine_on_mv) {
        return CAIRN_WAKE_ENGINE_VOLTAGE;
    }

    if (accel_rms_mg >= motion_threshold_mg) return CAIRN_WAKE_MOTION;

    /*
     * A long standby still reports in, so a parked device is distinguishable
     * from a dead one. Without this, weeks of correct silence and a failure
     * look identical in the data.
     */
    if (standby_ms >= cairn_engine_params()->standby_heartbeat_ms) {
        return CAIRN_WAKE_PERIODIC_HEALTH;
    }

    return CAIRN_WAKE_NONE;
}

/* ── the parked-silence invariant ─────────────────────────────────────────── */

const char *cairn_bus_silence_reason(const cairn_bus_evidence_t *e)
{
    /* Nothing is transmitted from inside the standby loop, whatever else holds:
     * the peripherals are down and the co-processor is in low-power mode. */
    if (e->in_standby) return "standing by";

    /*
     * A confirmed drive opens the bus, however the device came to be awake.
     *
     * The ordering is the fix for a latch. This clause used to sit *below* the
     * heartbeat check, so once a heartbeat had set last_wake the bus stayed
     * shut even after local evidence confirmed a real drive — and last_wake is
     * only overwritten by the next wake. If someone started the engine during
     * the brief awake window following a heartbeat, OBD stayed closed until a
     * further standby cycle re-latched it. Self-correcting, but it would have
     * silently dropped the opening minutes of a trip.
     *
     * Letting a confirmed drive win is also the correct semantics rather than
     * merely the convenient one. drive_confirmed is computed from the supply
     * rail and the accelerometer, neither of which a heartbeat can fabricate,
     * so "woken for a heartbeat" and "the car is running" are independent
     * facts and the second is the one that decides whether a trip exists.
     */
    if (e->trip_active || e->drive_confirmed) return NULL;

    /*
     * Not driving, so stay quiet. Naming the heartbeat case separately is worth
     * it because it is the one that used to be expensive: a periodic-health
     * wake exists to prove the device is alive, which needs the supply rail,
     * the accelerometer, storage counters and an uptime — none of which involve
     * the vehicle. Letting it reopen an OBD session cost four wake-ups a day on
     * a car that counts exactly that.
     */
    if (e->last_wake == CAIRN_WAKE_PERIODIC_HEALTH) {
        return "woken only for a health heartbeat";
    }

    return "parked, and no drive confirmed from local evidence";
}

bool cairn_bus_may_transmit(const cairn_bus_evidence_t *e)
{
    return cairn_bus_silence_reason(e) == NULL;
}

/* ── drive confirmation from local signals only ───────────────────────────── */

const char *cairn_drive_blocker(const cairn_drive_evidence_t *e)
{
    const cairn_engine_params_t *ep = cairn_engine_params();
    bool voltage_up = (e->battery_mv != CAIRN_U16_UNKNOWN &&
                       e->battery_mv >= cairn_engine_params()->engine_on_mv);
    bool moving     = (e->accel_rms_mg >= CAIRN_MOTION_ACCEL_RMS_MG);

    /*
     * Both at once is the unambiguous case and gets the shortest dwell. A
     * parked car does not simultaneously lift its supply rail and shake.
     */
    if (voltage_up && moving) {
        if (e->voltage_high_ms >= ep->drive_both_dwell_ms ||
            e->motion_ms >= ep->drive_both_dwell_ms) {
            return NULL;
        }
        return "voltage and motion agree but have not persisted yet";
    }

    if (voltage_up) {
        if (e->voltage_high_ms >= ep->drive_voltage_dwell_ms) return NULL;
        return "supply is above engine-on but has not persisted yet";
    }

    if (moving) {
        if (e->motion_ms >= ep->drive_motion_dwell_ms) return NULL;
        return "motion has not persisted long enough to be a drive";
    }

    /*
     * Deliberately not treating an unknown voltage as evidence either way. It
     * means the supply could not be read, which is a reason to keep quiet
     * rather than a reason to start talking.
     */
    if (e->battery_mv == CAIRN_U16_UNKNOWN) {
        return "no local evidence of a drive, and the supply is unreadable";
    }

    return "no local evidence of a drive";
}

bool cairn_drive_confirmed(const cairn_drive_evidence_t *e)
{
    return cairn_drive_blocker(e) == NULL;
}
