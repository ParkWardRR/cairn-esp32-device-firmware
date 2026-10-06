/*
 * The format-independent half of the digest generator: route reduction and
 * statistics. See cairn_digest.h. PROVISIONAL until contracts/digest/v1.
 *
 * Geometry is integer-only. Coordinates are 1e-7 degrees; longitude differences
 * are scaled by cos(latitude of the trip's first fix) in Q15 so distances are
 * roughly isotropic, and the arithmetic is done in int64 on differences clamped
 * to +-2^30, which keeps every product under 2^61: a corrupt coordinate cannot
 * overflow, it can only be implausible (and is then rejected). Identical
 * input gives identical output on every platform, which a float path would not.
 */

#include "cairn_digest.h"
#include "cairn_digest_internal.h"

#include <string.h>

#define NONE 0xFFFFu

#define MM_PER_UNIT_NUM 111195u   /* one scaled 1e-7 degree is 11.1195 mm */
#define MM_PER_UNIT_DEN 10000u

#define JITTER_MM       4000u     /* stationary GNSS noise is not distance */
#define UNKNOWN_SPEED_JITTER_MM 10000u
#define MAX_PLAUSIBLE_MM_PER_MS 100u   /* 100 m/s; plus slack below */
#define PLAUSIBLE_SLACK_MM 20000u
#define REJECT_RESEED_AFTER 5u    /* then trust the new position, not the old one */
#define MAX_SANE_SPEED_CMPS 12000u
#define MOVING_CMPS      50u
#define MOVING_GAP_CAP_MS 10000u
#define DEFAULT_SPACING_M 15u

static const uint16_t COS_Q15[91] = {
    32768, 32763, 32748, 32723, 32688, 32643, 32588, 32524, 32449, 32365,
    32270, 32166, 32052, 31928, 31795, 31651, 31499, 31336, 31164, 30983,
    30792, 30592, 30382, 30163, 29935, 29698, 29452, 29197, 28932, 28660,
    28378, 28088, 27789, 27482, 27166, 26842, 26510, 26170, 25822, 25466,
    25102, 24730, 24351, 23965, 23571, 23170, 22763, 22348, 21926, 21498,
    21063, 20622, 20174, 19720, 19261, 18795, 18324, 17847, 17364, 16877,
    16384, 15886, 15384, 14876, 14365, 13848, 13328, 12803, 12275, 11743,
    11207, 10668, 10126, 9580, 9032, 8481, 7927, 7371, 6813, 6252,
    5690, 5126, 4560, 3993, 3425, 2856, 2286, 1715, 1144, 572,
    0,
};

const char *cairn_digest_status_name(cairn_digest_status_t s)
{
    switch (s) {
    case CAIRN_DIGEST_OK:                return "OK";
    case CAIRN_DIGEST_BAD_ARG:           return "BAD_ARG";
    case CAIRN_DIGEST_FINISHED:          return "FINISHED";
    case CAIRN_DIGEST_BUDGET_TOO_SMALL:  return "BUDGET_TOO_SMALL";
    case CAIRN_DIGEST_BUFFER_TOO_SMALL:  return "BUFFER_TOO_SMALL";
    case CAIRN_DIGEST_SEAL_UNAVAILABLE:  return "SEAL_UNAVAILABLE";
    case CAIRN_DIGEST_SEAL_FAILED:       return "SEAL_FAILED";
    default:                             return "UNKNOWN";
    }
}

/* ── configuration ────────────────────────────────────────────────────────── */

void cairn_digest_config_defaults(cairn_digest_config_t *c)
{
    c->byte_budget   = CAIRN_DIGEST_BYTES_DEFAULT;
    c->max_points    = CAIRN_DIGEST_POINTS_DEFAULT;
    c->min_spacing_m = DEFAULT_SPACING_M;
    c->route_times   = true;
}

uint32_t cairn_digest_effective_budget(const cairn_digest_config_t *c)
{
    uint32_t b = (c != NULL) ? c->byte_budget : CAIRN_DIGEST_BYTES_DEFAULT;
    if (b > CAIRN_DIGEST_BYTES_HARD_CEILING) b = CAIRN_DIGEST_BYTES_HARD_CEILING;
    if (b < CAIRN_DIGEST_BYTES_FLOOR)        b = CAIRN_DIGEST_BYTES_FLOOR;
    return b;
}

uint32_t cairn_digest_effective_points(const cairn_digest_config_t *c)
{
    uint32_t p = (c != NULL) ? c->max_points : CAIRN_DIGEST_POINTS_DEFAULT;
    if (p > CAIRN_DIGEST_POINTS_HARD_CEILING) p = CAIRN_DIGEST_POINTS_HARD_CEILING;
    if (p < 2) p = 2;
    return p;
}

/* ── integer geometry ─────────────────────────────────────────────────────── */

static uint64_t isqrt_u64(uint64_t v)
{
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static int64_t clamp30(int64_t v)
{
    const int64_t lim = (int64_t)1 << 30;
    if (v > lim) return lim;
    if (v < -lim) return -lim;
    return v;
}

typedef struct { int64_t x, y; } vec_t;

static vec_t vec(const cairn_digest_t *d, int32_t lat_a, int32_t lon_a,
                 int32_t lat_b, int32_t lon_b)
{
    int64_t dlon = (int64_t)lon_b - (int64_t)lon_a;
    if (dlon > 1800000000LL) dlon -= 3600000000LL;       /* across the antimeridian */
    if (dlon < -1800000000LL) dlon += 3600000000LL;

    vec_t v;
    v.y = clamp30((int64_t)lat_b - (int64_t)lat_a);
    v.x = clamp30(dlon) * (int64_t)d->cos_q15 / 32768;
    return v;
}

static uint64_t vlen2(vec_t v) { return (uint64_t)(v.x * v.x + v.y * v.y); }

static uint32_t sat32(uint64_t v) { return v > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)v; }

static uint64_t dist_mm(const cairn_digest_t *d, int32_t la, int32_t oa, int32_t lb, int32_t ob)
{
    uint64_t units = isqrt_u64(vlen2(vec(d, la, oa, lb, ob)));
    return units * MM_PER_UNIT_NUM / MM_PER_UNIT_DEN;
}

static int32_t cos_for_lat(int32_t lat_e7)
{
    int64_t a = lat_e7 < 0 ? -(int64_t)lat_e7 : lat_e7;
    uint32_t deg = (uint32_t)(a / 10000000);
    uint32_t rem = (uint32_t)(a % 10000000);
    if (deg >= 90) return 0;
    int32_t c0 = COS_Q15[deg], c1 = COS_Q15[deg + 1];
    return c0 + (int32_t)(((int64_t)(c1 - c0) * (int64_t)rem) / 10000000);
}

/*
 * How far point b strays from the path a-c: its distance to the segment. This is
 * the quantity Ramer-Douglas-Peucker compares to its tolerance; here it ranks the
 * candidates for removal instead.
 */
static uint32_t deviation(const cairn_digest_t *d, const cairn_digest_pt_t *a,
                          const cairn_digest_pt_t *b, const cairn_digest_pt_t *c)
{
    vec_t ab = vec(d, a->lat_e7, a->lon_e7, b->lat_e7, b->lon_e7);
    vec_t ac = vec(d, a->lat_e7, a->lon_e7, c->lat_e7, c->lon_e7);
    vec_t bc = vec(d, b->lat_e7, b->lon_e7, c->lat_e7, c->lon_e7);

    uint64_t ac2 = vlen2(ac);
    if (ac2 == 0) return sat32(isqrt_u64(vlen2(ab)));

    int64_t t = ab.x * ac.x + ab.y * ac.y;
    if (t <= 0) return sat32(isqrt_u64(vlen2(ab)));
    if ((uint64_t)t >= ac2) return sat32(isqrt_u64(vlen2(bc)));

    int64_t cross = ab.x * ac.y - ab.y * ac.x;
    if (cross < 0) cross = -cross;
    uint64_t base = isqrt_u64(ac2);
    return sat32((uint64_t)cross / base);
}

/* ── the route list ───────────────────────────────────────────────────────── */

#define N_SLOTS (CAIRN_DIGEST_POINTS_HARD_CEILING + 2)

static int alloc_slot(const cairn_digest_t *d)
{
    for (uint16_t i = 0; i < N_SLOTS; i++) {
        if (!d->pt[i].used) return i;
    }
    return -1;
}

static void rescore(cairn_digest_t *d, uint16_t i)
{
    cairn_digest_pt_t *p = &d->pt[i];
    if (p->prev == NONE || p->next == NONE) {
        p->importance = 0xFFFFFFFFu;  /* the two ends are never removed */
        return;
    }
    p->importance = deviation(d, &d->pt[p->prev], p, &d->pt[p->next]);
}

bool cairn_digest_drop_least_important(cairn_digest_t *d)
{
    uint16_t best = NONE;
    uint32_t best_imp = 0;

    for (uint16_t i = d->head; i != NONE; i = d->pt[i].next) {
        if (d->pt[i].prev == NONE || d->pt[i].next == NONE) continue;
        if (best == NONE || d->pt[i].importance < best_imp) {
            best = i;
            best_imp = d->pt[i].importance;
        }
    }
    if (best == NONE) return false;

    uint16_t p = d->pt[best].prev, n = d->pt[best].next;
    d->pt[p].next = n;
    d->pt[n].prev = p;
    d->pt[best].used = false;
    d->count--;

    rescore(d, p);
    rescore(d, n);
    return true;
}

static void commit_point(cairn_digest_t *d, int32_t lat, int32_t lon, uint32_t t_s)
{
    int s = alloc_slot(d);
    if (s < 0) return;  /* unreachable: count is held at or below cap < N_SLOTS */

    cairn_digest_pt_t *p = &d->pt[s];
    p->lat_e7 = lat;
    p->lon_e7 = lon;
    p->t_s = t_s;
    p->used = true;
    p->importance = 0xFFFFFFFFu;
    p->next = NONE;
    p->prev = d->tail;

    uint16_t old_tail = d->tail;
    if (old_tail == NONE) d->head = (uint16_t)s;
    else d->pt[old_tail].next = (uint16_t)s;
    d->tail = (uint16_t)s;
    d->count++;

    if (old_tail != NONE) rescore(d, old_tail);

    while (d->count > d->cap) {
        if (!cairn_digest_drop_least_important(d)) break;
    }
}

/* ── streaming ────────────────────────────────────────────────────────────── */

void cairn_digest_init(cairn_digest_t *d, const cairn_digest_config_t *cfg)
{
    memset(d, 0, sizeof(*d));

    if (cfg != NULL) d->cfg = *cfg;
    else cairn_digest_config_defaults(&d->cfg);

    d->cfg.byte_budget = cairn_digest_effective_budget(&d->cfg);
    d->cap = cairn_digest_effective_points(&d->cfg);
    d->cfg.max_points = d->cap;

    uint32_t m = d->cfg.min_spacing_m != 0 ? d->cfg.min_spacing_m : DEFAULT_SPACING_M;
    /* metres -> scaled units: 1 m = 1000 mm = 1000 / 11.1195 units */
    d->spacing_units = (uint32_t)((uint64_t)m * 1000ULL * MM_PER_UNIT_DEN / MM_PER_UNIT_NUM);

    d->head = d->tail = NONE;
    d->cos_q15 = 32768;
    for (uint32_t i = 0; i < CAIRN_DIGEST_NCHAN; i++) {
        d->st.ch[i].min = INT32_MAX;
        d->st.ch[i].max = INT32_MIN;
    }
}

const cairn_digest_stats_t *cairn_digest_stats(const cairn_digest_t *d) { return &d->st; }

uint32_t cairn_digest_route_points(const cairn_digest_t *d) { return d->count; }

void cairn_digest_note_health(cairn_digest_t *d, uint32_t flag_bits)
{
    d->st.health_flags |= flag_bits;
}

static uint32_t secs_since_start(const cairn_digest_t *d, uint64_t t_ms)
{
    if (!d->st.started || t_ms < d->st.first_t_ms) return 0;
    uint64_t s = (t_ms - d->st.first_t_ms) / 1000ULL;
    return s > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)s;
}

bool cairn_digest_note_event(cairn_digest_t *d, uint8_t type, uint64_t t_ms)
{
    if (d->ev_count >= CAIRN_DIGEST_MAX_EVENTS) {
        d->st.events_dropped++;
        return false;
    }
    d->ev[d->ev_count].type = type;
    d->ev[d->ev_count].t_s = secs_since_start(d, t_ms);
    d->ev_count++;
    return true;
}

static bool fix_in_range(const cairn_digest_sample_t *s)
{
    if (s->lat_e7 < -900000000 || s->lat_e7 > 900000000) return false;
    if (s->lon_e7 < -1800000000 || s->lon_e7 > 1800000000) return false;
    if (s->lat_e7 == 0 && s->lon_e7 == 0) return false;  /* "no fix" reported as a place */
    return true;
}

cairn_digest_status_t cairn_digest_add(cairn_digest_t *d, const cairn_digest_sample_t *s)
{
    if (d == NULL || s == NULL) return CAIRN_DIGEST_BAD_ARG;
    if (d->finished) return CAIRN_DIGEST_FINISHED;

    cairn_digest_stats_t *st = &d->st;

    /* Time never runs backwards inside a digest. */
    uint64_t t = s->t_ms;
    uint64_t gap_ms = 0;
    if (!st->started) {
        st->started = true;
        st->first_t_ms = st->last_t_ms = t;
    } else {
        if (t < st->last_t_ms) t = st->last_t_ms;
        gap_ms = t - st->last_t_ms;
        st->last_t_ms = t;
    }
    st->samples++;

    for (uint32_t i = 0; i < CAIRN_DIGEST_NCHAN; i++) {
        if (!(s->ch_mask & (1u << i))) continue;
        st->ch[i].count++;
        if (s->ch[i] < st->ch[i].min) st->ch[i].min = s->ch[i];
        if (s->ch[i] > st->ch[i].max) st->ch[i].max = s->ch[i];
        st->ch[i].sum += s->ch[i];
    }

    if (s->speed_cmps != CAIRN_DIGEST_SPEED_UNKNOWN && s->speed_cmps <= MAX_SANE_SPEED_CMPS) {
        if (s->speed_cmps > st->max_speed_cmps) st->max_speed_cmps = s->speed_cmps;
        if (s->speed_cmps >= MOVING_CMPS) {
            st->moving_time_ms += (uint32_t)(gap_ms > MOVING_GAP_CAP_MS ? MOVING_GAP_CAP_MS : gap_ms);
        }
    }

    if (!s->has_fix) return CAIRN_DIGEST_OK;
    if (!fix_in_range(s)) {
        st->fixes_rejected++;
        return CAIRN_DIGEST_OK;
    }

    if (!d->have_prev) {
        d->cos_q15 = cos_for_lat(s->lat_e7);
        d->have_anchor = true;
        d->anchor_lat = s->lat_e7;
        d->anchor_lon = s->lon_e7;
    } else {
        uint64_t dt = t - d->prev_t_ms;
        uint64_t jump = dist_mm(d, d->prev_lat, d->prev_lon, s->lat_e7, s->lon_e7);
        uint64_t allowed = dt * MAX_PLAUSIBLE_MM_PER_MS + PLAUSIBLE_SLACK_MM;

        if (jump > allowed) {
            st->fixes_rejected++;
            if (++d->reject_run < REJECT_RESEED_AFTER) return CAIRN_DIGEST_OK;
            /* Several in a row agree with each other, not with the past: the past
             * was the glitch (or we re-acquired after a long gap). Continue from
             * here without counting the jump as distance. */
            d->have_anchor = true;
            d->anchor_lat = s->lat_e7;
            d->anchor_lon = s->lon_e7;
        } else {
            /* Stationary GNSS noise must not become distance: with a speed
             * reading, only count while moving; without one, ask for a bigger
             * step. The anchor holds still until then. */
            bool speed_known = s->speed_cmps != CAIRN_DIGEST_SPEED_UNKNOWN &&
                               s->speed_cmps <= MAX_SANE_SPEED_CMPS;
            uint64_t need = speed_known ? JITTER_MM : UNKNOWN_SPEED_JITTER_MM;
            uint64_t step = dist_mm(d, d->anchor_lat, d->anchor_lon, s->lat_e7, s->lon_e7);
            if (step >= need && (!speed_known || s->speed_cmps >= MOVING_CMPS)) {
                st->distance_mm += step;
                d->anchor_lat = s->lat_e7;
                d->anchor_lon = s->lon_e7;
            }
        }
    }

    d->reject_run = 0;
    d->have_prev = true;
    d->prev_lat = s->lat_e7;
    d->prev_lon = s->lon_e7;
    d->prev_t_ms = t;
    st->fixes_used++;

    uint32_t t_s = secs_since_start(d, t);
    d->have_last = true;
    d->last_lat = s->lat_e7;
    d->last_lon = s->lon_e7;
    d->last_t_s = t_s;

    /* The streaming pre-filter: a fix that has not moved far from the last kept
     * point is not a candidate (it is still the route's tail at finish). */
    if (d->tail == NONE) {
        commit_point(d, s->lat_e7, s->lon_e7, t_s);
    } else {
        const cairn_digest_pt_t *tl = &d->pt[d->tail];
        vec_t v = vec(d, tl->lat_e7, tl->lon_e7, s->lat_e7, s->lon_e7);
        uint64_t sp = d->spacing_units;
        if (vlen2(v) >= sp * sp) commit_point(d, s->lat_e7, s->lon_e7, t_s);
    }

    return CAIRN_DIGEST_OK;
}

size_t cairn_digest_route(const cairn_digest_t *d, int32_t *lat_e7, int32_t *lon_e7, size_t cap)
{
    size_t n = 0;
    for (uint16_t i = d->head; i != NONE && n < cap; i = d->pt[i].next) {
        lat_e7[n] = d->pt[i].lat_e7;
        lon_e7[n] = d->pt[i].lon_e7;
        n++;
    }
    return n;
}

/* ── finish: the byte budget ──────────────────────────────────────────────── */

cairn_digest_status_t cairn_digest_finish(cairn_digest_t *d, const cairn_digest_meta_t *meta,
                                          uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (d == NULL || meta == NULL || out == NULL || out_len == NULL) return CAIRN_DIGEST_BAD_ARG;

    uint32_t budget = cairn_digest_effective_budget(&d->cfg);
    if (out_cap < budget) return CAIRN_DIGEST_BUFFER_TOO_SMALL;

    if (!d->finished) {
        d->finished = true;

        /* The route always ends at the newest fix, kept or not. */
        if (d->have_last) {
            const cairn_digest_pt_t *tl = (d->tail != NONE) ? &d->pt[d->tail] : NULL;
            if (tl == NULL || tl->lat_e7 != d->last_lat || tl->lon_e7 != d->last_lon) {
                commit_point(d, d->last_lat, d->last_lon, d->last_t_s);
            }
        }
    }

    bool with_events = true;

    for (;;) {
        size_t n = cairn_digest_wire_encode(d, meta, budget, with_events, out);
        if (n != 0) {
            *out_len = n;
            return CAIRN_DIGEST_OK;
        }

        /* Too big. Reduce, never exceed: fewer points first (they are the bulk and
         * the least valuable), then the events, and if even that does not fit the
         * budget is below what a digest needs and nothing is emitted. */
        if (cairn_digest_drop_least_important(d)) {
            d->route_reduced_by_budget = true;
            continue;
        }
        if (with_events && d->ev_count > 0) {
            with_events = false;
            d->events_dropped_by_budget = true;
            continue;
        }
        *out_len = 0;
        return CAIRN_DIGEST_BUDGET_TOO_SMALL;
    }
}

/* ── the sealing seam ─────────────────────────────────────────────────────── */

cairn_digest_status_t cairn_digest_seal(const uint8_t *plain, size_t len,
                                        cairn_digest_seal_fn fn, void *ctx)
{
    if (plain == NULL || len == 0) return CAIRN_DIGEST_BAD_ARG;
    if (fn == NULL) return CAIRN_DIGEST_SEAL_UNAVAILABLE;
    return fn(ctx, plain, len) ? CAIRN_DIGEST_OK : CAIRN_DIGEST_SEAL_FAILED;
}
