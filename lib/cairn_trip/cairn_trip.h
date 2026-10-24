/*
 * Deciding when a drive starts and ends, from the evidence the controller has.
 *
 * Portable C with the clock passed in, so the rules are host-tested
 * (test/host/trip_test.c) against the evidence of real drives; src/lifecycle.cpp feeds
 * it every tick and acts on what it returns.
 *
 * It replaces logic that lost almost every drive between 2026-10-15 and 2026-10-23.
 * Measured on the dongle's own journal for one 22.8 h boot: the supply rail reached
 * 14.7-16.0 V (the alternator charging) in five separate hours, yet the capture state
 * machine considered a trip only twice and confirmed one for 8 seconds. Three faults
 * compounded:
 *
 *   1. The rail dwell reset on every unreadable sample. A drive is confirmed from local
 *      evidence after the rail has been at engine-on for 15 s, sampled every 500 ms, so
 *      it needed 30 good readings in a row. The co-processor link was shared, unlocked,
 *      between two tasks, and while driving half the readings came back garbled
 *      (9000 mV, 16000 mV, or unreadable). The dwell never completed, the bus stayed
 *      silent, and OBD was never polled. Here an unreadable sample is no information:
 *      only a valid reading below engine-on ends the dwell. (The link is now locked as
 *      well; see sensors_link_lock().)
 *
 *   2. A running engine counted for nothing. The start score needed 1.50 for 3 s from
 *      the accelerometer (>= 120 mg), OBD speed, and GNSS speed, each worth 1.00. With
 *      OBD silent and a smooth road (driving reads 90-130 mg) GNSS alone scored 1.00 and
 *      the trip never started. An engine confirmed running from the rail now counts
 *      1.00, so engine plus any one sign of movement starts a trip; an engine idling
 *      in the driveway does not.
 *
 *   3. Smooth driving ended trips. A trip went to TRAILING whenever the start score
 *      dipped below 1.50 and the accelerometer was under 120 mg -- i.e. on any smooth
 *      stretch -- and to IDLE two minutes later. The one confirmed trip lasted 8 s.
 *      Stopping now needs positive evidence: a fresh speed reading that is low, or the
 *      engine off with the car not moving, or, with nothing else known, the
 *      accelerometer below the 35 mg a moving car never reads.
 *
 * The start score also gets hysteresis: one tick below threshold no longer restarts
 * the 3 s dwell; the score has to stay low for CAIRN_TRIP_MOTION_GAP_MS.
 */
#ifndef CAIRN_TRIP_H
#define CAIRN_TRIP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAIRN_TRIP_UNKNOWN_MV        0xFFFFu
#define CAIRN_TRIP_ENGINE_ON_DEFAULT 13200u   /* overridden by the engine profile */

#define CAIRN_TRIP_START_THRESHOLD_E2 150u
#define CAIRN_TRIP_START_DWELL_MS     3000u
#define CAIRN_TRIP_MOTION_GAP_MS      2000u   /* score must stay low this long to reset */
#define CAIRN_TRIP_REENTRY_DWELL_MS   2000u
#define CAIRN_TRIP_STOP_DWELL_MS      120000u
#define CAIRN_TRIP_MOTION_MG          120u
#define CAIRN_TRIP_STILL_MG           35u
#define CAIRN_TRIP_MOVING_CMPS        280u    /* ~10 km/h */

/* Drive confirmation from local evidence (opens the vehicle bus). */
#define CAIRN_TRIP_RAIL_DWELL_MS      15000u
#define CAIRN_TRIP_MOTION_ONLY_MS     10000u
#define CAIRN_TRIP_BOTH_DWELL_MS      2000u
/* The rail counts as high while any high reading is this recent, and low only after
 * this long with nothing but low readings: one garbage sample (the co-processor link
 * returned 9.000 V mid-drive, which passes a 9-18 V sanity range) or a cranking dip
 * must not flip it. */
#define CAIRN_TRIP_RAIL_DEBOUNCE_MS   5000u
/* How long without any valid reading before the rail is simply unknown. */
#define CAIRN_TRIP_RAIL_HOLD_MS       10000u

typedef enum {
    CAIRN_TRIP_IDLE = 0,
    CAIRN_TRIP_PRETRIP,
    CAIRN_TRIP_ACTIVE,
    CAIRN_TRIP_TRAILING,
} cairn_trip_state_t;

/* What the controller knows this tick. */
typedef struct {
    uint32_t now_ms;
    uint16_t rail_mv;        /* validated supply reading, or CAIRN_TRIP_UNKNOWN_MV */
    bool     rail_fresh;     /* rail_mv is a new reading this tick */
    uint16_t accel_rms_mg;
    bool     obd_valid;      /* a fresh OBD speed */
    int16_t  obd_kph;
    bool     gnss_valid;     /* a fresh, vetted GNSS speed */
    uint16_t gnss_cmps;
    bool     bench;          /* USB power: never start a trip */
} cairn_trip_input_t;

typedef struct {
    cairn_trip_state_t state;
    uint16_t engine_on_mv;

    /* rail */
    uint32_t rail_last_high_ms;  /* 0 = never */
    uint32_t rail_last_low_ms;   /* 0 = never */
    uint32_t rail_high_since;    /* 0 = not high */

    /* motion */
    uint32_t motion_since;     /* 0 = no sustained motion */
    uint32_t motion_low_since; /* score below threshold since, for the gap */
    uint32_t still_since;      /* 0 = not stopped */
    uint32_t accel_since;      /* accelerometer >= MOTION_MG since */

    bool     drive_confirmed;  /* latched; opens the bus */

    /* last computed, for the transition record */
    uint16_t start_score_e2;
    uint16_t stop_score_e2;
    const char *why;           /* what the last decision rested on */
} cairn_trip_t;

typedef enum {
    CAIRN_TRIP_NONE = 0,
    CAIRN_TRIP_TO_PRETRIP,
    CAIRN_TRIP_TO_IDLE_REJECTED,   /* the candidate did not persist */
    CAIRN_TRIP_TO_ACTIVE,          /* a trip starts */
    CAIRN_TRIP_TO_TRAILING,
    CAIRN_TRIP_TO_ACTIVE_RESUMED,
    CAIRN_TRIP_TO_IDLE_ENDED,      /* the trip ends */
} cairn_trip_event_t;

void cairn_trip_init(cairn_trip_t *t, uint16_t engine_on_mv);

/* Feed one tick; returns the transition to take, which has already been applied to
 * t->state. The caller performs the side effects (pre-roll flush, seal, records). */
cairn_trip_event_t cairn_trip_step(cairn_trip_t *t, const cairn_trip_input_t *in);

/* The caller moved the state itself (a trip resumed across a reboot, bench mode). */
void cairn_trip_force(cairn_trip_t *t, cairn_trip_state_t s, uint32_t now_ms);

/* The engine is confirmed running from the rail (not merely a recent high sample). */
bool cairn_trip_engine_running(const cairn_trip_t *t, uint32_t now_ms);

/* The rail's current verdict: 1 high, 0 low, -1 unknown. */
int cairn_trip_rail(const cairn_trip_t *t, uint32_t now_ms);

const char *cairn_trip_state_name(cairn_trip_state_t s);

#ifdef __cplusplus
}
#endif

#endif
