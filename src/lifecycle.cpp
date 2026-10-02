#include "lifecycle.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include "board_config.h"
#include "cairn_log.h"
#include "cairn_fs.h"
#include "cairn_ota.h"
#include "cairn_power.h"
#include "cairn_sync.h"
#include "config.h"
#include "policy.h"
#include "preroll.h"
#include "sensor_task.h"

static const char *TAG = "LIFE";

const char *capture_state_name(CaptureState s)
{
    switch (s) {
    case CaptureState::Idle:     return "IDLE";
    case CaptureState::Pretrip:  return "PRETRIP";
    case CaptureState::Active:   return "ACTIVE";
    case CaptureState::Trailing: return "TRAILING";
    default:                     return "?";
    }
}

const char *link_state_name(LinkState s)
{
    switch (s) {
    case LinkState::Offline:     return "OFFLINE";
    case LinkState::Associating: return "ASSOCIATING";
    case LinkState::Online:      return "ONLINE";
    case LinkState::Syncing:     return "SYNCING";
    default:                     return "?";
    }
}

/* ── frame flags ──────────────────────────────────────────────────────────── */

/*
 * Flags describe the conditions a record was captured under, so a reader can
 * weigh it. PRETRIP marks data recorded before a trip was declared, DEGRADED
 * marks data captured while a sensor was unavailable, and POST_RECOVERY marks
 * everything written after a boot that had to repair a torn segment.
 */
static uint16_t frame_flags(const Lifecycle *lc)
{
    uint16_t flags = 0;

    if (lc->capture == CaptureState::Pretrip) flags |= CAIRN_FLAG_PRETRIP;
    if (lc->health_state != CAIRN_HEALTH_OK) flags |= CAIRN_FLAG_DEGRADED;
    if (lc->cap.recovery_state != CAIRN_RECOVERY_CLEAN) {
        flags |= CAIRN_FLAG_POST_RECOVERY;
    }
    if (!lc->have_utc_basis) flags |= CAIRN_FLAG_ESTIMATED_UTC;

    return flags;
}

/*
 * Record a trip event at the last known position.
 *
 * Position is taken from the most recent *valid* fix and is zero when there was
 * none — never a stale fix carried forward, because an event placed at a
 * position the vehicle has since left is worse than one with no position at
 * all. Validity travels with the nearest GNSS_SAMPLE, as the spec requires.
 */
static void emit_trip_event(Lifecycle *lc, uint8_t event_type, const char *detail)
{
    int32_t lat = 0, lon = 0;
    if (lc->have_recent_gnss && lc->last_gnss.fix_type >= 2) {
        lat = lc->last_gnss.lat_e7;
        lon = lc->last_gnss.lon_e7;
    }

    uint8_t payload[12 + CAIRN_MAX_EVENT_DETAIL];
    size_t  len = 0;

    if (cairn_encode_trip_event(event_type, lat, lon, detail, payload,
                                sizeof(payload), &len) != CAIRN_OK) {
        CAIRN_LOGE(TAG, "could not encode a %s event",
                   cairn_event_type_name(event_type));
        return;
    }

    cairn_capture_append(&lc->cap, CAIRN_CHAIN_CAPTURE, CAIRN_REC_TRIP_EVENT, 1,
                         frame_flags(lc), millis(), payload, len);

    CAIRN_LOGI(TAG, "event %s%s%s", cairn_event_type_name(event_type),
               (detail != NULL) ? ": " : "", (detail != NULL) ? detail : "");
}

/*
 * Attribute decisive dynamics to a cause, or admit that it cannot be done.
 *
 * Braking and cornering are indistinguishable from accelerometer magnitude
 * alone: telling them apart needs either the mounting orientation, which is
 * unknown without calibration, or a speed signal, which needs the ECU to be
 * answering. When neither is available the motion is recorded as
 * HARSH_MOTION — real, decisive, and honestly unattributed. A guess would be
 * indistinguishable from a measurement.
 */
static uint8_t attribute_event(const Lifecycle *lc, int32_t speed_delta_cmps,
                               bool have_speed)
{
    /* Far beyond any driving manoeuvre. Checked first because an impact must
     * not be filed as enthusiastic braking. */
    if (lc->last_accel_rms_mg >= 2000) return CAIRN_EVENT_IMPACT;

    if (have_speed) {
        if (speed_delta_cmps <= -500) return CAIRN_EVENT_HARSH_BRAKE;
        if (speed_delta_cmps >= 500) return CAIRN_EVENT_HARSH_ACCEL;

        /* Decisive lateral motion with the speed holding steady. */
        return CAIRN_EVENT_HARSH_CORNERING;
    }

    return CAIRN_EVENT_HARSH_MOTION;
}

/*
 * Write the active policy into the bundle, once, at confirmation.
 *
 * Spec §4.9. A version number identifies a policy but does not describe one, so
 * recording the values makes a trip captured under thresholds nobody remembers
 * explainable from the trip itself.
 */
static void emit_policy_snapshot(Lifecycle *lc)
{
    if (lc->policy_written) return;

    uint8_t payload[256];
    size_t  len = cairn_policy_encode(&lc->policy, payload, sizeof(payload));
    if (len == 0) {
        CAIRN_LOGE(TAG, "policy snapshot would not encode; the bundle will not "
                        "describe its own thresholds");
        return;
    }

    if (cairn_capture_append(&lc->cap, CAIRN_CHAIN_CAPTURE,
                             CAIRN_REC_POLICY_SNAPSHOT, 1, frame_flags(lc),
                             millis(), payload, len)) {
        lc->policy_written = true;
        CAIRN_LOGI(TAG, "policy v%u recorded in the bundle (%u bytes, adaptive "
                        "sampling %s)",
                   (unsigned)lc->policy.policy_version, (unsigned)len,
                   lc->policy.adaptive_sampling ? "on" : "off");
    }
}

/*
 * Every capture record goes through here, so no sampler can accidentally drop
 * data while the trip is unconfirmed. Before a trip is declared the record is
 * held in the pre-roll ring; once declared it is written straight through.
 */
static void emit_capture_record(Lifecycle *lc, uint8_t record_type,
                                uint8_t schema_version, const uint8_t *payload,
                                size_t payload_len)
{
    uint16_t flags = frame_flags(lc);
    uint32_t now = millis();

    if (lc->capture == CaptureState::Idle || lc->capture == CaptureState::Pretrip) {
        cairn_preroll_push(&lc->preroll, record_type, schema_version, flags, now,
                           payload, payload_len);
        return;
    }

    cairn_capture_append(&lc->cap, CAIRN_CHAIN_CAPTURE, record_type,
                         schema_version, flags, now, payload, payload_len);
}

/* ── transitions ──────────────────────────────────────────────────────────── */

/*
 * Record a transition on the journal chain. Every transition carries the
 * trigger, the scores that justified it and the policy version in force —
 * without the policy version, a threshold change makes every past decision
 * unexplainable.
 */
static void emit_transition(Lifecycle *lc, uint8_t region, uint8_t from,
                            uint8_t to, uint8_t trigger, uint8_t reason)
{
    cairn_state_transition_t t;
    memset(&t, 0, sizeof(t));

    t.region         = region;
    t.from_state     = from;
    t.to_state       = to;
    t.trigger_event  = trigger;
    t.reason_code    = reason;
    t.policy_version = CAIRN_POLICY_VERSION;
    t.start_score_e2 = lc->start_score_e2;
    t.stop_score_e2  = lc->stop_score_e2;
    t.wake_cause     = (uint32_t)esp_sleep_get_wakeup_cause();

    uint8_t payload[20];
    cairn_encode_state_transition(&t, payload);

    cairn_capture_append(&lc->cap, CAIRN_CHAIN_JOURNAL,
                         CAIRN_REC_STATE_TRANSITION, 1, frame_flags(lc),
                         millis(), payload, sizeof(payload));
}

static void set_capture_state(Lifecycle *lc, CaptureState next, uint8_t trigger,
                              uint8_t reason)
{
    if (lc->capture == next) return;

    CAIRN_LOGI(TAG, "capture %s -> %s (start %u.%02u, stop %u.%02u)",
               capture_state_name(lc->capture), capture_state_name(next),
               lc->start_score_e2 / 100, lc->start_score_e2 % 100,
               lc->stop_score_e2 / 100, lc->stop_score_e2 % 100);

    emit_transition(lc, CAIRN_REGION_CAPTURE, (uint8_t)lc->capture,
                    (uint8_t)next, trigger, reason);
    lc->capture = next;
}

static void set_link_state(Lifecycle *lc, LinkState next, uint8_t reason)
{
    if (lc->link == next) return;

    CAIRN_LOGI(TAG, "link %s -> %s", link_state_name(lc->link),
               link_state_name(next));
    emit_transition(lc, CAIRN_REGION_CONNECTIVITY, (uint8_t)lc->link,
                    (uint8_t)next, 0, reason);
    lc->link = next;
}


/* ── init ─────────────────────────────────────────────────────────────────── */

bool lifecycle_begin(Lifecycle *lc)
{
    if (!cairn_identity_load(lc->device_id, lc->key_seed, lc->key_public,
                             &lc->boot_count)) {
        return false;
    }
    cairn_new_boot_id(lc->boot_id);
    cairn_log_set_context(lc->boot_id);

    if (!sensors_begin(&lc->sensors)) {
        CAIRN_LOGE(TAG, "coprocessor unavailable; capture will be degraded");
    }

    /*
     * Order matters at boot. Interrupted seals are finished first so a bundle
     * that was already sealed is not reopened as a capture; interrupted prunes
     * come next so the card's free space reflects receipts already verified;
     * only then is a capture opened or resumed.
     */
    cairn_store_resume_interrupted_seals();
    cairn_sync_resume_interrupted_prunes();

    if (!cairn_capture_open_or_resume(&lc->cap, lc->device_id, lc->boot_id)) {
        CAIRN_LOGE(TAG, "cannot open a capture bundle");
        return false;
    }

    cairn_policy_defaults(&lc->policy);
    cairn_preroll_reset(&lc->preroll);

    /*
     * Sensing starts only after the capture is open. A fact arriving before
     * there is somewhere to put it would have to be dropped, and dropping the
     * first seconds of a boot is exactly what the pre-roll exists to prevent.
     */
    if (!sensor_task_start(&lc->sensors)) {
        CAIRN_LOGE(TAG, "sensing task failed to start; no data will be captured");
        return false;
    }

    uint32_t now = millis();
    lc->still_since_ms = now;
    lc->idle_since_ms  = now;

    /* The boot itself is a journal event, including why the device woke. */
    emit_transition(lc, CAIRN_REGION_BUNDLE, 0, (uint8_t)BundleState::Open, 0,
                    lc->cap.recovery_state);

    CAIRN_LOGI(TAG, "lifecycle up: boot %u, wake cause %d, sensors obd=%d imu=%d gnss=%d",
               (unsigned)lc->boot_count, (int)esp_sleep_get_wakeup_cause(),
               (int)lc->sensors.obd, (int)lc->sensors.imu, (int)lc->sensors.gnss);

    return true;
}

/* ── motion scoring ───────────────────────────────────────────────────────── */

/*
 * Combine the evidence available rather than trusting a single source. The IMU
 * sees motion with no ECU and no sky; OBD speed is authoritative when the ECU
 * answers; GNSS speed is good but can lag or be absent in a garage. Scores are
 * hundredths so they can be recorded in the transition payload exactly as they
 * were computed.
 */
static void update_motion_scores(Lifecycle *lc)
{
    const cairn_gnss_sample_t *gnss = &lc->last_gnss;
    const cairn_obd_snapshot_t *obd = &lc->last_obd;
    bool have_gnss = lc->have_recent_gnss;
    bool have_obd  = lc->have_recent_obd;

    uint32_t score = 0;

    /*
     * The accelerometer figure comes from the last FACT_MOTION rather than from
     * the driver: the controller must not read sensor state the sensing task
     * owns, and the fact carries the value as measured.
     */
    uint16_t rms = lc->last_accel_rms_mg;
    if (rms >= CAIRN_MOTION_ACCEL_RMS_MG) {
        score += 100u * rms / CAIRN_MOTION_ACCEL_RMS_MG;
    }

    if (have_obd && obd->speed_kph != CAIRN_I16_UNKNOWN && obd->speed_kph > 3) {
        score += 100;
    }
    if (have_gnss && gnss->speed_cmps != CAIRN_U16_UNKNOWN &&
        gnss->speed_cmps >= CAIRN_MOTION_SPEED_CMPS) {
        score += 100;
    }

    lc->start_score_e2 = (uint16_t)((score > 0xFFFF) ? 0xFFFF : score);

    /*
     * The stop score is not simply the inverse. Stopping requires the absence
     * of evidence, and absence is weaker than presence, which is why the
     * thresholds and dwells differ.
     */
    uint32_t stop = 0;
    if (rms < CAIRN_MOTION_ACCEL_RMS_MG) stop += 50;
    if (have_obd && obd->speed_kph != CAIRN_I16_UNKNOWN && obd->speed_kph <= 1) {
        stop += 30;
    }
    if (have_gnss && gnss->speed_cmps < CAIRN_MOTION_SPEED_CMPS) stop += 20;

    lc->stop_score_e2 = (uint16_t)stop;

    uint32_t now = millis();
    if (lc->start_score_e2 >= CAIRN_START_SCORE_THRESHOLD_E2) {
        if (lc->motion_since_ms == 0) lc->motion_since_ms = now;
        lc->still_since_ms = 0;
    } else {
        if (lc->still_since_ms == 0) lc->still_since_ms = now;
        lc->motion_since_ms = 0;
    }
}

/*
 * Enforce the parked-silence invariant, every tick.
 *
 * Two decisions, in order. First, has a drive started according to evidence the
 * vehicle network cannot see — the supply rail and the accelerometer? Second,
 * given that, may anything be transmitted onto the bus at all?
 *
 * Expressed as a predicate evaluated continuously rather than as a timeout,
 * because the previous arrangement was safe only by accident: a five-minute
 * idle threshold happened to fall inside BMW's eight-minute first sleep phase.
 * Nothing recorded that dependency and any change to either number would have
 * broken it silently. See cairn_power.h.
 */
static void enforce_bus_silence(Lifecycle *lc)
{
    uint32_t now = millis();

    /*
     * Sample the rail on its own short interval. getVoltage() reads the OBD
     * connector's +12V at the dongle through the co-processor — a local
     * measurement, no request onto the vehicle bus. This distinction is the
     * whole reason a bus-silent parked mode is possible, so it is worth being
     * explicit: this call is permitted while parked, readPID() is not.
     */
    if ((int32_t)(now - lc->next_battery_read_ms) >= 0) {
        lc->next_battery_read_ms = now + CAIRN_BATTERY_POLL_MS;
        lc->last_battery_mv = sensors_battery_mv();
    }

    /* Dwell tracking for the supply rail. Local measurement; no bus traffic. */
    bool rail_up = (lc->last_battery_mv != CAIRN_U16_UNKNOWN &&
                    lc->last_battery_mv >= CAIRN_ENGINE_ON_MV);
    if (rail_up) {
        if (lc->voltage_high_since_ms == 0) lc->voltage_high_since_ms = now;
    } else {
        lc->voltage_high_since_ms = 0;
    }

    cairn_drive_evidence_t de;
    memset(&de, 0, sizeof(de));
    de.battery_mv      = lc->last_battery_mv;
    de.voltage_high_ms = rail_up ? (now - lc->voltage_high_since_ms) : 0;
    de.accel_rms_mg    = lc->last_accel_rms_mg;
    de.motion_ms       = (lc->motion_since_ms != 0) ? (now - lc->motion_since_ms) : 0;

    /*
     * Latched while a capture is open. A drive that is already being recorded
     * stays confirmed even through a traffic light, where both the rail sags
     * and the car stops moving; un-confirming there would close the bus
     * mid-trip and lose OBD for the rest of the journey.
     */
    if (lc->capture != CaptureState::Idle) {
        lc->drive_confirmed = true;
    } else if (cairn_drive_confirmed(&de)) {
        if (!lc->drive_confirmed) {
            CAIRN_LOGI(TAG, "drive confirmed from local evidence (rail %u mV for "
                            "%u ms, accel %u mg for %u ms); opening the bus",
                       (unsigned)de.battery_mv, (unsigned)de.voltage_high_ms,
                       (unsigned)de.accel_rms_mg, (unsigned)de.motion_ms);
        }
        lc->drive_confirmed = true;
    } else {
        lc->drive_confirmed = false;
    }

    cairn_bus_evidence_t be;
    memset(&be, 0, sizeof(be));
    be.trip_active     = (lc->capture != CaptureState::Idle);
    be.drive_confirmed = lc->drive_confirmed;
    be.in_standby      = false; /* this runs only while awake */
    be.last_wake       = lc->last_wake;

    const char *why = cairn_bus_silence_reason(&be);
    sensor_task_set_bus_silent(why != nullptr);

    /* Logged on change only; this runs at tick rate. */
    if (why != lc->last_bus_silence_reason) {
        if (why != nullptr) {
            CAIRN_LOGI(TAG, "vehicle bus silent: %s", why);
        } else {
            CAIRN_LOGI(TAG, "vehicle bus open: a drive is confirmed");
        }
        lc->last_bus_silence_reason = why;
    }
}

/* ── sampling ─────────────────────────────────────────────────────────────── */

static void close_gnss_gap(Lifecycle *lc, uint8_t cause)
{
    if (!lc->in_gnss_gap) return;

    cairn_gnss_gap_t gap;
    memset(&gap, 0, sizeof(gap));
    gap.duration_ms      = millis() - lc->gnss_gap_started_ms;
    gap.expected_samples = (uint16_t)lc->gnss_expected_in_gap;
    gap.cause            = cause;

    uint8_t payload[12];
    cairn_encode_gnss_gap(&gap, payload);

    cairn_capture_append(&lc->cap, CAIRN_CHAIN_CAPTURE, CAIRN_REC_GNSS_GAP, 1,
                         frame_flags(lc), millis(), payload, sizeof(payload));

    CAIRN_LOGI(TAG, "GNSS gap closed: %u ms, %u samples missed, cause %u",
               (unsigned)gap.duration_ms, (unsigned)gap.expected_samples,
               (unsigned)cause);

    lc->in_gnss_gap = false;
    lc->gnss_expected_in_gap = 0;
}

/*
 * Facts are consumed here and nowhere else. The controller owns all state, so
 * these functions are the only place a sensor observation turns into a
 * transition or a frame on the card.
 */

static void on_gnss_sample(Lifecycle *lc, const fact_t *f)
{
    close_gnss_gap(lc, lc->sensors.gnss ? CAIRN_GAP_NO_FIX
                                        : CAIRN_GAP_POWERED_DOWN);

    lc->last_gnss = f->data.gnss;
    lc->have_recent_gnss = true;

    uint8_t payload[32];
    cairn_encode_gnss_sample(&f->data.gnss, payload);
    emit_capture_record(lc, CAIRN_REC_GNSS_SAMPLE, 1, payload, sizeof(payload));
}

static void on_gnss_no_fix(Lifecycle *lc, const fact_t *f)
{
    if (!lc->in_gnss_gap) {
        lc->in_gnss_gap = true;
        lc->gnss_gap_started_ms = f->monotonic_ms;
        lc->gnss_expected_in_gap = 0;
        CAIRN_LOGD(TAG, "GNSS gap opened");
    }
    lc->gnss_expected_in_gap++;

    /* A stale fix must not keep scoring motion forever. */
    lc->have_recent_gnss = false;
}

static void on_utc_basis(Lifecycle *lc, const fact_t *f)
{
    if (lc->have_utc_basis) return;

    cairn_capture_set_utc_basis(&lc->cap, f->data.utc.utc_ms, f->data.utc.acc_ms);
    lc->have_utc_basis = true;
    CAIRN_LOGI(TAG, "UTC basis established: %llu ms (+/- %u ms)",
               (unsigned long long)f->data.utc.utc_ms,
               (unsigned)f->data.utc.acc_ms);
}

static void on_imu_summary(Lifecycle *lc, const fact_t *f)
{
    uint8_t payload[20];
    cairn_encode_imu_summary(&f->data.imu, payload);
    emit_capture_record(lc, CAIRN_REC_IMU_SUMMARY, 1, payload, sizeof(payload));
}

static void on_obd_snapshot(Lifecycle *lc, const fact_t *f)
{
    lc->last_obd = f->data.obd;
    lc->have_recent_obd = true;

    uint8_t payload[24];
    cairn_encode_obd_snapshot(&f->data.obd, payload);
    emit_capture_record(lc, CAIRN_REC_OBD_SNAPSHOT, 1, payload, sizeof(payload));
}

static void on_obd_silent(Lifecycle *lc)
{
    /* No snapshot is written: a reading where nothing answered is a gap, not an
     * observation of zero. Scoring must stop trusting the last one. */
    lc->have_recent_obd = false;
}

/*
 * Compose the degraded-state bitmap from conditions actually observed.
 *
 * Every bit is set from evidence, never from inference: DEGRADED_GNSS means no
 * fix arrived, not that one looks unlikely. And because it is a bitmap rather
 * than a severity, simultaneous problems all survive — a low battery no longer
 * hides the fact that position was unavailable too.
 */
static uint8_t compose_health_state(const Lifecycle *lc,
                                    const cairn_device_health_t *h,
                                    uint64_t free_mib, uint32_t dropped_facts)
{
    uint8_t state = CAIRN_HEALTH_OK;

    if (!lc->sensors.gnss || !lc->have_recent_gnss || lc->in_gnss_gap) {
        state |= CAIRN_HEALTH_DEGRADED_GNSS;
    }

    if (lc->cap.write_errors > 0 ||
        (free_mib > 0 && free_mib < CAIRN_LOG_FREE_SPACE_FLOOR_MIB)) {
        state |= CAIRN_HEALTH_DEGRADED_STORAGE;
    }

    /*
     * Set before the first fix of every trip, which is normal rather than
     * exceptional. Saying so is what stops a reader treating monotonic-only
     * timestamps as though they were UTC.
     */
    if (!lc->have_utc_basis) state |= CAIRN_HEALTH_DEGRADED_TIME;

    /* Only degraded if there is something waiting that could not be delivered.
     * Being offline with nothing pending is the designed steady state, not a
     * fault. */
    if (lc->pending_bundles > 0 && lc->link == LinkState::Offline) {
        state |= CAIRN_HEALTH_DEGRADED_NETWORK;
    }

    if (h->battery_mv != CAIRN_U16_UNKNOWN && h->battery_mv < 11500) {
        state |= CAIRN_HEALTH_LOW_POWER;
    }

    if (lc->cap.needs_seal ||
        lc->cap.recovery_state == CAIRN_RECOVERY_SALVAGED) {
        state |= CAIRN_HEALTH_RECOVERY_REQUIRED;
    }

    if (!lc->sensors.imu || !lc->sensors.obd || dropped_facts > 0) {
        state |= CAIRN_HEALTH_DEGRADED_SENSING;
    }

    return state;
}

static void on_health(Lifecycle *lc, const fact_t *f)
{
    cairn_device_health_t h = f->data.health;

    h.reboot_count = (uint8_t)lc->boot_count;

    int rssi = cairn_sync_rssi();
    h.rssi_dbm = (rssi != 0) ? (int8_t)rssi : CAIRN_I8_UNKNOWN;

    uint64_t total = 0, used = 0, free_mib = 0;
    if (cairn_fs_space(&total, &used)) {
        free_mib = (total > used) ? (total - used) / (1024 * 1024) : 0;
        h.sd_free_mib = (uint16_t)((free_mib > 0xFFFE) ? 0xFFFE : free_mib);
    }

    /*
     * Dropped facts are reported in the data, not just the log: a queue that
     * overflowed means the record stream has holes, and a gap the data does not
     * admit to is worse than one it does.
     */
    uint32_t dropped = sensor_task_dropped();
    if (dropped != lc->reported_drops) {
        CAIRN_LOGW(TAG, "%u sensor facts dropped since boot (%u new); the "
                        "controller is not keeping up",
                   (unsigned)dropped, (unsigned)(dropped - lc->reported_drops));
        lc->reported_drops = dropped;
    }
    if (h.ext_sensor_1 == CAIRN_U16_UNKNOWN) {
        h.ext_sensor_1 = (uint16_t)((dropped > 0xFFFE) ? 0xFFFE : dropped);
    }

    uint8_t state = compose_health_state(lc, &h, free_mib, dropped);
    h.health_state = state;

    uint8_t payload[16];
    cairn_encode_device_health(&h, payload);

    /* Health belongs to the journal chain: it is true of the device, not of the
     * drive, and must be recordable while no trip is in progress. */
    cairn_capture_append(&lc->cap, CAIRN_CHAIN_JOURNAL, CAIRN_REC_DEVICE_HEALTH,
                         1, frame_flags(lc), f->monotonic_ms, payload,
                         sizeof(payload));

    /* A transition is journalled only when the set of active conditions
     * changes, so the journal records changes rather than a heartbeat. */
    if (state != lc->health_state) {
        char before[160], after[160];
        cairn_health_state_names(lc->health_state, before, sizeof(before));
        cairn_health_state_names(state, after, sizeof(after));

        CAIRN_LOGW(TAG, "health %s -> %s", before, after);

        emit_transition(lc, CAIRN_REGION_HEALTH, lc->health_state, state, 0, 0);
        lc->health_state = state;
    }

    char names[160];
    cairn_health_state_names(state, names, sizeof(names));

    /*
     * Render the unavailable sentinel as a word, not as 65535.
     *
     * battery_mv is CAIRN_U16_UNKNOWN whenever the ECU is not answering, since
     * the voltage is read over OBD. The comparisons that matter already guard
     * for it, but the log did not, so a parked device reported "health: 65535
     * mV" — a number that invites being read as a measurement, in the one place
     * someone looks to find out whether the supply is healthy.
     */
    char battery[16];
    if (h.battery_mv == CAIRN_U16_UNKNOWN) {
        snprintf(battery, sizeof(battery), "unknown");
    } else {
        snprintf(battery, sizeof(battery), "%u mV", (unsigned)h.battery_mv);
    }

    CAIRN_LOGD(TAG, "health: battery %s, %llu MiB free, %u write errors, "
                    "%u dropped facts, state %s",
               battery, (unsigned long long)free_mib,
               (unsigned)lc->cap.write_errors, (unsigned)dropped, names);

    /*
     * What the sensors are actually reporting, next to the health line and at
     * the same cadence.
     *
     * This exists because the log was silent about it. While a car is parked no
     * GNSS or IMU record reaches the card at all — they sit in the pre-roll ring
     * and are discarded unless a trip confirms — which is correct, and also
     * means an unusable GNSS antenna and a perfectly good one produce byte-for-
     * byte identical logs until someone drives somewhere and decodes the bundle
     * afterwards. Fix state is the single most useful thing to be able to read
     * at a glance before trusting a drive to the device, so it is INFO and goes
     * to the card.
     *
     * Fix type follows the NMEA convention the GNSS record uses: 0 none,
     * 2 two-dimensional, 3 three-dimensional.
     */
    if (lc->have_recent_gnss) {
        const cairn_gnss_sample_t *g = &lc->last_gnss;

        char speed[16];
        if (g->speed_cmps == CAIRN_U16_UNKNOWN) {
            snprintf(speed, sizeof(speed), "unknown");
        } else {
            snprintf(speed, sizeof(speed), "%u.%u km/h",
                     (unsigned)(g->speed_cmps * 36u / 1000u),
                     (unsigned)((g->speed_cmps * 36u / 100u) % 10u));
        }

        if (g->fix_type >= 2) {
            CAIRN_LOGI(TAG, "sensors: gnss fix=%u sats=%u hdop=%u.%02u %s, "
                            "lat=%ld lon=%ld | accel rms=%u mg | start=%u "
                            "stop=%u | obd=%s",
                       (unsigned)g->fix_type, (unsigned)g->sats_used,
                       (unsigned)(g->hdop_e2 / 100u), (unsigned)(g->hdop_e2 % 100u),
                       speed, (long)g->lat_e7, (long)g->lon_e7,
                       (unsigned)lc->last_accel_rms_mg,
                       (unsigned)lc->start_score_e2, (unsigned)lc->stop_score_e2,
                       lc->have_recent_obd ? "live" : "absent");
        } else {
            CAIRN_LOGI(TAG, "sensors: gnss NO FIX (sats=%u, needs sky view) | "
                            "accel rms=%u mg | start=%u stop=%u | obd=%s",
                       (unsigned)g->sats_used, (unsigned)lc->last_accel_rms_mg,
                       (unsigned)lc->start_score_e2, (unsigned)lc->stop_score_e2,
                       lc->have_recent_obd ? "live" : "absent");
        }
    } else {
        CAIRN_LOGW(TAG, "sensors: no GNSS sample yet | accel rms=%u mg | "
                        "start=%u stop=%u | obd=%s",
                   (unsigned)lc->last_accel_rms_mg,
                   (unsigned)lc->start_score_e2, (unsigned)lc->stop_score_e2,
                   lc->have_recent_obd ? "live" : "absent");
    }
}

/* ── sealing and sync ─────────────────────────────────────────────────────── */

static void seal_and_reopen(Lifecycle *lc, uint8_t reason)
{
    lc->bundle = BundleState::Sealing;
    emit_transition(lc, CAIRN_REGION_BUNDLE, (uint8_t)BundleState::Open,
                    (uint8_t)BundleState::Sealing, 0, reason);

    uint8_t sealed_id[16];
    bool ok = cairn_capture_seal(&lc->cap, lc->key_seed, lc->key_public,
                                 CAIRN_FIRMWARE_VERSION, CAIRN_POLICY_VERSION,
                                 sealed_id);

    /*
     * The seal is the deepest stack path this task takes — Merkle tree, CBOR
     * manifest and an Ed25519 signature. Arduino's default 8 KB loop stack was
     * not enough for it and tripped the canary on real hardware, so record the
     * remaining headroom every time rather than rediscovering the limit by
     * panicking in a parked car. See CAIRN_LOOP_STACK_BYTES.
     */
    {
        size_t free_bytes = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
        if (free_bytes < CAIRN_STACK_WARN_BYTES) {
            CAIRN_LOGW(TAG, "stack headroom after seal: %u bytes of %u — close "
                            "to the canary; raise CAIRN_LOOP_STACK_BYTES",
                       (unsigned)free_bytes, (unsigned)CAIRN_LOOP_STACK_BYTES);
        } else {
            CAIRN_LOGI(TAG, "stack headroom after seal: %u bytes of %u",
                       (unsigned)free_bytes, (unsigned)CAIRN_LOOP_STACK_BYTES);
        }
    }

    if (!ok) {
        /*
         * A failed seal leaves the capture directory intact. Nothing is
         * deleted and nothing is rewritten, so the next boot can try again.
         */
        CAIRN_LOGE(TAG, "seal failed; the capture stays open for the next attempt");
        lc->bundle = BundleState::Open;
        return;
    }

    lc->bundle = BundleState::Sealed;

    /* A fresh capture, with its own bundle id and its own chains. */
    if (!cairn_capture_open_or_resume(&lc->cap, lc->device_id, lc->boot_id)) {
        CAIRN_LOGE(TAG, "cannot open a new capture after sealing");
        return;
    }
    lc->bundle = BundleState::Open;
    lc->have_utc_basis = false;
    lc->policy_written = false; /* a new bundle must describe its own policy */

    emit_transition(lc, CAIRN_REGION_BUNDLE, (uint8_t)BundleState::Sealed,
                    (uint8_t)BundleState::Open, 0, 0);
}

/*
 * Check for firmware while parked and connected.
 *
 * Folded into the sync pass because both want the same conditions — idle,
 * associated, nothing urgent happening — and bringing the radio up twice would
 * cost power for no benefit. The preconditions in cairn_ota are checked again
 * inside, so this is a scheduling hint rather than the gate.
 */
static void maybe_update(Lifecycle *lc)
{
#if CAIRN_OTA_AVAILABLE
    uint32_t now = millis();
    if ((int32_t)(now - lc->next_ota_check_ms) < 0) return;
    lc->next_ota_check_ms = now + CAIRN_OTA_CHECK_INTERVAL_MS;

    bool parked = (lc->capture == CaptureState::Idle);

    /*
     * Read the supply directly rather than gating on a live ECU.
     *
     * This gate made OTA unreachable. The preconditions block while driving via
     * not_parked, and treat an unknown voltage as unhealthy — correctly, since
     * updating on the strength of a reading the device could not take is the
     * wrong direction. But a parked car has no ECU answering, so gating here
     * forced battery_mv to UNKNOWN exactly when parked, and the two conditions
     * between them rejected every state the device is ever in. The one surviving
     * window was engine running and vehicle stationary, which is close to the
     * opposite of the intent recorded in cairn_ota.c.
     *
     * sensors_battery_mv reads OBD-port voltage off the coprocessor and is
     * documented to work with the ignition off; measured at 5440 mV on bench USB
     * with no ECU present. A resting 12 V battery sits above OTA_MIN_SUPPLY_MV
     * and a weak one does not, which is the discrimination the threshold was
     * written for.
     */
    uint16_t battery = sensors_battery_mv();

    bool reboot = false;
    cairn_ota_result_t r = cairn_ota_check_and_install(parked, battery, &reboot);

    if (r != CAIRN_OTA_BLOCKED && r != CAIRN_OTA_UP_TO_DATE) {
        CAIRN_LOGI(TAG, "update check: %s", cairn_ota_result_name(r));
    }

    if (reboot) {
        /*
         * Seal and flush before restarting. The staged image is already
         * verified, so there is no hurry — and rebooting with an open capture
         * would leave a torn tail for the new firmware to recover, which is
         * recoverable but pointless when it can be avoided.
         */
        CAIRN_LOGW(TAG, "rebooting into the staged image");
        if (lc->cap.active) seal_and_reopen(lc, 2);
        cairn_log_flush();
        cairn_log_detach_sd();
        delay(200);
        ESP.restart();
    }
#else
    (void)lc;
#endif
}

/*
 * Stand by when there is demonstrably nothing to do.
 *
 * Ordering matters more than the saving. Standby stops the radio, so it stops
 * the only process that can turn unreceipted bundles into safe ones — which is
 * why it runs *after* a sync attempt and why pending bundles with a live link
 * block it outright. And it seals the open capture first: leaving one open for
 * days is recoverable, but it would sit undeliverable the whole time and the
 * receipt-gated prune could never reclaim the space.
 */
/* Distinct pointer so the change-only log filter can tell this reason apart. */
static const char *const k_settling = "letting the link settle after a heartbeat";

static void maybe_standby(Lifecycle *lc)
{
    /*
     * After a health heartbeat, stay up long enough to actually read the
     * supply.
     *
     * Measured on the card: the co-processor returns nothing for about fifteen
     * seconds after leaveLowPowerMode(), so the health record emitted straight
     * after a resume carries "battery unknown" and the first valid reading
     * arrives on the following sample. The retry inside sensors_battery_mv
     * covers 60 ms, which is two orders of magnitude short.
     *
     * That interacts badly with this function's own heartbeat handling, which
     * deliberately stopped rebasing idle_since_ms so the device returns to
     * standby immediately instead of burning five minutes polling. Correct for
     * the vehicle bus, but it meant the six-hour heartbeat went back to sleep
     * before the link could answer — recording an unknown voltage every time,
     * from the one sample whose entire purpose is recording voltage on a parked
     * device. The parked-drain series would have had a hole exactly where it
     * matters.
     *
     * Twenty seconds every six hours is 0.09% duty, which buys back the only
     * measurement the firmware can make about its own power cost.
     */
    if (lc->heartbeat_settle_until_ms != 0) {
        if ((int32_t)(millis() - lc->heartbeat_settle_until_ms) < 0) {
            if (lc->last_standby_blocker != k_settling) {
                CAIRN_LOGI(TAG, "holding off standby: letting the link settle so "
                                "the heartbeat can record a supply voltage");
                lc->last_standby_blocker = k_settling;
            }
            return;
        }

        CAIRN_LOGI(TAG, "heartbeat settled with supply %s; standing by",
                   (lc->last_battery_mv == CAIRN_U16_UNKNOWN)
                       ? "still unreadable"
                       : "recorded");
        lc->heartbeat_settle_until_ms = 0;
    }

    cairn_power_evidence_t e;
    memset(&e, 0, sizeof(e));

    e.trip_active = (lc->capture != CaptureState::Idle);
    e.capture_open = lc->cap.active && lc->cap.have_any_frame;
    e.idle_ms = millis() - lc->idle_since_ms;
    /* Same reasoning as the OTA precondition: the voltage is readable with the
     * ignition off, and gating it on a live ECU meant the engine-voltage standby
     * gate could never fire, since it only matters while parked. */
    e.battery_mv = sensors_battery_mv();
    e.pending_bundles = lc->pending_bundles;
    e.link_online = (lc->link != LinkState::Offline);

    /*
     * An open capture holding data is a reason to seal, not a reason to stay
     * awake forever — so seal it and re-evaluate on the next pass rather than
     * sleeping with it open.
     */
    if (!e.trip_active && e.capture_open &&
        e.idle_ms >= CAIRN_STANDBY_IDLE_MS) {
        CAIRN_LOGI(TAG, "sealing before standby");
        seal_and_reopen(lc, 3);
        return;
    }

    if (!cairn_power_should_standby(&e)) {
        /*
         * Log the blocker only when it changes. maybe_standby() runs on every
         * tick, so an unconditional line here is one write per 20 ms — on the
         * bench that was the overwhelming majority of the card log, and the
         * point of the log is that a real event is findable in it. The reason a
         * device stayed awake is worth recording; repeating it fifty times a
         * second is not.
         *
         * Compared by pointer, not strcmp: cairn_power_standby_blocker returns
         * string literals, so identity is both sufficient and exact.
         */
        const char *why = cairn_power_standby_blocker(&e);
        if (why != nullptr && why != lc->last_standby_blocker) {
            CAIRN_LOGI(TAG, "not standing by: %s", why);
            lc->last_standby_blocker = why;
        }
        return;
    }

    lc->last_standby_blocker = nullptr;

    emit_transition(lc, CAIRN_REGION_HEALTH, CAIRN_POWER_STATE_AWAKE,
                    CAIRN_POWER_STATE_STANDBY, CAIRN_TRIGGER_POWER, 0);
    cairn_log_flush();

    cairn_power_result_t r;
    cairn_power_standby(&r);

    lc->total_standby_ms += r.standby_ms;
    lc->standby_count++;

    /*
     * Close the pair. Without an exit record the journal shows a device going to
     * sleep and never confirming it came back, so every standby window is
     * open-ended and parked draw cannot be attributed: a voltage series cannot
     * tell six hours asleep from six hours awake, and those differ by an order
     * of magnitude.
     *
     * The wake reason travels in reason_code so the server can separate a
     * heartbeat — the uninteresting case, and the one that should dominate a
     * parked car — from motion or an engine start. Duration is exit − enter from
     * the two frames' monotonic_ms; light sleep leaves that clock running, which
     * is the property that makes the subtraction valid.
     */
    emit_transition(lc, CAIRN_REGION_HEALTH, CAIRN_POWER_STATE_STANDBY,
                    CAIRN_POWER_STATE_AWAKE, CAIRN_TRIGGER_POWER,
                    (uint8_t)r.wake_reason);

    /*
     * Waking is a fresh start for the sensors but not for the bundle: the
     * capture, the chain and the identity all survived in RAM, because this is
     * light sleep rather than a reset.
     */
    cairn_log_attach_sd(lc->boot_count);
    CAIRN_LOGW(TAG, "resumed after %u ms standby (%s); %u standby period(s) "
                    "totalling %u ms this boot",
               (unsigned)r.standby_ms, cairn_wake_reason_name(r.wake_reason),
               (unsigned)lc->standby_count, (unsigned)lc->total_standby_ms);

    /*
     * The timers all reference millis(), which kept running through light
     * sleep, so every schedule is now far in the past. Rebase them rather than
     * letting the first pass after waking fire everything at once.
     */
    uint32_t now = millis();
    lc->next_ota_check_ms = now + CAIRN_OTA_CHECK_INTERVAL_MS;
    lc->still_since_ms = now;
    lc->motion_since_ms = 0;
    lc->last_wake = r.wake_reason;

    /* The receiver was powered down, so position is unknown until it reacquires
     * — and the degraded bitmap should say so rather than carry a stale fix. */
    lc->have_recent_gnss = false;
    lc->have_recent_obd = false;

    /*
     * A health heartbeat must cost the vehicle nothing.
     *
     * Its purpose is proving a parked device is alive, which needs the supply
     * rail, the accelerometer, storage counters and an uptime — all local. The
     * previous behaviour rebased the idle timer and requested an OBD retry, so
     * every heartbeat spent five further minutes polling the bus: four
     * unauthorized wake-ups a day on a car whose energy management counts
     * exactly that. Leaving idle_since_ms alone means the device returns to
     * standby on the next pass instead, and the retry is withheld so nothing
     * reopens a diagnostic session.
     *
     * A motion or engine-voltage wake is different: something may actually be
     * happening, so those rebase the timer and retry the subsystems as before.
     */
    if (r.wake_reason == CAIRN_WAKE_PERIODIC_HEALTH) {
        CAIRN_LOGI(TAG, "health heartbeat: staying bus-silent and returning to "
                        "standby without polling the vehicle");
        lc->heartbeat_settle_until_ms = now + CAIRN_HEARTBEAT_SETTLE_MS;
    } else {
        lc->idle_since_ms = now;
        sensor_task_request_retry();
    }

    /*
     * Engine voltage is the early signal: it rises before the vehicle moves. A
     * wake on it means a drive is probably starting, so the pre-roll should be
     * filling rather than the device deciding it is still parked.
     */
    if (r.wake_reason == CAIRN_WAKE_ENGINE_VOLTAGE) {
        emit_trip_event(lc, CAIRN_EVENT_HARSH_MOTION, "engine start detected");
    }
}

/*
 * Sync only while idle. Uploading during a drive competes with capture for both
 * the CPU and the SPI bus the card is on, and nothing about this data is
 * time-critical.
 */
static void maybe_sync(Lifecycle *lc)
{
    if (lc->capture != CaptureState::Idle) return;

    /*
     * idle_since_ms is read here and never written.
     *
     * Writing it was a bug that disabled standby entirely. This function used
     * to rebase idle_since_ms on both a completed sync and a failed connect,
     * giving one timer two jobs with opposite requirements: "how long has the
     * vehicle been idle", which maybe_standby needs to grow without
     * interruption, and "when may sync be retried", which wants resetting.
     *
     * The result was a loop with no exit. Idle reached the standby threshold,
     * maybe_standby sealed the open capture, the seal produced a pending
     * bundle, sync uploaded it and reset the idle clock to zero — and a GNSS
     * gap record landed in the fresh capture before the dwell could elapse
     * again. Measured on the bench: five minutes between seals, and standby
     * never entered once in seven minutes of a parked device that had nothing
     * else to do. Retry pacing now has its own timer.
     */
    if (millis() - lc->idle_since_ms < CAIRN_SYNC_MIN_IDLE_MS) return;
    if ((int32_t)(millis() - lc->next_sync_ms) < 0) return;

    uint32_t pending = 0;
    uint64_t bytes = 0;
    bool have_stats = cairn_store_pending_stats(&pending, &bytes);
    lc->pending_bundles = have_stats ? pending : 0;
    if (!have_stats || pending == 0) return;

    CAIRN_LOGI(TAG, "%u bundle(s) pending, %llu bytes; attempting sync",
               (unsigned)pending, (unsigned long long)bytes);

    set_link_state(lc, LinkState::Associating, 0);

    if (!cairn_sync_connect(CAIRN_SYNC_CONNECT_TIMEOUT_MS)) {
        set_link_state(lc, LinkState::Offline, 1);
        /* Offline is normal, not a fault: the whole design is offline-first. */
        /* Back off this retry only. Touching idle_since_ms here would reset
         * the standby dwell, and an unreachable server is the one case where
         * sleeping matters most. */
        lc->next_sync_ms = millis() + CAIRN_SYNC_RETRY_MS;
        return;
    }

    set_link_state(lc, LinkState::Syncing, 0);

    cairn_sync_stats_t stats;
    cairn_sync_result_t r = cairn_sync_run(&stats);

    CAIRN_LOGI(TAG, "sync result %s", cairn_sync_result_name(r));

    /* While the radio is still up and the device is demonstrably idle. */
    maybe_update(lc);

    cairn_sync_disconnect();
    set_link_state(lc, LinkState::Offline, 0);

    /* Flush the log so the sync outcome is on the card even if power is cut
     * immediately afterwards. */
    cairn_log_flush();

    /* Deliberately does not touch idle_since_ms. A completed sync with nothing
     * left pending is precisely the moment standing by is safe. */
    lc->next_sync_ms = millis();
}

/* ── tick ─────────────────────────────────────────────────────────────────── */

void lifecycle_tick(Lifecycle *lc)
{
    uint32_t now = millis();

    /*
     * Drain everything waiting. The controller is the only consumer, so the
     * queue depth is the only buffer between a slow card write and a dropped
     * observation — emptying it fully each pass is what keeps that buffer
     * available.
     */
    fact_t f;
    while (sensor_task_poll(&f)) {
        switch (f.kind) {
        case FACT_GNSS_SAMPLE:   on_gnss_sample(lc, &f); break;
        case FACT_GNSS_NO_FIX:   on_gnss_no_fix(lc, &f); break;
        case FACT_GNSS_UTC_BASIS: on_utc_basis(lc, &f); break;
        case FACT_IMU_SUMMARY:   on_imu_summary(lc, &f); break;
        case FACT_OBD_SNAPSHOT:  on_obd_snapshot(lc, &f); break;
        case FACT_OBD_SILENT:    on_obd_silent(lc); break;
        case FACT_HEALTH:        on_health(lc, &f); break;

        case FACT_MOTION:
            lc->last_accel_rms_mg = f.data.motion.accel_rms_mg;
            break;
        }
    }

    update_motion_scores(lc);
    enforce_bus_silence(lc);

    /*
     * Choose sampling rates from what the vehicle appears to be doing. The
     * controller owns this because classifying it needs the whole picture —
     * speed from whichever source answered, the trip state, the policy — and
     * the sensing task knows none of that by design.
     */
    {
        bool trip_active = (lc->capture == CaptureState::Active ||
                            lc->capture == CaptureState::Trailing);

        uint16_t speed = CAIRN_U16_UNKNOWN;
        if (lc->have_recent_obd && lc->last_obd.speed_kph != CAIRN_I16_UNKNOWN) {
            /* km/h to cm/s. OBD speed is preferred over GNSS when the ECU
             * answers: it is the vehicle's own measurement and does not lag. */
            speed = (uint16_t)((int32_t)lc->last_obd.speed_kph * 1000 / 36);
        } else if (lc->have_recent_gnss) {
            speed = lc->last_gnss.speed_cmps;
        }

        int32_t delta = 0;
        if (speed != CAIRN_U16_UNKNOWN &&
            lc->last_speed_cmps != CAIRN_U16_UNKNOWN) {
            delta = (int32_t)speed - (int32_t)lc->last_speed_cmps;
        }
        lc->last_speed_cmps = speed;

        cairn_dynamics_t d = cairn_policy_classify(&lc->policy,
                                                   lc->last_accel_rms_mg, speed,
                                                   delta, trip_active);
        if (d != lc->dynamics) {
            CAIRN_LOGD(TAG, "dynamics %s -> %s", cairn_dynamics_name(lc->dynamics),
                       cairn_dynamics_name(d));

            /*
             * One event per entry into EVENT, not per tick while it lasts. A
             * single hard stop is one event; emitting at 50 Hz for its duration
             * would bury it in its own repetitions.
             */
            if (d == CAIRN_DYN_EVENT && lc->dynamics != CAIRN_DYN_EVENT &&
                lc->capture == CaptureState::Active) {
                char detail[CAIRN_MAX_EVENT_DETAIL];
                snprintf(detail, sizeof(detail), "rms=%umg dv=%ldcm/s",
                         (unsigned)lc->last_accel_rms_mg, (long)delta);
                emit_trip_event(lc, attribute_event(lc, delta,
                                                    speed != CAIRN_U16_UNKNOWN),
                                detail);
            }

            lc->dynamics = d;
        }

        cairn_rates_t rates;
        cairn_policy_rates(&lc->policy, d, trip_active, &rates);
        sensor_task_set_rates(&rates);
    }

    /* ── capture region ───────────────────────────────────────────────────── */

    switch (lc->capture) {
    case CaptureState::Idle:
        if (lc->motion_since_ms != 0) {
            set_capture_state(lc, CaptureState::Pretrip, 1, 0);
        }
        break;

    case CaptureState::Pretrip:
        if (lc->motion_since_ms == 0) {
            /*
             * The motion did not persist, so nothing is written. The ring keeps
             * filling and evicting while idle, which is what lets a real drive
             * recover its first seconds without a parked car producing bundles.
             *
             * idle_since_ms is deliberately NOT rebased here. This branch is
             * the pre-roll *rejecting* a candidate — it has just decided the
             * movement was not a drive — so the vehicle has been idle all
             * along and the standby dwell should keep running. Rebasing it
             * treated a rejected candidate as activity, and since the dwell is
             * five minutes while a rejection takes well under a second, any
             * stray bump reset the clock to zero.
             *
             * Measured on the bench: a single 638 ms transient
             * (IDLE -> PRETRIP -> IDLE, start score 2.58 then 0.00) was enough,
             * and the device never reached standby. In a car parked on a street
             * — passing lorries, doors, someone leaning on a wing — it would
             * never have slept at all. Only a genuine trip ending, the
             * Active -> Idle transition below after the stop dwell, marks the
             * moment the vehicle actually became idle.
             */
            set_capture_state(lc, CaptureState::Idle, 2, 0);
        } else if (now - lc->motion_since_ms >= CAIRN_START_DWELL_MS) {
            /*
             * Confirmed. The state is set first so the flushed frames are
             * written through rather than pushed straight back into the ring,
             * and so their PRETRIP flag is the only thing marking them apart.
             */
            set_capture_state(lc, CaptureState::Active, 1, 0);

            /* Policy first, then the pre-roll: the snapshot describes the
             * thresholds that admitted those very records, so it belongs ahead
             * of them in the chain. */
            emit_policy_snapshot(lc);
            cairn_preroll_flush(&lc->preroll, &lc->cap);

            /*
             * After the pre-roll, so the event marks where the *trip* was
             * declared rather than where the buffered history begins. The
             * PRETRIP flag already distinguishes those records.
             */
            emit_trip_event(lc, CAIRN_EVENT_TRIP_START, nullptr);

            /* A bundle that resumed an interrupted write says so in the data,
             * not only in the manifest's recovery_state. */
            if (lc->cap.recovery_state != CAIRN_RECOVERY_CLEAN) {
                char detail[CAIRN_MAX_EVENT_DETAIL];
                snprintf(detail, sizeof(detail), "state=%u discarded=%u",
                         (unsigned)lc->cap.recovery_state,
                         (unsigned)lc->cap.discarded_tail_bytes);
                emit_trip_event(lc, CAIRN_EVENT_CAPTURE_RECOVERED, detail);
            }
        }
        break;

    case CaptureState::Active:
        if (lc->still_since_ms != 0 &&
            lc->stop_score_e2 >= CAIRN_STOP_SCORE_THRESHOLD_E2) {
            set_capture_state(lc, CaptureState::Trailing, 3, 0);
        }
        break;

    case CaptureState::Trailing:
        if (lc->motion_since_ms != 0) {
            /* Moving again before the dwell expired: the same trip continues
             * rather than being split at every traffic light. */
            set_capture_state(lc, CaptureState::Active, 1, 1);
        } else if (lc->still_since_ms != 0 &&
                   now - lc->still_since_ms >= CAIRN_STOP_DWELL_MS) {
            set_capture_state(lc, CaptureState::Idle, 2, 0);
            lc->idle_since_ms = now;

            /* Close any open gap before sealing, so the bundle's last word
             * about GNSS is accurate. */
            close_gnss_gap(lc, CAIRN_GAP_NO_FIX);
            emit_trip_event(lc, CAIRN_EVENT_TRIP_END, nullptr);
            seal_and_reopen(lc, 0);
        }
        break;
    }

    /* The store may demand a seal regardless of the trip state — the member
     * budget is exhausted, or a segment could not be made appendable. */
    if (lc->cap.needs_seal && lc->bundle == BundleState::Open) {
        CAIRN_LOGW(TAG, "store requires a seal; sealing mid-trip");
        seal_and_reopen(lc, 1);
    }

    /*
     * Re-initialising a driver touches state the sensing task owns, so the
     * controller asks rather than doing it here.
     */
    static uint32_t next_retry_ms = 0;
    if ((int32_t)(now - next_retry_ms) >= 0) {
        next_retry_ms = now + 60000;
        if (!lc->sensors.gnss || !lc->sensors.obd || !lc->sensors.imu) {
            sensor_task_request_retry();
        }
    }

    maybe_sync(lc);

    /*
     * Last, so a sync has already had its chance this pass. Standing by before
     * syncing would strand unreceipted bundles for the length of the standby.
     */
    maybe_standby(lc);

    cairn_log_tick();
    cairn_capture_tick(&lc->cap);
}
