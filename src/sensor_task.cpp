#include "sensor_task.h"

#include <Arduino.h>

#include "cairn_engine.h"
#include "cairn_gnss.h"
#include "cairn_log.h"
#include "config.h"
#include "gnss_assist.h"
#include "mtprobe.h"

static const char *TAG = "SENSE";

/*
 * Every GNSS fix passes the plausibility gate (lib/cairn_gnss) before it becomes a
 * fact, so a position the car's own sensors contradict is never written. Refusals are
 * counted per rule and logged at most every 10 s.
 */
static cairn_gnss_gate_t s_gnss_gate;
static uint32_t          s_gnss_refusal_log_ms;

static bool vet_gnss(const fact_t *f, cairn_gnss_context_t *c, uint32_t obd_ms)
{
    /* The OBD reading's age at the moment of this fix; a reading taken after the fix
     * (a 5 Hz fix from earlier in the buffer) counts as fresh. */
    int32_t age = (int32_t)(f->monotonic_ms - obd_ms);
    c->obd_age_ms = age > 0 ? (uint32_t)age : 0;

    cairn_gnss_verdict_t v = cairn_gnss_gate(&s_gnss_gate, &f->data.gnss, f->monotonic_ms, c);
    if (v == CAIRN_GNSS_ACCEPT) return true;

    uint32_t now = millis();
    if ((int32_t)(now - s_gnss_refusal_log_ms) >= 10000) {
        s_gnss_refusal_log_ms = now;
        CAIRN_LOGW(TAG, "GNSS fix refused (%s): %u cm/s on %u sats, obd %d kph %u ms old, imu %u mg; "
                        "refused so far: settling %u obd %u still %u accel %u few-sats %u",
                   cairn_gnss_verdict_name(v), (unsigned)f->data.gnss.speed_cmps,
                   (unsigned)f->data.gnss.sats_used, c->obd_valid ? (int)c->obd_speed_kph : -1,
                   (unsigned)c->obd_age_ms, (unsigned)c->imu_rms_mg,
                   (unsigned)s_gnss_gate.rejected[CAIRN_GNSS_REJECT_SETTLING],
                   (unsigned)s_gnss_gate.rejected[CAIRN_GNSS_REJECT_OBD],
                   (unsigned)s_gnss_gate.rejected[CAIRN_GNSS_REJECT_STILL],
                   (unsigned)s_gnss_gate.rejected[CAIRN_GNSS_REJECT_ACCEL],
                   (unsigned)s_gnss_gate.rejected[CAIRN_GNSS_REJECT_FEW_SATS]);
    }
    return false;
}

static QueueHandle_t  s_queue;
static SensorStatus  *s_status;
static volatile uint32_t s_dropped;
static volatile bool  s_retry_requested;

/* The supply rail, sampled here because this task owns the co-processor link. Under a
 * spinlock: the ESP32 is 32-bit, and a reader must not see a voltage paired with the
 * wrong time. */
static portMUX_TYPE s_supply_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_supply_batt = CAIRN_U16_UNKNOWN, s_supply_raw;
static uint32_t s_supply_at, s_supply_seq;

/*
 * Consecutive cycles in which the ECU answered nothing, and the shape of the
 * backoff that follows.
 *
 * The grace period exists because a brief silence is normal — a gear change, a
 * busy bus — and the first few misses are cheap now that a dead cycle costs
 * ~700 ms rather than ~9 s. The cap is deliberately low: the backoff is there to
 * stop a silent ECU starving the GNSS poll that shares this loop, not to protect
 * the car, so there is nothing to gain from long intervals and a few seconds of
 * lost OBD on recovery is the whole downside.
 */
#define OBD_SILENT_GRACE_CYCLES   3u
#define OBD_SILENT_MAX_PERIOD_MS  5000u

static uint8_t s_obd_silent_cycles;

/* Current periods, set by the controller. Initialized to the policy defaults so
 * the first pass before any update samples at nominal rather than at zero. */
static volatile uint16_t s_gnss_period_ms = CAIRN_GNSS_PERIOD_MS;
static volatile uint16_t s_imu_window_ms  = CAIRN_IMU_WINDOW_MS;
static volatile uint16_t s_obd_period_ms  = CAIRN_OBD_PERIOD_MS; /* replaced by the active profile in sensor_task_start() */

/*
 * Default silent. A device that boots next to a parked car must not start
 * polling before the controller has decided anything — the safe initial state
 * is quiet, and the controller opens the bus once a drive is confirmed.
 */
static volatile bool s_bus_silent = true;
static volatile bool s_paused = false;

/*
 * 20 ms, giving a 50 Hz accelerometer rate. Ample for RMS and peak over a
 * one-second window, and now independent of whatever the controller is doing.
 */
#define SENSE_PERIOD_MS 20

/* Live motion reports between IMU windows, so trip start is not gated on a
 * whole window completing. */
#define MOTION_PERIOD_MS 200

static void post(const fact_t *f)
{
    /*
     * Never block. The sensing task stalling would defeat its own purpose — a
     * blocked sensor task is exactly the sampling gap this split removes. A
     * full queue means the controller is busy, so the fact is dropped and
     * counted.
     */
    if (xQueueSend(s_queue, f, 0) != pdTRUE) s_dropped++;
}

static void post_kind(fact_kind_t kind, uint32_t now)
{
    fact_t f;
    memset(&f, 0, sizeof(f));
    f.kind = kind;
    f.monotonic_ms = now;
    post(&f);
}

static void sensor_task(void *arg)
{
    (void)arg;

    uint32_t next_gnss   = millis();
    uint32_t next_obd    = millis();
    uint32_t next_imu    = millis() + s_imu_window_ms;
    uint32_t next_health = millis();
    uint32_t next_motion = millis();
    uint32_t next_supply = millis();
    uint32_t next_retry  = millis() + 60000;

    bool     have_utc_basis = false;
    uint32_t last_time_obs_ms = 0;

    /* Gap reporting state; see the GNSS branch below. */
    uint32_t last_gap_report_ms = millis();
    bool     have_reported_gap  = false;
    uint32_t last_accepted_fix_ms = millis();

    /* The last OBD road speed, for judging GNSS fixes against the wheels. */
    bool     have_obd_speed = false;
    int16_t  last_obd_kph   = 0;
    uint32_t last_obd_ms    = 0;

    for (;;) {
        /* Parked while the controller is in standby. Nothing is draining the
         * queue, so sampling would only fill it and count the overflow as
         * dropped facts. */
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint32_t now = millis();

        /* Highest rate first, and unconditionally: this is the sample stream
         * the window statistics are computed from. */
        sensors_imu_accumulate();

        if ((int32_t)(now - next_supply) >= 0) {
            next_supply = now + CAIRN_BATTERY_POLL_MS;
            uint16_t batt = CAIRN_U16_UNKNOWN, raw = 0;
            sensors_read_supply(&batt, &raw);
            portENTER_CRITICAL(&s_supply_lock);
            s_supply_batt = batt; s_supply_raw = raw; s_supply_at = now; s_supply_seq++;
            portEXIT_CRITICAL(&s_supply_lock);
        }

        if ((int32_t)(now - next_motion) >= 0) {
            next_motion = now + MOTION_PERIOD_MS;

            fact_t f;
            memset(&f, 0, sizeof(f));
            f.kind = FACT_MOTION;
            f.monotonic_ms = now;
            f.data.motion.accel_rms_mg = sensors_recent_accel_rms_mg();
            post(&f);
        }

        if ((int32_t)(now - next_imu) >= 0) {
            uint16_t window = s_imu_window_ms;
            next_imu = now + window;

            fact_t f;
            memset(&f, 0, sizeof(f));
            f.kind = FACT_IMU_SUMMARY;
            f.monotonic_ms = now;
            /* The window length is passed through so the summary records the
             * period it actually covers, not the one configured at boot. */
            if (sensors_imu_summarize(window, &f.data.imu)) post(&f);
        }

        if ((int32_t)(now - next_gnss) >= 0) {
            next_gnss = now + s_gnss_period_ms;

            /* What the rest of the car says now, for the plausibility gate. OBD is
             * read further down this loop, so this is the previous cycle's reading
             * with its true age. */
            cairn_gnss_context_t gctx;
            memset(&gctx, 0, sizeof(gctx));
            gctx.obd_valid     = have_obd_speed;
            gctx.obd_speed_kph = last_obd_kph;
            gctx.imu_valid     = s_status != nullptr && s_status->imu &&
                                 sensors_recent_accel_rms_mg() != 0;
            gctx.imu_rms_mg    = sensors_recent_accel_rms_mg();

            /*
             * ATGPS first: it is the receiver's parsed summary -- speed, heading, the
             * date for UTC observations -- and its time of day is the reference the
             * NMEA buffer is judged against.
             */
            fact_t f;
            memset(&f, 0, sizeof(f));
            f.monotonic_ms = now;
            uint32_t fix_age = 0;
            const bool have_summary = sensors_read_gnss(&f.data.gnss, &fix_age);

            /*
             * Then ATGRR for the 5 Hz fixes the summary does not carry. Each comes
             * back with its own age from its NMEA time of day, so it is stamped when
             * the receiver took it, not when this loop polled.
             */
            cairn_nmea_fix_t nmea_buf[8];
            int nmea_count = sensors_read_gnss_nmea(nmea_buf, 8);

            int accepted = 0;
            for (int i = 0; i < nmea_count; i++) {
                fact_t nf;
                memset(&nf, 0, sizeof(nf));
                nf.kind = FACT_GNSS_SAMPLE;
                nf.monotonic_ms = now - nmea_buf[i].age_ms;
                nf.data.gnss = nmea_buf[i].sample;
                if (vet_gnss(&nf, &gctx, last_obd_ms)) { post(&nf); accepted++; }
            }

            if (have_summary) {
                if (nmea_count == 0) {
                    f.kind = FACT_GNSS_SAMPLE;
                    if (vet_gnss(&f, &gctx, last_obd_ms)) { post(&f); accepted++; }
                }

                /* Time observations do not depend on the position being plausible:
                 * a receiver with a poor solution still has the time. */
                if (!have_utc_basis ||
                    (int32_t)(now - last_time_obs_ms) >= CAIRN_TIME_OBS_PERIOD_MS) {
                    fact_t u;
                    memset(&u, 0, sizeof(u));
                    u.kind = FACT_TIME_OBSERVATION;
                    u.monotonic_ms = now;
                    u.data.utc.source = CAIRN_TIME_SRC_GNSS;
                    if (sensors_gnss_utc(&u.data.utc.utc_ms, &u.data.utc.acc_ms)) {
                        have_utc_basis = true;
                        last_time_obs_ms = now;
                        post(&u);
                    }
                }
            }

            if (accepted > 0) {
                gnss_assist_on_fix();
                have_reported_gap = false;
                last_accepted_fix_ms = now;
            } else {
                /*
                 * No fix this cycle, or none the gate believed: either way there is no
                 * position for this stretch, and that is what the gap record says.
                 * A refused fix ages like a missing one.
                 */
                uint32_t since = have_summary ? (uint32_t)(now - last_accepted_fix_ms) : fix_age;
                if (since >= CAIRN_GNSS_STALE_MS &&
                    (!have_reported_gap ||
                     (int32_t)(now - last_gap_report_ms) >= CAIRN_GNSS_GAP_REPORT_MS)) {
                    post_kind(FACT_GNSS_NO_FIX, now);
                    last_gap_report_ms = now;
                    have_reported_gap  = true;
                }
            }
        }

        if ((int32_t)(now - next_obd) >= 0) {

            /*
             * The parked-silence invariant. While the controller holds the bus
             * silent, no OBD request is issued — not even one, and not on a
             * timer. A parked BMW's OBD connector is a D-CAN stub gated by the
             * body controller, so a request here is a wake-up of the car, and
             * the car keeps score.
             *
             * Reporting FACT_OBD_SILENT rather than simply skipping keeps the
             * controller's freshness model honest: have_recent_obd ages out and
             * the health bitmap says the ECU is not answering, which is true
             * and is the same thing a parked car with a sleeping ECU looks
             * like. Silently skipping would leave a stale snapshot looking
             * current.
             */
            if (s_bus_silent) {
                next_obd = now + s_obd_period_ms;
                post_kind(FACT_OBD_SILENT, now);
            } else {
                /*
                 * Timestamps. When the batch succeeds, the six hot PIDs
                 * arrived in a single CAN exchange (~56 ms) and are truly
                 * simultaneous, so both records share the pre-batch timestamp.
                 * When the batch fails or is absent, each record is stamped at
                 * its own read midpoint, halving the smear from sequential
                 * 120 ms requests.
                 */
                obd_batch_t batch;
                bool have_batch = sensors_read_obd_batch(&batch);
                if (!have_batch) batch.valid = false;

                fact_t f;
                memset(&f, 0, sizeof(f));

                uint32_t t0 = millis();
                bool     got_snapshot = sensors_read_obd(&f.data.obd, &batch);
                uint32_t t1 = millis();

                if (batch.valid) {
                    f.monotonic_ms = t0;
                } else {
                    f.monotonic_ms = t0 + (t1 - t0) / 2;
                }

                if (got_snapshot) {
                    f.kind = FACT_OBD_SNAPSHOT;
                    post(&f);
                    if (f.data.obd.speed_kph != CAIRN_I16_UNKNOWN) {
                        have_obd_speed = true;
                        last_obd_kph   = f.data.obd.speed_kph;
                        last_obd_ms    = f.monotonic_ms;
                    }
                } else {
                    post_kind(FACT_OBD_SILENT, f.monotonic_ms);
                }

                fact_t fe;
                memset(&fe, 0, sizeof(fe));

                uint32_t te0 = millis();
                bool     got_ext = sensors_read_obd_extended(&fe.data.obd_ext, &batch);
                uint32_t te1 = millis();

                if (batch.valid) {
                    fe.monotonic_ms = t0;
                } else {
                    fe.monotonic_ms = te0 + (te1 - te0) / 2;
                }

                if (got_ext) {
                    fe.kind = FACT_OBD_EXTENDED;
                    post(&fe);
                }

                /*
                 * When to ask again.
                 *
                 * The fast period belongs to a *complete* batch: that is the
                 * ~56 ms cycle it was measured against. A partial batch leaves
                 * the read functions fetching the rest one request at a time, so
                 * the cycle costs what a sequential sweep costs and asking again
                 * in 200 ms would just mean polling continuously.
                 *
                 * On top of that, back off while the ECU says nothing at all.
                 * Before fail-fast landed, a silent ECU cost ~9 s per cycle of
                 * blocked timeouts, which was accidentally acting as a backoff; a
                 * dead cycle now costs ~700 ms, so at a 1.2 s period the task
                 * would spend more than half its time blocked on an ECU that is
                 * not answering — and that time is taken from the GNSS poll in the
                 * same loop, which is exactly how the 2026-10-07 drive lost its
                 * position data.
                 *
                 * So the backoff is here to stop starving the other sensors, not
                 * to protect the car: a parked car is already covered by the
                 * silence gate above, and this only runs when a drive is
                 * confirmed. That is why it is deliberately shallow — doubling
                 * from the base and capped — and why it recovers on the *first*
                 * answer rather than ramping down. The cost of being wrong is
                 * losing a few seconds of OBD when the ECU comes back, and on a
                 * car being driven for tune data those seconds are the product.
                 */
                if (got_snapshot || got_ext) {
                    s_obd_silent_cycles = 0;
                } else if (s_obd_silent_cycles < 0xFF) {
                    s_obd_silent_cycles++;
                }

                uint32_t period = batch.complete ? cairn_engine_params()->obd_batch_period_ms
                                                 : s_obd_period_ms;
                if (s_obd_silent_cycles > OBD_SILENT_GRACE_CYCLES) {
                    uint8_t  steps = (uint8_t)(s_obd_silent_cycles - OBD_SILENT_GRACE_CYCLES);
                    uint32_t scaled = period;
                    for (uint8_t s = 0; s < steps && scaled < OBD_SILENT_MAX_PERIOD_MS; s++) {
                        scaled *= 2u;
                    }
                    period = (scaled > OBD_SILENT_MAX_PERIOD_MS) ? OBD_SILENT_MAX_PERIOD_MS
                                                                 : scaled;
                }
                next_obd = now + period;
            }
        }

        /*
         * Assisted GNSS: at most one ATGDS line (or one ATGRR read while the receiver's
         * identity is being asked for) per pass, paced inside, so injecting a day of
         * orbits takes seconds of link time spread over the loop instead of a block.
         */
        gnss_assist_sense_step(now);

#if CAIRN_MTPROBE
        /*
         * Discovery probe. After the production OBD work so it only takes bus
         * time the capture path has left, and behind the same silence gate: a
         * parked car is never asked anything.
         */
        if (!s_bus_silent && s_status->obd) mtprobe_tick();
#endif

        if ((int32_t)(now - next_health) >= 0) {
            next_health = now + CAIRN_HEALTH_PERIOD_MS;

            fact_t f;
            memset(&f, 0, sizeof(f));
            f.kind = FACT_HEALTH;
            f.monotonic_ms = now;

            /*
             * The health state and RSSI are the controller's to know, so they
             * are left zero here and filled in when the fact is consumed. This
             * task reports only what it can measure.
             */
            sensors_fill_health(&f.data.health, 0, 0, 0);
            post(&f);
        }

        bool retry_now = s_retry_requested;
        if (retry_now || (int32_t)(now - next_retry) >= 0) {
            next_retry = now + 60000;
            s_retry_requested = false;

            if (!s_status->gnss || !s_status->obd || !s_status->imu) {
                sensors_retry_failed(s_status, !s_bus_silent);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SENSE_PERIOD_MS));
    }
}

bool sensor_task_start(SensorStatus *status)
{
    s_status = status;
    s_dropped = 0;
    s_retry_requested = false;
    s_obd_silent_cycles = 0;
    s_obd_period_ms = cairn_engine_params()->obd_period_ms;

    s_queue = xQueueCreate(CAIRN_FACT_QUEUE_DEPTH, sizeof(fact_t));
    if (s_queue == nullptr) {
        CAIRN_LOGE(TAG, "cannot allocate the fact queue (%d x %u bytes)",
                   CAIRN_FACT_QUEUE_DEPTH, (unsigned)sizeof(fact_t));
        return false;
    }

    /*
     * Pinned to core 0; Arduino's loop() runs on core 1. That pinning is the
     * point of the split: a card write on the controller cannot stall sampling
     * here.
     *
     * 4 KB of stack covers the driver call depth with room to spare; the IMU
     * and GNSS paths do not recurse.
     */
    BaseType_t ok = xTaskCreatePinnedToCore(sensor_task, "cairn-sense", 4096,
                                            nullptr, 2, nullptr, 0);
    if (ok != pdPASS) {
        CAIRN_LOGE(TAG, "cannot start the sensing task");
        return false;
    }

    CAIRN_LOGI(TAG, "sensing task up on core 0: %d-slot queue, %d ms period",
               CAIRN_FACT_QUEUE_DEPTH, SENSE_PERIOD_MS);
    return true;
}

bool sensor_task_poll(fact_t *out)
{
    if (s_queue == nullptr) return false;
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

bool sensor_task_post_fact(const fact_t *f)
{
    if (s_queue == nullptr) return false;
    return xQueueSend(s_queue, f, 0) == pdTRUE;
}

uint32_t sensor_task_dropped(void) { return s_dropped; }

void sensor_task_request_retry(void) { s_retry_requested = true; }

uint32_t sensor_task_supply(uint16_t *battery_mv, uint16_t *raw_mv, uint32_t *at_ms)
{
    portENTER_CRITICAL(&s_supply_lock);
    uint16_t b = s_supply_batt, r = s_supply_raw;
    uint32_t a = s_supply_at, q = s_supply_seq;
    portEXIT_CRITICAL(&s_supply_lock);
    if (battery_mv) *battery_mv = b;
    if (raw_mv)     *raw_mv     = r;
    if (at_ms)      *at_ms      = a;
    return q;
}

void sensor_task_set_rates(const cairn_rates_t *r)
{
    s_gnss_period_ms = r->gnss_period_ms;
    s_imu_window_ms  = r->imu_window_ms;
    s_obd_period_ms  = r->obd_period_ms;
}

void sensor_task_set_bus_silent(bool silent) { s_bus_silent = silent; }

bool sensor_task_bus_silent(void) { return s_bus_silent; }

void sensor_task_set_paused(bool paused) { s_paused = paused; }
