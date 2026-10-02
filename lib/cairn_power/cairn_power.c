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

    if (e->idle_ms < CAIRN_STANDBY_IDLE_MS) return "not idle long enough";

    /*
     * A supply already near the engine-on threshold means the engine is
     * probably running, whatever the accelerometer says.
     */
    if (e->battery_mv != CAIRN_U16_UNKNOWN &&
        e->battery_mv >= CAIRN_ENGINE_ON_MV) {
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
    if (battery_mv != CAIRN_U16_UNKNOWN && battery_mv >= CAIRN_ENGINE_ON_MV) {
        return CAIRN_WAKE_ENGINE_VOLTAGE;
    }

    if (accel_rms_mg >= motion_threshold_mg) return CAIRN_WAKE_MOTION;

    /*
     * A long standby still reports in, so a parked device is distinguishable
     * from a dead one. Without this, weeks of correct silence and a failure
     * look identical in the data.
     */
    if (standby_ms >= CAIRN_STANDBY_HEARTBEAT_MS) {
        return CAIRN_WAKE_PERIODIC_HEALTH;
    }

    return CAIRN_WAKE_NONE;
}
