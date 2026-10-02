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

/* ── the parked-silence invariant ─────────────────────────────────────────── */

/*
 * No diagnostic request may be transmitted unless a drive is confirmed.
 *
 * This is a vehicle-network rule, not a power rule, and it is the single most
 * important constraint in this firmware for a car that polices its own bus.
 *
 * On a BMW F3x the OBD-II connector carries D-CAN only, and access to it is
 * gated by the body domain controller (FEM/BDC). A passive listener therefore
 * sees nothing at all while the car is parked: D-CAN is silent without a
 * tester. Any device that records OBD data while parked must be polling, and
 * polling must bring the gateway up. BMW's energy management counts vehicle
 * wake-ups it did not authorize and can shut the supply down via terminal 30F,
 * storing a fault. Whether a given request reliably produces that record is
 * unverified on an F32 and should not be assumed either way — which is exactly
 * why the conservative rule is cheap insurance.
 *
 * It is expressed as a predicate rather than a timeout on purpose. The previous
 * arrangement was safe only because a five-minute idle threshold happened to
 * fall inside BMW's eight-minute first sleep phase: a coincidence, undocumented,
 * and silently broken by any future change to either number. A named invariant
 * with a test cannot be broken that quietly.
 *
 * Note that reading supply voltage is *not* covered by this rule.
 * COBD::getVoltage() measures the connector's +12V rail at the dongle, which
 * puts nothing on the vehicle bus; readPID() does. Conflating the two is what
 * makes a bus-silent parked mode look impossible when it is not.
 */
typedef struct {
    bool trip_active;     /* a capture is in progress */
    bool drive_confirmed; /* the two-signal transition below has been satisfied */
    bool in_standby;      /* currently inside the standby loop */

    /* The reason for the most recent wake. A periodic-health wake must never
     * open the bus: its whole purpose is proving the device is alive, which
     * needs nothing from the vehicle. */
    cairn_wake_reason_t last_wake;
} cairn_bus_evidence_t;

/* True when a diagnostic request is permitted. */
bool cairn_bus_may_transmit(const cairn_bus_evidence_t *e);

/* Why the bus is being kept silent, for the log. NULL when transmitting is
 * allowed. */
const char *cairn_bus_silence_reason(const cairn_bus_evidence_t *e);

/* ── drive confirmation from local signals only ───────────────────────────── */

/*
 * Decide that a drive has started using evidence the vehicle network cannot
 * see: the connector's supply rail and the accelerometer.
 *
 * Both are deliberately dwell-based. A single bump is a door slam, a tow truck
 * nudge or someone leaning on the wing; a momentary voltage blip is a courtesy
 * light or a central-locking actuator. Requiring either signal to persist, or
 * both to be present at once, is what stops a parked car from opening an OBD
 * session because of a passing lorry.
 *
 * Voltage alone is good for *detecting* a start — the rail lifts well before
 * the car moves — but poor as a definition of an ongoing trip, because BMW's
 * variable alternator strategy lowers system voltage while driving. So it
 * confirms the transition and nothing more; the trip itself is held open by the
 * existing motion scoring.
 */
typedef struct {
    uint16_t battery_mv;        /* CAIRN_U16_UNKNOWN when unreadable */
    uint32_t voltage_high_ms;   /* how long the rail has been above engine-on */
    uint16_t accel_rms_mg;
    uint32_t motion_ms;         /* how long motion has been sustained */
} cairn_drive_evidence_t;

bool cairn_drive_confirmed(const cairn_drive_evidence_t *e);

/* What is still missing before a drive can be declared. NULL once confirmed. */
const char *cairn_drive_blocker(const cairn_drive_evidence_t *e);

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
