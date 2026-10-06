/*
 * LTE data accounting and limits: a simulated modem in front of the real
 * cairn_usage code, with power cuts and torn writes injected into its store.
 *
 * PROVISIONAL, like the code under test: contracts/config/v1 is unreleased, so
 * there are no contract vectors and "an authorised message" is a flag the test
 * sets (CAIRN_AUTH_SIGNED); no config crypto exists here. What these rows pin is
 * behaviour that must survive the contract:
 *
 *   - a cap stops traffic, and the report says why;
 *   - the count survives a power cut and a torn write, erring only upward;
 *   - an unsigned message cannot raise a limit or the ceiling;
 *   - a stuck retry loop is impossible, including by rebooting.
 *
 * The fault injection follows test/host/faults.c: each row arms a fault that would
 * violate the claim and asserts what survived, and randomised rows print the seed
 * that reproduces them.
 *
 *   ./usage
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cairn_kv.h"
#include "cairn_log.h"
#include "cairn_usage.h"

void cairn_kv_host_set_path(const char *path);

/* ── harness ──────────────────────────────────────────────────────────────── */

static int  g_pass, g_fail;
static char g_failures[64][512];
static int  g_failure_count;
static const char *g_row;
static uint64_t g_seed;

static void fail(const char *fmt, ...)
{
    char detail[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    if (g_failure_count < 64) {
        snprintf(g_failures[g_failure_count++], 512, "%s (seed %llu): %s", g_row, (unsigned long long)g_seed, detail);
    }
}

#define CHECK(cond, ...)       \
    do {                       \
        if (!(cond)) {         \
            fail(__VA_ARGS__); \
            return false;      \
        }                      \
    } while (0)

static uint64_t g_rng;
static uint32_t rnd(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 0x2545F4914F6CDD1DULL) >> 32);
}

/* ── an in-memory store with a power switch ───────────────────────────────── */

typedef struct {
    uint8_t data[2][512];
    size_t  len[2];
    bool    present[2];
    bool    dead;            /* power is out: every write is lost */
    int     tear_after;      /* >= 0: the Nth write from now is torn, then power dies */
    int     writes;
} mem_store_t;

static mem_store_t g_mem;

static int slot_of(const char *key) { return strcmp(key, "usage_b") == 0 ? 1 : 0; }

static bool mem_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    mem_store_t *m = ctx;
    int s = slot_of(key);
    if (!m->present[s]) return false;
    *len = m->len[s];
    if (m->len[s] <= cap) memcpy(buf, m->data[s], m->len[s]);
    return true;
}

static bool mem_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    mem_store_t *m = ctx;
    if (m->dead) return false;
    int s = slot_of(key);
    m->writes++;
    if (m->tear_after >= 0 && m->tear_after-- == 0) {
        /* The lights go out mid-write: a prefix of the new image over the old slot. */
        size_t keep = len / 2;
        memcpy(m->data[s], buf, keep);
        m->len[s] = len;           /* the length is intact, the tail is not: the CRC must catch it */
        m->present[s] = true;
        memset(m->data[s] + keep, 0xEE, len - keep);
        m->dead = true;
        return false;
    }
    memcpy(m->data[s], buf, len);
    m->len[s] = len;
    m->present[s] = true;
    return true;
}

static const cairn_usage_store_t MEM = { mem_read, mem_write, &g_mem };

static void mem_reset(void)
{
    memset(&g_mem, 0, sizeof(g_mem));
    g_mem.tear_after = -1;
}

/* ── clock ────────────────────────────────────────────────────────────────── */

static cairn_usage_clock_t g_now;

/* days_from_civil, to build timestamps readably. */
static uint64_t ymd(int y, unsigned m, unsigned d, unsigned hh)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (uint64_t)days * 86400u + hh * 3600u;
}

/* ── a simulated modem ────────────────────────────────────────────────────── */

typedef struct {
    uint64_t air_up, air_down;     /* what the carrier's meter saw: the truth */
    uint32_t attempts_started;
    bool     roaming;
    bool     fail_transfer;        /* the link comes up and the transfer fails */
} sim_t;

static int g_cut_chunk = -1;       /* >= 0: power dies after that chunk is sent, before it is recorded */
static int g_chunk_no;

typedef enum { UP_OK, UP_FAILED, UP_REFUSED, UP_CUT } up_t;

static uint64_t air_total(const sim_t *m) { return m->air_up + m->air_down; }

static cairn_usage_tuning_t tuning(void)
{
    cairn_usage_tuning_t t;
    cairn_usage_tuning_defaults(&t);
    return t;
}

/*
 * One upload attempt for `trip`: gate it, then move `total` bytes in chunks,
 * asking before every chunk and recording after it, the way the modem layer will.
 * A chunk is `n` bytes up and n/10 of protocol overhead down.
 */
static up_t upload(cairn_usage_t *u, sim_t *m, uint64_t trip, cairn_usage_traffic_t kind, uint32_t total,
                   uint32_t chunk, bool declare_total, cairn_usage_reason_t *why)
{
    cairn_usage_req_t req = { kind, trip, declare_total ? total + total / 10 : 0, m->roaming, false };
    cairn_usage_reason_t r = cairn_usage_attempt_begin(u, &req, &g_now);
    if (g_mem.dead) return UP_CUT;
    if (r != CAIRN_STOP_NONE) {
        *why = r;
        return UP_REFUSED;
    }
    m->attempts_started++;

    uint32_t sent = 0;
    while (sent < total) {
        uint32_t n = total - sent < chunk ? total - sent : chunk;
        uint32_t down = n / 10;

        cairn_usage_req_t cr = { kind, trip, n + down, m->roaming, true };
        r = cairn_usage_gate(u, &cr, &g_now);
        if (r != CAIRN_STOP_NONE) {
            cairn_usage_attempt_end(u, false, &g_now);
            *why = r;
            return UP_REFUSED;
        }

        m->air_up += n;
        m->air_down += down;
        if (g_cut_chunk >= 0 && g_chunk_no++ == g_cut_chunk) {
            g_mem.dead = true;      /* sent, never recorded */
            return UP_CUT;
        }
        cairn_usage_record(u, CAIRN_PATH_LTE, n, down, &g_now);
        if (g_mem.dead) return UP_CUT;
        sent += n;
    }

    bool ok = !m->fail_transfer;
    cairn_usage_attempt_end(u, ok, &g_now);
    if (g_mem.dead) return UP_CUT;
    return ok ? UP_OK : UP_FAILED;
}

static uint64_t lte_period_total(const cairn_usage_t *u)
{
    return u->period_bytes[CAIRN_PATH_LTE][0] + u->period_bytes[CAIRN_PATH_LTE][1];
}

/* Power-cycle: RAM is gone, the store is what it is, then the device boots. */
static cairn_usage_load_t power_cycle(cairn_usage_t *u, const cairn_usage_tuning_t *t)
{
    g_mem.dead = false;
    g_mem.tear_after = -1;
    g_cut_chunk = -1;
    g_now.mono_s = 0;           /* the monotonic clock restarts */
    return cairn_usage_init(u, &MEM, t, &g_now);
}

/* LTE on, with the given caps (all within the compiled ceilings). */
static bool enable(cairn_usage_t *u, uint32_t monthly, uint32_t daily, uint32_t trip)
{
    cairn_usage_config_t c;
    cairn_usage_config_defaults(&c);
    c.lte_enabled = true;
    c.monthly_cap_bytes = monthly;
    c.daily_cap_bytes = daily;
    c.trip_cap_bytes = trip;
    return cairn_usage_apply_config(u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_OK;
}

static void fresh(cairn_usage_t *u, const cairn_usage_tuning_t *t)
{
    mem_reset();
    g_now.utc_s = ymd(2026, 10, 5, 12);
    g_now.mono_s = 0;
    g_cut_chunk = -1;
    g_chunk_no = 0;
    cairn_usage_init(u, &MEM, t, &g_now);
}

static cairn_usage_t g_u;

/* ── rows: defaults and policy ────────────────────────────────────────────── */

static bool row_defaults_are_closed(void)
{
    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);

    cairn_usage_report_t r;
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(!r.lte_enabled, "LTE is on by default");
    CHECK(!r.roaming_allowed, "roaming is on by default");
    CHECK(r.mode == CAIRN_USAGE_MODE_DIGESTS_ONLY, "full bundles are on by default");
    CHECK(!r.paused, "paused by default");
    CHECK(r.reason == CAIRN_STOP_DISABLED, "reason %s, want DISABLED", cairn_usage_reason_name(r.reason));
    CHECK(r.monthly_cap <= CAIRN_USAGE_CEIL_MONTHLY_BYTES && r.daily_cap <= CAIRN_USAGE_CEIL_DAILY_BYTES,
          "default caps above the ceilings");
    CHECK(r.loaded == CAIRN_LOAD_FRESH, "first boot should be FRESH");

    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why = CAIRN_STOP_NONE;
    CHECK(upload(&g_u, &m, 0x11, CAIRN_TRAFFIC_DIGEST, 2000, 500, true, &why) == UP_REFUSED && why == CAIRN_STOP_DISABLED,
          "an unconfigured device sent traffic");
    CHECK(air_total(&m) == 0, "bytes left the radio");
    return true;
}

static bool row_policy_reasons(void)
{
    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 1000000, 500000, 200000), "enable");

    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why = CAIRN_STOP_NONE;

    CHECK(upload(&g_u, &m, 0x21, CAIRN_TRAFFIC_DIGEST, 2000, 500, true, &why) == UP_OK, "a digest should go");

    /* Full bundles: refused in the default mode, with the right reason. */
    CHECK(upload(&g_u, &m, 0x22, CAIRN_TRAFFIC_FULL_BUNDLE, 50000, 1000, true, &why) == UP_REFUSED &&
              why == CAIRN_STOP_MODE_DIGESTS_ONLY, "full bundle in digests-only mode: %s", cairn_usage_reason_name(why));

    cairn_usage_config_t c = g_u.cfg;
    c.mode = CAIRN_USAGE_MODE_FULL_UP_TO_X;
    c.full_bundle_max_bytes = 100000;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_OK, "mode change");
    CHECK(upload(&g_u, &m, 0x22, CAIRN_TRAFFIC_FULL_BUNDLE, 50000, 1000, true, &why) == UP_OK, "bundle under X should go");
    CHECK(upload(&g_u, &m, 0x23, CAIRN_TRAFFIC_FULL_BUNDLE, 150000, 1000, true, &why) == UP_REFUSED &&
              why == CAIRN_STOP_BUNDLE_TOO_LARGE, "bundle over X: %s", cairn_usage_reason_name(why));

    /* Roaming is off by default. */
    m.roaming = true;
    CHECK(upload(&g_u, &m, 0x24, CAIRN_TRAFFIC_DIGEST, 2000, 500, true, &why) == UP_REFUSED && why == CAIRN_STOP_ROAMING,
          "roaming: %s", cairn_usage_reason_name(why));
    uint64_t before = air_total(&m);
    m.roaming = false;

    /* Pause is immediate and says so. An unsigned message may pause (it only tightens). */
    c = g_u.cfg;
    c.paused = true;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_NONE, &g_now) == CAIRN_CFG_OK, "unsigned pause refused");
    CHECK(upload(&g_u, &m, 0x25, CAIRN_TRAFFIC_DIGEST, 2000, 500, true, &why) == UP_REFUSED && why == CAIRN_STOP_PAUSED,
          "paused: %s", cairn_usage_reason_name(why));
    cairn_usage_report_t r;
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.reason == CAIRN_STOP_PAUSED && r.paused, "the report does not say why traffic is stopped");
    CHECK(air_total(&m) == before, "bytes moved while refused");

    /* Resuming loosens: unsigned cannot, signed can. */
    c.paused = false;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_NONE, &g_now) == CAIRN_CFG_UNSIGNED_LOOSEN, "unsigned resume accepted");
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_OK, "signed resume");

    /* CONTROL traffic is allowed in digests-only mode. */
    cairn_usage_config_t d = g_u.cfg;
    d.mode = CAIRN_USAGE_MODE_DIGESTS_ONLY;
    CHECK(cairn_usage_apply_config(&g_u, &d, CAIRN_AUTH_NONE, &g_now) == CAIRN_CFG_OK, "tighten to digests-only");
    CHECK(upload(&g_u, &m, 0, CAIRN_TRAFFIC_CONTROL, 600, 300, true, &why) == UP_OK, "control traffic refused: %s",
          cairn_usage_reason_name(why));
    return true;
}

/* ── rows: caps ───────────────────────────────────────────────────────────── */

/* A cap stops traffic. The carrier's meter, not our counter, is what must stay under it. */
static bool row_monthly_cap_stops_traffic(void)
{
    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);
    const uint32_t cap = 100000;
    CHECK(enable(&g_u, cap, 1000000, 1000000), "enable");

    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why = CAIRN_STOP_NONE;
    int sent = 0;
    for (int i = 0; i < 200; i++) {
        t.max_attempts_per_trip = 5;
        up_t r = upload(&g_u, &m, 0x1000 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 7000, 1000, true, &why);
        if (r == UP_OK) { sent++; continue; }
        CHECK(r == UP_REFUSED, "unexpected result %d", r);
        break;
    }
    CHECK(why == CAIRN_STOP_CAP_MONTHLY, "stopped for %s, want CAP_MONTHLY", cairn_usage_reason_name(why));
    CHECK(sent >= 10 && sent < 200, "sent %d digests", sent);
    CHECK(air_total(&m) <= cap, "the carrier's meter reads %llu, over the %u cap", (unsigned long long)air_total(&m), cap);

    uint64_t at_stop = air_total(&m);
    for (int i = 0; i < 100; i++) {
        /* Another full digest cannot fit in what is left. */
        CHECK(upload(&g_u, &m, 0x9000 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 7000, 1000, true, &why) == UP_REFUSED,
              "traffic resumed past the cap");
    }
    CHECK(air_total(&m) == at_stop, "bytes moved after the cap");

    cairn_usage_report_t r;
    /* What is left is smaller than any digest; once other traffic uses it the cap is reached. */
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, (uint32_t)(cap - lte_period_total(&g_u)), 0, &g_now);
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.reason == CAIRN_STOP_CAP_MONTHLY, "report reason %s", cairn_usage_reason_name(r.reason));
    CHECK(r.monthly_pct >= 90, "report pct %u", r.monthly_pct);
    CHECK((r.alerts & CAIRN_ALERT_MONTHLY_THRESHOLD), "no threshold alert at %u%%", r.monthly_pct);

    /* The cap lifts with the billing period, and only then. */
    g_now.utc_s = ymd(2026, 11, 1, 0);
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.reason == CAIRN_STOP_NONE, "still stopped in the new period: %s", cairn_usage_reason_name(r.reason));
    CHECK(upload(&g_u, &m, 0xA000, CAIRN_TRAFFIC_DIGEST, 2000, 1000, true, &why) == UP_OK, "no traffic in the new period");
    return true;
}

static bool row_daily_and_trip_caps(void)
{
    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 10000000, 60000, 20000), "enable");

    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why = CAIRN_STOP_NONE;

    /* Per trip: unknown-length traffic is stopped mid-transfer, between chunks. */
    up_t r = upload(&g_u, &m, 0x31, CAIRN_TRAFFIC_DIGEST, 100000, 1000, false, &why);
    CHECK(r == UP_REFUSED && why == CAIRN_STOP_CAP_TRIP, "per-trip: %d %s", r, cairn_usage_reason_name(why));
    CHECK(air_total(&m) <= 20000, "trip used %llu of a 20000 cap", (unsigned long long)air_total(&m));
    CHECK(air_total(&m) > 15000, "stopped too early (%llu)", (unsigned long long)air_total(&m));

    /* A trip that is over its cap stays over it, across retries and a reboot. */
    uint64_t at = air_total(&m);
    CHECK(power_cycle(&g_u, &t) == CAIRN_LOAD_OK, "reload");
    t.max_attempts_per_trip = 5;
    g_now.mono_s = 100000;
    r = upload(&g_u, &m, 0x31, CAIRN_TRAFFIC_DIGEST, 1000, 500, true, &why);
    CHECK(r == UP_REFUSED && why == CAIRN_STOP_CAP_TRIP, "the trip's cap did not survive a reboot: %s", cairn_usage_reason_name(why));
    CHECK(air_total(&m) == at, "bytes moved");

    /* Per day: other trips go until the day is spent. */
    for (int i = 0; i < 50; i++) {
        g_now.mono_s += 100000;
        r = upload(&g_u, &m, 0x40 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 9000, 1000, true, &why);
        if (r != UP_OK) break;
    }
    CHECK(r == UP_REFUSED && why == CAIRN_STOP_CAP_DAILY, "daily: %s", cairn_usage_reason_name(why));
    CHECK(air_total(&m) <= 60000, "the day used %llu of 60000", (unsigned long long)air_total(&m));

    /* Midnight UTC refills the day, not the month. */
    g_now.utc_s = ymd(2026, 10, 6, 0);
    cairn_usage_report_t rep;
    cairn_usage_report(&g_u, &g_now, false, &rep);
    CHECK(rep.reason == CAIRN_STOP_NONE, "the next day is still stopped: %s", cairn_usage_reason_name(rep.reason));
    CHECK(rep.period_up[CAIRN_PATH_LTE] + rep.period_down[CAIRN_PATH_LTE] >= 50000, "the period lost its count at midnight");
    return true;
}

static bool row_alert_thresholds_and_paths(void)
{
    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 100000, 1000000, 1000000), "enable");

    cairn_usage_report_t r;
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 70000, 5000, &g_now);   /* 75% */
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.monthly_pct == 75 && !(r.alerts & CAIRN_ALERT_MONTHLY_THRESHOLD), "75%%: pct %u alerts %x", r.monthly_pct, r.alerts);
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 4000, 1000, &g_now);    /* 80% */
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.monthly_pct == 80 && (r.alerts & CAIRN_ALERT_MONTHLY_THRESHOLD) && !(r.alerts & CAIRN_ALERT_MONTHLY_REACHED),
          "80%%: pct %u alerts %x", r.monthly_pct, r.alerts);
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 20000, 0, &g_now);      /* 100% */
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK((r.alerts & CAIRN_ALERT_MONTHLY_REACHED), "no REACHED alert at the cap");

    /* Wi-Fi and BLE are counted, separately, and never count against LTE caps. */
    cairn_usage_record(&g_u, CAIRN_PATH_WIFI, 5000000, 1000, &g_now);
    cairn_usage_record(&g_u, CAIRN_PATH_BLE, 3000, 4000, &g_now);
    cairn_usage_note_sim_counters(&g_u, 111, 222);
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.period_up[CAIRN_PATH_WIFI] == 5000000 && r.period_down[CAIRN_PATH_WIFI] == 1000, "wifi count");
    CHECK(r.period_up[CAIRN_PATH_BLE] == 3000 && r.day_down[CAIRN_PATH_BLE] == 4000, "ble count");
    CHECK(r.period_up[CAIRN_PATH_LTE] == 94000 + 0 + 0 + 0 || r.period_up[CAIRN_PATH_LTE] == 94000, "lte up %llu",
          (unsigned long long)r.period_up[CAIRN_PATH_LTE]);
    CHECK(r.sim_valid && r.sim_up == 111 && r.sim_down == 222, "the SIM's own counters are not reported alongside");

    /* A configured alert threshold moves the alert (signed: fewer warnings is a loosening). */
    cairn_usage_config_t c = g_u.cfg;
    c.alert_pct = 50;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_NONE, &g_now) == CAIRN_CFG_OK, "lowering the alert threshold");
    return true;
}

static bool row_billing_period_calendar(void)
{
    /* billing day 15: the period that contains the date starts on the 15th. */
    CHECK(cairn_usage_period_id(ymd(2026, 3, 14, 5), 15) == 2026 * 12 + 1, "Mar 14 belongs to the period from Feb 15");
    CHECK(cairn_usage_period_id(ymd(2026, 3, 15, 0), 15) == 2026 * 12 + 2, "Mar 15 starts a period");
    CHECK(cairn_usage_period_id(ymd(2026, 1, 10, 0), 15) == 2025 * 12 + 11, "Jan 10 belongs to the period from Dec 15");
    CHECK(cairn_usage_period_id(ymd(2026, 12, 31, 23), 1) == 2026 * 12 + 11, "Dec 31, day 1");
    CHECK(cairn_usage_period_id(ymd(2028, 2, 29, 0), 28) == 2028 * 12 + 1, "leap day");
    CHECK(cairn_usage_period_id(ymd(2028, 3, 1, 0), 28) == 2028 * 12 + 1, "Mar 1, day 28");

    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);
    g_now.utc_s = ymd(2026, 3, 14, 12);
    cairn_usage_init(&g_u, &MEM, &t, &g_now);
    cairn_usage_config_t c;
    cairn_usage_config_defaults(&c);
    c.lte_enabled = true;
    c.billing_day = 15;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_OK, "config");

    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 1000, 0, &g_now);
    g_now.utc_s = ymd(2026, 3, 14, 23);
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 500, 0, &g_now);
    CHECK(g_u.period_bytes[CAIRN_PATH_LTE][0] == 1500, "the period reset early: %llu", (unsigned long long)g_u.period_bytes[CAIRN_PATH_LTE][0]);
    CHECK(g_u.day_bytes[CAIRN_PATH_LTE][0] == 1500, "same UTC day");

    g_now.utc_s = ymd(2026, 3, 15, 0);
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 7, 0, &g_now);
    CHECK(g_u.period_bytes[CAIRN_PATH_LTE][0] == 7, "the period did not roll on the billing day: %llu",
          (unsigned long long)g_u.period_bytes[CAIRN_PATH_LTE][0]);

    /* A clock that jumps backwards refills nothing. */
    g_now.utc_s = ymd(2026, 2, 1, 0);
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 3, 0, &g_now);
    CHECK(g_u.period_bytes[CAIRN_PATH_LTE][0] == 10, "a backwards clock reset the counters");

    /* No time known: keep counting into the current buckets. */
    cairn_usage_clock_t none = { 0, 5 };
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 5, 0, &none);
    CHECK(g_u.period_bytes[CAIRN_PATH_LTE][0] == 15, "no clock lost bytes");

    /* Moving the billing day could reset a period early, so it is a loosening. */
    c.billing_day = 20;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_NONE, &g_now) == CAIRN_CFG_UNSIGNED_LOOSEN, "an unsigned billing-day move");
    c.billing_day = 0;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_INVALID, "billing day 0");
    c.billing_day = 29;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_INVALID, "billing day 29");
    return true;
}

/* ── rows: the count survives a power cut ─────────────────────────────────── */

/* The simplest case first: through the real key-value store, across a clean power cycle. */
static bool row_survives_clean_restart_via_kv(void)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/cairn-usage-kv-%d", (int)getpid());
    if (system("true") != 0) { /* keep -Wunused-result quiet */ }
    char rmcmd[100];
    snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", dir);
    if (system(rmcmd) != 0) { /* nothing to remove */ }
    CHECK(mkdir(dir, 0775) == 0, "mkdir %s", dir);
    char path[300];
    snprintf(path, sizeof(path), "%s/kv.bin", dir);
    cairn_kv_host_set_path(path);
    CHECK(cairn_kv_begin(), "kv");

    cairn_usage_tuning_t t = tuning();
    g_now.utc_s = ymd(2026, 10, 5, 12);
    g_now.mono_s = 0;
    CHECK(cairn_usage_init(&g_u, NULL, &t, &g_now) == CAIRN_LOAD_FRESH, "fresh");
    CHECK(enable(&g_u, 1000000, 500000, 200000), "enable");
    cairn_usage_record(&g_u, CAIRN_PATH_WIFI, 12345, 678, &g_now);
    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why;
    CHECK(upload(&g_u, &m, 0x77, CAIRN_TRAFFIC_DIGEST, 3000, 1000, true, &why) == UP_OK, "upload");
    CHECK(cairn_usage_flush(&g_u), "flush");

    cairn_kv_end();
    CHECK(cairn_kv_begin(), "kv reopen");   /* the file is what survives */
    cairn_usage_t again;
    CHECK(cairn_usage_init(&again, NULL, &t, &g_now) == CAIRN_LOAD_OK, "reload");
    CHECK(lte_period_total(&again) == air_total(&m), "LTE count %llu, carrier %llu", (unsigned long long)lte_period_total(&again),
          (unsigned long long)air_total(&m));
    CHECK(again.period_bytes[CAIRN_PATH_WIFI][0] == 12345, "wifi count lost");
    CHECK(again.cfg.lte_enabled && again.cfg.monthly_cap_bytes == 1000000, "config lost");

    cairn_kv_end();
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* nothing to clean */ }
    return true;
}

/*
 * Power dies at a random moment: between a chunk leaving the radio and being
 * recorded, or mid-write (a torn image). After every cut the reloaded count must
 * be at least what the carrier saw, and the cap must hold over the whole run.
 */
static bool row_power_cut_never_undercounts(void)
{
    const uint32_t cap = 300000;
    int cuts_total = 0, torn = 0, recovered = 0;

    for (uint64_t seed = 1; seed <= 400; seed++) {
        g_seed = seed;
        g_rng = seed * 0x9E3779B97F4A7C15ULL + 1;

        cairn_usage_tuning_t t = tuning();
        fresh(&g_u, &t);
        if (!enable(&g_u, cap, cap, cap)) { fail("enable"); return false; }

        sim_t m;
        memset(&m, 0, sizeof(m));
        int cuts = 0;
        cairn_usage_reason_t why = CAIRN_STOP_NONE;

        for (int step = 0; step < 400; step++) {
            /* Arm a fault for this upload, about one in three times. */
            uint32_t pick = rnd() % 6;
            if (pick == 0) g_cut_chunk = g_chunk_no + (int)(rnd() % 12);
            if (pick == 1) g_mem.tear_after = (int)(rnd() % 8);

            g_now.mono_s += 100000;   /* backoff and breaker are not what this row tests */
            up_t r = upload(&g_u, &m, 0x5000 + (uint64_t)step, CAIRN_TRAFFIC_DIGEST, 1000 + rnd() % 9000, 1000, rnd() % 2, &why);
            if (r == UP_REFUSED && (why == CAIRN_STOP_CAP_MONTHLY || why == CAIRN_STOP_CAP_DAILY)) {
                g_cut_chunk = -1;
                g_mem.tear_after = -1;
                break;
            }
            g_cut_chunk = -1;

            if (r == UP_CUT || g_mem.dead) {
                cuts++;
                if (g_mem.tear_after < 0 && g_mem.dead) {}
                cairn_usage_load_t l = power_cycle(&g_u, &t);
                if (l == CAIRN_LOAD_RECOVERED) recovered++;
                if (l == CAIRN_LOAD_LOST) { fail("both slots lost with a single torn write"); return false; }
                if (lte_period_total(&g_u) < air_total(&m)) {
                    fail("after cut %d the count is %llu but the carrier saw %llu", cuts, (unsigned long long)lte_period_total(&g_u),
                         (unsigned long long)air_total(&m));
                    return false;
                }
                if (lte_period_total(&g_u) > air_total(&m) + (uint64_t)cuts * 4 * t.batch_bytes) {
                    fail("after cut %d the margin is too large: count %llu, carrier %llu", cuts, (unsigned long long)lte_period_total(&g_u),
                         (unsigned long long)air_total(&m));
                    return false;
                }
            }
            if (air_total(&m) > cap) {
                fail("the carrier saw %llu bytes, over the %u cap, after %d cuts", (unsigned long long)air_total(&m), cap, cuts);
                return false;
            }
        }
        if (air_total(&m) > cap) { fail("over the cap at the end: %llu", (unsigned long long)air_total(&m)); return false; }
        cuts_total += cuts;
        torn += g_mem.writes > 0;
    }
    CHECK(cuts_total > 200, "only %d power cuts exercised", cuts_total);
    CHECK(recovered > 10, "the torn-write path ran only %d times", recovered);
    (void)torn;
    return true;
}

/* A torn newest slot falls back to the older one, with extra margin; both torn fails closed. */
static bool row_torn_slots(void)
{
    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 1000000, 1000000, 1000000), "enable");
    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why;
    for (int i = 0; i < 5; i++) CHECK(upload(&g_u, &m, 0x600 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 5000, 1000, true, &why) == UP_OK, "upload");
    uint64_t air = air_total(&m);

    /* Flip a byte in the newest slot: its CRC must reject it. */
    int newest = (g_u.next_slot ^ 1);
    g_mem.data[newest][40] ^= 0xFF;
    CHECK(power_cycle(&g_u, &t) == CAIRN_LOAD_RECOVERED, "a corrupt newest slot should be RECOVERED");
    CHECK(lte_period_total(&g_u) >= air, "recovered count %llu below the carrier's %llu", (unsigned long long)lte_period_total(&g_u),
          (unsigned long long)air);
    CHECK(g_u.cfg.lte_enabled, "config lost on recovery");

    /* The margin was persisted: a second reboot does not add it again. */
    uint64_t once = lte_period_total(&g_u);
    CHECK(power_cycle(&g_u, &t) == CAIRN_LOAD_OK, "second reboot");
    CHECK(lte_period_total(&g_u) == once, "a reboot loop kept inflating the count: %llu -> %llu", (unsigned long long)once,
          (unsigned long long)lte_period_total(&g_u));

    /* Both slots unreadable: unknown, so closed. Not zero. */
    g_mem.data[0][40] ^= 0xFF;
    g_mem.data[1][40] ^= 0xFF;
    CHECK(power_cycle(&g_u, &t) == CAIRN_LOAD_LOST, "both corrupt should be LOST");
    cairn_usage_report_t r;
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.reason == CAIRN_STOP_STATE_LOST && r.state_lost, "reason %s", cairn_usage_reason_name(r.reason));
    sim_t m2;
    memset(&m2, 0, sizeof(m2));
    CHECK(upload(&g_u, &m2, 0x700, CAIRN_TRAFFIC_DIGEST, 1000, 500, true, &why) == UP_REFUSED && why == CAIRN_STOP_STATE_LOST,
          "traffic flowed with unknown counters");
    CHECK(air_total(&m2) == 0, "bytes moved");

    /* It stays closed across reboots, and only a signed clear (or a new period) reopens it. */
    CHECK(power_cycle(&g_u, &t) == CAIRN_LOAD_OK && g_u.state_lost, "STATE_LOST did not persist");
    CHECK(!cairn_usage_clear_state_lost(&g_u, CAIRN_AUTH_NONE), "an unsigned message cleared STATE_LOST");
    CHECK(g_u.state_lost, "still lost");
    CHECK(cairn_usage_clear_state_lost(&g_u, CAIRN_AUTH_SIGNED), "signed clear");
    cairn_usage_report(&g_u, &g_now, false, &r);
    /* The config was lost with the counters: defaults, so LTE is off until it is configured again. */
    CHECK(r.reason == CAIRN_STOP_DISABLED, "reason %s after clear", cairn_usage_reason_name(r.reason));
    CHECK(enable(&g_u, 1000000, 1000000, 1000000), "re-enable");
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.reason == CAIRN_STOP_NONE, "reason %s after re-enable", cairn_usage_reason_name(r.reason));

    /* A new billing period also ends it (the lost counts belonged to the old one). */
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 1000000, 1000000, 1000000), "enable");
    cairn_usage_record(&g_u, CAIRN_PATH_LTE, 100, 0, &g_now);
    cairn_usage_flush(&g_u);
    g_mem.data[0][40] ^= 0xFF;
    g_mem.data[1][40] ^= 0xFF;
    CHECK(power_cycle(&g_u, &t) == CAIRN_LOAD_LOST, "lost");
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.reason == CAIRN_STOP_STATE_LOST, "lost state lifted within the same period");
    g_now.utc_s = ymd(2026, 11, 2, 0);
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.reason == CAIRN_STOP_DISABLED && !r.state_lost, "a new period did not lift the stop: %s", cairn_usage_reason_name(r.reason));
    return true;
}

/* Flash wear: writes are batched, and the batching is real. */
static bool row_flash_writes_batched(void)
{
    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 50u * 1024 * 1024, 10u * 1024 * 1024, 5u * 1024 * 1024), "enable");
    uint32_t base = g_u.flash_writes;

    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why;
    CHECK(upload(&g_u, &m, 0x900, CAIRN_TRAFFIC_DIGEST, 1024 * 1024, 1000, false, &why) == UP_OK, "1 MiB upload: %s", cairn_usage_reason_name(why));
    uint32_t writes = g_u.flash_writes - base;
    uint64_t chunks = (1024 * 1024) / 1000 + 1;
    uint64_t bound = air_total(&m) / t.batch_bytes + 4;
    CHECK(writes <= bound, "%u flash writes for %llu bytes (bound %llu): not batched", writes, (unsigned long long)air_total(&m),
          (unsigned long long)bound);
    CHECK(writes > 100, "only %u writes: the bytes are not being persisted", writes);
    CHECK(writes * 3 < chunks, "%u writes for %llu chunks", writes, (unsigned long long)chunks);

    /* Wi-Fi bulk transfers are uncapped and best-effort, so they flush rarely. */
    base = g_u.flash_writes;
    for (int i = 0; i < 20000; i++) cairn_usage_record(&g_u, CAIRN_PATH_WIFI, 1460, 100, &g_now);   /* 31 MB */
    uint32_t wifi_writes = g_u.flash_writes - base;
    CHECK(wifi_writes <= 31000000u / t.batch_other_bytes + 2, "%u writes for 31 MB of Wi-Fi", wifi_writes);

    /* If writes fail, the margin no longer bounds the error, so the gate closes. */
    mem_reset();
    g_mem.dead = true;
    fresh(&g_u, &t);
    g_mem.dead = false;
    CHECK(enable(&g_u, 1000000, 1000000, 1000000), "enable");
    g_mem.dead = true;                     /* the store stops accepting writes, power stays on */
    memset(&m, 0, sizeof(m));
    CHECK(cairn_usage_attempt_begin(&g_u, &(cairn_usage_req_t){ CAIRN_TRAFFIC_DIGEST, 0x9, 100, false, false }, &g_now) ==
              CAIRN_STOP_STORE_FAILED, "an attempt began when it could not be written ahead");
    for (int i = 0; i < 20; i++) cairn_usage_record(&g_u, CAIRN_PATH_LTE, 1000, 0, &g_now);
    cairn_usage_req_t q = { CAIRN_TRAFFIC_DIGEST, 0, 0, false, true };
    CHECK(cairn_usage_gate(&g_u, &q, &g_now) == CAIRN_STOP_STORE_FAILED, "unpersistable counts did not stop traffic");
    return true;
}

/* ── rows: nothing unsigned can raise a limit ─────────────────────────────── */

static bool row_unsigned_cannot_raise(void)
{
    cairn_usage_tuning_t t = tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 1000000, 500000, 200000), "enable");
    cairn_usage_config_t before = g_u.cfg;
    cairn_usage_ceilings_t cbefore = g_u.ceil;

    /* Each loosening, unsigned, on its own. */
    cairn_usage_config_t c;
    for (int k = 0; k < 8; k++) {
        c = g_u.cfg;
        switch (k) {
        case 0: c.monthly_cap_bytes += 1; break;
        case 1: c.daily_cap_bytes += 1; break;
        case 2: c.trip_cap_bytes += 1; break;
        case 3: c.roaming_allowed = true; break;
        case 4: c.mode = CAIRN_USAGE_MODE_FULL_UP_TO_X; break;
        case 5: c.full_bundle_max_bytes += 1; break;
        case 6: c.alert_pct = 95; break;
        case 7: c.lte_enabled = false; c.paused = true; c.monthly_cap_bytes += 1; break;   /* a tightening smuggling a loosening */
        }
        cairn_usage_cfg_result_t r = cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_NONE, &g_now);
        CHECK(r == CAIRN_CFG_UNSIGNED_LOOSEN, "loosening %d unsigned: %s", k, cairn_usage_cfg_result_name(r));
        CHECK(memcmp(&g_u.cfg, &before, sizeof(before)) == 0, "loosening %d changed the config despite the refusal", k);
    }

    /* The ceiling: an unsigned message cannot raise it, by any route. */
    cairn_usage_ceilings_t hi = g_u.ceil;
    hi.monthly *= 4;
    CHECK(cairn_usage_set_ceilings(&g_u, &hi, CAIRN_AUTH_NONE) == CAIRN_CFG_UNSIGNED_LOOSEN, "unsigned ceiling raise accepted");
    CHECK(memcmp(&g_u.ceil, &cbefore, sizeof(cbefore)) == 0, "ceiling moved");

    c = g_u.cfg;
    c.monthly_cap_bytes = CAIRN_USAGE_CEIL_MONTHLY_BYTES + 1;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_NONE, &g_now) == CAIRN_CFG_ABOVE_CEILING, "above the ceiling, unsigned");
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_ABOVE_CEILING,
          "a signed config above the ceiling must be rejected, not clamped: raise the ceiling first");

    /* Unsigned tightening is fine. */
    c = g_u.cfg;
    c.monthly_cap_bytes = 400000;
    c.roaming_allowed = false;
    c.paused = true;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_NONE, &g_now) == CAIRN_CFG_OK, "unsigned tightening refused");

    /* Signed: the ceiling moves, but never past the compiled absolute maximum. */
    hi = g_u.ceil;
    hi.monthly = CAIRN_USAGE_ABS_MONTHLY_BYTES + 1;
    CHECK(cairn_usage_set_ceilings(&g_u, &hi, CAIRN_AUTH_SIGNED) == CAIRN_CFG_ABOVE_CEILING, "past the absolute maximum");
    hi.monthly = 200u * 1024 * 1024;
    CHECK(cairn_usage_set_ceilings(&g_u, &hi, CAIRN_AUTH_SIGNED) == CAIRN_CFG_OK, "signed ceiling raise");
    c = g_u.cfg;
    c.monthly_cap_bytes = 150u * 1024 * 1024;
    c.paused = false;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_OK, "signed cap above the old ceiling");

    /* The raise is durable and so is its absence. */
    CHECK(power_cycle(&g_u, &t) == CAIRN_LOAD_OK, "reload");
    CHECK(g_u.ceil.monthly == 200u * 1024 * 1024 && g_u.cfg.monthly_cap_bytes == 150u * 1024 * 1024, "raise not persisted");

    /* Lowering the ceiling clamps a cap that is already above it, at use. */
    cairn_usage_ceilings_t lo = g_u.ceil;
    lo.monthly = 50000;
    CHECK(cairn_usage_set_ceilings(&g_u, &lo, CAIRN_AUTH_NONE) == CAIRN_CFG_OK, "an unsigned LOWERING is a tightening");
    cairn_usage_report_t r;
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.monthly_cap == 50000, "the enforced cap is %u, not min(config, ceiling)", r.monthly_cap);

    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why;
    for (int i = 0; i < 100; i++) {
        up_t u = upload(&g_u, &m, 0x4000 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 3000, 1000, true, &why);
        if (u != UP_OK) break;
    }
    CHECK(air_total(&m) <= 50000 && why == CAIRN_STOP_CAP_MONTHLY, "traffic passed the clamped cap: %llu", (unsigned long long)air_total(&m));
    return true;
}

/* ── rows: a stuck retry loop is impossible ───────────────────────────────── */

static cairn_usage_tuning_t retry_tuning(void)
{
    cairn_usage_tuning_t t = tuning();
    t.max_attempts_per_trip = 3;
    t.breaker_threshold = 4;
    t.breaker_cooldown_s = 100;
    t.breaker_cooldown_max_s = 800;
    t.backoff_base_s = 10;
    t.backoff_max_s = 60;
    return t;
}

static bool row_retry_max_attempts_and_backoff(void)
{
    cairn_usage_tuning_t t = retry_tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 5000000, 5000000, 5000000), "enable");

    sim_t m;
    memset(&m, 0, sizeof(m));
    m.fail_transfer = true;
    cairn_usage_reason_t why = CAIRN_STOP_NONE;
    const uint64_t T = 0xBEEF;

    CHECK(upload(&g_u, &m, T, CAIRN_TRAFFIC_DIGEST, 1000, 1000, true, &why) == UP_FAILED, "attempt 1");
    CHECK(upload(&g_u, &m, T, CAIRN_TRAFFIC_DIGEST, 1000, 1000, true, &why) == UP_REFUSED && why == CAIRN_STOP_BACKOFF,
          "an immediate retry: %s", cairn_usage_reason_name(why));
    g_now.mono_s += 10;
    CHECK(upload(&g_u, &m, T, CAIRN_TRAFFIC_DIGEST, 1000, 1000, true, &why) == UP_FAILED, "attempt 2 after the backoff");
    g_now.mono_s += 19;   /* the second backoff is 20 s */
    CHECK(upload(&g_u, &m, T, CAIRN_TRAFFIC_DIGEST, 1000, 1000, true, &why) == UP_REFUSED && why == CAIRN_STOP_BACKOFF,
          "the backoff did not double: %s", cairn_usage_reason_name(why));
    g_now.mono_s += 1;
    CHECK(upload(&g_u, &m, T, CAIRN_TRAFFIC_DIGEST, 1000, 1000, true, &why) == UP_FAILED, "attempt 3");
    CHECK(m.attempts_started == 3, "attempts_started %u", m.attempts_started);

    /* Out of attempts: waiting does not help, and neither does rebooting. */
    uint64_t air = air_total(&m);
    for (int i = 0; i < 20000; i++) {
        if (i % 500 == 0) power_cycle(&g_u, &t);
        g_now.mono_s += 7919;
        up_t r = upload(&g_u, &m, T, CAIRN_TRAFFIC_DIGEST, 1000, 1000, true, &why);
        CHECK(r == UP_REFUSED && why == CAIRN_STOP_TRIP_ATTEMPTS, "retry %d: %d %s", i, r, cairn_usage_reason_name(why));
    }
    CHECK(m.attempts_started == 3 && air_total(&m) == air, "a stuck loop got %u attempts through", m.attempts_started);

    /* The budget is per kind: the same trip's full bundle has its own. */
    cairn_usage_config_t c = g_u.cfg;
    c.mode = CAIRN_USAGE_MODE_FULL_UP_TO_X;
    c.full_bundle_max_bytes = 100000;
    CHECK(cairn_usage_apply_config(&g_u, &c, CAIRN_AUTH_SIGNED, &g_now) == CAIRN_CFG_OK, "mode");
    m.fail_transfer = false;
    CHECK(upload(&g_u, &m, T, CAIRN_TRAFFIC_FULL_BUNDLE, 1000, 1000, true, &why) == UP_OK, "another kind on the same trip");
    return true;
}

static bool row_retry_circuit_breaker(void)
{
    cairn_usage_tuning_t t = retry_tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 5000000, 5000000, 5000000), "enable");

    sim_t m;
    memset(&m, 0, sizeof(m));
    m.fail_transfer = true;
    cairn_usage_reason_t why = CAIRN_STOP_NONE;

    /* Four different trips fail in a row (a dead modem, a broken server): the breaker opens. */
    for (int i = 0; i < 4; i++) {
        CHECK(upload(&g_u, &m, 0x100 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why) == UP_FAILED, "failure %d", i);
    }
    cairn_usage_report_t r;
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.breaker == CAIRN_BREAKER_OPEN && r.reason == CAIRN_STOP_BREAKER_OPEN, "breaker %d reason %s", r.breaker,
          cairn_usage_reason_name(r.reason));
    CHECK(m.attempts_started == 4, "attempts %u", m.attempts_started);

    /* A tight loop of fresh trips gets nothing through: the breaker is not per trip. */
    uint64_t air = air_total(&m);
    for (int i = 0; i < 50000; i++) {
        up_t u = upload(&g_u, &m, 0x10000 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why);
        CHECK(u == UP_REFUSED && why == CAIRN_STOP_BREAKER_OPEN, "tight loop %d: %d %s", i, u, cairn_usage_reason_name(why));
    }
    CHECK(m.attempts_started == 4 && air_total(&m) == air, "the breaker let %u attempts through", m.attempts_started);

    /* It is open through a reboot loop too (the cooldown restarts: fail closed). */
    for (int i = 0; i < 10; i++) {
        power_cycle(&g_u, &t);
        CHECK(upload(&g_u, &m, 0x20000 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why) == UP_REFUSED &&
                  why == CAIRN_STOP_BREAKER_OPEN, "reboot %d: %s", i, cairn_usage_reason_name(why));
    }

    /* After the cooldown it lets ONE probe through; a failed probe re-opens it for longer. */
    g_now.mono_s = 99;
    CHECK(upload(&g_u, &m, 0x30000, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why) == UP_REFUSED, "open at 99 s");
    g_now.mono_s = 100;
    CHECK(upload(&g_u, &m, 0x30001, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why) == UP_FAILED, "the probe at 100 s");
    CHECK(m.attempts_started == 5, "exactly one probe, got %u attempts", m.attempts_started);
    g_now.mono_s = 100 + 199;
    CHECK(upload(&g_u, &m, 0x30002, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why) == UP_REFUSED && why == CAIRN_STOP_BREAKER_OPEN,
          "the second cooldown (200 s) did not apply: %s", cairn_usage_reason_name(why));
    g_now.mono_s = 100 + 200;
    m.fail_transfer = false;
    CHECK(upload(&g_u, &m, 0x30003, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why) == UP_OK, "the second probe");

    /* A successful probe closes it, and traffic flows again. */
    cairn_usage_report(&g_u, &g_now, false, &r);
    CHECK(r.breaker == CAIRN_BREAKER_CLOSED && r.consecutive_failures == 0, "breaker %d, failures %u", r.breaker, r.consecutive_failures);
    CHECK(upload(&g_u, &m, 0x30004, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why) == UP_OK, "traffic after recovery");

    /* Cooldown growth is capped. */
    m.fail_transfer = true;
    for (int round = 0; round < 12; round++) {
        g_now.mono_s += 100000;
        for (int i = 0; i < 4 && cairn_usage_breaker_state(&g_u, &g_now) != CAIRN_BREAKER_OPEN; i++) {
            upload(&g_u, &m, 0x40000 + (uint64_t)round * 10 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 500, 500, true, &why);
        }
    }
    uint32_t open_until = g_u.breaker_open_until;
    CHECK(open_until - g_now.mono_s <= 800, "cooldown %u s exceeds the cap", open_until - g_now.mono_s);
    return true;
}

/* A crash inside an upload, over and over, is a failure each time and cannot loop forever. */
static bool row_retry_crash_loop(void)
{
    cairn_usage_tuning_t t = retry_tuning();
    fresh(&g_u, &t);
    CHECK(enable(&g_u, 5000000, 5000000, 5000000), "enable");

    sim_t m;
    memset(&m, 0, sizeof(m));
    cairn_usage_reason_t why = CAIRN_STOP_NONE;
    int boots = 0;
    for (int i = 0; i < 1000; i++) {
        g_cut_chunk = g_chunk_no;       /* die in the first chunk, every time */
        up_t r = upload(&g_u, &m, 0x77, CAIRN_TRAFFIC_DIGEST, 4000, 1000, false, &why);
        if (r != UP_CUT) {
            CHECK(r == UP_REFUSED, "unexpected %d", r);
            break;
        }
        power_cycle(&g_u, &t);
        boots++;
    }
    CHECK(boots <= 3, "a crash loop rebooted the device %d times before being stopped", boots);
    CHECK(m.attempts_started <= 3, "attempts_started %u", m.attempts_started);
    CHECK(why == CAIRN_STOP_TRIP_ATTEMPTS || why == CAIRN_STOP_BREAKER_OPEN || why == CAIRN_STOP_BACKOFF, "stopped for %s",
          cairn_usage_reason_name(why));

    /* And the crashes cost the breaker, across trips, not just the trip. */
    int more = 0;
    for (int i = 0; i < 20; i++) {
        g_cut_chunk = g_chunk_no;
        up_t r = upload(&g_u, &m, 0x1000 + (uint64_t)i, CAIRN_TRAFFIC_DIGEST, 4000, 1000, false, &why);
        if (r != UP_CUT) break;
        power_cycle(&g_u, &t);
        more++;
    }
    CHECK(more <= 4, "crash loops across new trips ran %d more times", more);
    return true;
}

static bool row_image_is_what_the_header_says(void)
{
    CHECK(cairn_usage_image_len() <= 400, "image %zu bytes", cairn_usage_image_len());
    printf("        (persisted image: %zu bytes, two alternating slots)\n", cairn_usage_image_len());
    return true;
}

/* ── runner ───────────────────────────────────────────────────────────────── */

typedef struct {
    const char *family;
    const char *name;
    bool (*fn)(void);
} row_t;

static const row_t ROWS[] = {
    { "policy",  "nothing leaves until LTE is configured on",              row_defaults_are_closed },
    { "policy",  "each refusal has its own reason code",                   row_policy_reasons },
    { "caps",    "a monthly cap stops traffic (carrier meter stays under)", row_monthly_cap_stops_traffic },
    { "caps",    "daily and per-trip caps stop traffic, across reboot",     row_daily_and_trip_caps },
    { "caps",    "alert thresholds, per-path counts, SIM counters",         row_alert_thresholds_and_paths },
    { "period",  "billing day, midnight, backwards clock, no clock",        row_billing_period_calendar },
    { "persist", "counts and config survive a clean restart via kv",        row_survives_clean_restart_via_kv },
    { "persist", "a power cut or torn write never under-counts",            row_power_cut_never_undercounts },
    { "persist", "torn slots: recover with margin, or fail closed",         row_torn_slots },
    { "persist", "flash writes are batched, and fail closed if they fail",  row_flash_writes_batched },
    { "auth",    "an unsigned message cannot raise a limit or the ceiling", row_unsigned_cannot_raise },
    { "retry",   "max attempts per trip and backoff, even across reboots",  row_retry_max_attempts_and_backoff },
    { "retry",   "the circuit breaker opens, probes once, and closes",      row_retry_circuit_breaker },
    { "retry",   "a crash loop inside an upload is stopped",                row_retry_crash_loop },
    { "persist", "the persisted image is small",                            row_image_is_what_the_header_says },
};

int main(void)
{
    cairn_log_init(0);

    size_t n = sizeof(ROWS) / sizeof(ROWS[0]);
    for (size_t i = 0; i < n; i++) {
        g_row = ROWS[i].name;
        g_seed = 0;
        bool ok = ROWS[i].fn();
        if (ok) {
            g_pass++;
            printf("  pass  [%-7s] %s\n", ROWS[i].family, ROWS[i].name);
        } else {
            g_fail++;
            printf("  FAIL  [%-7s] %s\n", ROWS[i].family, ROWS[i].name);
        }
    }
    printf("\nusage matrix (PROVISIONAL: contracts/config/v1 unreleased, no vectors): %d/%zu passed\n", g_pass, n);

    if (g_failure_count > 0) {
        printf("\n%d failure(s):\n", g_failure_count);
        for (int i = 0; i < g_failure_count; i++) printf("  %s\n", g_failures[i]);
        return 1;
    }
    return g_fail == 0 ? 0 : 1;
}
