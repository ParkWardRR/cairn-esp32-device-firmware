#include "lifecycle.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include "board_config.h"
#include "boot_timing.h"
#include "device_info.h"
#include "cairn_log.h"
#include "ble_companion.h"
#include "ble_offload.h"
#include "cairn_engine.h"
#include "cairn_fs.h"
#include "cairn_kv.h"
#include "cairn_power.h"
#include "cairn_prune.h"
#include "config.h"
#include "garage_store.h"
#include "gnss_assist.h"
#include "policy.h"
#include "cairn_trip.h"
#include "preroll.h"
#include "sensor_task.h"
#include "sensors.h"
#include "uplink_runner.h"

static const char *TAG = "LIFE";

static void close_gnss_gap(Lifecycle *lc, uint8_t cause);
static void seal_and_reopen(Lifecycle *lc, uint8_t reason);

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
                                size_t payload_len, uint32_t sampled_ms)
{
    uint16_t flags = frame_flags(lc);
    /*
     * The frame carries the moment the sensor was read, not the moment this task got
     * round to writing it. They used to be the same call to millis() here, which is
     * right only while the queue is empty: during a seal (~2.5 s) facts back up, and on
     * the 2026-10-14 drive every new bundle opened with up to eight GNSS fixes -- 30 m
     * of road -- stamped with one shared time. A TIME_OBSERVATION was skewed the same
     * way, its utc_ms paired with a frame time later than the instant it was read.
     * 0 means the producer did not say, and only then is the write time used.
     */
    uint32_t now = sampled_ms != 0 ? sampled_ms : millis();

    /* Pre-roll or capture, the sample has entered the pipeline: that is T_capture. */
    boot_timing_mark(CAIRN_BOOT_FIRST_SAMPLE);

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

/*
 * Whether a drive is in progress, for the BLE offload, which runs on its own task.
 * Reading one byte of state from another task is benign here: the worst a stale
 * read does is serve or refuse one request a moment early. Any capture state but
 * Idle counts, including a suspected start and the trailing dwell, because offload
 * competes with capture for the card.
 */
static const Lifecycle *s_offload_lc = nullptr;

static bool offload_trip_active(void)
{
    return s_offload_lc == nullptr || s_offload_lc->capture != CaptureState::Idle;
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


/* ── the garage: which car this is ─────────────────────────────────────────── */

/*
 * At boot the device assumes the car it was last in: that car's assignment is the
 * one in `storage`, and its engine profile is selected here, before the sensing task
 * starts, so the trip thresholds and the OBD table are that car's from the first tick.
 */
static void garage_begin(Lifecycle *lc)
{
    lc->have_garage = garage_load(&lc->garage);
    if (!lc->have_garage) return;
    int i = cairn_garage_find_assignment(&lc->garage, lc->storage.assignment_id);
    if (i < 0) {
        CAIRN_LOGW(TAG, "garage: the installed assignment is not one of its cars; "
                        "the first VIN read will choose");
        return;
    }
    const cairn_garage_slot_t *s = &lc->garage.slot[i];
    if (s->engine_id[0] != '\0' && cairn_engine_select(s->engine_id)) {
        CAIRN_LOGI(TAG, "garage: assuming slot %d (engine %s) until the ECU gives its VIN", i,
                   s->engine_id);
    } else {
        CAIRN_LOGW(TAG, "garage: slot %d names engine \"%s\", which this build does not carry",
                   i, s->engine_id);
    }
}

/*
 * The VIN arrives when the ECU first answers (sensors_retry_failed, on the sensing
 * task). If it is another car of the garage, bundles from now on are that car's: the
 * assignment is switched and the open bundle sealed, so no bundle names two cars.
 * The ones sealed earlier in this boot carry the car assumed at boot; a boot is one
 * car (unplugging powers the dongle off), and the server files the whole boot under
 * the car identified last.
 */
static void garage_check(Lifecycle *lc)
{
    if (!lc->have_garage) return;

    char vin[sizeof(lc->sensors.vin)];
    sensors_link_lock(); /* the sensing task writes it under the link lock */
    memcpy(vin, lc->sensors.vin, sizeof(vin));
    sensors_link_unlock();
    vin[sizeof(vin) - 1] = '\0';
    if (strlen(vin) != 17 || strcmp(vin, lc->vin_seen) == 0) return;
    memcpy(lc->vin_seen, vin, sizeof(lc->vin_seen));

    uint8_t h[CAIRN_GARAGE_VIN_HASH];
    cairn_garage_vin_hash(vin, h);
    const cairn_engine_catalogue_entry_t *hit =
        cairn_engine_match_code(cairn_engine_catalogue(), cairn_engine_catalogue_count(), vin);
    cairn_garage_how_t how;
    int i = cairn_garage_pick(&lc->garage, h, hit != nullptr ? hit->id : nullptr, &how);
    if (i < 0) {
        CAIRN_LOGW(TAG, "garage: VIN read, but %s (engine %s): staying with the car assumed",
                   cairn_garage_how_name(how), hit != nullptr ? hit->id : "unknown");
        return;
    }
    cairn_garage_slot_t *s = &lc->garage.slot[i];
    if (how == CAIRN_GARAGE_BY_ENGINE) {
        s->has_vin = true;
        memcpy(s->vin_hash, h, sizeof(h));
        if (!garage_save(&lc->garage)) {
            CAIRN_LOGW(TAG, "garage: could not store the VIN learned for slot %d", i);
        }
    }
    if (memcmp(s->assignment_id, lc->storage.assignment_id, 16) == 0) {
        CAIRN_LOGI(TAG, "garage: slot %d confirmed %s", i, cairn_garage_how_name(how));
        return;
    }
    if (!cairn_storage_set_assignment(s->vehicle_id, s->assignment_id)) {
        CAIRN_LOGE(TAG, "garage: slot %d identified, but its assignment could not be stored", i);
        return;
    }
    memcpy(lc->storage.vehicle_id, s->vehicle_id, 16);
    memcpy(lc->storage.assignment_id, s->assignment_id, 16);
    lc->storage.assigned = true;
    CAIRN_LOGW(TAG, "garage: this is slot %d, identified %s: switching assignment and "
                    "sealing the open bundle", i, cairn_garage_how_name(how));
    if (lc->bundle == BundleState::Open) seal_and_reopen(lc, 4);
}

/* ── init ─────────────────────────────────────────────────────────────────── */

bool lifecycle_begin(Lifecycle *lc)
{
    if (!cairn_identity_load(lc->device_id, lc->key_seed, lc->key_public,
                             &lc->boot_count)) {
        return false;
    }

    /*
     * The storage root every frame is sealed under. Without one nothing can be
     * written that anyone could later decrypt, so failing to load or persist it
     * is as fatal as failing to load the signing key. A missing *assignment* is
     * not fatal: capture continues, bound to zero ids, and the loader says so.
     */
    if (!cairn_storage_identity_load(&lc->storage)) {
        CAIRN_LOGE(TAG, "no storage key; refusing to capture data nobody could read");
        return false;
    }
    device_info_set_identity(lc->device_id, lc->key_public, lc->storage.storage_key_version,
                             lc->storage.assigned);
    cairn_new_boot_id(lc->boot_id);
    cairn_log_set_context(lc->boot_id);

    if (!sensors_begin(&lc->sensors)) {
        CAIRN_LOGE(TAG, "coprocessor unavailable; capture will be degraded");
    }
    garage_begin(lc);
    boot_timing_mark(CAIRN_BOOT_SENSORS_READY);

    /*
     * Order matters at boot. Interrupted seals are finished first so a bundle
     * that was already sealed is not reopened as a capture; interrupted prunes
     * come next so the card's free space reflects receipts already verified;
     * only then is a capture opened or resumed.
     */
    cairn_store_resume_interrupted_seals();
    cairn_prune_resume_interrupted();
    boot_timing_mark(CAIRN_BOOT_STORE_READY);

    if (!cairn_capture_open_or_resume(&lc->cap, lc->device_id, lc->boot_id,
                                      &lc->storage)) {
        CAIRN_LOGE(TAG, "cannot open a capture bundle");
        return false;
    }

    boot_timing_mark(CAIRN_BOOT_CAPTURE_OPEN);

    cairn_policy_defaults(&lc->policy);
    cairn_preroll_reset(&lc->preroll);

    lc->trip_seq = cairn_kv_get_u32("trip_seq", 0);

    /*
     * A capture left open by an earlier boot is sealed now, not extended. (After
     * trip_seq is read: the seal stamps it into the manifest.)
     *
     * Its frames are on that boot's monotonic clock and its basis (utcbasis.bin) is UTC
     * at that clock's zero. This boot's clock started again at zero, so a frame written
     * now into the same bundle would be dated against the wrong origin -- or, when this
     * boot's first time observation is adopted, every earlier frame would be. Measured:
     * the 2026-10-14 drive resumed the capture the 2026-10-13 drive left open, and the
     * bundle came out holding the earlier boot's 7 h of parked status records dated a
     * day late, under the earlier boot's id, which then named the wrong boot for the
     * later trip. One bundle, one clock: seal it as it is and start this boot afresh.
     */
    if (lc->cap.active && memcmp(lc->cap.boot_id, lc->boot_id, 16) != 0) {
        CAIRN_LOGW(TAG, "resumed capture belongs to an earlier boot; sealing it before capturing");
        lc->bundle = BundleState::Open;
        seal_and_reopen(lc, 2);
    }

    cairn_trip_init(&lc->trip, cairn_engine_params()->engine_on_mv);
    if (cairn_kv_get_u32("trip_active", 0)) {
        lc->capture = CaptureState::Active;
        cairn_trip_force(&lc->trip, CAIRN_TRIP_ACTIVE, millis());
        emit_policy_snapshot(lc);
        emit_trip_event(lc, CAIRN_EVENT_TRIP_START, "resumed");
        CAIRN_LOGI(TAG, "trip %u resumed across reboot", (unsigned)lc->trip_seq);
    }

    /*
     * Sensing starts only after the capture is open. A fact arriving before
     * there is somewhere to put it would have to be dropped, and dropping the
     * first seconds of a boot is exactly what the pre-roll exists to prevent.
     */
    /* The stored orbits and receiver identity, before the sensing task starts
     * injecting them. */
    gnss_assist_begin();

    if (!sensor_task_start(&lc->sensors)) {
        CAIRN_LOGE(TAG, "sensing task failed to start; no data will be captured");
        return false;
    }

    uint32_t now = millis();
    lc->idle_since_ms  = now;

    /* The boot itself is a journal event, including why the device woke. */
    emit_transition(lc, CAIRN_REGION_BUNDLE, 0, (uint8_t)BundleState::Open, 0,
                    lc->cap.recovery_state);

    /* Before the radio starts: the offload refuses transfers while a drive is in
     * progress, and must never be able to answer that question before it can. */
    s_offload_lc = lc;
    ble_offload_set_trip_source(offload_trip_active);

    if (!ble_companion_begin()) {
        CAIRN_LOGW(TAG, "BLE companion init failed; phone GPS unavailable");
    } else {
        boot_timing_mark(CAIRN_BOOT_BLE_ADVERTISING);
    }

    CAIRN_LOGI(TAG, "lifecycle up: boot %u, wake cause %d, sensors obd=%d imu=%d gnss=%d",
               (unsigned)lc->boot_count, (int)esp_sleep_get_wakeup_cause(),
               (int)lc->sensors.obd, (int)lc->sensors.imu, (int)lc->sensors.gnss);

    uplink_runner_begin();

    return true;
}

/* ── supply rail and trip detection ───────────────────────────────────────── */

/*
 * Take the newest supply reading from the sensing task, which owns the co-processor
 * link (the controller used to read it here, on the same link, from another task; see
 * sensors_link_lock()). Bench detection and the ignition-off flush stay here because
 * they act on the capture. Returns true and the vehicle-range reading when there is a
 * new sample since the last call.
 */
static bool read_supply(Lifecycle *lc, uint16_t *fresh_mv)
{
    uint16_t batt = CAIRN_U16_UNKNOWN, raw = 0;
    uint32_t seq = sensor_task_supply(&batt, &raw, nullptr);
    if (seq == 0 || seq == lc->supply_seq) return false;
    lc->supply_seq = seq;
    lc->last_battery_mv = batt;
    *fresh_mv = batt;

    lc->prev_supply_mv_raw = lc->last_supply_mv_raw;
    lc->last_supply_mv_raw = raw;

    uint16_t engine_on = cairn_engine_params()->engine_on_mv;
    if (lc->prev_supply_mv_raw >= engine_on &&
        lc->last_supply_mv_raw > 0 &&
        lc->last_supply_mv_raw < engine_on &&
        lc->capture != CaptureState::Idle) {
        CAIRN_LOGW(TAG, "ignition off (supply %u -> %u mV); forcing flush",
                   (unsigned)lc->prev_supply_mv_raw, (unsigned)lc->last_supply_mv_raw);
        lc->cap.last_flush_ms = 0;
    }

    /* Bench-mode auto-detect from the raw co-processor voltage. USB-only power leaves
     * OBD-II pin 16 floating; in the car with ignition off the pin is on unswitched
     * battery (~12.4 V). The bench range is well below any car reading, so a few
     * consecutive hits is a safe latch. One-way: once bench, stays bench until reset,
     * so plugging into a car later does not silently drop into standby mid-development. */
    if (!lc->bench_mode) {
        bool in_bench_range = (raw >= 3000 && raw <= 7500);
        if (in_bench_range) {
            if (lc->bench_hits < 3) lc->bench_hits++;
            if (lc->bench_hits >= 3) {
                lc->bench_mode = true;
                CAIRN_LOGW(TAG, "bench mode: USB power, raw supply %u mV, no OBD rail; "
                                "radio and MCU will stay up until reset", (unsigned)raw);
                /* A trip resumed from NVS or triggered by boot vibration is not a real
                 * drive on a bench. End it so the uplink schedule can reach Idle and
                 * upload pending bundles over Wi-Fi. */
                if (lc->capture != CaptureState::Idle) {
                    CAIRN_LOGW(TAG, "bench mode: ending capture so bundles can upload");
                    if (lc->capture == CaptureState::Active ||
                        lc->capture == CaptureState::Trailing) {
                        close_gnss_gap(lc, CAIRN_GAP_NO_FIX);
                        emit_trip_event(lc, CAIRN_EVENT_TRIP_END, "bench");
                        cairn_kv_set_u32("trip_active", 0);
                    }
                    set_capture_state(lc, CaptureState::Idle, 2, 0);
                    cairn_trip_force(&lc->trip, CAIRN_TRIP_IDLE, millis());
                    lc->idle_since_ms = millis();
                    seal_and_reopen(lc, 0);
                }
            }
        } else if (raw > 0) {
            lc->bench_hits = 0;
        }
    }
    return true;
}

/* Feed the trip detector (lib/cairn_trip) one tick of evidence. */
static cairn_trip_event_t trip_step(Lifecycle *lc)
{
    cairn_trip_input_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = millis();
    uint16_t mv = CAIRN_U16_UNKNOWN;
    in.rail_fresh = read_supply(lc, &mv);
    in.rail_mv = (mv == CAIRN_U16_UNKNOWN) ? CAIRN_TRIP_UNKNOWN_MV : mv;
    in.accel_rms_mg = lc->last_accel_rms_mg;
    in.obd_valid = lc->have_recent_obd && lc->last_obd.speed_kph != CAIRN_I16_UNKNOWN;
    in.obd_kph = in.obd_valid ? lc->last_obd.speed_kph : 0;
    in.gnss_valid = lc->have_recent_gnss && lc->last_gnss.fix_type >= 2 &&
                    lc->last_gnss.speed_cmps != CAIRN_U16_UNKNOWN;
    in.gnss_cmps = in.gnss_valid ? lc->last_gnss.speed_cmps : 0;
    in.bench = lc->bench_mode;

    cairn_trip_event_t e = cairn_trip_step(&lc->trip, &in);
    lc->start_score_e2 = lc->trip.start_score_e2;
    lc->stop_score_e2  = lc->trip.stop_score_e2;
    return e;
}

/*
 * Enforce the parked-silence invariant, every tick.
 *
 * Whether a drive has started is decided by lib/cairn_trip from evidence the vehicle
 * network cannot see -- the supply rail and the accelerometer -- and latched for the
 * life of a trip. This only turns that verdict into permission to transmit. See
 * cairn_power.h for why a parked BMW's bus must stay silent.
 */
static void enforce_bus_silence(Lifecycle *lc)
{
    bool confirmed = lc->trip.drive_confirmed;
    if (confirmed && !lc->drive_confirmed) {
        CAIRN_LOGI(TAG, "drive confirmed from local evidence (rail %s, accel %u mg); opening the bus",
                   cairn_trip_engine_running(&lc->trip, millis()) ? "engine running" : "not confirmed",
                   (unsigned)lc->last_accel_rms_mg);
        /* Ask for the ECU now rather than at the next minute's retry: the first
         * minute of a drive is exactly the data that was going missing. */
        if (!lc->sensors.obd) sensor_task_request_retry();
    }
    lc->drive_confirmed = confirmed;

    cairn_bus_evidence_t be;
    memset(&be, 0, sizeof(be));
    /* A trip, not a candidate: PRETRIP is reached by a door slam on a parked car,
     * and opening the bus for it is exactly the wake-up the invariant forbids. */
    be.trip_active     = (lc->capture == CaptureState::Active ||
                          lc->capture == CaptureState::Trailing);
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
    bool from_phone = (f->data.gnss.source_flags & CAIRN_SOURCE_PHONE) != 0;

    if (from_phone) {
        lc->last_gnss_phone    = f->data.gnss;
        lc->phone_gnss_last_ms = f->monotonic_ms;
        lc->phone_gnss_active  = true;
        lc->last_gnss          = f->data.gnss;
    } else {
        close_gnss_gap(lc, lc->sensors.gnss ? CAIRN_GAP_NO_FIX
                                            : CAIRN_GAP_POWERED_DOWN);
        if (f->data.gnss.fix_type > 0) boot_timing_mark(CAIRN_BOOT_GNSS_FIRST_FIX);
        lc->last_gnss_internal        = f->data.gnss;
        lc->have_recent_gnss_internal = true;
        lc->internal_gnss_last_ms     = f->monotonic_ms;

        bool phone_fresh = lc->phone_gnss_active &&
                           (millis() - lc->phone_gnss_last_ms <
                            CAIRN_PHONE_GNSS_STALE_MS) &&
                           lc->last_gnss_phone.fix_type > 0;
        if (!phone_fresh)
            lc->last_gnss = f->data.gnss;
    }

    lc->have_recent_gnss = true;

    uint8_t payload[32];
    cairn_encode_gnss_sample(&f->data.gnss, payload);
    emit_capture_record(lc, CAIRN_REC_GNSS_SAMPLE, 1, payload, sizeof(payload), f->monotonic_ms);
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

    lc->have_recent_gnss_internal = false;

    bool phone_fresh = lc->phone_gnss_active &&
                       (millis() - lc->phone_gnss_last_ms <
                        CAIRN_PHONE_GNSS_STALE_MS);
    lc->have_recent_gnss = phone_fresh;
}

static void on_time_observation(Lifecycle *lc, const fact_t *f)
{
    /*
     * Every observation is recorded; the first one also becomes the basis.
     *
     * Those are deliberately separate. The manifest's basis cannot be revised
     * once samples have been written against it, so it stays first-wins -- but
     * that is exactly what left every trip dated 1970 when GNSS was the only
     * source and its fix arrived late or never. The records are the evidence
     * trail: each carries its source and accuracy, and utc_ms minus the frame's
     * monotonic_ms is the basis that source implies, so a consumer can compare
     * them and pick without the device having to guess.
     */
    /*
     * One plausibility floor for every source, here at the point of adoption.
     *
     * Each driver has its own reason to produce a date that parses and is wrong
     * by decades -- see CAIRN_TIME_PLAUSIBLE_FLOOR_MS -- and the GNSS path is the
     * one that actually runs on every drive: its two-digit year means a receiver
     * reporting yy=00 before it has a fix yields a flawless 2000-01-01. Checking
     * once here rather than in three drivers means a new source cannot be added
     * without the guard, and the guard cannot drift between them.
     *
     * Refused rather than recorded-but-not-adopted: an impossible date is noise,
     * not evidence, and leaving it in the bundle invites a consumer to pick it.
     */
    if (f->data.utc.utc_ms < CAIRN_TIME_PLAUSIBLE_FLOOR_MS) {
        CAIRN_LOGW(TAG, "time observation from source %u is %llu ms, before the "
                        "plausible floor; refused",
                   (unsigned)f->data.utc.source,
                   (unsigned long long)f->data.utc.utc_ms);
        return;
    }

    const bool adopt = !lc->have_utc_basis;

    cairn_time_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    obs.utc_ms = f->data.utc.utc_ms;
    obs.acc_ms = f->data.utc.acc_ms;
    obs.source = f->data.utc.source;
    if (adopt) obs.flags |= CAIRN_TIME_OBS_ADOPTED;

    uint8_t payload[16];
    cairn_encode_time_observation(&obs, payload);
    emit_capture_record(lc, CAIRN_REC_TIME_OBSERVATION, 1, payload, sizeof(payload), f->monotonic_ms);

    /* The phone's and the network's clocks also become the receiver's time aiding
     * (src/gnss_assist.cpp); the receiver's own time is of no use to itself. */
    gnss_assist_note_time(f->data.utc.utc_ms, f->data.utc.acc_ms, f->data.utc.source,
                          f->monotonic_ms);

    if (!adopt) return;

    /* The fact carries the monotonic reading of the same instant, which is what
     * turns a sampled wall clock into the basis a reader can add monotonic_ms to. */
    cairn_capture_set_utc_basis(&lc->cap, f->data.utc.utc_ms, f->data.utc.acc_ms,
                                f->monotonic_ms);
    lc->have_utc_basis = true;
    CAIRN_LOGI(TAG, "UTC basis established from source %u: %llu ms at monotonic %u (+/- %u ms)",
               (unsigned)f->data.utc.source,
               (unsigned long long)f->data.utc.utc_ms,
               (unsigned)f->monotonic_ms,
               (unsigned)f->data.utc.acc_ms);
}

static void on_imu_summary(Lifecycle *lc, const fact_t *f)
{
    uint8_t payload[20];
    cairn_encode_imu_summary(&f->data.imu, payload);
    emit_capture_record(lc, CAIRN_REC_IMU_SUMMARY, 1, payload, sizeof(payload), f->monotonic_ms);
}

static void on_obd_snapshot(Lifecycle *lc, const fact_t *f)
{
    lc->last_obd = f->data.obd;
    lc->have_recent_obd = true;
    boot_timing_mark(CAIRN_BOOT_OBD_FIRST_ANSWER);

    uint8_t payload[24];
    cairn_encode_obd_snapshot(&f->data.obd, payload);
    emit_capture_record(lc, CAIRN_REC_OBD_SNAPSHOT, 1, payload, sizeof(payload), f->monotonic_ms);
}

/*
 * The boost and mixture record goes to the capture chain like the basic
 * snapshot, and deliberately does not touch have_recent_obd: that flag feeds
 * the motion score, which reads speed and RPM from OBD_SNAPSHOT. An ECU that
 * answers none of the extended PIDs must not therefore look like an ECU that
 * has gone silent.
 */
static void on_obd_extended(Lifecycle *lc, const fact_t *f)
{
    uint8_t payload[24];
    cairn_encode_obd_extended(&f->data.obd_ext, payload);
    /*
     * schema_version 2: byte 23 carries pedal_pct, where version 1 had a
     * reserved zero.
     *
     * The version is what makes reclaiming that byte safe. A version 1 record's
     * zero there is indistinguishable by value from a genuine 0% pedal, and the
     * obvious discriminator does not work — pids_requested is documented as a
     * bitmap but the firmware has always written a plain count, so it cannot say
     * *which* PIDs were asked for. Per-record schema versioning is exactly the
     * mechanism for this, so a reader keyed on it never has to guess.
     */
    emit_capture_record(lc, CAIRN_REC_OBD_EXTENDED, 2, payload, sizeof(payload), f->monotonic_ms);
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

    if (!lc->sensors.gnss || !lc->have_recent_gnss_internal || lc->in_gnss_gap) {
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

    /* There is no Wi-Fi radio on this device, so there is no signal strength. */
    h.rssi_dbm = CAIRN_I8_UNKNOWN;

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
            /*
             * Report the NMEA counters alongside the satellite count, because
             * a zero satellite count on its own is ambiguous. Climbing
             * sentences with no satellites means the receiver is healthy and
             * simply cannot see sky; a frozen count means it is not talking at
             * all, which is a wiring or antenna-power fault rather than a
             * location problem. A whole drive once recorded 272 GNSS samples
             * and not one fix, and the logs could not tell those apart.
             *
             * Reported as reading freshness rather than NMEA sentence counts.
             * Those counters are only updated on the driver's direct-UART
             * branch, and gpsBegin() puts this board on the co-processor
             * branch, so they read 0/0 whether the receiver is perfect or
             * absent — which looks like evidence and is none.
             */
            uint32_t age_ms = UINT32_MAX;
            uint8_t  raw_sats = 0;
            char link[40];
            if (!sensors_gnss_freshness(&age_ms, &raw_sats)) {
                snprintf(link, sizeof(link), "receiver not initialised");
            } else if (age_ms == UINT32_MAX) {
                snprintf(link, sizeof(link), "no reading ever arrived");
            } else {
                snprintf(link, sizeof(link), "last reading %ums ago",
                         (unsigned)age_ms);
            }

            CAIRN_LOGI(TAG, "sensors: gnss NO FIX (sats=%u, %s) | "
                            "accel rms=%u mg | start=%u stop=%u | obd=%s",
                       (unsigned)g->sats_used, link,
                       (unsigned)lc->last_accel_rms_mg,
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

    /*
     * Stamp the active engine profile into the manifest's firmware_version.
     *
     * Nothing in a bundle said which profile produced its numbers. The profile
     * decides the OBD request, every value conversion, the cadence and the
     * engine-on and dwell thresholds, so a reader holding the data could not tell
     * whether a timing figure came through clamp(A/2-64) or something else — in a
     * format whose whole purpose is being checkable later, that is a hole.
     *
     * It rides in firmware_version rather than a manifest key of its own because
     * key 29 would be a contracts/format/v3 change, and v3 is pinned here at
     * contracts-v0.2.0 (the spec states a manifest has 27 or 28 fields). This is
     * honest in the field it uses: the profile tables are compiled into the image,
     * so they are part of what this firmware *is*. The first-class key is the
     * right end state and is tracked separately.
     *
     * The profile's own sha256 is the stamp rather than just the name, because it
     * pins the formulas and cadences too — a profile edited without a version bump
     * is a different profile, and the digest says so. Six hex digits of it, which
     * is what fits beside the version in 32 bytes.
     */
    char fw[32];
    const cairn_engine_profile_t *prof = cairn_engine_active();
    if (prof != nullptr) {
        /* Manifest key 29. The firmware_version suffix below stays for readers
         * that have not learned the key yet, but this is the one with room for
         * the full digest and no risk of being dropped to fit. */
        cairn_capture_set_engine_profile(&lc->cap, prof->id, (uint8_t)prof->version,
                                         prof->sha256);
        int n = snprintf(fw, sizeof(fw), "%s+%s@%02x%02x%02x",
                         CAIRN_FIRMWARE_VERSION, prof->id,
                         prof->sha256[0], prof->sha256[1], prof->sha256[2]);
        if (n < 0 || (size_t)n >= sizeof(fw)) {
            /* Truncated: the stamp would be a lie, so record the version alone.
             * Loud, because silently losing provenance is the failure this guards. */
            CAIRN_LOGW(TAG, "engine stamp does not fit in firmware_version; "
                            "sealing without it");
            snprintf(fw, sizeof(fw), "%s", CAIRN_FIRMWARE_VERSION);
        }
    } else {
        snprintf(fw, sizeof(fw), "%s", CAIRN_FIRMWARE_VERSION);
    }

    uint8_t sealed_id[16];
    bool ok = cairn_capture_seal(&lc->cap, lc->key_seed, lc->key_public,
                                 fw, CAIRN_POLICY_VERSION,
                                 lc->trip_seq, sealed_id);

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

    /* The basis is UTC at monotonic zero, and the monotonic clock is this boot's, so
     * the one the sealed capture used is just as true for the next. It used to be
     * dropped here: the new capture opened with no basis and got one only when the
     * next time observation arrived, which GNSS repeats every two minutes. Any bundle
     * sealed before then was signed with utc_basis_ms = 0 and decoded to 1970 --
     * measured on the 2026-10-14 evening drive, 12 of 37 bundles in one boot, which
     * the server then left out of the trip as straight-line gaps in the track. */
    const bool     carry_basis = lc->have_utc_basis && lc->cap.utc_basis_ms != 0;
    const uint64_t carried_ms  = lc->cap.utc_basis_ms;
    const uint32_t carried_acc = lc->cap.utc_basis_acc_ms;

    /* A fresh capture, with its own bundle id and its own chains. */
    if (!cairn_capture_open_or_resume(&lc->cap, lc->device_id, lc->boot_id,
                                      &lc->storage)) {
        CAIRN_LOGE(TAG, "cannot open a new capture after sealing");
        return;
    }
    lc->bundle = BundleState::Open;
    lc->have_utc_basis = false;
    if (carry_basis && lc->cap.utc_basis_ms == 0) {
        /* Sampled at monotonic zero, so set_utc_basis stores carried_ms unchanged. */
        cairn_capture_set_utc_basis(&lc->cap, carried_ms, carried_acc, 0);
        lc->have_utc_basis = true;
        CAIRN_LOGI(TAG, "UTC basis carried into the new capture: %llu ms (+/- %u ms)",
                   (unsigned long long)carried_ms, (unsigned)carried_acc);
    }
    lc->policy_written = false; /* a new bundle must describe its own policy */

    emit_transition(lc, CAIRN_REGION_BUNDLE, (uint8_t)BundleState::Sealed,
                    (uint8_t)BundleState::Open, 0, 0);
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
    e.battery_mv = lc->last_battery_mv;
    e.pending_bundles = lc->pending_bundles;
    /* A phone working the offload counts as a live link: standing by switches the
     * radio off, which would drop a transfer in progress and strand the bundles it
     * was carrying. Bounded in ble_offload_active(). */
    e.link_online = (lc->link != LinkState::Offline) || ble_offload_active();
    e.bench_mode = lc->bench_mode;

    /*
     * An open capture holding data is a reason to seal, not a reason to stay
     * awake forever — so seal it and re-evaluate on the next pass rather than
     * sleeping with it open.
     */
    if (!e.trip_active && e.capture_open &&
        e.idle_ms >= cairn_engine_params()->standby_idle_ms) {
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
        /*
         * A transfer in progress holds standby by itself. The power module
         * knows nothing about the uplink, and sleeping mid-upload would drop a
         * TLS session and an open offer for no gain -- the slot is already
         * bounded, so waiting for it costs at most that bound.
         */
        if (uplink_runner_busy()) {
            static const char *const k_uplinking = "an uplink transfer is in progress";
            if (lc->last_standby_blocker != k_uplinking) {
                CAIRN_LOGI(TAG, "not standing by: %s", k_uplinking);
                lc->last_standby_blocker = k_uplinking;
            }
            return;
        }

        const char *why = cairn_power_standby_blocker(&e);
        if (why != nullptr && why != lc->last_standby_blocker) {
            CAIRN_LOGI(TAG, "not standing by: %s", why);
            lc->last_standby_blocker = why;
        }
        return;
    }

    lc->last_standby_blocker = nullptr;

    ble_companion_radio_off();

    emit_transition(lc, CAIRN_REGION_HEALTH, CAIRN_POWER_STATE_AWAKE,
                    CAIRN_POWER_STATE_STANDBY, CAIRN_TRIGGER_POWER, 0);
    cairn_log_flush();

    if (lc->have_recent_gnss)
        sensors_gnss_save_aiding(&lc->last_gnss);

    cairn_power_result_t r;
    cairn_power_standby(&r);

    lc->total_standby_ms += r.standby_ms;
    lc->standby_count++;
    gnss_assist_note_standby(r.standby_ms);

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
    cairn_trip_force(&lc->trip, CAIRN_TRIP_IDLE, now);
    lc->last_wake = r.wake_reason;

    /* The receiver was powered down, so position is unknown until it reacquires
     * — and the degraded bitmap should say so rather than carry a stale fix. */
    lc->have_recent_gnss = false;
    lc->have_recent_gnss_internal = false;
    lc->phone_gnss_active = false;
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

    ble_companion_radio_on();
}

/*
 * Count what is waiting to leave the device.
 *
 * There is no network on this device. Sealed bundles leave it over BLE, pulled
 * by the enrolled phone app (contracts/ble/v1/offload.md), and a bundle is only deleted
 * from the card once the app hands back a server receipt that verifies against
 * the pinned key. All this stage does is keep the pending count honest while the
 * vehicle is stopped; it starts no radio and moves no data.
 */
static void refresh_pending(Lifecycle *lc)
{
    if (lc->capture != CaptureState::Idle) return;
    if (millis() - lc->idle_since_ms < CAIRN_PENDING_MIN_IDLE_MS) return;
    if ((int32_t)(millis() - lc->next_pending_check_ms) < 0) return;
    lc->next_pending_check_ms = millis() + CAIRN_PENDING_REFRESH_MS;

    uint32_t pending = 0;
    uint64_t bytes = 0;
    if (!cairn_store_pending_stats(&pending, &bytes)) return;

    if (pending != lc->pending_bundles) {
        CAIRN_LOGI(TAG, "%u bundle(s) awaiting hand-off to the app, %llu bytes",
                   (unsigned)pending, (unsigned long long)bytes);
    }
    lc->pending_bundles = pending;
    device_info_set_pending(pending);
}

/* ── tick ─────────────────────────────────────────────────────────────────── */

void lifecycle_tick(Lifecycle *lc)
{
    uint32_t now = millis();

    /* Once, when both T_capture and T_ble are known. */
    {
        static bool logged = false;
        if (!logged) {
            cairn_boottime_t bt;
            boot_timing_snapshot(&bt);
            if (cairn_boottime_reached(&bt, CAIRN_BOOT_FIRST_SAMPLE) &&
                cairn_boottime_reached(&bt, CAIRN_BOOT_BLE_ADVERTISING)) {
                boot_timing_log();
                logged = true;
            }
        }
    }

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
        case FACT_TIME_OBSERVATION: on_time_observation(lc, &f); break;
        case FACT_IMU_SUMMARY:   on_imu_summary(lc, &f); break;
        case FACT_OBD_SNAPSHOT:  on_obd_snapshot(lc, &f); break;
        case FACT_OBD_EXTENDED:  on_obd_extended(lc, &f); break;
        case FACT_OBD_SILENT:    on_obd_silent(lc); break;
        case FACT_HEALTH:        on_health(lc, &f); break;

        case FACT_MOTION:
            lc->last_accel_rms_mg = f.data.motion.accel_rms_mg;
            break;
        }
    }

    garage_check(lc);
    const cairn_trip_event_t trip_event = trip_step(lc);
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
        if (trip_active && ble_companion_phone_charging())
            d = CAIRN_DYN_OVERDRIVE;

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

    /* The decisions are lib/cairn_trip's; this performs what each one means. */
    switch (trip_event) {
    case CAIRN_TRIP_NONE:
        break;

    case CAIRN_TRIP_TO_PRETRIP:
        set_capture_state(lc, CaptureState::Pretrip, 1, 0);
        break;

    case CAIRN_TRIP_TO_IDLE_REJECTED:
        /*
         * The motion did not persist, so nothing is written. The ring keeps filling
         * and evicting while idle, which is what lets a real drive recover its first
         * seconds without a parked car producing bundles. idle_since_ms is
         * deliberately NOT rebased: a rejected candidate is not activity, and
         * rebasing it once kept a parked car out of standby indefinitely.
         */
        set_capture_state(lc, CaptureState::Idle, 2, 0);
        break;

    case CAIRN_TRIP_TO_ACTIVE: {
        /* Confirmed. The state is set first so the flushed frames are written
         * through rather than pushed straight back into the ring. */
        set_capture_state(lc, CaptureState::Active, 1, 0);
        lc->trip_seq++;
        cairn_kv_set_u32("trip_seq", lc->trip_seq);
        cairn_kv_set_u32("trip_active", 1);

        /* Policy first, then the pre-roll: the snapshot describes the thresholds
         * that admitted those very records, so it belongs ahead of them. */
        emit_policy_snapshot(lc);
        cairn_preroll_flush(&lc->preroll, &lc->cap);
        emit_trip_event(lc, CAIRN_EVENT_TRIP_START, nullptr);

        if (lc->cap.recovery_state != CAIRN_RECOVERY_CLEAN) {
            char detail[CAIRN_MAX_EVENT_DETAIL];
            snprintf(detail, sizeof(detail), "state=%u discarded=%u",
                     (unsigned)lc->cap.recovery_state,
                     (unsigned)lc->cap.discarded_tail_bytes);
            emit_trip_event(lc, CAIRN_EVENT_CAPTURE_RECOVERED, detail);
        }
        break;
    }

    case CAIRN_TRIP_TO_TRAILING:
        set_capture_state(lc, CaptureState::Trailing, 3, 0);
        break;

    case CAIRN_TRIP_TO_ACTIVE_RESUMED:
        set_capture_state(lc, CaptureState::Active, 1, 1);
        break;

    case CAIRN_TRIP_TO_IDLE_ENDED:
        set_capture_state(lc, CaptureState::Idle, 2, 0);
        lc->idle_since_ms = now;
        cairn_kv_set_u32("trip_active", 0);
        /* Close any open gap before sealing, so the bundle's last word about GNSS
         * is accurate. */
        close_gnss_gap(lc, CAIRN_GAP_NO_FIX);
        emit_trip_event(lc, CAIRN_EVENT_TRIP_END, nullptr);
        seal_and_reopen(lc, 0);
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

    /* ── BLE companion notifications (1 Hz) ────────────────────────────── */

    static uint32_t next_ble_notify_ms = 0;
    if ((int32_t)(now - next_ble_notify_ms) >= 0) {
        next_ble_notify_ms = now + 1000;

        if (lc->phone_gnss_active) {
            bool stale = (now - lc->phone_gnss_last_ms >= CAIRN_PHONE_GNSS_STALE_MS);
            if (stale || !ble_companion_connected()) {
                lc->phone_gnss_active = false;
                memset(&lc->last_gnss_phone, 0, sizeof(lc->last_gnss_phone));
                if (lc->last_gnss.source_flags & CAIRN_SOURCE_PHONE)
                    lc->last_gnss = lc->last_gnss_internal;
                if (!lc->have_recent_gnss_internal)
                    lc->have_recent_gnss = false;
            }
        }

        if (ble_companion_connected()) {
            uint32_t fix_age = lc->have_recent_gnss_internal
                ? (now - lc->internal_gnss_last_ms)
                : UINT32_MAX;
            ble_companion_notify_quality(
                lc->last_gnss_internal.fix_type,
                lc->last_gnss_internal.sats_used,
                lc->last_gnss_internal.hdop_e2,
                fix_age);
            ble_companion_notify_status();
        }
    }

    refresh_pending(lc);

    /*
     * After refresh_pending, so the schedule sees the current count rather than
     * last minute's, and before maybe_standby, so a slot that is about to run
     * is not raced by the device deciding to sleep.
     */
    gnss_assist_controller_tick();
    uplink_runner_tick(lc);

    /*
     * Last, so a sync has already had its chance this pass. Standing by before
     * syncing would strand unreceipted bundles for the length of the standby.
     */
    maybe_standby(lc);

    cairn_log_tick();
    cairn_capture_tick(&lc->cap);
}
