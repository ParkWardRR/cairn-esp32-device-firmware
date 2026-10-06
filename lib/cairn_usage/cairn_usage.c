/*
 * LTE data accounting and limits. See cairn_usage.h for what this is and the
 * PROVISIONAL status (contracts/config/v1 is unreleased).
 *
 * Persistence design, because it is the part that is easy to get subtly wrong:
 *
 *   Flash wear says write rarely; a power cut says what was not written is lost.
 *   The two are reconciled by making the loss one-sided. LTE bytes are written in
 *   batches (tune.batch_bytes), so at any moment fewer than one batch (plus the
 *   chunk in flight) is unflushed. The image carries a DIRTY flag that is set
 *   before the first LTE byte of an attempt moves and cleared only by a flush
 *   with nothing in flight. On load, a dirty image means "up to that much was
 *   counted in RAM and never written", so the loader ADDS it, as a margin, to the
 *   LTE counters. The count after a power cut is therefore never below what the
 *   carrier saw, and a cap can only be reached early, never overrun. The price is
 *   at most 2 * batch_bytes of over-count per crash, paid once: the loader
 *   persists the corrected value (clean) before returning, so a reboot loop does
 *   not keep adding it.
 *
 *   The image is written to two alternating slots, each with a sequence number and
 *   a CRC-32. A write torn by the power cut corrupts only the slot being written;
 *   the other still holds the previous image. If the newest slot is unreadable the
 *   older one is used with a second margin (the lost write may have held a batch
 *   more). If both are present but unreadable the counters are UNKNOWN: LTE is
 *   blocked (STATE_LOST) until the next billing period or a signed clear, because
 *   restarting from zero would hand a corrupted device an unlimited allowance.
 *
 *   Attempts are persisted write-ahead: the attempt is counted and flushed before
 *   it starts, so crash-looping inside an upload cannot reset the retry budget.
 */

#include "cairn_usage.h"

#include <string.h>

#include "cairn_format.h"
#include "cairn_kv.h"

#define SLOT_A "usage_a"
#define SLOT_B "usage_b"

#define IMG_MAGIC0 'C'
#define IMG_MAGIC1 'U'
#define IMG_MAGIC2 'S'
#define IMG_MAGIC3 0x01  /* private layout version 1 (not a contract) */

#define IMG_MAX 400
#define PERIOD_UNKNOWN INT32_MIN
#define NO_TRIP (-1)

const char *cairn_usage_reason_name(cairn_usage_reason_t r)
{
    switch (r) {
    case CAIRN_STOP_NONE:             return "NONE";
    case CAIRN_STOP_STATE_LOST:       return "STATE_LOST";
    case CAIRN_STOP_STORE_FAILED:     return "STORE_FAILED";
    case CAIRN_STOP_DISABLED:         return "DISABLED";
    case CAIRN_STOP_PAUSED:           return "PAUSED";
    case CAIRN_STOP_ROAMING:          return "ROAMING";
    case CAIRN_STOP_MODE_DIGESTS_ONLY:return "MODE_DIGESTS_ONLY";
    case CAIRN_STOP_BUNDLE_TOO_LARGE: return "BUNDLE_TOO_LARGE";
    case CAIRN_STOP_BUSY:             return "BUSY";
    case CAIRN_STOP_BREAKER_OPEN:     return "BREAKER_OPEN";
    case CAIRN_STOP_TRIP_ATTEMPTS:    return "TRIP_ATTEMPTS";
    case CAIRN_STOP_BACKOFF:          return "BACKOFF";
    case CAIRN_STOP_CAP_TRIP:         return "CAP_TRIP";
    case CAIRN_STOP_CAP_DAILY:        return "CAP_DAILY";
    case CAIRN_STOP_CAP_MONTHLY:      return "CAP_MONTHLY";
    default:                          return "UNKNOWN";
    }
}

const char *cairn_usage_cfg_result_name(cairn_usage_cfg_result_t r)
{
    switch (r) {
    case CAIRN_CFG_OK:              return "OK";
    case CAIRN_CFG_UNSIGNED_LOOSEN: return "UNSIGNED_LOOSEN";
    case CAIRN_CFG_ABOVE_CEILING:   return "ABOVE_CEILING";
    case CAIRN_CFG_INVALID:         return "INVALID";
    case CAIRN_CFG_STORE_FAILED:    return "STORE_FAILED";
    default:                        return "UNKNOWN";
    }
}

/* ── defaults ─────────────────────────────────────────────────────────────── */

void cairn_usage_config_defaults(cairn_usage_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->lte_enabled = false;            /* nothing leaves over LTE until someone says so */
    c->roaming_allowed = false;
    c->paused = false;
    c->mode = CAIRN_USAGE_MODE_DIGESTS_ONLY;
    c->billing_day = 1;
    c->alert_pct = CAIRN_USAGE_DEFAULT_ALERT_PCT;
    c->monthly_cap_bytes = CAIRN_USAGE_DEFAULT_MONTHLY_BYTES;
    c->daily_cap_bytes = CAIRN_USAGE_DEFAULT_DAILY_BYTES;
    c->trip_cap_bytes = CAIRN_USAGE_DEFAULT_TRIP_BYTES;
    c->full_bundle_max_bytes = 0;
}

void cairn_usage_ceilings_defaults(cairn_usage_ceilings_t *c)
{
    c->monthly = CAIRN_USAGE_CEIL_MONTHLY_BYTES;
    c->daily = CAIRN_USAGE_CEIL_DAILY_BYTES;
    c->trip = CAIRN_USAGE_CEIL_TRIP_BYTES;
    c->full_bundle = CAIRN_USAGE_CEIL_FULL_BUNDLE;
}

void cairn_usage_tuning_defaults(cairn_usage_tuning_t *t)
{
    t->batch_bytes = 4096;
    t->batch_other_bytes = 256u * 1024u;
    t->max_attempts_per_trip = 5;
    t->breaker_threshold = 5;
    t->breaker_cooldown_s = 600;
    t->breaker_cooldown_max_s = 6u * 3600u;
    t->backoff_base_s = 30;
    t->backoff_max_s = 3600;
}

/* ── the default store: cairn_kv ──────────────────────────────────────────── */

static bool kv_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    return cairn_kv_get_blob_var(key, buf, cap, len);
}

static bool kv_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    (void)ctx;
    return cairn_kv_set_blob(key, buf, len);
}

const cairn_usage_store_t *cairn_usage_kv_store(void)
{
    static const cairn_usage_store_t s = { kv_read, kv_write, NULL };
    return &s;
}

/* ── calendar ─────────────────────────────────────────────────────────────── */

/* Howard Hinnant's civil_from_days. */
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

int32_t cairn_usage_period_id(uint64_t utc_s, uint8_t billing_day)
{
    int64_t y;
    unsigned m, d;
    civil_from_days((int64_t)(utc_s / 86400u), &y, &m, &d);
    int64_t id = y * 12 + (int64_t)(m - 1);
    if (d < billing_day) id--;
    return (int32_t)id;
}

/* ── little-endian image ──────────────────────────────────────────────────── */

typedef struct {
    uint8_t *b;
    size_t   n;
} w_t;

static void w8(w_t *w, uint8_t v) { w->b[w->n++] = v; }
static void w16(w_t *w, uint16_t v) { w8(w, (uint8_t)v); w8(w, (uint8_t)(v >> 8)); }
static void w32(w_t *w, uint32_t v) { for (int i = 0; i < 4; i++) w8(w, (uint8_t)(v >> (8 * i))); }
static void w64(w_t *w, uint64_t v) { for (int i = 0; i < 8; i++) w8(w, (uint8_t)(v >> (8 * i))); }

typedef struct {
    const uint8_t *b;
    size_t         n;
} r_t;

static uint8_t r8(r_t *r) { return r->b[r->n++]; }
static uint16_t r16(r_t *r) { uint16_t v = r8(r); v |= (uint16_t)(r8(r) << 8); return v; }
static uint32_t r32(r_t *r) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)r8(r) << (8 * i); return v; }
static uint64_t r64(r_t *r) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)r8(r) << (8 * i); return v; }

#define F_DIRTY    0x01
#define F_LOST     0x02
#define F_INFLIGHT 0x04
#define F_LTE      0x08
#define F_ROAMING  0x10
#define F_PAUSED   0x20
#define F_BREAKER  0x40

#define IMG_LEN (4 + 4 + 1 + 4 + 2 + 4 + 4 + (CAIRN_PATH_COUNT * 2 * 2 * 8) + 16 + 16 + \
                 (CAIRN_USAGE_TRIPS_TRACKED * (8 + 8 + CAIRN_TRAFFIC_KINDS * 2)) + 4)

size_t cairn_usage_image_len(void) { return IMG_LEN; }

static size_t build_image(const cairn_usage_t *u, uint32_t seq, bool dirty, uint8_t *out)
{
    w_t w = { out, 0 };
    w8(&w, IMG_MAGIC0); w8(&w, IMG_MAGIC1); w8(&w, IMG_MAGIC2); w8(&w, IMG_MAGIC3);
    w32(&w, seq);

    uint8_t flags = 0;
    if (dirty) flags |= F_DIRTY;
    if (u->state_lost) flags |= F_LOST;
    if (u->inflight) flags |= F_INFLIGHT;
    if (u->cfg.lte_enabled) flags |= F_LTE;
    if (u->cfg.roaming_allowed) flags |= F_ROAMING;
    if (u->cfg.paused) flags |= F_PAUSED;
    if (u->breaker_open) flags |= F_BREAKER;
    w8(&w, flags);
    w8(&w, u->cfg.mode);
    w8(&w, u->cfg.billing_day);
    w8(&w, u->cfg.alert_pct);
    w8(&w, u->breaker_exp);
    w16(&w, u->consec_failures);
    w32(&w, (uint32_t)u->period_id);
    w32(&w, u->day_index);

    for (int p = 0; p < CAIRN_PATH_COUNT; p++) {
        for (int d = 0; d < 2; d++) {
            w64(&w, u->day_bytes[p][d]);
            w64(&w, u->period_bytes[p][d]);
        }
    }
    w32(&w, u->cfg.monthly_cap_bytes);
    w32(&w, u->cfg.daily_cap_bytes);
    w32(&w, u->cfg.trip_cap_bytes);
    w32(&w, u->cfg.full_bundle_max_bytes);
    w32(&w, u->ceil.monthly);
    w32(&w, u->ceil.daily);
    w32(&w, u->ceil.trip);
    w32(&w, u->ceil.full_bundle);

    for (int i = 0; i < CAIRN_USAGE_TRIPS_TRACKED; i++) {
        w64(&w, u->trips[i].key);
        w64(&w, u->trips[i].bytes);
        for (int k = 0; k < CAIRN_TRAFFIC_KINDS; k++) w16(&w, u->trips[i].attempts[k]);
    }

    w32(&w, cairn_crc32(out, w.n));
    return w.n;
}

/* Parse into `o` (persisted fields only). false: not a usable image. */
static bool parse_image(const uint8_t *b, size_t len, cairn_usage_t *o, uint32_t *seq, bool *dirty,
                        bool *inflight)
{
    if (len != IMG_LEN) return false;
    if (b[0] != IMG_MAGIC0 || b[1] != IMG_MAGIC1 || b[2] != IMG_MAGIC2 || b[3] != IMG_MAGIC3) return false;

    uint32_t crc = 0;
    for (int i = 0; i < 4; i++) crc |= (uint32_t)b[len - 4 + (size_t)i] << (8 * i);
    if (crc != cairn_crc32(b, len - 4)) return false;

    r_t r = { b, 4 };
    *seq = r32(&r);
    uint8_t flags = r8(&r);
    o->cfg.mode = r8(&r);
    o->cfg.billing_day = r8(&r);
    o->cfg.alert_pct = r8(&r);
    o->breaker_exp = r8(&r);
    o->consec_failures = r16(&r);
    o->period_id = (int32_t)r32(&r);
    o->day_index = r32(&r);
    for (int p = 0; p < CAIRN_PATH_COUNT; p++) {
        for (int d = 0; d < 2; d++) {
            o->day_bytes[p][d] = r64(&r);
            o->period_bytes[p][d] = r64(&r);
        }
    }
    o->cfg.monthly_cap_bytes = r32(&r);
    o->cfg.daily_cap_bytes = r32(&r);
    o->cfg.trip_cap_bytes = r32(&r);
    o->cfg.full_bundle_max_bytes = r32(&r);
    o->ceil.monthly = r32(&r);
    o->ceil.daily = r32(&r);
    o->ceil.trip = r32(&r);
    o->ceil.full_bundle = r32(&r);
    for (int i = 0; i < CAIRN_USAGE_TRIPS_TRACKED; i++) {
        o->trips[i].key = r64(&r);
        o->trips[i].bytes = r64(&r);
        for (int k = 0; k < CAIRN_TRAFFIC_KINDS; k++) o->trips[i].attempts[k] = r16(&r);
    }

    /* A CRC proves it is what was written, not that what was written is sane. */
    if (o->cfg.mode > CAIRN_USAGE_MODE_FULL_UP_TO_X) return false;
    if (o->cfg.billing_day < 1 || o->cfg.billing_day > 28) return false;
    if (o->cfg.alert_pct < 1 || o->cfg.alert_pct > 100) return false;
    if (o->ceil.monthly > CAIRN_USAGE_ABS_MONTHLY_BYTES || o->ceil.daily > CAIRN_USAGE_ABS_DAILY_BYTES ||
        o->ceil.trip > CAIRN_USAGE_ABS_TRIP_BYTES || o->ceil.full_bundle > CAIRN_USAGE_ABS_FULL_BUNDLE) {
        return false;
    }

    o->state_lost = (flags & F_LOST) != 0;
    o->cfg.lte_enabled = (flags & F_LTE) != 0;
    o->cfg.roaming_allowed = (flags & F_ROAMING) != 0;
    o->cfg.paused = (flags & F_PAUSED) != 0;
    o->breaker_open = (flags & F_BREAKER) != 0;
    *dirty = (flags & F_DIRTY) != 0;
    *inflight = (flags & F_INFLIGHT) != 0;
    return true;
}

typedef enum { SLOT_ABSENT, SLOT_CORRUPT, SLOT_VALID } slot_state_t;

static slot_state_t read_slot(const cairn_usage_t *u, const char *key, cairn_usage_t *img, uint32_t *seq,
                              bool *dirty, bool *inflight)
{
    uint8_t buf[IMG_MAX];
    size_t  len = 0;
    if (!u->store.read(u->store.ctx, key, buf, sizeof(buf), &len)) return SLOT_ABSENT;
    if (len > sizeof(buf)) return SLOT_CORRUPT;
    return parse_image(buf, len, img, seq, dirty, inflight) ? SLOT_VALID : SLOT_CORRUPT;
}

/* Write the whole state to the next slot. */
static bool write_image(cairn_usage_t *u, bool dirty)
{
    uint8_t buf[IMG_MAX];
    uint32_t seq = u->seq + 1;
    size_t n = build_image(u, seq, dirty, buf);

    if (!u->store.write(u->store.ctx, u->next_slot ? SLOT_B : SLOT_A, buf, n)) return false;

    u->seq = seq;
    u->next_slot ^= 1;
    u->dirty_on_flash = dirty;
    u->unflushed_lte = 0;
    u->unflushed_other = 0;
    u->flash_writes++;
    return true;
}

bool cairn_usage_flush(cairn_usage_t *u)
{
    /* Clean means nothing can be unaccounted for: only when no attempt is in flight. */
    return write_image(u, u->inflight);
}

/* ── arithmetic ───────────────────────────────────────────────────────────── */

static uint64_t sat_add(uint64_t a, uint64_t b)
{
    return (a + b < a) ? UINT64_MAX : a + b;
}

static uint64_t lte_day(const cairn_usage_t *u)
{
    return sat_add(u->day_bytes[CAIRN_PATH_LTE][0], u->day_bytes[CAIRN_PATH_LTE][1]);
}

static uint64_t lte_period(const cairn_usage_t *u)
{
    return sat_add(u->period_bytes[CAIRN_PATH_LTE][0], u->period_bytes[CAIRN_PATH_LTE][1]);
}

/* A cap never exceeds the runtime ceiling, whatever the stored config says. */
uint32_t cairn_usage_effective_cap(const cairn_usage_t *u, uint32_t configured, uint32_t ceiling)
{
    (void)u;
    /* MUTATION-SITE ceiling begin */
    return configured < ceiling ? configured : ceiling;
    /* MUTATION-SITE ceiling end */
}

/* ── time: day and billing-period rollover ────────────────────────────────── */

static void roll(cairn_usage_t *u, const cairn_usage_clock_t *now)
{
    if (now == NULL || now->utc_s == 0) return;  /* no time: keep counting into the current buckets */

    uint32_t day = (uint32_t)(now->utc_s / 86400u);
    if (u->day_index == 0) {
        u->day_index = day;                       /* adopt: bytes counted so far belong to today */
    } else if (day > u->day_index) {
        for (int p = 0; p < CAIRN_PATH_COUNT; p++) u->day_bytes[p][0] = u->day_bytes[p][1] = 0;
        u->day_index = day;
    }  /* a clock that went backwards resets nothing: rewinding it must not refill an allowance */

    int32_t pid = cairn_usage_period_id(now->utc_s, u->cfg.billing_day);
    if (u->period_id == PERIOD_UNKNOWN) {
        u->period_id = pid;
    } else if (pid > u->period_id) {
        for (int p = 0; p < CAIRN_PATH_COUNT; p++) u->period_bytes[p][0] = u->period_bytes[p][1] = 0;
        u->period_id = pid;
        u->state_lost = false;                    /* the unknown counts belonged to the old period */
    }
}

/* ── trips ────────────────────────────────────────────────────────────────── */

static int trip_find(const cairn_usage_t *u, uint64_t key)
{
    if (key == 0) return NO_TRIP;
    for (int i = 0; i < CAIRN_USAGE_TRIPS_TRACKED; i++) {
        if (u->trips[i].key == key) return i;
    }
    return NO_TRIP;
}

static int trip_get(cairn_usage_t *u, uint64_t key)
{
    int i = trip_find(u, key);
    if (i == NO_TRIP) {
        int victim = 0;
        for (int k = 0; k < CAIRN_USAGE_TRIPS_TRACKED; k++) {
            if (u->trips[k].key == 0) { victim = k; break; }
            if (u->trips[k].stamp < u->trips[victim].stamp) victim = k;
        }
        memset(&u->trips[victim], 0, sizeof(u->trips[victim]));
        u->trips[victim].key = key;
        i = victim;
    }
    u->trips[i].stamp = ++u->trip_stamp;
    return i;
}

/* ── circuit breaker ──────────────────────────────────────────────────────── */

static uint32_t cooldown_s(const cairn_usage_t *u, uint8_t exp)
{
    uint64_t c = u->tune.breaker_cooldown_s;
    for (uint8_t i = 0; i < exp && c < u->tune.breaker_cooldown_max_s; i++) c *= 2;
    if (c > u->tune.breaker_cooldown_max_s) c = u->tune.breaker_cooldown_max_s;
    return (uint32_t)c;
}

cairn_usage_breaker_t cairn_usage_breaker_state(cairn_usage_t *u, const cairn_usage_clock_t *now)
{
    if (!u->breaker_open) return CAIRN_BREAKER_CLOSED;

    uint32_t mono = now != NULL ? now->mono_s : 0;
    if (!u->breaker_deadline_valid) {
        /* Loaded open after a reboot: the monotonic clock restarted, so the
         * cooldown restarts too. Failing closed after a crash is the point. */
        u->breaker_open_until = mono + cooldown_s(u, u->breaker_exp > 0 ? (uint8_t)(u->breaker_exp - 1) : 0);
        u->breaker_deadline_valid = true;
    }
    return mono < u->breaker_open_until ? CAIRN_BREAKER_OPEN : CAIRN_BREAKER_HALF_OPEN;
}

static void breaker_fail(cairn_usage_t *u, const cairn_usage_clock_t *now)
{
    if (u->consec_failures < UINT16_MAX) u->consec_failures++;

    /* A failed probe (the breaker was already open) re-opens it, longer; so does
     * reaching the threshold while closed. */
    if (u->breaker_open || u->consec_failures >= u->tune.breaker_threshold) {
        uint32_t mono = now != NULL ? now->mono_s : 0;
        u->breaker_open_until = mono + cooldown_s(u, u->breaker_exp);
        u->breaker_deadline_valid = true;
        u->breaker_open = true;
        if (u->breaker_exp < 30) u->breaker_exp++;
    }
}

/* ── init ─────────────────────────────────────────────────────────────────── */

cairn_usage_load_t cairn_usage_init(cairn_usage_t *u, const cairn_usage_store_t *store,
                                    const cairn_usage_tuning_t *tune, const cairn_usage_clock_t *now)
{
    memset(u, 0, sizeof(*u));
    u->store = (store != NULL) ? *store : *cairn_usage_kv_store();
    if (tune != NULL) u->tune = *tune; else cairn_usage_tuning_defaults(&u->tune);
    cairn_usage_config_defaults(&u->cfg);
    cairn_usage_ceilings_defaults(&u->ceil);
    u->period_id = PERIOD_UNKNOWN;
    u->inflight_trip = NO_TRIP;

    static cairn_usage_t a, b;   /* scratch images; init is not reentrant */
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    uint32_t sa = 0, sb = 0;
    bool da = false, db = false, ia = false, ib = false;
    slot_state_t ta = read_slot(u, SLOT_A, &a, &sa, &da, &ia);
    slot_state_t tb = read_slot(u, SLOT_B, &b, &sb, &db, &ib);

    bool need_persist = false;

    if (ta == SLOT_ABSENT && tb == SLOT_ABSENT) {
        u->loaded = CAIRN_LOAD_FRESH;
    } else if (ta != SLOT_VALID && tb != SLOT_VALID) {
        u->loaded = CAIRN_LOAD_LOST;
        u->state_lost = true;
        need_persist = true;
    } else {
        bool use_a = (ta == SLOT_VALID) && (tb != SLOT_VALID || sa >= sb);
        const cairn_usage_t *img = use_a ? &a : &b;
        bool dirty = use_a ? da : db;
        bool inflight = use_a ? ia : ib;
        slot_state_t other = use_a ? tb : ta;

        u->seq = use_a ? sa : sb;
        u->next_slot = use_a ? 1 : 0;
        u->cfg = img->cfg;
        u->ceil = img->ceil;
        memcpy(u->day_bytes, img->day_bytes, sizeof(u->day_bytes));
        memcpy(u->period_bytes, img->period_bytes, sizeof(u->period_bytes));
        u->day_index = img->day_index;
        u->period_id = img->period_id;
        memcpy(u->trips, img->trips, sizeof(u->trips));
        u->consec_failures = img->consec_failures;
        u->breaker_exp = img->breaker_exp;
        u->breaker_open = img->breaker_open;
        u->state_lost = img->state_lost;
        u->loaded = (other == SLOT_CORRUPT) ? CAIRN_LOAD_RECOVERED : CAIRN_LOAD_OK;

        uint64_t margin = 0;
        if (dirty) margin += 2ull * u->tune.batch_bytes;           /* unflushed batch + chunk in flight */
        if (other == SLOT_CORRUPT) margin += 2ull * u->tune.batch_bytes;  /* the torn write's batch */
        if (margin > 0) {
            /* Over-count, never under-count: the cap can only be reached early. */
            u->day_bytes[CAIRN_PATH_LTE][0] = sat_add(u->day_bytes[CAIRN_PATH_LTE][0], margin);
            u->period_bytes[CAIRN_PATH_LTE][0] = sat_add(u->period_bytes[CAIRN_PATH_LTE][0], margin);
            need_persist = true;
        }
        if (inflight) {
            /* Power was lost, or the firmware crashed, during an upload. An
             * attempt that never ended is a failed attempt. */
            breaker_fail(u, now);
            need_persist = true;
        }
        if (other == SLOT_CORRUPT) need_persist = true;
    }

    if (now != NULL) roll(u, now);

    /* inflight was loaded only to be judged above; nothing is in flight now. */
    if (need_persist) (void)write_image(u, false);
    /* A lost state is written to BOTH slots: otherwise the surviving corrupt slot
     * would make every later boot look like a recovery and add the margin again. */
    if (u->loaded == CAIRN_LOAD_LOST) (void)write_image(u, false);
    return u->loaded;
}

/* ── counting ─────────────────────────────────────────────────────────────── */

void cairn_usage_record(cairn_usage_t *u, cairn_usage_path_t path, uint32_t up, uint32_t down,
                        const cairn_usage_clock_t *now)
{
    if ((unsigned)path >= CAIRN_PATH_COUNT) return;
    roll(u, now);

    u->day_bytes[path][0] = sat_add(u->day_bytes[path][0], up);
    u->day_bytes[path][1] = sat_add(u->day_bytes[path][1], down);
    u->period_bytes[path][0] = sat_add(u->period_bytes[path][0], up);
    u->period_bytes[path][1] = sat_add(u->period_bytes[path][1], down);

    uint64_t n = (uint64_t)up + down;

    if (path == CAIRN_PATH_LTE) {
        if (u->inflight && u->inflight_trip != NO_TRIP) {
            u->trips[u->inflight_trip].bytes = sat_add(u->trips[u->inflight_trip].bytes, n);
        }
        u->unflushed_lte += n;
        /* Traffic is moving: the image on flash must say it may be unflushed. */
        if (!u->dirty_on_flash || u->unflushed_lte >= u->tune.batch_bytes) (void)write_image(u, true);
    } else {
        u->unflushed_other += n;
        if (u->unflushed_other >= u->tune.batch_other_bytes) (void)write_image(u, u->dirty_on_flash);
    }
}

void cairn_usage_note_sim_counters(cairn_usage_t *u, uint64_t up, uint64_t down)
{
    u->sim_valid = true;
    u->sim_up = up;
    u->sim_down = down;
}

/* ── the gate ─────────────────────────────────────────────────────────────── */

static cairn_usage_reason_t gate(cairn_usage_t *u, const cairn_usage_req_t *req,
                                 const cairn_usage_clock_t *now)
{
    roll(u, now);

    if (u->state_lost) return CAIRN_STOP_STATE_LOST;
    /* If writes keep failing, the over-count margin no longer bounds the error. */
    if (u->unflushed_lte > 2ull * u->tune.batch_bytes) return CAIRN_STOP_STORE_FAILED;
    if (!u->cfg.lte_enabled) return CAIRN_STOP_DISABLED;
    if (u->cfg.paused) return CAIRN_STOP_PAUSED;
    if (req->roaming && !u->cfg.roaming_allowed) return CAIRN_STOP_ROAMING;

    if (req->kind == CAIRN_TRAFFIC_FULL_BUNDLE) {
        if (u->cfg.mode != CAIRN_USAGE_MODE_FULL_UP_TO_X) return CAIRN_STOP_MODE_DIGESTS_ONLY;
        uint32_t x = cairn_usage_effective_cap(u, u->cfg.full_bundle_max_bytes, u->ceil.full_bundle);
        if (req->bytes_wanted > x) return CAIRN_STOP_BUNDLE_TOO_LARGE;
    }

    int ti = trip_find(u, req->trip_key);

    if (!req->continuing) {
        if (u->inflight) return CAIRN_STOP_BUSY;
        if (cairn_usage_breaker_state(u, now) == CAIRN_BREAKER_OPEN) return CAIRN_STOP_BREAKER_OPEN;
        if (ti != NO_TRIP) {
            if (u->trips[ti].attempts[req->kind] >= u->tune.max_attempts_per_trip) return CAIRN_STOP_TRIP_ATTEMPTS;
            if (now != NULL && u->trips[ti].next_ok_mono > now->mono_s) return CAIRN_STOP_BACKOFF;
        }
    }

    /* The caps. "Used + wanted over the cap" refuses a transfer that cannot finish
     * rather than starting it and stopping half-way; "used at the cap" refuses even a
     * zero-byte question. */
    uint64_t want = req->bytes_wanted;

    uint64_t used = lte_period(u);
    uint64_t cap = cairn_usage_effective_cap(u, u->cfg.monthly_cap_bytes, u->ceil.monthly);
    if (used >= cap || used + want > cap) return CAIRN_STOP_CAP_MONTHLY;

    used = lte_day(u);
    cap = cairn_usage_effective_cap(u, u->cfg.daily_cap_bytes, u->ceil.daily);
    if (used >= cap || used + want > cap) return CAIRN_STOP_CAP_DAILY;

    if (req->trip_key != 0) {
        used = (ti != NO_TRIP) ? u->trips[ti].bytes : 0;
        cap = cairn_usage_effective_cap(u, u->cfg.trip_cap_bytes, u->ceil.trip);
        if (used >= cap || used + want > cap) return CAIRN_STOP_CAP_TRIP;
    }

    return CAIRN_STOP_NONE;
}

cairn_usage_reason_t cairn_usage_gate(cairn_usage_t *u, const cairn_usage_req_t *req,
                                      const cairn_usage_clock_t *now)
{
    return gate(u, req, now);
}

cairn_usage_reason_t cairn_usage_attempt_begin(cairn_usage_t *u, const cairn_usage_req_t *req,
                                               const cairn_usage_clock_t *now)
{
    cairn_usage_req_t r = *req;
    r.continuing = false;

    cairn_usage_reason_t why = gate(u, &r, now);
    if (why != CAIRN_STOP_NONE) return why;

    int ti = NO_TRIP;
    if (req->trip_key != 0) {
        ti = trip_get(u, req->trip_key);
        u->trips[ti].attempts[req->kind]++;
    }
    u->inflight = true;
    u->inflight_trip = ti;
    u->inflight_kind = req->kind;
    u->probe_inflight = (cairn_usage_breaker_state(u, now) == CAIRN_BREAKER_HALF_OPEN);

    /* Write-ahead: the attempt is on flash, and the image says traffic may follow
     * (dirty), before any byte moves. If that cannot be written nothing is sent. */
    if (!write_image(u, true)) {
        if (ti != NO_TRIP) u->trips[ti].attempts[req->kind]--;
        u->inflight = false;
        u->inflight_trip = NO_TRIP;
        u->probe_inflight = false;
        return CAIRN_STOP_STORE_FAILED;
    }
    return CAIRN_STOP_NONE;
}

void cairn_usage_attempt_end(cairn_usage_t *u, bool success, const cairn_usage_clock_t *now)
{
    if (!u->inflight) return;

    int ti = u->inflight_trip;
    uint16_t attempts = (ti != NO_TRIP) ? u->trips[ti].attempts[u->inflight_kind] : 0;

    u->inflight = false;
    u->inflight_trip = NO_TRIP;
    u->probe_inflight = false;

    if (success) {
        u->consec_failures = 0;
        u->breaker_open = false;
        u->breaker_deadline_valid = false;
        u->breaker_exp = 0;
        if (ti != NO_TRIP) u->trips[ti].next_ok_mono = 0;
    } else {
        breaker_fail(u, now);
        if (ti != NO_TRIP) {
            uint64_t d = u->tune.backoff_base_s;
            for (uint16_t i = 1; i < attempts && d < u->tune.backoff_max_s; i++) d *= 2;
            if (d > u->tune.backoff_max_s) d = u->tune.backoff_max_s;
            u->trips[ti].next_ok_mono = (now != NULL ? now->mono_s : 0) + (uint32_t)d;
        }
    }

    /* Nothing is in flight now, so this flush is clean. */
    (void)write_image(u, false);
}

/* ── configuration ────────────────────────────────────────────────────────── */

static bool config_loosens(const cairn_usage_config_t *o, const cairn_usage_config_t *n)
{
    return (!o->lte_enabled && n->lte_enabled) ||
           (!o->roaming_allowed && n->roaming_allowed) ||
           (o->paused && !n->paused) ||
           (o->mode == CAIRN_USAGE_MODE_DIGESTS_ONLY && n->mode == CAIRN_USAGE_MODE_FULL_UP_TO_X) ||
           n->monthly_cap_bytes > o->monthly_cap_bytes ||
           n->daily_cap_bytes > o->daily_cap_bytes ||
           n->trip_cap_bytes > o->trip_cap_bytes ||
           n->full_bundle_max_bytes > o->full_bundle_max_bytes ||
           n->alert_pct > o->alert_pct ||            /* fewer warnings */
           n->billing_day != o->billing_day;         /* could reset a period early */
}

cairn_usage_cfg_result_t cairn_usage_apply_config(cairn_usage_t *u, const cairn_usage_config_t *c,
                                                  cairn_usage_auth_t auth, const cairn_usage_clock_t *now)
{
    if (c->mode > CAIRN_USAGE_MODE_FULL_UP_TO_X || c->billing_day < 1 || c->billing_day > 28 ||
        c->alert_pct < 1 || c->alert_pct > 100) {
        return CAIRN_CFG_INVALID;
    }
    if (c->monthly_cap_bytes > u->ceil.monthly || c->daily_cap_bytes > u->ceil.daily ||
        c->trip_cap_bytes > u->ceil.trip || c->full_bundle_max_bytes > u->ceil.full_bundle) {
        return CAIRN_CFG_ABOVE_CEILING;
    }

    /* MUTATION-SITE unsigned-loosen begin */
    if (config_loosens(&u->cfg, c) && auth != CAIRN_AUTH_SIGNED) return CAIRN_CFG_UNSIGNED_LOOSEN;
    /* MUTATION-SITE unsigned-loosen end */

    cairn_usage_config_t old = u->cfg;
    u->cfg = *c;
    if (!write_image(u, u->inflight || u->dirty_on_flash)) {
        u->cfg = old;
        return CAIRN_CFG_STORE_FAILED;
    }
    /* A new billing day re-bases the period id without touching the counts, so the
     * next rollover lands on the new day instead of a month late. (Only a signed
     * message gets here with a changed day.) */
    if (c->billing_day != old.billing_day) u->period_id = PERIOD_UNKNOWN;
    roll(u, now);
    return CAIRN_CFG_OK;
}

cairn_usage_cfg_result_t cairn_usage_set_ceilings(cairn_usage_t *u, const cairn_usage_ceilings_t *c,
                                                  cairn_usage_auth_t auth)
{
    if (c->monthly > CAIRN_USAGE_ABS_MONTHLY_BYTES || c->daily > CAIRN_USAGE_ABS_DAILY_BYTES ||
        c->trip > CAIRN_USAGE_ABS_TRIP_BYTES || c->full_bundle > CAIRN_USAGE_ABS_FULL_BUNDLE) {
        return CAIRN_CFG_ABOVE_CEILING;
    }

    bool raises = c->monthly > u->ceil.monthly || c->daily > u->ceil.daily || c->trip > u->ceil.trip ||
                  c->full_bundle > u->ceil.full_bundle;
    /* MUTATION-SITE ceiling-raise begin */
    if (raises && auth != CAIRN_AUTH_SIGNED) return CAIRN_CFG_UNSIGNED_LOOSEN;
    /* MUTATION-SITE ceiling-raise end */

    cairn_usage_ceilings_t old = u->ceil;
    u->ceil = *c;
    if (!write_image(u, u->inflight || u->dirty_on_flash)) {
        u->ceil = old;
        return CAIRN_CFG_STORE_FAILED;
    }
    return CAIRN_CFG_OK;
}

bool cairn_usage_clear_state_lost(cairn_usage_t *u, cairn_usage_auth_t auth)
{
    if (auth != CAIRN_AUTH_SIGNED) return false;
    memset(u->day_bytes, 0, sizeof(u->day_bytes));
    memset(u->period_bytes, 0, sizeof(u->period_bytes));
    u->state_lost = false;
    return write_image(u, u->inflight);
}

/* ── report ───────────────────────────────────────────────────────────────── */

static uint32_t pct_of(uint64_t used, uint32_t cap)
{
    if (cap == 0) return 100;
    uint64_t p = used * 100u / cap;
    return p > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)p;
}

void cairn_usage_report(cairn_usage_t *u, const cairn_usage_clock_t *now, bool roaming,
                        cairn_usage_report_t *o)
{
    roll(u, now);
    memset(o, 0, sizeof(*o));

    cairn_usage_req_t q;
    memset(&q, 0, sizeof(q));
    q.kind = CAIRN_TRAFFIC_DIGEST;
    q.roaming = roaming;
    q.continuing = u->inflight;
    o->reason = gate(u, &q, now);

    o->lte_enabled = u->cfg.lte_enabled;
    o->paused = u->cfg.paused;
    o->roaming_allowed = u->cfg.roaming_allowed;
    o->mode = u->cfg.mode;
    o->billing_day = u->cfg.billing_day;
    o->alert_pct = u->cfg.alert_pct;

    for (int p = 0; p < CAIRN_PATH_COUNT; p++) {
        o->day_up[p] = u->day_bytes[p][0];
        o->day_down[p] = u->day_bytes[p][1];
        o->period_up[p] = u->period_bytes[p][0];
        o->period_down[p] = u->period_bytes[p][1];
    }
    o->day_index = u->day_index;
    o->period_id = u->period_id;

    o->monthly_cap = cairn_usage_effective_cap(u, u->cfg.monthly_cap_bytes, u->ceil.monthly);
    o->daily_cap = cairn_usage_effective_cap(u, u->cfg.daily_cap_bytes, u->ceil.daily);
    o->trip_cap = cairn_usage_effective_cap(u, u->cfg.trip_cap_bytes, u->ceil.trip);
    o->full_bundle_max = cairn_usage_effective_cap(u, u->cfg.full_bundle_max_bytes, u->ceil.full_bundle);

    o->monthly_pct = pct_of(lte_period(u), o->monthly_cap);
    o->daily_pct = pct_of(lte_day(u), o->daily_cap);
    if (o->monthly_pct >= u->cfg.alert_pct) o->alerts |= CAIRN_ALERT_MONTHLY_THRESHOLD;
    if (lte_period(u) >= o->monthly_cap) o->alerts |= CAIRN_ALERT_MONTHLY_REACHED;
    if (o->daily_pct >= u->cfg.alert_pct) o->alerts |= CAIRN_ALERT_DAILY_THRESHOLD;
    if (lte_day(u) >= o->daily_cap) o->alerts |= CAIRN_ALERT_DAILY_REACHED;

    o->breaker = cairn_usage_breaker_state(u, now);
    o->consecutive_failures = u->consec_failures;
    o->state_lost = u->state_lost;
    o->flash_writes = u->flash_writes;
    o->loaded = u->loaded;
    o->sim_valid = u->sim_valid;
    o->sim_up = u->sim_up;
    o->sim_down = u->sim_down;
}
