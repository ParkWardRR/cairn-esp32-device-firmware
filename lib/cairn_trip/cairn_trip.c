#include "cairn_trip.h"

#include <string.h>

void cairn_trip_init(cairn_trip_t *t, uint16_t engine_on_mv)
{
    memset(t, 0, sizeof(*t));
    t->state = CAIRN_TRIP_IDLE;
    t->engine_on_mv = engine_on_mv ? engine_on_mv : CAIRN_TRIP_ENGINE_ON_DEFAULT;
}

const char *cairn_trip_state_name(cairn_trip_state_t s)
{
    switch (s) {
    case CAIRN_TRIP_IDLE:     return "IDLE";
    case CAIRN_TRIP_PRETRIP:  return "PRETRIP";
    case CAIRN_TRIP_ACTIVE:   return "ACTIVE";
    case CAIRN_TRIP_TRAILING: return "TRAILING";
    default:                  return "?";
    }
}

/* Elapsed since `since`, which is a timestamp or 0 for "not running". 0 is a legal
 * millis() value only in the first millisecond of a boot; it is nudged to 1 below so
 * that "started at zero" and "not started" cannot be confused. */
static uint32_t since(uint32_t now, uint32_t t0) { return t0 ? now - t0 : 0; }
static uint32_t stamp(uint32_t now) { return now ? now : 1; }

int cairn_trip_rail(const cairn_trip_t *t, uint32_t now_ms)
{
    if (t->rail_last_high_ms && now_ms - t->rail_last_high_ms <= CAIRN_TRIP_RAIL_DEBOUNCE_MS) return 1;
    if (t->rail_last_low_ms && now_ms - t->rail_last_low_ms <= CAIRN_TRIP_RAIL_HOLD_MS) return 0;
    return -1;
}

bool cairn_trip_engine_running(const cairn_trip_t *t, uint32_t now_ms)
{
    return cairn_trip_rail(t, now_ms) == 1 && since(now_ms, t->rail_high_since) >= CAIRN_TRIP_RAIL_DWELL_MS;
}

static void track_rail(cairn_trip_t *t, const cairn_trip_input_t *in)
{
    uint32_t now = in->now_ms;
    if (in->rail_fresh && in->rail_mv != CAIRN_TRIP_UNKNOWN_MV) {
        if (in->rail_mv >= t->engine_on_mv) t->rail_last_high_ms = stamp(now);
        else                                t->rail_last_low_ms  = stamp(now);
    }
    /* The dwell runs while the debounced verdict is high, and only that ends it. */
    if (cairn_trip_rail(t, now) == 1) {
        if (!t->rail_high_since) t->rail_high_since = stamp(now);
    } else {
        t->rail_high_since = 0;
    }
}

static void confirm_drive(cairn_trip_t *t, const cairn_trip_input_t *in)
{
    uint32_t now = in->now_ms;
    bool rail_up = cairn_trip_rail(t, now) == 1;
    bool moving  = in->accel_rms_mg >= CAIRN_TRIP_MOTION_MG;
    if (moving) { if (!t->accel_since) t->accel_since = stamp(now); }
    else t->accel_since = 0;

    if (t->state == CAIRN_TRIP_ACTIVE || t->state == CAIRN_TRIP_TRAILING) {
        /* Latched for the life of a trip: a stop at a light sags the rail (and auto
         * start-stop cuts the engine) without ending the drive. Not in PRETRIP, which
         * a door slam reaches: the bus is opened by evidence, not by a candidate. */
        t->drive_confirmed = true;
        return;
    }
    bool ok = (rail_up && moving && (since(now, t->rail_high_since) >= CAIRN_TRIP_BOTH_DWELL_MS ||
                                     since(now, t->accel_since) >= CAIRN_TRIP_BOTH_DWELL_MS)) ||
              (rail_up && since(now, t->rail_high_since) >= CAIRN_TRIP_RAIL_DWELL_MS) ||
              (moving && since(now, t->accel_since) >= CAIRN_TRIP_MOTION_ONLY_MS);
    if (ok) {
        t->drive_confirmed = true;
    } else if (cairn_trip_rail(t, now) == 0 && !moving) {
        /* Released only on positive evidence the engine is off and the car still: an
         * unreadable rail used to close the bus in the middle of a drive, and with it
         * the OBD session that had only just been opened. */
        t->drive_confirmed = false;
    }
}

cairn_trip_event_t cairn_trip_step(cairn_trip_t *t, const cairn_trip_input_t *in)
{
    const uint32_t now = in->now_ms;
    track_rail(t, in);
    confirm_drive(t, in);

    const bool engine = cairn_trip_engine_running(t, now);
    const bool rail_low = cairn_trip_rail(t, now) == 0;

    /* ── start score ── */
    uint32_t score = 0;
    if (in->accel_rms_mg >= CAIRN_TRIP_MOTION_MG) score += 100u * in->accel_rms_mg / CAIRN_TRIP_MOTION_MG;
    if (in->obd_valid && in->obd_kph > 3) score += 100;
    if (in->gnss_valid && in->gnss_cmps >= CAIRN_TRIP_MOVING_CMPS) score += 100;
    if (engine) score += 100;
    t->start_score_e2 = (uint16_t)(score > 0xFFFF ? 0xFFFF : score);
    const bool motion_now = t->start_score_e2 >= CAIRN_TRIP_START_THRESHOLD_E2;

    if (motion_now) {
        if (!t->motion_since) t->motion_since = stamp(now);
        t->motion_low_since = 0;
    } else if (t->motion_since) {
        if (!t->motion_low_since) t->motion_low_since = stamp(now);
        if (since(now, t->motion_low_since) >= CAIRN_TRIP_MOTION_GAP_MS) {
            t->motion_since = 0;
            t->motion_low_since = 0;
        }
    }

    /* ── stop evidence: positive, not the mere absence of a strong signal ── */
    const bool speed_known = in->obd_valid || in->gnss_valid;
    const bool speed_low =
        (in->obd_valid ? in->obd_kph <= 1 : true) &&
        (in->gnss_valid ? in->gnss_cmps < CAIRN_TRIP_MOVING_CMPS : true);
    const bool quiet = in->accel_rms_mg < CAIRN_TRIP_MOTION_MG;
    bool stopped;
    if (speed_known)      { stopped = speed_low && quiet;                 t->why = "speed"; }
    else if (rail_low)    { stopped = quiet;                               t->why = "engine off"; }
    else                  { stopped = in->accel_rms_mg < CAIRN_TRIP_STILL_MG; t->why = "accelerometer"; }

    uint32_t stop = 0;
    if (quiet) stop += 50;
    if (in->obd_valid && in->obd_kph <= 1) stop += 30;
    if (in->gnss_valid && in->gnss_cmps < CAIRN_TRIP_MOVING_CMPS) stop += 20;
    if (rail_low) stop += 30;
    t->stop_score_e2 = (uint16_t)stop;

    if (stopped && !motion_now) { if (!t->still_since) t->still_since = stamp(now); }
    else t->still_since = 0;

    /* ── transitions ── */
    switch (t->state) {
    case CAIRN_TRIP_IDLE:
        if (t->motion_since && !in->bench) { t->state = CAIRN_TRIP_PRETRIP; return CAIRN_TRIP_TO_PRETRIP; }
        break;
    case CAIRN_TRIP_PRETRIP:
        if (!t->motion_since || in->bench) { t->state = CAIRN_TRIP_IDLE; return CAIRN_TRIP_TO_IDLE_REJECTED; }
        if (since(now, t->motion_since) >= CAIRN_TRIP_START_DWELL_MS) {
            t->state = CAIRN_TRIP_ACTIVE;
            t->drive_confirmed = true;
            return CAIRN_TRIP_TO_ACTIVE;
        }
        break;
    case CAIRN_TRIP_ACTIVE:
        if (t->still_since) { t->state = CAIRN_TRIP_TRAILING; return CAIRN_TRIP_TO_TRAILING; }
        break;
    case CAIRN_TRIP_TRAILING:
        if (t->motion_since && since(now, t->motion_since) >= CAIRN_TRIP_REENTRY_DWELL_MS) {
            t->state = CAIRN_TRIP_ACTIVE;
            return CAIRN_TRIP_TO_ACTIVE_RESUMED;
        }
        if (t->still_since && since(now, t->still_since) >= CAIRN_TRIP_STOP_DWELL_MS) {
            t->state = CAIRN_TRIP_IDLE;
            t->motion_since = 0;
            return CAIRN_TRIP_TO_IDLE_ENDED;
        }
        break;
    }
    return CAIRN_TRIP_NONE;
}

void cairn_trip_force(cairn_trip_t *t, cairn_trip_state_t s, uint32_t now_ms)
{
    t->state = s;
    if (s == CAIRN_TRIP_IDLE) {
        t->motion_since = 0;
        t->still_since = 0;
        t->motion_low_since = 0;
    } else {
        t->drive_confirmed = true;
        if (s == CAIRN_TRIP_ACTIVE) t->still_since = 0;
    }
    (void)now_ms;
}
