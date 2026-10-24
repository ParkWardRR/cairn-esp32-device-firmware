/*
 * Trip detection (lib/cairn_trip), replayed against the conditions of the drives the
 * dongle lost between 2026-10-15 and 2026-10-23: a garbled supply rail, OBD silent,
 * smooth roads. And the things that must still not start a trip.
 */

#include <stdio.h>
#include <string.h>

#include "cairn_trip.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [trip] %s\n", name); }           \
        else      { g_fail++; printf("  FAIL  [trip] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* A tiny deterministic generator, so "half the rail readings are garbage" is the same
 * garbage every run. */
static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 16) & 0x7FFF; }

typedef struct {
    uint16_t rail_mv;      /* 0 = unknown */
    int      rail_garble;  /* percent of readings unreadable or out of range */
    uint16_t imu_mg;
    int      imu_jitter;   /* +/- mg */
    int      obd_kph;      /* -1 = OBD silent */
    int      gnss_cmps;    /* -1 = no fix */
    int      bench;
} scene_t;

typedef struct {
    int active_ticks, trips_started, trips_ended, max_state;
    uint32_t first_active_ms;
} tally_t;

/* Runs `ms` of a scene at the controller's 100 ms tick, sampling the rail every 500 ms
 * as the firmware does. */
static void run(cairn_trip_t *t, uint32_t *now, uint32_t ms, const scene_t *s, tally_t *k)
{
    for (uint32_t end = *now + ms; *now < end; *now += 100) {
        cairn_trip_input_t in;
        memset(&in, 0, sizeof(in));
        in.now_ms = *now;
        in.rail_fresh = (*now % 500) == 0;
        in.rail_mv = CAIRN_TRIP_UNKNOWN_MV;
        if (in.rail_fresh && s->rail_mv) {
            if ((int)(rnd() % 100) < s->rail_garble) in.rail_mv = (rnd() & 1) ? CAIRN_TRIP_UNKNOWN_MV : 9000;
            else in.rail_mv = s->rail_mv;
        }
        int j = s->imu_jitter ? (int)(rnd() % (2 * s->imu_jitter + 1)) - s->imu_jitter : 0;
        in.accel_rms_mg = (uint16_t)(s->imu_mg + j < 0 ? 0 : s->imu_mg + j);
        in.obd_valid = s->obd_kph >= 0;
        in.obd_kph = (int16_t)(s->obd_kph >= 0 ? s->obd_kph : 0);
        in.gnss_valid = s->gnss_cmps >= 0;
        in.gnss_cmps = (uint16_t)(s->gnss_cmps >= 0 ? s->gnss_cmps : 0);
        in.bench = s->bench;
        cairn_trip_event_t e = cairn_trip_step(t, &in);
        if (e == CAIRN_TRIP_TO_ACTIVE) { k->trips_started++; if (!k->first_active_ms) k->first_active_ms = *now; }
        if (e == CAIRN_TRIP_TO_IDLE_ENDED) k->trips_ended++;
        if (t->state == CAIRN_TRIP_ACTIVE) k->active_ticks++;
        if ((int)t->state > k->max_state) k->max_state = t->state;
    }
}

static void test_the_lost_drive_is_captured(void)
{
    /* Oct 22-23: engine running (14.8 V) but half the rail readings garbled, OBD
     * silent, a GNSS fix at 15 m/s, a smooth road at 90-120 mg. */
    cairn_trip_t t; cairn_trip_init(&t, 13200);
    uint32_t now = 1000; tally_t k = {0};
    scene_t parked = {12600, 0, 9, 2, -1, -1, 0};
    scene_t drive  = {14800, 50, 105, 15, -1, 1500, 0};
    run(&t, &now, 60000, &parked, &k);
    CHECK("a parked car with the rail at 12.6 V does not start a trip", k.trips_started == 0);
    uint32_t t0 = now;
    run(&t, &now, 20 * 60000, &drive, &k);
    CHECK("a 20-minute drive with a garbled rail and no OBD is one trip", k.trips_started == 1);
    CHECK("and it starts within 30 s of driving off", k.first_active_ms && k.first_active_ms - t0 <= 30000);
    CHECK("the bus opens (drive confirmed) despite the garbled rail", t.drive_confirmed);
    CHECK("a smooth road does not end the trip", t.state == CAIRN_TRIP_ACTIVE && k.trips_ended == 0);
}

static void test_engine_and_accelerometer_alone(void)
{
    /* No fix at all (a garage exit, a cold receiver), OBD silent, but the engine on
     * and the car clearly moving. */
    cairn_trip_t t; cairn_trip_init(&t, 13200);
    uint32_t now = 1000; tally_t k = {0};
    scene_t drive = {14700, 0, 140, 20, -1, -1, 0};
    run(&t, &now, 5 * 60000, &drive, &k);
    CHECK("engine plus movement starts a trip with no GNSS and no OBD", k.trips_started == 1);
    scene_t cruise = {14700, 0, 95, 10, -1, -1, 0};
    run(&t, &now, 5 * 60000, &cruise, &k);
    CHECK("and a smooth stretch with nothing else known keeps it going", t.state == CAIRN_TRIP_ACTIVE);
}

static void test_what_must_not_start_a_trip(void)
{
    cairn_trip_t t; cairn_trip_init(&t, 13200);
    uint32_t now = 1000; tally_t k = {0};
    scene_t idling = {14500, 20, 9, 3, 0, 0, 0};
    run(&t, &now, 10 * 60000, &idling, &k);
    CHECK("ten minutes idling in the driveway is not a trip", k.trips_started == 0);

    cairn_trip_t u; cairn_trip_init(&u, 13200);
    uint32_t n2 = 1000; tally_t k2 = {0};
    scene_t parked = {12500, 0, 8, 2, -1, -1, 0};
    scene_t slam   = {12500, 0, 400, 50, -1, -1, 0};
    for (int i = 0; i < 5; i++) {
        run(&u, &n2, 30000, &parked, &k2);
        run(&u, &n2, 600, &slam, &k2);  /* a door, someone leaning on the car */
    }
    CHECK("door slams on a parked car never become a trip", k2.trips_started == 0);
    CHECK("nor open the bus", !u.drive_confirmed);

    cairn_trip_t b; cairn_trip_init(&b, 13200);
    uint32_t n3 = 1000; tally_t k3 = {0};
    scene_t bench = {14800, 0, 200, 50, 30, 1500, 1};
    run(&b, &n3, 5 * 60000, &bench, &k3);
    CHECK("bench power never starts a trip", k3.trips_started == 0);
}

static void test_stops(void)
{
    cairn_trip_t t; cairn_trip_init(&t, 13200);
    uint32_t now = 1000; tally_t k = {0};
    scene_t drive = {14800, 30, 110, 20, 50, 1400, 0};
    scene_t light = {14200, 30, 9, 2, 0, 0, 0};      /* stopped, engine running */
    scene_t ss    = {12300, 30, 6, 2, 0, 0, 0};      /* auto start-stop: engine off at the light */
    run(&t, &now, 5 * 60000, &drive, &k);
    run(&t, &now, 60000, &light, &k);
    run(&t, &now, 3 * 60000, &drive, &k);
    run(&t, &now, 90000, &ss, &k);
    run(&t, &now, 3 * 60000, &drive, &k);
    CHECK("red lights, even with start-stop, do not split the trip", k.trips_started == 1 && k.trips_ended == 0);
    CHECK("the bus stays open through them", t.drive_confirmed);

    scene_t tunnel = {14800, 30, 100, 20, -1, -1, 0};  /* no fix, OBD dropped out */
    run(&t, &now, 2 * 60000, &tunnel, &k);
    CHECK("a tunnel with no speed source does not end the trip", t.state == CAIRN_TRIP_ACTIVE && k.trips_ended == 0);

    scene_t parked = {12400, 30, 8, 2, 0, 0, 0};
    run(&t, &now, 3 * 60000, &parked, &k);
    CHECK("parking with the engine off ends it after the stop dwell", k.trips_ended == 1 && t.state == CAIRN_TRIP_IDLE);

    scene_t parked_unknown = {12400, 0, 8, 2, -1, -1, 0};
    run(&t, &now, 2 * 60000, &parked_unknown, &k);
    CHECK("after parking the bus closes on a valid low rail", !t.drive_confirmed);
}

static void test_rail_tolerance(void)
{
    cairn_trip_t t; cairn_trip_init(&t, 13200);
    uint32_t now = 1000; tally_t k = {0};
    scene_t engine = {14800, 50, 9, 2, -1, -1, 0};   /* half the readings garbage, as measured */
    run(&t, &now, 20000, &engine, &k);
    CHECK("the engine is recognised running through 50 % garbage rail samples", cairn_trip_engine_running(&t, now));
    scene_t dead = {0, 0, 9, 2, -1, -1, 0};          /* the rail goes unreadable */
    run(&t, &now, 3000, &dead, &k);
    CHECK("an unreadable rail keeps its verdict for a few seconds", cairn_trip_rail(&t, now) == 1);
    run(&t, &now, 14000, &dead, &k);
    CHECK("then admits it does not know", cairn_trip_rail(&t, now) == -1 && !cairn_trip_engine_running(&t, now));
}

static void test_resume_and_force(void)
{
    cairn_trip_t t; cairn_trip_init(&t, 13200);
    cairn_trip_force(&t, CAIRN_TRIP_ACTIVE, 5000);
    CHECK("a trip resumed across a reboot is active with the bus open", t.state == CAIRN_TRIP_ACTIVE && t.drive_confirmed);
    uint32_t now = 5000; tally_t k = {0};
    scene_t drive = {14800, 0, 110, 10, 60, 1600, 0};
    run(&t, &now, 60000, &drive, &k);
    CHECK("and carries on", t.state == CAIRN_TRIP_ACTIVE && k.trips_ended == 0);
    cairn_trip_force(&t, CAIRN_TRIP_IDLE, now);
    CHECK("bench mode can end it", t.state == CAIRN_TRIP_IDLE);
}

int main(void)
{
    printf("trip\n");
    test_the_lost_drive_is_captured();
    test_engine_and_accelerometer_alone();
    test_what_must_not_start_a_trip();
    test_stops();
    test_rail_tolerance();
    test_resume_and_force();
    printf("trip: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
