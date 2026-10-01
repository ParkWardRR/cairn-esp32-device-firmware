#include "lifecycle.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include "board_config.h"
#include "cairn_log.h"
#include "cairn_fs.h"
#include "cairn_sync.h"
#include "config.h"
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

const char *health_state_name(HealthState s)
{
    switch (s) {
    case HealthState::Ok:       return "OK";
    case HealthState::Degraded: return "DEGRADED";
    case HealthState::Critical: return "CRITICAL";
    default:                    return "?";
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
    if (lc->health != HealthState::Ok)        flags |= CAIRN_FLAG_DEGRADED;
    if (lc->cap.recovery_state != CAIRN_RECOVERY_CLEAN) {
        flags |= CAIRN_FLAG_POST_RECOVERY;
    }
    if (!lc->have_utc_basis) flags |= CAIRN_FLAG_ESTIMATED_UTC;

    return flags;
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

static void set_health_state(Lifecycle *lc, HealthState next, uint8_t reason)
{
    if (lc->health == next) return;

    CAIRN_LOGW(TAG, "health %s -> %s (reason %u)", health_state_name(lc->health),
               health_state_name(next), reason);
    emit_transition(lc, CAIRN_REGION_HEALTH, (uint8_t)lc->health, (uint8_t)next,
                    0, reason);
    lc->health = next;
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

static void on_health(Lifecycle *lc, const fact_t *f)
{
    cairn_device_health_t h = f->data.health;

    /* The sensing task cannot know these; the controller owns them. */
    h.health_state = (uint8_t)lc->health;
    h.reboot_count = (uint8_t)lc->boot_count;

    int rssi = cairn_sync_rssi();
    h.rssi_dbm = (rssi != 0) ? (int8_t)rssi : CAIRN_I8_UNKNOWN;

    uint64_t total = 0, used = 0;
    uint64_t free_mib = 0;
    if (cairn_fs_space(&total, &used)) {
        free_mib = (total > used) ? (total - used) / (1024 * 1024) : 0;
        h.sd_free_mib = (uint16_t)((free_mib > 0xFFFE) ? 0xFFFE : free_mib);
    }

    /*
     * Dropped facts are reported in the data, not just the log. A queue that
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

    uint8_t payload[16];
    cairn_encode_device_health(&h, payload);

    /* Health belongs to the journal chain: it is true of the device, not of the
     * drive, and must be recordable while no trip is in progress. */
    cairn_capture_append(&lc->cap, CAIRN_CHAIN_JOURNAL, CAIRN_REC_DEVICE_HEALTH,
                         1, frame_flags(lc), f->monotonic_ms, payload,
                         sizeof(payload));

    /* Degradation is derived from what is actually missing or failing. */
    HealthState want = HealthState::Ok;
    uint8_t reason = 0;

    if (h.battery_mv != CAIRN_U16_UNKNOWN && h.battery_mv < 11500) {
        want = HealthState::Critical;
        reason = 1;
    } else if (free_mib > 0 && free_mib < CAIRN_LOG_FREE_SPACE_FLOOR_MIB) {
        want = HealthState::Degraded;
        reason = 2;
    } else if (!lc->sensors.gnss || !lc->sensors.imu) {
        want = HealthState::Degraded;
        reason = 3;
    } else if (lc->cap.write_errors > 0) {
        want = HealthState::Degraded;
        reason = 4;
    } else if (dropped > 0) {
        want = HealthState::Degraded;
        reason = 5;
    }

    set_health_state(lc, want, reason);

    CAIRN_LOGD(TAG, "health: %u mV, %llu MiB free, %u write errors, %u dropped "
                    "facts, state %s",
               (unsigned)h.battery_mv, (unsigned long long)free_mib,
               (unsigned)lc->cap.write_errors, (unsigned)dropped,
               health_state_name(lc->health));
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

    emit_transition(lc, CAIRN_REGION_BUNDLE, (uint8_t)BundleState::Sealed,
                    (uint8_t)BundleState::Open, 0, 0);
}

/*
 * Sync only while idle. Uploading during a drive competes with capture for both
 * the CPU and the SPI bus the card is on, and nothing about this data is
 * time-critical.
 */
static void maybe_sync(Lifecycle *lc)
{
    if (lc->capture != CaptureState::Idle) return;
    if (millis() - lc->idle_since_ms < CAIRN_SYNC_MIN_IDLE_MS) return;

    uint32_t pending = 0;
    uint64_t bytes = 0;
    if (!cairn_store_pending_stats(&pending, &bytes) || pending == 0) return;

    CAIRN_LOGI(TAG, "%u bundle(s) pending, %llu bytes; attempting sync",
               (unsigned)pending, (unsigned long long)bytes);

    set_link_state(lc, LinkState::Associating, 0);

    if (!cairn_sync_connect(CAIRN_SYNC_CONNECT_TIMEOUT_MS)) {
        set_link_state(lc, LinkState::Offline, 1);
        /* Offline is normal, not a fault: the whole design is offline-first. */
        lc->idle_since_ms = millis();
        return;
    }

    set_link_state(lc, LinkState::Syncing, 0);

    cairn_sync_stats_t stats;
    cairn_sync_result_t r = cairn_sync_run(&stats);

    CAIRN_LOGI(TAG, "sync result %s", cairn_sync_result_name(r));

    cairn_sync_disconnect();
    set_link_state(lc, LinkState::Offline, 0);

    /* Flush the log so the sync outcome is on the card even if power is cut
     * immediately afterwards. */
    cairn_log_flush();

    lc->idle_since_ms = millis();
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
             */
            set_capture_state(lc, CaptureState::Idle, 2, 0);
            lc->idle_since_ms = now;
        } else if (now - lc->motion_since_ms >= CAIRN_START_DWELL_MS) {
            /*
             * Confirmed. The state is set first so the flushed frames are
             * written through rather than pushed straight back into the ring,
             * and so their PRETRIP flag is the only thing marking them apart.
             */
            set_capture_state(lc, CaptureState::Active, 1, 0);
            cairn_preroll_flush(&lc->preroll, &lc->cap);
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
    cairn_log_tick();
    cairn_capture_tick(&lc->cap);
}
