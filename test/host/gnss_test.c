/*
 * GNSS fix vetting (lib/cairn_gnss): the plausibility gate, replayed against the fixes
 * of the 2026-10-14 drive that drew a phantom spur after the car parked, and the NMEA
 * path that used to stamp a whole buffer with one time and write speed 0.
 */

#include <stdio.h>
#include <string.h>

#include "cairn_gnss.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [gnss] %s\n", name); }           \
        else      { g_fail++; printf("  FAIL  [gnss] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

static cairn_gnss_sample_t fix(double speed_mps, int sats)
{
    cairn_gnss_sample_t s;
    memset(&s, 0, sizeof(s));
    s.lat_e7 = 340333000;
    s.lon_e7 = -1184678000;
    s.alt_cm = 3000;
    s.speed_cmps = (uint16_t)(speed_mps * 100.0 + 0.5);
    s.sats_used = (uint8_t)sats;
    s.fix_type = 3;
    return s;
}

static cairn_gnss_context_t ctx(int obd_kph, uint32_t obd_age_ms, int imu_mg)
{
    cairn_gnss_context_t c;
    memset(&c, 0, sizeof(c));
    c.obd_valid = obd_kph >= 0;
    c.obd_speed_kph = (int16_t)(obd_kph >= 0 ? obd_kph : 0);
    c.obd_age_ms = obd_age_ms;
    c.imu_valid = imu_mg >= 0;
    c.imu_rms_mg = (uint16_t)(imu_mg >= 0 ? imu_mg : 0);
    return c;
}

/* Milliseconds since 03:00:00 UTC, the clock the replay below uses. */
#define T(m, s, ms) ((uint32_t)(((m) * 60 + (s)) * 1000 + (ms)))

static void test_real_driving_is_accepted(void)
{
    cairn_gnss_gate_t g;
    cairn_gnss_gate_reset(&g);
    /* 03:16:20-03:16:30, the last real fixes before the car parked: 12-13 m/s on 9-12
     * satellites, OBD at 44-48 kph, the IMU at 115-135 mg. */
    struct { uint32_t t; double v; int sats; int obd; int imu; } r[] = {
        { T(16, 20, 116), 13.27, 12, 48, 135 }, { T(16, 21, 212), 13.58, 12, 48, 135 },
        { T(16, 22, 406), 13.40, 12, 45, 115 }, { T(16, 24, 234), 12.98, 11, 45, 115 },
        { T(16, 25, 438), 12.70,  9, 45, 115 }, { T(16, 26, 630), 12.40, 10, 45, 115 },
        { T(16, 28, 261), 12.66, 12, 44, 115 }, { T(16, 29, 909), 11.82, 12, 44, 115 },
        { T(16, 30, 952), 11.31, 12, 44, 107 },
    };
    int accepted = 0;
    for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
        cairn_gnss_sample_t s = fix(r[i].v, r[i].sats);
        cairn_gnss_context_t c = ctx(r[i].obd, 400, r[i].imu);
        if (cairn_gnss_gate(&g, &s, r[i].t, &c) == CAIRN_GNSS_ACCEPT) accepted++;
    }
    CHECK("every real fix of the last minute before parking is accepted", accepted == 9);
}

static void test_the_phantom_run_is_refused(void)
{
    cairn_gnss_gate_t g;
    cairn_gnss_gate_reset(&g);
    cairn_gnss_sample_t last = fix(11.31, 12);
    cairn_gnss_context_t driving = ctx(44, 400, 107);
    cairn_gnss_gate(&g, &last, T(16, 30, 952), &driving);

    /*
     * The 37 fixes, as stored: time, speed, satellites, and what OBD and the IMU said.
     * The IMU is the last completed window's RMS (40 mg while the car crept, 32, then
     * 9 at idle); OBD answered 0-3 kph from 03:18:23.4 to 03:18:28.9 and nothing else
     * in the minute.
     */
    struct { uint32_t t; double v; int sats; int obd; uint32_t obd_age; int imu; } r[] = {
        { T(18, 11, 646), 48.02, 4, -1, 0, 40 }, { T(18, 12,  84), 47.78, 5, -1, 0, 40 },
        { T(18, 12, 937), 44.29, 5, -1, 0, 40 }, { T(18, 13, 721), 33.45, 5, -1, 0, 40 },
        { T(18, 14, 566), 20.89, 5, -1, 0, 40 }, { T(18, 16, 160), 10.21, 4, -1, 0, 40 },
        { T(18, 16, 968), 18.54, 3, -1, 0, 40 }, { T(18, 17, 792), 24.01, 3, -1, 0, 40 },
        { T(18, 18, 600), 31.39, 3, -1, 0, 40 },
        { T(18, 23,   1), 37.99, 6, -1, 0, 32 }, { T(18, 23, 324), 42.00, 7, -1, 0, 32 },
        { T(18, 23, 692), 44.64, 7,  0, 93, 32 }, { T(18, 24,  97), 48.77, 7, 3, 89, 32 },
        { T(18, 24, 506), 48.58, 8,  3, 101, 32 },
        { T(18, 44, 308), 47.80, 8, -1, 0, 9 }, { T(18, 44, 538), 46.92, 9, -1, 0, 9 },
        { T(18, 45, 612), 45.91, 9, -1, 0, 9 }, { T(18, 45, 877), 46.13, 7, -1, 0, 9 },
        { T(18, 46,  74), 46.24, 7, -1, 0, 9 }, { T(18, 46, 294), 46.36, 6, -1, 0, 9 },
        { T(18, 46, 524), 46.51, 6, -1, 0, 9 }, { T(18, 46, 742), 46.82, 6, -1, 0, 9 },
        { T(18, 48, 825), 47.27, 7, -1, 0, 9 }, { T(18, 49, 553), 47.16, 8, -1, 0, 9 },
        { T(18, 51, 620), 46.79, 6, -1, 0, 9 }, { T(18, 52, 710), 46.15, 8, -1, 0, 9 },
        { T(18, 52, 952), 44.78, 8, -1, 0, 9 }, { T(18, 53, 462), 43.98, 6, -1, 0, 9 },
        { T(18, 53, 683), 44.06, 6, -1, 0, 9 }, { T(18, 53, 907), 44.13, 6, -1, 0, 9 },
        { T(18, 54, 385), 43.80, 7, -1, 0, 9 }, { T(18, 54, 553), 43.77, 7, -1, 0, 9 },
        { T(18, 54, 775), 43.74, 7, -1, 0, 9 }, { T(18, 54, 999), 43.72, 6, -1, 0, 9 },
        { T(18, 58,  31), 42.88, 4, -1, 0, 9 }, { T(18, 58, 431), 45.46, 5, -1, 0, 9 },
        { T(18, 58, 611), 48.72, 4, -1, 0, 9 },
    };
    const size_t n = sizeof(r) / sizeof(r[0]);
    int accepted = 0;
    for (size_t i = 0; i < n; i++) {
        cairn_gnss_sample_t s = fix(r[i].v, r[i].sats);
        cairn_gnss_context_t c = ctx(r[i].obd, r[i].obd_age, r[i].imu);
        if (cairn_gnss_gate(&g, &s, r[i].t, &c) == CAIRN_GNSS_ACCEPT) accepted++;
    }
    CHECK("the replay has all 37 phantom fixes", n == 37);
    CHECK("none of the 37 phantom fixes is accepted", accepted == 0);
    CHECK("re-acquiring on 3-5 satellites is refused as settling",
          g.rejected[CAIRN_GNSS_REJECT_SETTLING] >= 9);
    CHECK("OBD at 0-3 kph refuses 45-49 m/s", g.rejected[CAIRN_GNSS_REJECT_OBD] == 3);
    CHECK("an idling accelerometer refuses the rest", g.rejected[CAIRN_GNSS_REJECT_STILL] >= 15);
}

static void test_a_hard_launch_is_not_refused(void)
{
    cairn_gnss_gate_t g;
    cairn_gnss_gate_reset(&g);
    cairn_gnss_sample_t s0 = fix(0.2, 11);
    cairn_gnss_context_t c0 = ctx(0, 100, 60);
    CHECK("standing still at a light", cairn_gnss_gate(&g, &s0, 10000, &c0) == CAIRN_GNSS_ACCEPT);
    /* 0 to 8.5 m/s in 1.8 s (4.7 m/s^2), against an OBD reading taken at the start. */
    cairn_gnss_sample_t s1 = fix(4.1, 11), s2 = fix(8.5, 11);
    cairn_gnss_context_t c1 = ctx(0, 900, 180), c2 = ctx(0, 1800, 220);
    CHECK("pulling away", cairn_gnss_gate(&g, &s1, 10900, &c1) == CAIRN_GNSS_ACCEPT);
    CHECK("past 29 kph while OBD still says 0 from 1.8 s ago",
          cairn_gnss_gate(&g, &s2, 11800, &c2) == CAIRN_GNSS_ACCEPT);
}

static void test_physics(void)
{
    cairn_gnss_gate_t g;
    cairn_gnss_gate_reset(&g);
    cairn_gnss_sample_t a = fix(20.0, 12), b = fix(32.0, 12), c = fix(21.0, 12), d = fix(35.0, 12);
    cairn_gnss_gate(&g, &a, 1000, NULL);
    CHECK("12 m/s gained in a second is refused",
          cairn_gnss_gate(&g, &b, 2000, NULL) == CAIRN_GNSS_REJECT_ACCEL);
    CHECK("and the reference stays the last good fix",
          cairn_gnss_gate(&g, &c, 2200, NULL) == CAIRN_GNSS_ACCEPT);
    /* Beyond the window the change is not judged: a fix 3 s later can be far off. */
    CHECK("speed is not compared across more than 2 s",
          cairn_gnss_gate(&g, &d, 5300, NULL) == CAIRN_GNSS_ACCEPT);

    cairn_gnss_gate_t h;
    cairn_gnss_gate_reset(&h);
    cairn_gnss_sample_t five = fix(10.0, 5), six = fix(10.0, 6);
    CHECK("a first fix on five satellites waits", cairn_gnss_gate(&h, &five, 1000, NULL) == CAIRN_GNSS_REJECT_SETTLING);
    CHECK("on six it is taken", cairn_gnss_gate(&h, &six, 2000, NULL) == CAIRN_GNSS_ACCEPT);
    CHECK("once settled, five satellites are enough", cairn_gnss_gate(&h, &five, 3000, NULL) == CAIRN_GNSS_ACCEPT);
    CHECK("after a 31 s silence it settles again", cairn_gnss_gate(&h, &five, 34000, NULL) == CAIRN_GNSS_REJECT_SETTLING);

    cairn_gnss_gate_t k;
    cairn_gnss_gate_reset(&k);
    cairn_gnss_sample_t slow = fix(3.0, 12);
    cairn_gnss_context_t idle = ctx(0, 100, 9);
    CHECK("a slow fix is not judged by the accelerometer",
          cairn_gnss_gate(&k, &slow, 1000, &idle) == CAIRN_GNSS_ACCEPT);
}

/* ── NMEA ─────────────────────────────────────────────────────────────────── */

/* Appends "$body*CS\r\n" to buf. */
static void sentence(char *buf, const char *body)
{
    unsigned char x = 0;
    for (const char *p = body; *p; p++) x ^= (unsigned char)*p;
    char s[160];
    snprintf(s, sizeof(s), "$%s*%02X\r\n", body, x);
    strcat(buf, s);
}

/* A GGA and RMC pair at hhmmss.ss, 12 satellites, 25 knots on a course of 45. */
static void pair(char *buf, const char *tod, double lat_min)
{
    char b[160];
    snprintf(b, sizeof(b), "GNGGA,%s,34%07.4f,N,11828.0000,W,1,12,0.9,30.0,M,-33.0,M,,", tod, lat_min);
    sentence(buf, b);
    snprintf(b, sizeof(b), "GNRMC,%s,A,34%07.4f,N,11828.0000,W,25.0,45.0,151026,,,A", tod, lat_min);
    sentence(buf, b);
}

static void test_nmea_pairs_and_ages(void)
{
    char buf[1024] = "";
    pair(buf, "031820.00", 2.0000);
    pair(buf, "031820.20", 2.0010);
    pair(buf, "031820.40", 2.0020);
    cairn_nmea_state_t st;
    cairn_nmea_reset(&st);
    cairn_nmea_fix_t out[8];
    uint32_t ref = cairn_nmea_parse_tod("031820.50");
    int n = cairn_nmea_collect(buf, strlen(buf), ref, &st, out, 8);
    CHECK("three fixes from a 5 Hz buffer", n == 3);
    CHECK("each has its own age, not the poll's", n == 3 && out[0].age_ms == 500 &&
                                                  out[1].age_ms == 300 && out[2].age_ms == 100);
    CHECK("speed is the RMC's, not zero", n == 3 && out[0].sample.speed_cmps == 1286);
    CHECK("course is the RMC's", n == 3 && out[0].sample.heading_cdeg == 4500);
    CHECK("HDOP is x100 as the format says", n == 3 && out[0].sample.hdop_e2 == 90);
    CHECK("position", n == 3 && out[0].sample.lat_e7 == 340333333 && out[0].sample.lon_e7 == -1184666667);
}

static void test_nmea_stale_buffer(void)
{
    /* The receiver says it is 03:18:58; the co-processor hands back what it held from
     * 03:16:30, before the car parked. None of it may be stamped as now. */
    char buf[1024] = "";
    pair(buf, "031630.00", 2.0000);
    pair(buf, "031630.20", 2.0010);
    cairn_nmea_state_t st;
    cairn_nmea_reset(&st);
    cairn_nmea_fix_t out[8];
    int n = cairn_nmea_collect(buf, strlen(buf), cairn_nmea_parse_tod("031858.00"), &st, out, 8);
    CHECK("a stale buffer yields nothing", n == 0);
    CHECK("and is counted", st.stale_buffers == 1);

    /* Within the allowance it is current. */
    cairn_nmea_reset(&st);
    n = cairn_nmea_collect(buf, strlen(buf), cairn_nmea_parse_tod("031631.50"), &st, out, 8);
    CHECK("1.3 s behind the receiver is not stale", n == 2);
}

static void test_nmea_repeats_across_polls(void)
{
    char a[1024] = "", b[1024] = "";
    pair(a, "031820.00", 2.0000);
    pair(a, "031820.20", 2.0010);
    /* The next poll returns the tail of the last one again, then something new. */
    pair(b, "031820.20", 2.0010);
    pair(b, "031820.40", 2.0020);
    cairn_nmea_state_t st;
    cairn_nmea_reset(&st);
    cairn_nmea_fix_t out[8];
    int n1 = cairn_nmea_collect(a, strlen(a), CAIRN_NMEA_NO_TOD, &st, out, 8);
    int n2 = cairn_nmea_collect(b, strlen(b), CAIRN_NMEA_NO_TOD, &st, out, 8);
    CHECK("first poll", n1 == 2);
    CHECK("a fix read before is not written twice", n2 == 1 && out[0].tod_ms == cairn_nmea_parse_tod("031820.40"));
    CHECK("the repeat is counted", st.repeats == 1);
}

static void test_nmea_damage(void)
{
    char buf[1024] = "";
    pair(buf, "031820.00", 2.0000);
    /* A GGA with no RMC beside it: no speed to write. */
    sentence(buf, "GNGGA,031820.20,3402.0010,N,11828.0000,W,1,12,0.9,30.0,M,-33.0,M,,");
    /* A corrupted checksum. */
    strcat(buf, "$GNGGA,031820.40,3402.0020,N,11828.0000,W,1,12,0.9,30.0,M,-33.0,M,,*00\r\n");
    /* The serial read cut the last sentence off. */
    strcat(buf, "$GNGGA,031820.60,3402.00");
    cairn_nmea_state_t st;
    cairn_nmea_reset(&st);
    cairn_nmea_fix_t out[8];
    int n = cairn_nmea_collect(buf, strlen(buf), CAIRN_NMEA_NO_TOD, &st, out, 8);
    CHECK("only the whole, paired, checksummed fix survives", n == 1);
    CHECK("an unpaired GGA is dropped, not written at 0 m/s", st.unpaired == 1);
    CHECK("bad and truncated sentences are counted once each", st.bad_checksum == 2);

    char inv[512] = "";
    sentence(inv, "GNGGA,031820.00,3402.0000,N,11828.0000,W,0,00,99.9,,M,,M,,");
    sentence(inv, "GNRMC,031820.00,V,,,,,,,151026,,,N");
    cairn_nmea_reset(&st);
    CHECK("no fix in either sentence, nothing out", cairn_nmea_collect(inv, strlen(inv), CAIRN_NMEA_NO_TOD, &st, out, 8) == 0);
}

static void test_nmea_midnight(void)
{
    CHECK("tod parse", cairn_nmea_parse_tod("235959.95") == 86399950u);
    CHECK("tod refuses junk", cairn_nmea_parse_tod("2359") == CAIRN_NMEA_NO_TOD &&
                              cairn_nmea_parse_tod("246000") == CAIRN_NMEA_NO_TOD);
    CHECK("00:00:00.10 is after 23:59:59.90", cairn_nmea_tod_diff(100, 86399900u) == 200);
    char buf[1024] = "";
    pair(buf, "235959.90", 2.0000);
    pair(buf, "000000.10", 2.0010);
    cairn_nmea_state_t st;
    cairn_nmea_reset(&st);
    cairn_nmea_fix_t out[8];
    int n = cairn_nmea_collect(buf, strlen(buf), cairn_nmea_parse_tod("000000.30"), &st, out, 8);
    CHECK("a buffer across midnight keeps both, in order", n == 2 && out[0].age_ms == 400 && out[1].age_ms == 200);
}

int main(void)
{
    printf("gnss\n");
    test_real_driving_is_accepted();
    test_the_phantom_run_is_refused();
    test_a_hard_launch_is_not_refused();
    test_physics();
    test_nmea_pairs_and_ages();
    test_nmea_stale_buffer();
    test_nmea_repeats_across_polls();
    test_nmea_damage();
    test_nmea_midnight();
    printf("gnss: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
