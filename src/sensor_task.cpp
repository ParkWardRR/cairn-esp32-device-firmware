#include "sensor_task.h"

#include <Arduino.h>

#include "cairn_log.h"
#include "config.h"

static const char *TAG = "SENSE";

static QueueHandle_t  s_queue;
static SensorStatus  *s_status;
static volatile uint32_t s_dropped;
static volatile bool  s_retry_requested;

/* Current periods, set by the controller. Initialized to the policy defaults so
 * the first pass before any update samples at nominal rather than at zero. */
static volatile uint16_t s_gnss_period_ms = CAIRN_GNSS_PERIOD_MS;
static volatile uint16_t s_imu_window_ms  = CAIRN_IMU_WINDOW_MS;
static volatile uint16_t s_obd_period_ms  = CAIRN_OBD_PERIOD_MS;

/*
 * Default silent. A device that boots next to a parked car must not start
 * polling before the controller has decided anything — the safe initial state
 * is quiet, and the controller opens the bus once a drive is confirmed.
 */
static volatile bool s_bus_silent = true;

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
    uint32_t next_retry  = millis() + 60000;

    bool have_utc_basis = false;

    for (;;) {
        uint32_t now = millis();

        /* Highest rate first, and unconditionally: this is the sample stream
         * the window statistics are computed from. */
        sensors_imu_accumulate();

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

            fact_t f;
            memset(&f, 0, sizeof(f));
            f.monotonic_ms = now;

            uint32_t fix_age = 0;
            if (sensors_read_gnss(&f.data.gnss, &fix_age)) {
                f.kind = FACT_GNSS_SAMPLE;
                post(&f);

                /*
                 * The basis is reported once, the first time the receiver has a
                 * date it considers valid. Re-reporting it would let a later,
                 * worse fix move the bundle's time reference after samples had
                 * already been written against the original.
                 */
                if (!have_utc_basis) {
                    fact_t u;
                    memset(&u, 0, sizeof(u));
                    u.kind = FACT_GNSS_UTC_BASIS;
                    u.monotonic_ms = now;
                    if (sensors_gnss_utc(&u.data.utc.utc_ms, &u.data.utc.acc_ms)) {
                        have_utc_basis = true;
                        post(&u);
                    }
                }
            } else {
                /* Reported, not skipped: silence in the data would be
                 * indistinguishable from the device being switched off. */
                post_kind(FACT_GNSS_NO_FIX, now);
            }
        }

        if ((int32_t)(now - next_obd) >= 0) {
            next_obd = now + s_obd_period_ms;

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
                post_kind(FACT_OBD_SILENT, now);
            } else {
                fact_t f;
                memset(&f, 0, sizeof(f));
                f.monotonic_ms = now;

                if (sensors_read_obd(&f.data.obd)) {
                    f.kind = FACT_OBD_SNAPSHOT;
                    post(&f);
                } else {
                    post_kind(FACT_OBD_SILENT, now);
                }
            }
        }

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
                /* Re-initialising touches the same driver state this task owns,
                 * so it happens here rather than on the controller. */
                sensors_retry_failed(s_status);
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

uint32_t sensor_task_dropped(void) { return s_dropped; }

void sensor_task_request_retry(void) { s_retry_requested = true; }

void sensor_task_set_rates(const cairn_rates_t *r)
{
    s_gnss_period_ms = r->gnss_period_ms;
    s_imu_window_ms  = r->imu_window_ms;
    s_obd_period_ms  = r->obd_period_ms;
}

void sensor_task_set_bus_silent(bool silent) { s_bus_silent = silent; }

bool sensor_task_bus_silent(void) { return s_bus_silent; }
