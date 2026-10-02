/*
 * Power management while parked.
 *
 * A device wired into an OBD port draws from the vehicle's battery whenever the
 * engine is off, so a logger that idles at full power is a logger that
 * eventually strands its owner. That makes this a correctness concern rather
 * than an optimisation.
 *
 * What this is NOT
 * ----------------
 *
 * It is not ESP32 deep sleep, and that is a hardware constraint rather than a
 * choice. Waking on motion needs the IMU's interrupt line on an RTC-capable
 * GPIO, and on the ONE+ that line is not routed to the ESP32 at all — the
 * vendored library defines no such pin, only the ICM-42627's internal register
 * names. The official Freematics firmware reaches the same conclusion and
 * polls: its standby() powers peripherals down and then blocks in a motion or
 * voltage loop, configuring no wake source whatsoever.
 *
 * Timer-wake deep sleep is the remaining option and is worse here. Every wake
 * is a full reset, and this firmware's boot mounts the card and runs a recovery
 * scan over every segment — at any polling interval short enough to catch the
 * start of a drive, that costs more energy than it saves, and it churns the boot
 * counter and the log tree for nothing.
 *
 * So the structure follows the vendor's proven one, with two savings it does not
 * take: the CPU is clocked down, and the core light-sleeps between polls. Light
 * sleep preserves RAM and the open capture, so there is no reset and no state to
 * rebuild. It is only entered after the coprocessor is in its own low-power mode
 * and GNSS is off, because a light sleep with a UART mid-transfer loses
 * characters.
 *
 * None of the resulting draw has been measured. The numbers that would justify
 * a particular polling interval need a multimeter on real hardware, so the
 * interval is configuration rather than a claim.
 */

#ifndef CAIRN_POWER_H
#define CAIRN_POWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── when to stand by, and when to come back ──────────────────────────────── */

/*
 * Evidence the decision is made from. Kept as a plain struct so the logic below
 * is portable C and reachable by host tests — the execution is Arduino, but
 * *whether* to sleep is the part worth testing.
 */
typedef struct {
    bool     trip_active;        /* never stand by mid-trip */
    bool     capture_open;       /* an unsealed capture must be sealed first */
    uint32_t idle_ms;            /* how long the vehicle has been at rest */
    uint16_t battery_mv;         /* CAIRN_U16_UNKNOWN when unreadable */
    uint32_t pending_bundles;    /* sealed bundles awaiting a receipt */
    bool     link_online;        /* a sync is in progress or possible */
} cairn_power_evidence_t;

/*
 * True when standing by is the right thing to do.
 *
 * Deliberately conservative: anything that looks like work still to do keeps
 * the device awake. The cost of staying up too long is battery; the cost of
 * sleeping too early is a missed trip, and those are not symmetrical.
 */
bool cairn_power_should_standby(const cairn_power_evidence_t *e);

/* Why standby was declined, for the log. */
const char *cairn_power_standby_blocker(const cairn_power_evidence_t *e);

/*
 * Reasons to leave standby. Motion is the common one; engine voltage is the
 * reliable one, since a vehicle starting lifts the rail well before it moves.
 */
typedef enum {
    CAIRN_WAKE_NONE = 0,
    CAIRN_WAKE_MOTION,
    CAIRN_WAKE_ENGINE_VOLTAGE,
    CAIRN_WAKE_PERIODIC_HEALTH, /* a long standby still reports in */
} cairn_wake_reason_t;

const char *cairn_wake_reason_name(cairn_wake_reason_t r);

/*
 * Decide whether the sampled conditions justify waking.
 *
 * `accel_rms_mg` is the live accelerometer figure, `battery_mv` the supply, and
 * `standby_ms` how long standby has lasted. A voltage that merely drifts up
 * does not count: the threshold is the one the v1 firmware used for engine-on,
 * which is comfortably above a resting battery.
 */
cairn_wake_reason_t cairn_power_should_wake(uint16_t accel_rms_mg,
                                            uint16_t battery_mv,
                                            uint32_t standby_ms,
                                            uint16_t motion_threshold_mg);

/* ── execution ────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t standby_ms;             /* how long the last standby lasted */
    uint32_t polls;                  /* wake-check iterations */
    cairn_wake_reason_t wake_reason;
    uint16_t wake_battery_mv;
} cairn_power_result_t;

/*
 * Power peripherals down and block until something justifies waking.
 *
 * Returns only once awake. The caller is responsible for having sealed the open
 * capture first — this does not seal, because sealing needs the signing key and
 * the lifecycle owns that.
 */
void cairn_power_standby(cairn_power_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_POWER_H */
