/*
 * The LTE digest generator: streaming reduction, byte budget, and the one rule
 * that matters most, that a digest acknowledgement can never authorise a prune.
 *
 * PROVISIONAL, like the code under test: contracts/digest/v1 is unreleased, so
 * there are no contract vectors here and the serialised layout is a placeholder.
 * What these rows pin is the behaviour that must survive the contract: bounded
 * state, a hard budget the config cannot raise, and the receipt/ack separation.
 * No real trips exist yet; every trip below is synthetic (see the generator).
 *
 *   ./digest [table]     rows, then the size table
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "board_config.h"
#include "cairn_digest.h"
#include "cairn_digest_ack.h"
#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_kv.h"
#include "cairn_log.h"
#include "cairn_platform.h"
#include "cairn_prune.h"
#include "cairn_store.h"

void cairn_host_seed(uint64_t seed);
void cairn_kv_host_set_path(const char *path);

/* ── harness ──────────────────────────────────────────────────────────────── */

static int  g_pass, g_fail;
static char g_failures[64][512];
static int  g_failure_count;
static const char *g_row;

static void fail(const char *fmt, ...)
{
    char detail[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    if (g_failure_count < 64) snprintf(g_failures[g_failure_count++], 512, "%s: %s", g_row, detail);
}

#define CHECK(cond, ...)       \
    do {                       \
        if (!(cond)) {         \
            fail(__VA_ARGS__); \
            return false;      \
        }                      \
    } while (0)

/* ── a synthetic trip ─────────────────────────────────────────────────────── */

typedef enum { TRIP_CITY, TRIP_HIGHWAY, TRIP_CHAOS } trip_kind_t;

typedef struct {
    trip_kind_t kind;
    double      lat, lon, heading, speed, target_speed;
    uint64_t    t_ms, rng;
    uint32_t    hz, n;
    double      turn_left;     /* degrees still to turn */
} gen_t;

static uint32_t rnd(gen_t *g)
{
    g->rng ^= g->rng >> 12;
    g->rng ^= g->rng << 25;
    g->rng ^= g->rng >> 27;
    return (uint32_t)((g->rng * 0x2545F4914F6CDD1DULL) >> 32);
}

static double urand(gen_t *g) { return (double)rnd(g) / 4294967296.0; }              /* [0,1) */
static double noise(gen_t *g, double amp) { return (urand(g) + urand(g) + urand(g) - 1.5) * amp; }

static void gen_init(gen_t *g, trip_kind_t kind, uint32_t hz, uint64_t seed)
{
    memset(g, 0, sizeof(*g));
    g->kind = kind;
    g->lat = 37.3300;
    g->lon = -121.9000;
    g->heading = 40.0;
    g->hz = hz;
    g->t_ms = 1760000000000ULL;
    g->rng = seed * 0x9E3779B97F4A7C15ULL + 12345;
    g->target_speed = (kind == TRIP_HIGHWAY) ? 29.0 : 10.0;
    g->speed = g->target_speed;
}

static void gen_next(gen_t *g, cairn_digest_sample_t *s)
{
    double dt = 1.0 / g->hz;
    g->n++;

    switch (g->kind) {
    case TRIP_CITY:
        if (g->n % (20u * g->hz) == 0) {
            static const double sp[] = { 0.0, 6.0, 12.0, 16.0 };
            g->target_speed = sp[rnd(g) % 4];
        }
        if (g->n % (25u * g->hz) == 0 && urand(g) < 0.5) g->turn_left = (urand(g) < 0.5) ? 90.0 : -90.0;
        break;
    case TRIP_HIGHWAY:
        g->heading += noise(g, 0.6) * dt;
        if (g->n % (600u * g->hz) == 0) g->turn_left = (urand(g) - 0.5) * 60.0;
        break;
    case TRIP_CHAOS:
        g->heading = urand(g) * 360.0;
        g->target_speed = 35.0;
        break;
    }

    double rate = 30.0 * dt;  /* degrees per sample while turning */
    if (g->turn_left != 0.0) {
        double step = g->turn_left > 0 ? fmin(rate, g->turn_left) : fmax(-rate, g->turn_left);
        g->heading += step;
        g->turn_left -= step;
    }
    g->speed += (g->target_speed - g->speed) * fmin(1.0, 0.3 * dt);

    double h = g->heading * M_PI / 180.0;
    double dn = g->speed * dt * cos(h), de = g->speed * dt * sin(h);
    g->lat += dn / 111195.0;
    g->lon += de / (111195.0 * cos(g->lat * M_PI / 180.0));
    g->t_ms += 1000u / g->hz;

    memset(s, 0, sizeof(*s));
    s->t_ms = g->t_ms;
    s->has_fix = true;
    s->lat_e7 = (int32_t)llround((g->lat + noise(g, 2.0) / 111195.0) * 1e7);
    s->lon_e7 = (int32_t)llround((g->lon + noise(g, 2.0) / 111195.0) * 1e7);
    s->speed_cmps = (uint16_t)(g->speed * 100.0);
    s->ch_mask = 0x07;
    s->ch[CAIRN_DIGEST_CH_BOOST]  = (int32_t)(g->speed * 3.0) + (int32_t)(rnd(g) % 20);
    s->ch[CAIRN_DIGEST_CH_TRIM]   = (int32_t)(rnd(g) % 80) - 40;
    s->ch[CAIRN_DIGEST_CH_LAMBDA] = 1000 + (int32_t)(rnd(g) % 100) - 50;
}

typedef struct {
    int32_t *lat, *lon;  /* optional record of every fix fed */
    size_t   n, cap;
} orig_t;

static void feed(cairn_digest_t *d, trip_kind_t kind, uint32_t secs, uint32_t hz, uint64_t seed,
                 orig_t *orig)
{
    gen_t g;
    gen_init(&g, kind, hz, seed);
    cairn_digest_sample_t s;
    for (uint64_t i = 0; i < (uint64_t)secs * hz; i++) {
        gen_next(&g, &s);
        cairn_digest_add(d, &s);
        if (orig != NULL && orig->n < orig->cap) {
            orig->lat[orig->n] = s.lat_e7;
            orig->lon[orig->n] = s.lon_e7;
            orig->n++;
        }
    }
}

static cairn_digest_meta_t test_meta(void)
{
    cairn_digest_meta_t m;
    memset(&m, 0, sizeof(m));
    for (int i = 0; i < 32; i++) m.trip_root[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 16; i++) { m.vehicle_id[i] = (uint8_t)i; m.assignment_id[i] = (uint8_t)(0x40 + i); }
    m.engine_profile_id = 7;
    m.engine_profile_version = 3;
    return m;
}

/* ── a decoder, for the tests only: the device never reads a digest ───────── */

typedef struct {
    const uint8_t *p;
    size_t         n, at;
    bool           bad;
} rd_t;

static uint64_t r_varint(rd_t *r)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        if (r->at >= r->n) { r->bad = true; return 0; }
        uint8_t b = r->p[r->at++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
    }
    r->bad = true;
    return 0;
}

static int64_t r_zigzag(rd_t *r)
{
    uint64_t u = r_varint(r);
    return (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
}

static uint8_t r_u8(rd_t *r)
{
    if (r->at >= r->n) { r->bad = true; return 0; }
    return r->p[r->at++];
}

typedef struct {
    uint8_t  flags;
    uint8_t  trip_root[32], vehicle[16], assignment[16];
    uint64_t profile_id, profile_ver, start_ms, duration_s, distance_m, max_speed, moving_s, fixes, rejected;
    uint8_t  mask;
    struct { uint64_t count; int64_t min, max, mean; } ch[CAIRN_DIGEST_NCHAN];
    uint64_t health;
    uint8_t  nev, ev_type[CAIRN_DIGEST_MAX_EVENTS];
    uint64_t ev_t[CAIRN_DIGEST_MAX_EVENTS];
    uint8_t  coord_exp;
    uint32_t npts;
    int32_t  lat[CAIRN_DIGEST_POINTS_HARD_CEILING], lon[CAIRN_DIGEST_POINTS_HARD_CEILING];
    uint32_t t[CAIRN_DIGEST_POINTS_HARD_CEILING];
} dec_t;

static bool decode(const uint8_t *b, size_t n, dec_t *o)
{
    rd_t r = { b, n, 0, false };
    memset(o, 0, sizeof(*o));
    if (n < 4 || b[0] != 'C' || b[1] != 'D' || b[2] != 'G' || b[3] != 0xD0) return false;
    r.at = 4;
    o->flags = r_u8(&r);
    for (int i = 0; i < 32; i++) o->trip_root[i] = r_u8(&r);
    for (int i = 0; i < 16; i++) o->vehicle[i] = r_u8(&r);
    for (int i = 0; i < 16; i++) o->assignment[i] = r_u8(&r);
    o->profile_id = r_varint(&r);
    o->profile_ver = r_varint(&r);
    o->start_ms = r_varint(&r);
    o->duration_s = r_varint(&r);
    o->distance_m = r_varint(&r);
    o->max_speed = r_varint(&r);
    o->moving_s = r_varint(&r);
    o->fixes = r_varint(&r);
    o->rejected = r_varint(&r);
    o->mask = r_u8(&r);
    for (int i = 0; i < CAIRN_DIGEST_NCHAN; i++) {
        if (!(o->mask & (1 << i))) continue;
        o->ch[i].count = r_varint(&r);
        o->ch[i].min = r_zigzag(&r);
        o->ch[i].max = r_zigzag(&r);
        o->ch[i].mean = r_zigzag(&r);
    }
    o->health = r_varint(&r);
    o->nev = r_u8(&r);
    if (o->nev > CAIRN_DIGEST_MAX_EVENTS) return false;
    for (int i = 0; i < o->nev; i++) {
        o->ev_type[i] = r_u8(&r);
        o->ev_t[i] = r_varint(&r);
    }
    o->coord_exp = r_u8(&r);
    o->npts = (uint32_t)r_varint(&r);
    if (o->npts > CAIRN_DIGEST_POINTS_HARD_CEILING) return false;
    int64_t la = 0, lo = 0;
    uint64_t t = 0;
    for (uint32_t i = 0; i < o->npts; i++) {
        la += r_zigzag(&r);
        lo += r_zigzag(&r);
        if (o->flags & 0x01) t += r_varint(&r);
        o->lat[i] = (int32_t)la;
        o->lon[i] = (int32_t)lo;
        o->t[i] = (uint32_t)t;
    }
    return !r.bad && r.at == n;  /* nothing missing, nothing trailing */
}

static int32_t quant5(int32_t e7)
{
    return e7 >= 0 ? (e7 + 50) / 100 : (e7 - 50) / 100;
}

/* ── geometry for judging the reduction ───────────────────────────────────── */

/* Worst distance (metres) from any original fix to the reduced polyline. */
static double max_deviation_m(const orig_t *o, const int32_t *rl, const int32_t *ro, size_t rn)
{
    double coslat = cos((double)o->lat[0] * 1e-7 * M_PI / 180.0);
    double worst = 0;
    size_t stride = o->n > 4000 ? o->n / 4000 : 1;

    for (size_t i = 0; i < o->n; i += stride) {
        double px = (double)o->lon[i] * 1e-7 * 111195.0 * coslat;
        double py = (double)o->lat[i] * 1e-7 * 111195.0;
        double best = 1e18;
        for (size_t k = 0; k + 1 < rn; k++) {
            double ax = (double)ro[k] * 1e-7 * 111195.0 * coslat, ay = (double)rl[k] * 1e-7 * 111195.0;
            double bx = (double)ro[k + 1] * 1e-7 * 111195.0 * coslat, by = (double)rl[k + 1] * 1e-7 * 111195.0;
            double vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
            double len2 = vx * vx + vy * vy;
            double t = len2 > 0 ? (vx * wx + vy * wy) / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            double dx = px - (ax + t * vx), dy = py - (ay + t * vy);
            double dd = sqrt(dx * dx + dy * dy);
            if (dd < best) best = dd;
        }
        if (best > worst) worst = best;
    }
    return worst;
}

/* ── rows: the reduction ──────────────────────────────────────────────────── */

static cairn_digest_t g_d;   /* static: about 6 KiB, never on a task stack */
static uint8_t        g_out[CAIRN_DIGEST_BYTES_HARD_CEILING * 2];

static cairn_digest_config_t cfg_with(uint32_t budget, uint32_t points)
{
    cairn_digest_config_t c;
    cairn_digest_config_defaults(&c);
    c.byte_budget = budget;
    c.max_points = points;
    return c;
}

/* A route with one corner keeps the corner when only three points are allowed. */
static bool row_corner_survives(void)
{
    cairn_digest_config_t c = cfg_with(2048, 3);
    c.min_spacing_m = 5;
    cairn_digest_init(&g_d, &c);

    cairn_digest_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_fix = true;
    s.speed_cmps = CAIRN_DIGEST_SPEED_UNKNOWN;
    uint64_t t = 1000;
    int32_t lat0 = 370000000, lon0 = -1220000000;

    /* 2 km east in 10 m steps, then 2 km north. */
    for (int i = 0; i <= 200; i++) {
        s.t_ms = t += 1000;
        s.lat_e7 = lat0;
        s.lon_e7 = lon0 + i * 1126;   /* ~10 m at this latitude */
        cairn_digest_add(&g_d, &s);
    }
    for (int i = 1; i <= 200; i++) {
        s.t_ms = t += 1000;
        s.lat_e7 = lat0 + i * 900;
        s.lon_e7 = lon0 + 200 * 1126;
        cairn_digest_add(&g_d, &s);
    }

    CHECK(cairn_digest_route_points(&g_d) == 3, "route has %u points, want 3",
          cairn_digest_route_points(&g_d));
    int32_t la[8], lo[8];
    CHECK(cairn_digest_route(&g_d, la, lo, 8) == 3, "route copy");
    CHECK(la[0] == lat0 && lo[0] == lon0, "route does not start at the first fix");
    CHECK(la[2] == lat0 + 200 * 900, "route does not end at the last fix");
    /* The middle point must be the corner, not a point part-way along a leg. */
    CHECK(abs(la[1] - lat0) < 3000 && abs(lo[1] - (lon0 + 200 * 1126)) < 3000,
          "the corner was dropped: middle point is (%d,%d)", la[1] - lat0, lo[1] - lon0);
    return true;
}

/* Bounded state: a very long trip never holds more than the cap, and the working
 * state is a fixed size that fits a small RAM budget. */
static bool row_bounded_state(void)
{
    CHECK(sizeof(cairn_digest_t) <= 8192, "context is %zu bytes", sizeof(cairn_digest_t));

    cairn_digest_config_t c = cfg_with(2048, 64);
    cairn_digest_init(&g_d, &c);

    gen_t g;
    gen_init(&g, TRIP_CITY, 10, 99);
    cairn_digest_sample_t s;
    uint32_t peak = 0;
    for (uint64_t i = 0; i < 2000000ULL; i++) {   /* 55 hours at 10 Hz */
        gen_next(&g, &s);
        cairn_digest_add(&g_d, &s);
        uint32_t n = cairn_digest_route_points(&g_d);
        if (n > peak) peak = n;
        CHECK(n <= 64, "route grew to %u points (cap 64)", n);
    }
    CHECK(peak >= 60, "the cap was never approached (%u): the test is not exercising it", peak);

    size_t len = 0;
    cairn_digest_meta_t m = test_meta();
    CHECK(cairn_digest_finish(&g_d, &m, g_out, sizeof(g_out), &len) == CAIRN_DIGEST_OK, "finish");
    CHECK(len <= 2048, "len %zu", len);
    return true;
}

/* The reduced route stays close to what was driven. */
static bool row_route_accuracy(void)
{
    static int32_t lat[40000], lon[40000];
    orig_t o = { lat, lon, 0, 40000 };

    struct { trip_kind_t k; uint32_t secs; double limit_m; const char *name; } cases[] = {
        { TRIP_CITY,    1800, 150.0, "city 30 min" },
        { TRIP_HIGHWAY, 7200, 350.0, "highway 2 h" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cairn_digest_config_t c = cfg_with(4096, 192);
        cairn_digest_init(&g_d, &c);
        o.n = 0;
        feed(&g_d, cases[i].k, cases[i].secs, 1, 5, &o);

        int32_t rl[200], ro[200];
        size_t n = cairn_digest_route(&g_d, rl, ro, 200);
        double dev = max_deviation_m(&o, rl, ro, n);
        CHECK(dev < cases[i].limit_m, "%s: worst deviation %.1f m (limit %.0f)", cases[i].name, dev,
              cases[i].limit_m);
    }
    return true;
}

/* ── rows: statistics ─────────────────────────────────────────────────────── */

static bool row_stats_distance_and_jitter(void)
{
    cairn_digest_config_t c = cfg_with(2048, 64);
    cairn_digest_init(&g_d, &c);

    cairn_digest_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_fix = true;
    uint64_t t = 0;

    /* 20 km due north at 25 m/s, one fix a second. 20000 m of latitude is
     * 20000/111195 degrees. */
    for (int i = 0; i <= 800; i++) {
        s.t_ms = t += 1000;
        s.lat_e7 = 370000000 + (int32_t)llround(i * 25.0 / 111195.0 * 1e7);
        s.lon_e7 = -1220000000;
        s.speed_cmps = 2500;
        cairn_digest_add(&g_d, &s);
    }
    double km = (double)cairn_digest_stats(&g_d)->distance_mm / 1e6;
    CHECK(fabs(km - 20.0) < 0.1, "distance %.3f km, want 20.0", km);
    CHECK(cairn_digest_stats(&g_d)->max_speed_cmps == 2500, "max speed");
    CHECK(cairn_digest_stats(&g_d)->moving_time_ms >= 799000, "moving time %u",
          cairn_digest_stats(&g_d)->moving_time_ms);

    /* Then an hour parked, jittering by a few metres, with and without a speed. */
    uint64_t before = cairn_digest_stats(&g_d)->distance_mm;
    gen_t g;
    gen_init(&g, TRIP_CITY, 1, 3);
    for (int i = 0; i < 3600; i++) {
        s.t_ms = t += 1000;
        s.lat_e7 = 370000000 + (int32_t)llround(800 * 25.0 / 111195.0 * 1e7) + (int32_t)(noise(&g, 6.0) / 111195.0 * 1e7);
        s.lon_e7 = -1220000000 + (int32_t)(noise(&g, 6.0) / 100000.0 * 1e7);
        s.speed_cmps = (i % 2) ? 0 : CAIRN_DIGEST_SPEED_UNKNOWN;
        cairn_digest_add(&g_d, &s);
    }
    double extra = (double)(cairn_digest_stats(&g_d)->distance_mm - before) / 1000.0;
    CHECK(extra < 300.0, "an hour parked added %.0f m of distance", extra);
    return true;
}

static bool row_stats_rejects_glitches(void)
{
    cairn_digest_config_t c = cfg_with(2048, 64);
    cairn_digest_init(&g_d, &c);

    cairn_digest_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_fix = true;
    s.speed_cmps = 1500;
    uint64_t t = 0;
    for (int i = 0; i < 100; i++) {
        s.t_ms = t += 1000;
        s.lat_e7 = 370000000 + i * 1350;   /* ~15 m a second */
        s.lon_e7 = -1220000000;
        cairn_digest_add(&g_d, &s);
        if (i == 50) {   /* one teleport, 300 km away, then back */
            cairn_digest_sample_t bad = s;
            bad.t_ms = t += 1000;
            bad.lat_e7 += 30000000;
            cairn_digest_add(&g_d, &bad);
        }
    }
    const cairn_digest_stats_t *st = cairn_digest_stats(&g_d);
    CHECK(st->fixes_rejected == 1, "rejected %u fixes, want 1", st->fixes_rejected);
    double km = (double)st->distance_mm / 1e6;
    CHECK(km < 1.6, "a single glitch inflated distance to %.1f km", km);

    /* Null island and out-of-range fixes are not places. */
    cairn_digest_sample_t z = s;
    z.t_ms = t += 1000; z.lat_e7 = 0; z.lon_e7 = 0;
    cairn_digest_add(&g_d, &z);
    z.t_ms = t += 1000; z.lat_e7 = 950000000;
    cairn_digest_add(&g_d, &z);
    CHECK(st->fixes_rejected == 3, "null island / out of range not rejected");

    /* If the *first* fix was the bad one, the trip must not be rejected for good. */
    cairn_digest_init(&g_d, &c);
    cairn_digest_sample_t first = s;
    first.t_ms = 1000; first.lat_e7 = 800000000;
    cairn_digest_add(&g_d, &first);
    for (int i = 0; i < 40; i++) {
        s.t_ms = 2000 + (uint64_t)i * 1000;
        s.lat_e7 = 370000000 + i * 1350;
        cairn_digest_add(&g_d, &s);
    }
    CHECK(cairn_digest_stats(&g_d)->fixes_used > 30, "the stream never recovered from a bad first fix");
    return true;
}

static bool row_stats_channels_health_events(void)
{
    cairn_digest_config_t c = cfg_with(2048, 32);
    cairn_digest_init(&g_d, &c);

    cairn_digest_sample_t s;
    memset(&s, 0, sizeof(s));
    int32_t vals[] = { -40, 10, 30, 0 };
    for (int i = 0; i < 4; i++) {
        s.t_ms = 5000 + (uint64_t)i * 1000;
        s.ch_mask = 0x02;
        s.ch[CAIRN_DIGEST_CH_TRIM] = vals[i];
        cairn_digest_add(&g_d, &s);
    }
    cairn_digest_note_health(&g_d, 0x5);
    cairn_digest_note_health(&g_d, 0x20);
    CHECK(cairn_digest_note_event(&g_d, 9, 7000), "event refused");

    uint8_t out[CAIRN_DIGEST_BYTES_HARD_CEILING];
    size_t len = 0;
    cairn_digest_meta_t m = test_meta();
    CHECK(cairn_digest_finish(&g_d, &m, out, sizeof(out), &len) == CAIRN_DIGEST_OK, "finish");

    static dec_t dd;
    CHECK(decode(out, len, &dd), "decode failed");
    CHECK(dd.mask == 0x02, "channel mask %02x", dd.mask);
    CHECK(dd.ch[1].count == 4 && dd.ch[1].min == -40 && dd.ch[1].max == 30 && dd.ch[1].mean == 0,
          "channel stats %llu %lld %lld %lld", (unsigned long long)dd.ch[1].count,
          (long long)dd.ch[1].min, (long long)dd.ch[1].max, (long long)dd.ch[1].mean);
    CHECK(dd.health == 0x25, "health %llx", (unsigned long long)dd.health);
    CHECK(dd.nev == 1 && dd.ev_type[0] == 9 && dd.ev_t[0] == 2, "event");
    CHECK(dd.duration_s == 3 && dd.start_ms == 5000, "times");
    CHECK(memcmp(dd.trip_root, m.trip_root, 32) == 0 && dd.profile_id == 7 && dd.profile_ver == 3,
          "identity");

    /* Events beyond the list are counted, not silently lost. */
    for (int i = 0; i < 20; i++) cairn_digest_note_event(&g_d, 1, 8000);
    CHECK(cairn_digest_stats(&g_d)->events_dropped == 20 - (CAIRN_DIGEST_MAX_EVENTS - 1),
          "dropped %u", cairn_digest_stats(&g_d)->events_dropped);
    return true;
}

/* ── rows: the wire layer and the budget ──────────────────────────────────── */

static bool row_encoding_round_trips(void)
{
    cairn_digest_config_t c = cfg_with(4096, 120);
    cairn_digest_init(&g_d, &c);
    feed(&g_d, TRIP_CITY, 1800, 1, 11, NULL);

    cairn_digest_meta_t m = test_meta();
    size_t len = 0;
    CHECK(cairn_digest_finish(&g_d, &m, g_out, sizeof(g_out), &len) == CAIRN_DIGEST_OK, "finish");

    static dec_t dd;
    CHECK(decode(g_out, len, &dd), "decode failed (%zu bytes)", len);
    CHECK(dd.coord_exp == 5, "coord exponent");

    int32_t rl[200], ro[200];
    size_t n = cairn_digest_route(&g_d, rl, ro, 200);
    CHECK(dd.npts == n, "decoded %u points, route has %zu", dd.npts, n);
    for (size_t i = 0; i < n; i++) {
        CHECK(dd.lat[i] == quant5(rl[i]) && dd.lon[i] == quant5(ro[i]),
              "point %zu: delta decode %d,%d != %d,%d", i, dd.lat[i], dd.lon[i], quant5(rl[i]), quant5(ro[i]));
        if (i > 0) CHECK(dd.t[i] >= dd.t[i - 1], "time went backwards at %zu", i);
    }
    CHECK(dd.fixes == cairn_digest_stats(&g_d)->fixes_used, "fix count");
    return true;
}

static bool row_deterministic(void)
{
    static uint8_t a[CAIRN_DIGEST_BYTES_HARD_CEILING], b[CAIRN_DIGEST_BYTES_HARD_CEILING];
    size_t la = 0, lb = 0;
    cairn_digest_meta_t m = test_meta();
    cairn_digest_config_t c = cfg_with(1024, 96);

    cairn_digest_init(&g_d, &c);
    feed(&g_d, TRIP_CITY, 900, 5, 21, NULL);
    CHECK(cairn_digest_finish(&g_d, &m, a, sizeof(a), &la) == CAIRN_DIGEST_OK, "finish a");

    cairn_digest_init(&g_d, &c);
    feed(&g_d, TRIP_CITY, 900, 5, 21, NULL);
    CHECK(cairn_digest_finish(&g_d, &m, b, sizeof(b), &lb) == CAIRN_DIGEST_OK, "finish b");

    CHECK(la == lb && memcmp(a, b, la) == 0, "same input gave different bytes");

    /* Finishing again re-encodes the same reduced state. */
    size_t lc = 0;
    static uint8_t cbuf[CAIRN_DIGEST_BYTES_HARD_CEILING];
    CHECK(cairn_digest_finish(&g_d, &m, cbuf, sizeof(cbuf), &lc) == CAIRN_DIGEST_OK, "finish again");
    CHECK(lc == lb && memcmp(cbuf, b, lb) == 0, "second finish differs");

    /* And the stream is over. */
    cairn_digest_sample_t s;
    memset(&s, 0, sizeof(s));
    CHECK(cairn_digest_add(&g_d, &s) == CAIRN_DIGEST_FINISHED, "add after finish accepted");
    return true;
}

/*
 * THE BUDGET. For every budget, every kind of trip, the output never exceeds the
 * effective budget, the effective budget never exceeds the compiled ceiling, and
 * nothing is written past the budget.
 */
static bool row_budget_never_exceeded(void)
{
    const uint32_t budgets[] = { 0, 1, 100, 384, 400, 512, 700, 1024, 2048, 4096, 4097, 100000, 0xFFFFFFFFu };
    const trip_kind_t kinds[] = { TRIP_CITY, TRIP_HIGHWAY, TRIP_CHAOS };
    cairn_digest_meta_t m = test_meta();

    for (size_t bi = 0; bi < sizeof(budgets) / sizeof(budgets[0]); bi++) {
        for (size_t ki = 0; ki < 3; ki++) {
            cairn_digest_config_t c = cfg_with(budgets[bi], 0xFFFF);
            uint32_t eff = cairn_digest_effective_budget(&c);
            CHECK(eff <= CAIRN_DIGEST_BYTES_HARD_CEILING, "budget %u -> effective %u beyond the ceiling",
                  budgets[bi], eff);
            CHECK(eff >= CAIRN_DIGEST_BYTES_FLOOR, "budget %u -> effective %u below the floor", budgets[bi], eff);

            cairn_digest_init(&g_d, &c);
            for (int e = 0; e < 12; e++) cairn_digest_note_event(&g_d, (uint8_t)e, 1000u * (uint32_t)e);
            feed(&g_d, kinds[ki], 3600, 1, 31 + bi, NULL);

            memset(g_out, 0xA5, sizeof(g_out));
            size_t len = 0;
            cairn_digest_status_t st = cairn_digest_finish(&g_d, &m, g_out, sizeof(g_out), &len);
            CHECK(st == CAIRN_DIGEST_OK, "budget %u kind %zu: %s", budgets[bi], ki, cairn_digest_status_name(st));
            CHECK(len <= eff, "budget %u kind %zu: %zu bytes exceeds the effective budget %u", budgets[bi], ki, len, eff);
            for (size_t i = eff; i < sizeof(g_out); i++) {
                CHECK(g_out[i] == 0xA5, "budget %u kind %zu: byte %zu past the budget was written", budgets[bi], ki, i);
            }
            static dec_t dd;
            CHECK(decode(g_out, len, &dd), "budget %u kind %zu: undecodable", budgets[bi], ki);
            CHECK(dd.npts >= 2, "route reduced below two points");
        }
    }

    /* The config cannot raise the ceiling, and the buffer check uses the effective value. */
    cairn_digest_config_t big = cfg_with(0xFFFFFFFFu, 0xFFFFu);
    CHECK(cairn_digest_effective_budget(&big) == CAIRN_DIGEST_BYTES_HARD_CEILING, "ceiling not applied");
    CHECK(cairn_digest_effective_points(&big) == CAIRN_DIGEST_POINTS_HARD_CEILING, "point ceiling not applied");
    cairn_digest_init(&g_d, &big);
    feed(&g_d, TRIP_CITY, 600, 1, 1, NULL);
    size_t len = 0;
    uint8_t exact[CAIRN_DIGEST_BYTES_HARD_CEILING];
    CHECK(cairn_digest_finish(&g_d, &m, exact, sizeof(exact), &len) == CAIRN_DIGEST_OK,
          "a buffer of exactly the ceiling must suffice for any config");
    uint8_t small[CAIRN_DIGEST_BYTES_HARD_CEILING - 1];
    CHECK(cairn_digest_finish(&g_d, &m, small, sizeof(small), &len) == CAIRN_DIGEST_BUFFER_TOO_SMALL,
          "an undersized buffer must be refused");
    return true;
}

/* A tight budget is met by dropping route points, and says so. */
static bool row_budget_reduces_route(void)
{
    cairn_digest_meta_t m = test_meta();
    cairn_digest_config_t roomy = cfg_with(4096, 192);
    cairn_digest_init(&g_d, &roomy);
    feed(&g_d, TRIP_CITY, 3600, 1, 4, NULL);
    size_t big_len = 0;
    CHECK(cairn_digest_finish(&g_d, &m, g_out, sizeof(g_out), &big_len) == CAIRN_DIGEST_OK, "roomy");
    uint32_t big_pts = cairn_digest_route_points(&g_d);

    cairn_digest_config_t tight = cfg_with(512, 192);
    cairn_digest_init(&g_d, &tight);
    feed(&g_d, TRIP_CITY, 3600, 1, 4, NULL);
    size_t len = 0;
    CHECK(cairn_digest_finish(&g_d, &m, g_out, sizeof(g_out), &len) == CAIRN_DIGEST_OK, "tight");
    CHECK(len <= 512, "tight digest is %zu bytes", len);
    CHECK(cairn_digest_route_points(&g_d) < big_pts, "tight budget kept %u points, roomy kept %u",
          cairn_digest_route_points(&g_d), big_pts);
    CHECK(g_out[4] & 0x04, "the digest does not say its route was reduced to fit");
    return true;
}

/* The floor really is enough for the worst-case fixed part. */
static bool row_floor_fits_worst_case(void)
{
    cairn_digest_config_t c = cfg_with(CAIRN_DIGEST_BYTES_FLOOR, 192);
    cairn_digest_init(&g_d, &c);

    cairn_digest_sample_t s;
    memset(&s, 0, sizeof(s));
    s.has_fix = true;
    s.ch_mask = 0x0F;
    uint64_t t = 0xFFFFFFFFFFULL;
    for (int i = 0; i < 3; i++) {
        s.t_ms = t + (uint64_t)i * 3000000000ULL;     /* huge gaps: biggest varints */
        s.lat_e7 = (i % 2) ? 899999999 : -899999999;
        s.lon_e7 = (i % 2) ? 1799999999 : -1799999999;
        s.speed_cmps = 12000;
        for (int k = 0; k < 4; k++) s.ch[k] = (i % 2) ? INT32_MAX : INT32_MIN;
        cairn_digest_add(&g_d, &s);
    }
    cairn_digest_note_health(&g_d, 0xFFFFFFFFu);
    for (int i = 0; i < CAIRN_DIGEST_MAX_EVENTS; i++) cairn_digest_note_event(&g_d, 0xFF, UINT64_MAX / 2);

    cairn_digest_meta_t m = test_meta();
    memset(m.trip_root, 0xFF, 32);
    m.engine_profile_id = 0xFFFF;
    m.engine_profile_version = 0xFFFF;
    size_t len = 0;
    cairn_digest_status_t st = cairn_digest_finish(&g_d, &m, g_out, sizeof(g_out), &len);
    CHECK(st == CAIRN_DIGEST_OK, "worst case at the floor: %s", cairn_digest_status_name(st));
    CHECK(len <= CAIRN_DIGEST_BYTES_FLOOR, "%zu bytes", len);
    return true;
}

static bool seal_capture(void *ctx, const uint8_t *plain, size_t len)
{
    struct { const uint8_t *p; size_t n; bool ok; } *c = ctx;
    c->p = plain;
    c->n = len;
    return c->ok;
}

static bool row_seal_hook(void)
{
    cairn_digest_init(&g_d, NULL);
    feed(&g_d, TRIP_CITY, 300, 1, 2, NULL);
    cairn_digest_meta_t m = test_meta();
    size_t len = 0;
    CHECK(cairn_digest_finish(&g_d, &m, g_out, sizeof(g_out), &len) == CAIRN_DIGEST_OK, "finish");

    CHECK(cairn_digest_seal(g_out, len, NULL, NULL) == CAIRN_DIGEST_SEAL_UNAVAILABLE,
          "without a sealer there must be nothing shippable");

    struct { const uint8_t *p; size_t n; bool ok; } c = { NULL, 0, true };
    CHECK(cairn_digest_seal(g_out, len, seal_capture, &c) == CAIRN_DIGEST_OK, "seal");
    CHECK(c.p == g_out && c.n == len, "the sealer was not handed the plaintext");
    c.ok = false;
    CHECK(cairn_digest_seal(g_out, len, seal_capture, &c) == CAIRN_DIGEST_SEAL_FAILED, "sealer failure hidden");
    return true;
}

/* ── rows: the digest acknowledgement is not a receipt ────────────────────── */

static const char SERVER_SEED_TEXT[] = "cairn-digest-test-server-key!!!";
static const char OTHER_SEED_TEXT[]  = "cairn-digest-test-other-key!!!!";
#define SERVER_SEED ((const uint8_t *)SERVER_SEED_TEXT)
#define OTHER_SEED  ((const uint8_t *)OTHER_SEED_TEXT)

static char g_root[256];

static void rm_rf(const char *path)
{
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* nothing to remove is fine */ }
}

static bool fresh_card(const char *name)
{
    snprintf(g_root, sizeof(g_root), "/tmp/cairn-digest-%s", name);
    rm_rf(g_root);
    if (mkdir(g_root, 0775) != 0) return false;

    char kv[300], card[300];
    snprintf(kv, sizeof(kv), "%s/kv.bin", g_root);
    cairn_kv_host_set_path(kv);
    snprintf(card, sizeof(card), "%s/card", g_root);
    if (mkdir(card, 0775) != 0) return false;
    if (!cairn_fs_begin(card)) return false;
    cairn_host_seed(1);
    return cairn_store_init() && cairn_kv_begin();
}

static void card_done(void)
{
    cairn_kv_end();
    cairn_fs_end();
    rm_rf(g_root);
}

/* A genuine receipt, signed the way the server does; copied from the storage matrix. */
static size_t make_receipt(const uint8_t seed[32], const uint8_t root[32], const uint8_t bundle_id[16],
                           uint8_t *out, size_t cap)
{
    uint8_t pub[32];
    cairn_ed25519_public_from_seed(seed, pub);

    cairn_receipt_t r;
    memset(&r, 0, sizeof(r));
    r.receipt_version = CAIRN_RECEIPT_VERSION;
    memcpy(r.receipt_id, bundle_id, 16);
    memcpy(r.bundle_id, bundle_id, 16);
    memcpy(r.content_root, root, 32);
    r.server_ingest_utc_ms = 1760000000000ULL;
    r.ingest_schema_version = 1;
    cairn_device_key_id(pub, r.server_key_id);
    snprintf(r.signature_algorithm, sizeof(r.signature_algorithm), "%s", CAIRN_SIGALG_ED25519);

    uint8_t signing[1024];
    size_t  signing_len = 0;
    if (cairn_receipt_signing_bytes(&r, signing, sizeof(signing), &signing_len) != CAIRN_OK) return 0;
    cairn_ed25519_sign(signing, signing_len, seed, pub, r.signature);

    size_t written = 0;
    if (cairn_receipt_encode(&r, out, cap, &written) != CAIRN_OK) return 0;
    return written;
}

/* A real server's ack for a digest: the same key that signs receipts. */
static cairn_digest_ack_t make_ack(const uint8_t seed[32], const uint8_t trip_root[32])
{
    uint8_t pub[32];
    cairn_ed25519_public_from_seed(seed, pub);
    cairn_digest_ack_t a;
    memset(&a, 0, sizeof(a));
    memcpy(a.trip_root, trip_root, 32);
    a.server_ingest_utc_ms = 1760000000000ULL;
    cairn_device_key_id(pub, a.server_key_id);
    uint8_t msg[CAIRN_DIGEST_ACK_SIGNING_MAX];
    size_t n = cairn_digest_ack_signing_bytes(&a, msg, sizeof(msg));
    cairn_ed25519_sign(msg, n, seed, pub, a.signature);
    return a;
}

/*
 * What a careless implementer would do: take the signature on a digest ack and put
 * it in a receipt-shaped message with the same fields. If the ack's signing
 * context were the receipt's, this would verify and prune.
 */
static size_t transplant_into_receipt(const cairn_digest_ack_t *a, uint8_t *out, size_t cap)
{
    cairn_receipt_t r;
    memset(&r, 0, sizeof(r));
    r.receipt_version = CAIRN_RECEIPT_VERSION;
    memcpy(r.receipt_id, a->trip_root, 16);
    memcpy(r.bundle_id, a->trip_root, 16);
    memcpy(r.content_root, a->trip_root, 32);
    r.server_ingest_utc_ms = a->server_ingest_utc_ms;
    r.ingest_schema_version = 1;
    memcpy(r.server_key_id, a->server_key_id, 8);
    snprintf(r.signature_algorithm, sizeof(r.signature_algorithm), "%s", CAIRN_SIGALG_ED25519);
    memcpy(r.signature, a->signature, 64);
    size_t written = 0;
    if (cairn_receipt_encode(&r, out, cap, &written) != CAIRN_OK) return 0;
    return written;
}

static bool make_fake_bundle(const char *id)
{
    char dir[200], file[300];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, id);
    if (!cairn_fs_mkdir(dir)) return false;
    snprintf(file, sizeof(file), "%s/segment-0.seg", dir);
    cairn_file_t *f = cairn_fs_open(file, CAIRN_FS_WRITE);
    if (f == NULL) return false;
    const char payload[] = "sealed bundle bytes that exist nowhere else";
    bool ok = cairn_fs_write(f, payload, sizeof(payload)) == sizeof(payload);
    cairn_fs_close(f);
    return ok;
}

static bool bundle_present(const char *id)
{
    char dir[200], file[300];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, id);
    snprintf(file, sizeof(file), "%s/segment-0.seg", dir);
    return cairn_fs_exists(dir) && cairn_fs_exists(file);
}

static bool nothing_happened(const char *id)
{
    char jpath[256];
    snprintf(jpath, sizeof(jpath), "%s/prune-%s.json", CAIRN_DIR_STATE, id);
    return bundle_present(id) && !cairn_fs_exists(jpath) && !cairn_receipt_exists(id);
}

static bool row_ack_never_prunes(void)
{
    const char *id = "01JDIGESTACKNOTARECEIPT000";   /* 26 chars */
    CHECK(fresh_card("ack"), "card setup");
    CHECK(make_fake_bundle(id), "bundle setup");

    uint8_t root[32];
    for (int i = 0; i < 32; i++) root[i] = (uint8_t)(0x30 + i);
    uint8_t pinned[32];
    cairn_ed25519_public_from_seed(SERVER_SEED, pinned);

    cairn_digest_ack_t ack = make_ack(SERVER_SEED, root);
    uint8_t wire[256], rcpt[1024];
    size_t wlen = cairn_digest_ack_encode(&ack, wire, sizeof(wire));
    CHECK(wlen == CAIRN_DIGEST_ACK_LEN, "ack encode");

    /* It is a good ack... */
    CHECK(cairn_digest_ack_check(wire, wlen, pinned, root) == CAIRN_DIGEST_ACK_OK, "the genuine ack does not verify");

    /* (1) ...but presented to the prune gate as a receipt it is refused, intact. */
    cairn_prune_result_t r = cairn_prune_if_receipted(id, wire, wlen, pinned, root);
    CHECK(r != CAIRN_PRUNE_OK, "the prune gate ACCEPTED a digest acknowledgement");
    CHECK(r == CAIRN_PRUNE_RECEIPT_MALFORMED, "result %s, want RECEIPT_MALFORMED", cairn_prune_result_name(r));
    CHECK(nothing_happened(id), "a refused ack still changed the card (deleted, journalled or stored)");

    /* (2) The same signature in a receipt-shaped message: different signing context. */
    size_t tlen = transplant_into_receipt(&ack, rcpt, sizeof(rcpt));
    CHECK(tlen > 0, "transplant");
    r = cairn_prune_if_receipted(id, rcpt, tlen, pinned, root);
    CHECK(r == CAIRN_PRUNE_RECEIPT_UNVERIFIED, "transplanted signature: %s, want RECEIPT_UNVERIFIED",
          cairn_prune_result_name(r));
    CHECK(cairn_receipt_check(rcpt, tlen, pinned, root) == CAIRN_PRUNE_RECEIPT_UNVERIFIED, "check half");
    CHECK(nothing_happened(id), "the transplant changed the card");

    /* (3) Truncations and the wrong length of an ack are refused too. */
    for (size_t cut = 1; cut < wlen; cut += 7) {
        r = cairn_prune_if_receipted(id, wire, cut, pinned, root);
        CHECK(r != CAIRN_PRUNE_OK, "a truncated ack authorised a prune");
    }
    CHECK(nothing_happened(id), "truncated acks changed the card");

    /* (4) The control: a genuine receipt for the same root DOES prune, so the
     * refusals above are the gate working and not the fixture being unusable. */
    size_t rlen = make_receipt(SERVER_SEED, root, root, rcpt, sizeof(rcpt));
    CHECK(rlen > 0, "receipt");
    CHECK(cairn_receipt_check(rcpt, rlen, pinned, root) == CAIRN_PRUNE_OK, "genuine receipt does not check");
    r = cairn_prune_if_receipted(id, rcpt, rlen, pinned, root);
    CHECK(r == CAIRN_PRUNE_OK, "control receipt: %s", cairn_prune_result_name(r));
    CHECK(!bundle_present(id), "control: the bundle survived a genuine prune");

    card_done();
    return true;
}

static bool row_ack_verifier_strict(void)
{
    uint8_t root[32], other_root[32], pinned[32], wrong[32];
    for (int i = 0; i < 32; i++) { root[i] = (uint8_t)(0x30 + i); other_root[i] = (uint8_t)(0x90 + i); }
    cairn_ed25519_public_from_seed(SERVER_SEED, pinned);
    cairn_ed25519_public_from_seed(OTHER_SEED, wrong);

    cairn_digest_ack_t ack = make_ack(SERVER_SEED, root);
    uint8_t wire[CAIRN_DIGEST_ACK_LEN];
    size_t n = cairn_digest_ack_encode(&ack, wire, sizeof(wire));
    CHECK(n == CAIRN_DIGEST_ACK_LEN, "encode");

    CHECK(cairn_digest_ack_check(wire, n, wrong, root) == CAIRN_DIGEST_ACK_UNVERIFIED, "wrong key accepted");
    CHECK(cairn_digest_ack_check(wire, n, pinned, other_root) == CAIRN_DIGEST_ACK_WRONG_TRIP,
          "an ack for another trip was accepted as this one's");
    CHECK(cairn_digest_ack_check(wire, n, NULL, root) == CAIRN_DIGEST_ACK_NO_KEY, "no key");
    uint8_t zero[32] = { 0 };
    CHECK(cairn_digest_ack_check(wire, n, zero, root) == CAIRN_DIGEST_ACK_NO_KEY, "zero key");
    CHECK(cairn_digest_ack_check(wire, n - 1, pinned, root) == CAIRN_DIGEST_ACK_MALFORMED, "short");

    /* Message-type confusion: a signed ack with its magic changed is not an ack. */
    for (int i = 0; i < 4; i++) {
        uint8_t bad[CAIRN_DIGEST_ACK_LEN];
        memcpy(bad, wire, n);
        bad[i] ^= 0x01;
        CHECK(cairn_digest_ack_check(bad, n, pinned, root) == CAIRN_DIGEST_ACK_MALFORMED,
              "magic byte %d not checked", i);
    }
    /* Any bit flip in the body breaks the signature. */
    for (size_t i = 4; i < n; i++) {
        uint8_t bad[CAIRN_DIGEST_ACK_LEN];
        memcpy(bad, wire, n);
        bad[i] ^= 0x80;
        CHECK(cairn_digest_ack_check(bad, n, pinned, root) != CAIRN_DIGEST_ACK_OK, "flip at %zu accepted", i);
    }

    /* And the other direction: a genuine receipt is not an ack. */
    uint8_t rcpt[1024];
    size_t rl = make_receipt(SERVER_SEED, root, root, rcpt, sizeof(rcpt));
    CHECK(rl > 0 && cairn_digest_ack_check(rcpt, rl, pinned, root) == CAIRN_DIGEST_ACK_MALFORMED,
          "a receipt was accepted as a digest ack");

    /* The signing context is its own: the bytes begin with the label, which no receipt signs. */
    uint8_t sb[CAIRN_DIGEST_ACK_SIGNING_MAX];
    size_t sl = cairn_digest_ack_signing_bytes(&ack, sb, sizeof(sb));
    CHECK(sl > 0 && memcmp(sb, CAIRN_DIGEST_ACK_SIGN_LABEL, sizeof(CAIRN_DIGEST_ACK_SIGN_LABEL) - 1) == 0,
          "signing bytes lack the domain label");
    return true;
}

/* ── the size table ───────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    trip_kind_t kind;
    uint32_t    secs, hz;
} table_row_t;

static const table_row_t TABLE[] = {
    { "city, 5 min",                  TRIP_CITY,    300,   1 },
    { "city, 30 min",                 TRIP_CITY,    1800,  1 },
    { "city, 30 min @10 Hz",          TRIP_CITY,    1800, 10 },
    { "highway, 2 h",                 TRIP_HIGHWAY, 7200,  1 },
    { "highway, 8 h",                 TRIP_HIGHWAY, 28800, 1 },
    { "chaos (random 35 m/s), 1 h",   TRIP_CHAOS,   3600,  1 },
};

static void print_table(void)
{
    static int32_t lat[40000], lon[40000];
    cairn_digest_meta_t m = test_meta();
    const uint32_t budgets[] = { 512, 1024, 2048, 4096 };

    printf("\nsynthetic trips (no real trips exist yet); plaintext digest bytes, before compression/sealing\n");
    printf("| trip | samples | km | pts@2048 | max dev @2048 (m) | 512 B | 1024 B | 2048 B | 4096 B | B/km @2048 |\n");
    printf("|---|---|---|---|---|---|---|---|---|---|\n");

    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        size_t sizes[4] = { 0 };
        uint32_t pts = 0, samples = 0;
        double km = 0, dev = 0;
        for (size_t b = 0; b < 4; b++) {
            cairn_digest_config_t c = cfg_with(budgets[b], 192);
            cairn_digest_init(&g_d, &c);
            orig_t o = { lat, lon, 0, 40000 };
            feed(&g_d, TABLE[i].kind, TABLE[i].secs, TABLE[i].hz, 77, (budgets[b] == 2048 && TABLE[i].hz == 1) ? &o : NULL);
            cairn_digest_finish(&g_d, &m, g_out, sizeof(g_out), &sizes[b]);
            if (budgets[b] == 2048) {
                pts = cairn_digest_route_points(&g_d);
                samples = cairn_digest_stats(&g_d)->samples;
                km = (double)cairn_digest_stats(&g_d)->distance_mm / 1e6;
                if (o.n > 0) {
                    int32_t rl[200], ro[200];
                    size_t n = cairn_digest_route(&g_d, rl, ro, 200);
                    dev = max_deviation_m(&o, rl, ro, n);
                }
            }
        }
        char devs[16];
        if (dev > 0) snprintf(devs, sizeof(devs), "%.0f", dev); else snprintf(devs, sizeof(devs), "-");
        printf("| %s | %u | %.1f | %u | %s | %zu | %zu | %zu | %zu | %.1f |\n", TABLE[i].name, samples, km, pts,
               devs, sizes[0], sizes[1], sizes[2], sizes[3], km > 0 ? (double)sizes[2] / km : 0.0);
    }
    printf("\nceiling %u B, floor %u B, default %u B; max points %u (default %u); context %zu B\n",
           CAIRN_DIGEST_BYTES_HARD_CEILING, CAIRN_DIGEST_BYTES_FLOOR, CAIRN_DIGEST_BYTES_DEFAULT,
           CAIRN_DIGEST_POINTS_HARD_CEILING, CAIRN_DIGEST_POINTS_DEFAULT, sizeof(cairn_digest_t));

    /* The cost of one sample, for the "must not delay the boot path" requirement. */
    cairn_digest_config_t c = cfg_with(2048, 96);
    cairn_digest_init(&g_d, &c);
    clock_t t0 = clock();
    feed(&g_d, TRIP_CITY, 360000, 1, 5, NULL);
    double us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 360000.0;
    printf("host cost: %.2f us per sample (360000 samples, 96 points; the ESP32 is perhaps 20-50x slower)\n", us);
}

/* ── runner ───────────────────────────────────────────────────────────────── */

typedef struct {
    const char *family;
    const char *name;
    bool (*fn)(void);
} row_t;

static const row_t ROWS[] = {
    { "reduce", "a corner survives a three-point cap",                 row_corner_survives },
    { "reduce", "state is bounded however long the trip",              row_bounded_state },
    { "reduce", "the reduced route stays near the driven one",         row_route_accuracy },
    { "stats",  "distance is right and parked jitter is not distance", row_stats_distance_and_jitter },
    { "stats",  "implausible fixes are rejected and recover",          row_stats_rejects_glitches },
    { "stats",  "channels, health and events accumulate",              row_stats_channels_health_events },
    { "wire",   "delta+varint coding round-trips the route",           row_encoding_round_trips },
    { "wire",   "the same input gives the same bytes",                 row_deterministic },
    { "budget", "no config exceeds the budget or the ceiling",         row_budget_never_exceeded },
    { "budget", "a tight budget reduces the route, not the limit",     row_budget_reduces_route },
    { "budget", "the floor holds a worst-case digest",                 row_floor_fits_worst_case },
    { "seal",   "no sealer means nothing shippable",                   row_seal_hook },
    { "ack",    "a digest ack presented to the prune gate is refused", row_ack_never_prunes },
    { "ack",    "the ack verifier is strict and not a receipt parser", row_ack_verifier_strict },
};

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cairn_log_init(0);

    size_t n = sizeof(ROWS) / sizeof(ROWS[0]);
    for (size_t i = 0; i < n; i++) {
        g_row = ROWS[i].name;
        bool ok = ROWS[i].fn();
        if (ok) {
            g_pass++;
            printf("  pass  [%-7s] %s\n", ROWS[i].family, ROWS[i].name);
        } else {
            g_fail++;
            printf("  FAIL  [%-7s] %s\n", ROWS[i].family, ROWS[i].name);
        }
    }
    printf("\ndigest matrix (PROVISIONAL: contracts/digest/v1 unreleased, no vectors): %d/%zu passed\n", g_pass, n);

    if (g_failure_count > 0) {
        printf("\n%d failure(s):\n", g_failure_count);
        for (int i = 0; i < g_failure_count; i++) printf("  %s\n", g_failures[i]);
        return 1;
    }

    print_table();
    return g_fail == 0 ? 0 : 1;
}
