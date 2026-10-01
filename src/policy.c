#include "policy.h"

#include <string.h>

#include "cairn_format.h"
#include "cairn_store.h"

/* ── defaults ─────────────────────────────────────────────────────────────── */

void cairn_policy_defaults(cairn_policy_t *p)
{
    memset(p, 0, sizeof(*p));

    p->policy_version = CAIRN_POLICY_VERSION;

    p->gnss_period_ms   = CAIRN_GNSS_PERIOD_MS;
    p->imu_window_ms    = CAIRN_IMU_WINDOW_MS;
    p->obd_period_ms    = CAIRN_OBD_PERIOD_MS;
    p->health_period_ms = CAIRN_HEALTH_PERIOD_MS;

    p->start_score_threshold_e2 = CAIRN_START_SCORE_THRESHOLD_E2;
    p->stop_score_threshold_e2  = CAIRN_STOP_SCORE_THRESHOLD_E2;
    p->start_dwell_ms           = CAIRN_START_DWELL_MS;
    p->stop_dwell_ms            = CAIRN_STOP_DWELL_MS;

    p->motion_accel_rms_mg = CAIRN_MOTION_ACCEL_RMS_MG;
    p->motion_speed_cmps   = CAIRN_MOTION_SPEED_CMPS;

    p->preroll_window_ms    = CAIRN_PREROLL_WINDOW_MS;
    p->preroll_ring_samples = CAIRN_PREROLL_RING_SAMPLES;
    p->segment_max_bytes    = CAIRN_SEGMENT_MAX_BYTES;

    p->adaptive_sampling = CAIRN_ADAPTIVE_SAMPLING != 0;
}

/* ── POLICY_SNAPSHOT ──────────────────────────────────────────────────────── */

#define KEY_POLICY_VERSION      1
#define KEY_GNSS_PERIOD         2
#define KEY_IMU_WINDOW          3
#define KEY_OBD_PERIOD          4
#define KEY_HEALTH_PERIOD       5
#define KEY_START_SCORE         6
#define KEY_STOP_SCORE          7
#define KEY_START_DWELL         8
#define KEY_STOP_DWELL          9
#define KEY_MOTION_ACCEL       10
#define KEY_MOTION_SPEED       11
#define KEY_PREROLL_WINDOW     12
#define KEY_PREROLL_SAMPLES    13
#define KEY_SEGMENT_MAX        14
#define KEY_ADAPTIVE           15

#define POLICY_FIELD_COUNT     15

size_t cairn_policy_encode(const cairn_policy_t *p, uint8_t *out, size_t cap)
{
    cairn_cbor_enc_t e;
    cairn_cbor_init(&e, out, cap);

    cairn_cbor_map(&e, POLICY_FIELD_COUNT);

    cairn_cbor_key(&e, KEY_POLICY_VERSION);
    cairn_cbor_uint(&e, p->policy_version);
    cairn_cbor_key(&e, KEY_GNSS_PERIOD);
    cairn_cbor_uint(&e, p->gnss_period_ms);
    cairn_cbor_key(&e, KEY_IMU_WINDOW);
    cairn_cbor_uint(&e, p->imu_window_ms);
    cairn_cbor_key(&e, KEY_OBD_PERIOD);
    cairn_cbor_uint(&e, p->obd_period_ms);
    cairn_cbor_key(&e, KEY_HEALTH_PERIOD);
    cairn_cbor_uint(&e, p->health_period_ms);
    cairn_cbor_key(&e, KEY_START_SCORE);
    cairn_cbor_uint(&e, p->start_score_threshold_e2);
    cairn_cbor_key(&e, KEY_STOP_SCORE);
    cairn_cbor_uint(&e, p->stop_score_threshold_e2);
    cairn_cbor_key(&e, KEY_START_DWELL);
    cairn_cbor_uint(&e, p->start_dwell_ms);
    cairn_cbor_key(&e, KEY_STOP_DWELL);
    cairn_cbor_uint(&e, p->stop_dwell_ms);
    cairn_cbor_key(&e, KEY_MOTION_ACCEL);
    cairn_cbor_uint(&e, p->motion_accel_rms_mg);
    cairn_cbor_key(&e, KEY_MOTION_SPEED);
    cairn_cbor_uint(&e, p->motion_speed_cmps);
    cairn_cbor_key(&e, KEY_PREROLL_WINDOW);
    cairn_cbor_uint(&e, p->preroll_window_ms);
    cairn_cbor_key(&e, KEY_PREROLL_SAMPLES);
    cairn_cbor_uint(&e, p->preroll_ring_samples);
    cairn_cbor_key(&e, KEY_SEGMENT_MAX);
    cairn_cbor_uint(&e, p->segment_max_bytes);
    cairn_cbor_key(&e, KEY_ADAPTIVE);
    cairn_cbor_uint(&e, p->adaptive_sampling ? 1u : 0u);

    /* Overflow also fires on a descending key, which would be a programming
     * error that silently produced a non-canonical map. */
    if (e.overflow) return 0;
    return e.len;
}

/* ── adaptive sampling ────────────────────────────────────────────────────── */

const char *cairn_dynamics_name(cairn_dynamics_t d)
{
    switch (d) {
    case CAIRN_DYN_IDLE:   return "IDLE";
    case CAIRN_DYN_CRUISE: return "CRUISE";
    case CAIRN_DYN_ACTIVE: return "ACTIVE";
    case CAIRN_DYN_EVENT:  return "EVENT";
    default:               return "?";
    }
}

cairn_dynamics_t cairn_policy_classify(const cairn_policy_t *p,
                                       uint16_t accel_rms_mg,
                                       uint16_t speed_cmps,
                                       int32_t speed_delta_cmps,
                                       bool trip_active)
{
    if (!trip_active) return CAIRN_DYN_IDLE;

    /*
     * A sharp transient. Three times the motion threshold is a decisive
     * manoeuvre rather than road noise, and this is the case where extra
     * resolution is worth the write load.
     */
    if (accel_rms_mg >= (uint16_t)(p->motion_accel_rms_mg * 3)) {
        return CAIRN_DYN_EVENT;
    }

    /*
     * A large speed change between OBD samples is the other decisive signal,
     * and it catches braking that the accelerometer's RMS smooths away. 500
     * cm/s over one OBD period is roughly 18 km/h, which no gentle driving
     * produces.
     */
    if (speed_delta_cmps > 500 || speed_delta_cmps < -500) {
        return CAIRN_DYN_EVENT;
    }

    if (accel_rms_mg >= (uint16_t)(p->motion_accel_rms_mg + (p->motion_accel_rms_mg / 2))) {
        return CAIRN_DYN_ACTIVE;
    }

    /*
     * Moving but smooth. Nominal rates already resolve a straight line well,
     * so there is nothing to gain by sampling harder — and the card and the
     * battery both prefer not to.
     */
    if (speed_cmps != CAIRN_U16_UNKNOWN && speed_cmps >= p->motion_speed_cmps) {
        return CAIRN_DYN_CRUISE;
    }

    return CAIRN_DYN_ACTIVE;
}

void cairn_policy_rates(const cairn_policy_t *p, cairn_dynamics_t d,
                        bool trip_active, cairn_rates_t *out)
{
    /* Nominal, which is also the slowest permitted during a trip. */
    out->gnss_period_ms = p->gnss_period_ms;
    out->imu_window_ms  = p->imu_window_ms;
    out->obd_period_ms  = p->obd_period_ms;

    if (!p->adaptive_sampling) return;

    switch (d) {
    case CAIRN_DYN_IDLE:
        /*
         * The only case that samples *slower* than nominal, and it is safe
         * because no trip is underway: these records go to the pre-roll ring,
         * where the guarantee is a window of history rather than a rate. A
         * parked car has nothing to resolve and every reason to save power.
         */
        if (!trip_active) {
            out->gnss_period_ms = (uint16_t)(p->gnss_period_ms * 4);
            out->obd_period_ms  = (uint16_t)(p->obd_period_ms * 4);
        }
        break;

    case CAIRN_DYN_CRUISE:
        /* Nominal. A straight line does not reward more samples. */
        break;

    case CAIRN_DYN_ACTIVE:
        out->gnss_period_ms = (uint16_t)(p->gnss_period_ms / 2);
        out->imu_window_ms  = (uint16_t)(p->imu_window_ms / 2);
        break;

    case CAIRN_DYN_EVENT:
        /*
         * As fast as the sources usefully go. The IMU window shrinks furthest
         * because peak and RMS over a quarter second describe a hard stop,
         * while over a full second they average it away.
         */
        out->gnss_period_ms = (uint16_t)(p->gnss_period_ms / 4);
        out->imu_window_ms  = (uint16_t)(p->imu_window_ms / 4);
        out->obd_period_ms  = (uint16_t)(p->obd_period_ms / 2);
        break;
    }

    /* Floors. Sampling GNSS faster than the receiver produces fixes would
     * manufacture duplicate records, and an IMU window shorter than a few
     * samples has no statistics to summarize. */
    if (out->gnss_period_ms < 200) out->gnss_period_ms = 200;
    if (out->imu_window_ms < 100) out->imu_window_ms = 100;
    if (out->obd_period_ms < 250) out->obd_period_ms = 250;
}
