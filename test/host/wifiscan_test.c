/*
 * Wi-Fi position scans (lib/cairn_wifiscan) and the WIFI_SCAN record (format v3
 * section 4.13): when the dongle listens for access points, which ones it keeps, and the
 * bytes it writes.
 *
 * The schedule rows are the situations the feature exists for -- a cold start, a garage
 * with no sky, a drive with a good fix teaching the server where access points are -- and
 * the one rule it must never break: no scan while the uplink owns the radio.
 */

#include <stdio.h>
#include <string.h>

#include "cairn_format.h"
#include "cairn_wifiscan.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [wifiscan] %s\n", name); }       \
        else      { g_fail++; printf("  FAIL  [wifiscan] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* ── a simulated lifecycle ────────────────────────────────────────────────── */

typedef struct {
    cairn_wifiscan_t          s;
    cairn_wifiscan_evidence_t ev;
    uint32_t                  now;
    uint32_t                  scan_ms;   /* how long each simulated scan takes */
    int                       started;
    int                       learning;
    cairn_wifiscan_reason_t   last;
    uint32_t                  last_at;
    bool                      scanning;
    uint32_t                  scan_done_at;
} sim_t;

static void sim_init(sim_t *m)
{
    memset(m, 0, sizeof(*m));
    cairn_wifiscan_init(&m->s);
    m->ev.capture = CAIRN_WIFISCAN_IDLE;
    m->now        = 1000;
    m->scan_ms    = 2500;
}

/* One 20 ms lifecycle tick: finish a scan that is due, then ask the schedule. */
static void sim_tick(sim_t *m)
{
    if (m->scanning && (int32_t)(m->now - m->scan_done_at) >= 0) {
        bool learn = cairn_wifiscan_is_learning(&m->ev, m->now);
        cairn_wifiscan_finished(&m->s, learn);
        if (learn) m->learning++;
        m->scanning = false;
    }
    cairn_wifiscan_reason_t r = cairn_wifiscan_tick(&m->s, &m->ev, m->now);
    if (r != CAIRN_WIFISCAN_NONE) {
        cairn_wifiscan_started(&m->s, r, m->now);
        m->started++;
        m->last         = r;
        m->last_at      = m->now;
        m->scanning     = true;
        m->scan_done_at = m->now + m->scan_ms;
    }
    m->now += 20;
}

/* Run for `ms`, keeping a fix fresh if `fix_fresh`; returns the scans started. */
static int sim_run(sim_t *m, uint32_t ms, bool fix_fresh)
{
    int before = m->started;
    uint32_t until = m->now + ms;
    while ((int32_t)(m->now - until) < 0) {
        if (fix_fresh) {
            m->ev.have_fix = true;
            m->ev.fix_ms   = m->now - (m->now % 200);   /* 5 Hz fixes */
        }
        sim_tick(m);
    }
    return m->started - before;
}

/* ── schedule ─────────────────────────────────────────────────────────────── */

static void test_cold_start(void)
{
    sim_t m;
    sim_init(&m);

    int n = sim_run(&m, 100, false);
    CHECK("cold start: no fix since boot, parked, scans at once", n == 1 &&
          m.last == CAIRN_WIFISCAN_NO_FIX_PARKED);

    n = sim_run(&m, 290000, false);
    CHECK("parked with no fix: nothing more inside five minutes", n == 0);
    n = sim_run(&m, 20000, false);
    CHECK("and one at five minutes", n == 1 && m.last == CAIRN_WIFISCAN_NO_FIX_PARKED);

    /* Motion is suspected: the pre-roll is filling and the exit from wherever the car
     * is parked is the stretch with no position. */
    m.ev.capture = CAIRN_WIFISCAN_PRETRIP;
    n = sim_run(&m, 100, false);
    CHECK("a suspected trip with no fix scans at once (last scan > 10 s ago)", n == 1 &&
          m.last == CAIRN_WIFISCAN_NO_FIX_TRIP);
    CHECK("and none is learning", m.learning == 0);
}

static void test_garage(void)
{
    sim_t m;
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;

    int n = sim_run(&m, 60000, false);
    /* t = 0, 10, 20, 30, 40, 50 s */
    CHECK("garage: a trip with no fix ever scans every 10 s", n == 6);
    CHECK("every one a position scan", m.last == CAIRN_WIFISCAN_NO_FIX_TRIP && m.learning == 0);

    /* A scan that runs long is never overlapped by the next. */
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    m.scan_ms = 25000;
    n = sim_run(&m, 20000, false);
    CHECK("a scan still in progress holds the next one", n == 1);
    n = sim_run(&m, 6000, false);
    CHECK("which starts as soon as it completes", n == 1);
}

static void test_fix_age(void)
{
    sim_t m;
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    m.ev.have_fix = true;

    m.ev.fix_ms = m.now - 4980;
    cairn_wifiscan_reason_t r = cairn_wifiscan_tick(&m.s, &m.ev, m.now);
    CHECK("a fix 4.98 s old is neither lost nor fresh: no scan", r == CAIRN_WIFISCAN_NONE);

    m.ev.fix_ms = m.now - 5000;
    r = cairn_wifiscan_tick(&m.s, &m.ev, m.now);
    CHECK("at 5 s without an accepted fix the position is lost", r == CAIRN_WIFISCAN_NO_FIX_TRIP);

    cairn_wifiscan_evidence_t ev = m.ev;
    ev.fix_ms = m.now - 2000;
    CHECK("a fix exactly 2 s old makes a learning scan", cairn_wifiscan_is_learning(&ev, m.now));
    ev.fix_ms = m.now - 2001;
    CHECK("2.001 s does not", !cairn_wifiscan_is_learning(&ev, m.now));
    ev.have_fix = false;
    ev.fix_ms = m.now;
    CHECK("no fix since boot is never learning", !cairn_wifiscan_is_learning(&ev, m.now));
    ev.have_fix = true;
    ev.fix_ms = m.now + 15;
    CHECK("a fix stamped a tick ahead of the controller's clock is fresh, not ancient",
          cairn_wifiscan_is_learning(&ev, m.now));
}

static void test_learning(void)
{
    sim_t m;
    sim_init(&m);

    /* Parked with a good fix: nothing to learn that the server lacks. */
    int n = sim_run(&m, 600000, true);
    CHECK("parked with a fresh fix: no scans at all", n == 0);

    m.ev.capture = CAIRN_WIFISCAN_PRETRIP;
    n = sim_run(&m, 3000, true);
    CHECK("a suspected trip with a fix: none either", n == 0);

    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    n = sim_run(&m, 100, true);
    CHECK("trip confirmed with a fresh fix: the trip-start learning scan", n == 1 &&
          m.last == CAIRN_WIFISCAN_LEARN_TRIP_START);

    n = sim_run(&m, 59000, true);
    CHECK("then nothing for a minute", n == 0);
    n = sim_run(&m, 1100, true);
    CHECK("then the driving cadence", n == 1 && m.last == CAIRN_WIFISCAN_LEARN_CADENCE);
    n = sim_run(&m, 210000, true);
    CHECK("one a minute while driving (3 in the next 3.5 min)", n == 3);
    CHECK("every scan was learning", m.learning == m.started);

    m.ev.capture = CAIRN_WIFISCAN_TRAILING;
    n = sim_run(&m, 100, true);
    CHECK("the car stops: the trip-end learning scan", n == 1 &&
          m.last == CAIRN_WIFISCAN_LEARN_TRIP_END);
    n = sim_run(&m, 120000, true);
    CHECK("and no cadence while stopped", n == 0);

    m.ev.capture = CAIRN_WIFISCAN_IDLE;
    n = sim_run(&m, 1000, true);
    CHECK("sealing the trip asks for nothing", n == 0);
}

static void test_edges(void)
{
    sim_t m;
    sim_init(&m);

    /* A position scan just ran in pretrip (no fix); the fix arrives at confirmation. */
    m.ev.capture = CAIRN_WIFISCAN_PRETRIP;
    int n = sim_run(&m, 100, false);
    CHECK("pretrip with no fix: a position scan", n == 1);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    n = sim_run(&m, 3000, true);
    CHECK("confirmed 3 s later with a fix: the start scan waits out the 10 s gap", n == 0);
    n = sim_run(&m, 8000, true);
    CHECK("then runs", n == 1 && m.last == CAIRN_WIFISCAN_LEARN_TRIP_START);

    /* A start scan owed with no fresh fix expires rather than firing minutes later. */
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_PRETRIP;
    sim_run(&m, 100, true);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    m.ev.have_fix = true;
    m.ev.fix_ms = m.now - 3000;   /* grey zone: neither lost nor fresh */
    for (int i = 0; i < 1600; i++) { m.ev.fix_ms = m.now - 3000; sim_tick(&m); }
    CHECK("a start scan owed through 32 s of a 3 s-old fix never fires", m.started == 0);
    n = sim_run(&m, 100, true);
    CHECK("and is gone: a fresh fix then gets a cadence scan, not a start scan",
          n == 1 && m.last == CAIRN_WIFISCAN_LEARN_CADENCE);

    /* A long red light: trailing, then moving again before the end scan could run. */
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    sim_run(&m, 100, true);              /* start scan */
    sim_run(&m, 5000, true);
    m.ev.capture = CAIRN_WIFISCAN_TRAILING;
    n = sim_run(&m, 2000, true);
    CHECK("trailing within 10 s of the last scan: the end scan waits", n == 0);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    n = sim_run(&m, 20000, true);
    CHECK("moving again drops it; no start scan for a resumed trip either", n == 0);

    /* A rejected pretrip is not a trip end, and owes nothing. */
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_PRETRIP;
    sim_run(&m, 3000, true);
    m.ev.capture = CAIRN_WIFISCAN_IDLE;
    n = sim_run(&m, 30000, true);
    CHECK("a rejected pretrip scans nothing", n == 0);
}

static void test_uplink_owns_radio(void)
{
    sim_t m;
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    m.ev.radio_busy = true;

    int n = sim_run(&m, 60000, false);
    CHECK("never while the uplink owns the radio, however overdue", n == 0);
    CHECK("the held scan is counted", m.s.refused_busy > 0);

    m.ev.radio_busy = false;
    n = sim_run(&m, 100, false);
    CHECK("the first tick the radio is free, the due scan runs", n == 1);

    /* Busy mid-cadence: learning scans are held too. */
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_ACTIVE;
    sim_run(&m, 100, true);
    m.ev.radio_busy = true;
    n = sim_run(&m, 120000, true);
    CHECK("learning scans are held as well", n == 0);

    /* An uplink slot that cancels a scan does not cause an immediate retry after it. */
    sim_init(&m);
    m.ev.capture = CAIRN_WIFISCAN_IDLE;
    sim_run(&m, 100, false);
    CHECK("a parked scan started", m.started == 1 && m.s.in_progress);
    cairn_wifiscan_cancelled(&m.s);
    m.scanning = false;
    CHECK("cancelled: no longer in progress, and counted", !m.s.in_progress && m.s.cancelled == 1);
    n = sim_run(&m, 60000, false);
    CHECK("and the parked cadence still runs from the cancelled scan's start", n == 0);
}

/* ── selection ────────────────────────────────────────────────────────────── */

static cairn_wifiscan_raw_t ap(uint8_t o0, uint8_t o5, int8_t rssi, uint8_t ch, const char *ssid)
{
    cairn_wifiscan_raw_t a;
    memset(&a, 0, sizeof(a));
    a.bssid[0] = o0; a.bssid[1] = 0xdc; a.bssid[2] = 0x97;
    a.bssid[3] = 0x07; a.bssid[4] = 0x10; a.bssid[5] = o5;
    a.rssi_dbm = rssi;
    a.channel  = ch;
    a.ssid     = (const uint8_t *)ssid;
    a.ssid_len = ssid ? strlen(ssid) : 0;
    return a;
}

static void test_filters(void)
{
    cairn_wifiscan_raw_t a;

    a = ap(0x88, 1, -50, 6, "HomeNet");
    CHECK("an ordinary access point is kept", cairn_wifiscan_ap_allowed(&a));
    a = ap(0x8A, 1, -50, 6, "HomeNet");
    CHECK("a locally administered BSSID (0x02 in octet 0) is dropped", !cairn_wifiscan_ap_allowed(&a));
    a = ap(0xDA, 1, -40, 6, "Pixel_7");
    CHECK("a phone hotspot's randomised address is dropped", !cairn_wifiscan_ap_allowed(&a));
    a = ap(0x89, 1, -50, 6, "x");
    CHECK("bit 0 alone is not the locally administered bit", cairn_wifiscan_ap_allowed(&a));
    a = ap(0x88, 1, -50, 6, "Cafe_nomap");
    CHECK("an SSID ending _nomap is dropped", !cairn_wifiscan_ap_allowed(&a));
    a = ap(0x88, 1, -50, 6, "Cafe_optout");
    CHECK("an SSID ending _optout is dropped", !cairn_wifiscan_ap_allowed(&a));
    a = ap(0x88, 1, -50, 6, "Cafe_NoMap");
    CHECK("the suffix is matched case-insensitively", !cairn_wifiscan_ap_allowed(&a));
    a = ap(0x88, 1, -50, 6, "_nomap");
    CHECK("an SSID that is only the suffix is dropped", !cairn_wifiscan_ap_allowed(&a));
    a = ap(0x88, 1, -50, 6, "_nomap_home");
    CHECK("the marker must end the SSID, not merely appear in it", cairn_wifiscan_ap_allowed(&a));
    a = ap(0x88, 1, -50, 6, "nomap");
    CHECK("\"nomap\" without the underscore is not the marker", cairn_wifiscan_ap_allowed(&a));
    a = ap(0x88, 1, -50, 6, NULL);
    CHECK("a hidden network (no SSID) is kept on its BSSID", cairn_wifiscan_ap_allowed(&a));

    /* The radio's SSID buffer need not be NUL-terminated: the length decides. */
    uint8_t raw_ssid[8] = { 'A', '_', 'n', 'o', 'm', 'a', 'p', 'Z' };
    a = ap(0x88, 1, -50, 6, NULL);
    a.ssid = raw_ssid; a.ssid_len = 7;
    CHECK("the SSID length, not a terminator, bounds the match", !cairn_wifiscan_ap_allowed(&a));
    a.ssid_len = 8;
    CHECK("(and the eighth byte changes the verdict)", cairn_wifiscan_ap_allowed(&a));
}

static void test_selection(void)
{
    cairn_wifiscan_raw_t raw[24];
    size_t n = 0;

    /* 20 allowed access points with signals -90..-33, in no particular order. */
    static const int8_t rssi[20] = { -60, -33, -90, -71, -45, -52, -88, -67, -40, -75,
                                     -58, -81, -49, -63, -36, -84, -70, -55, -79, -42 };
    for (int i = 0; i < 20; i++) raw[n++] = ap(0x88, (uint8_t)i, rssi[i], (uint8_t)(1 + i % 11), "Street");
    raw[n++] = ap(0x8A, 50, -20, 1, "iPhone");        /* strongest of all, and a phone */
    raw[n++] = ap(0x88, 51, -21, 6, "Cafe_nomap");
    raw[n++] = ap(0x88, 3, -30, 6, "Street");         /* BSSID 3 again, stronger */
    raw[n++] = ap(0x88, 1, -80, 6, "Street");         /* BSSID 1 again, weaker */

    cairn_wifi_scan_t s;
    cairn_wifiscan_stats_t st;
    cairn_wifiscan_select(raw, n, CAIRN_WIFI_SCAN_LEARNING | 0x80, 2480, &s, &st);

    CHECK("sixteen kept from twenty-four heard", s.count == 16 && st.kept == 16 && st.heard == 24);
    CHECK("two dropped by the privacy filters", st.dropped_filter == 2);
    CHECK("two duplicate BSSIDs dropped", st.dropped_dup == 2);
    CHECK("four dropped as weaker than the sixteen", st.dropped_cap == 4);

    bool ordered = true;
    for (int i = 1; i < s.count; i++)
        if (s.ap[i].rssi_dbm > s.ap[i - 1].rssi_dbm) ordered = false;
    CHECK("strongest first", ordered);
    CHECK("the duplicate is kept at its stronger reading", s.ap[0].bssid[5] == 3 && s.ap[0].rssi_dbm == -30);
    CHECK("then -33", s.ap[1].rssi_dbm == -33);
    CHECK("the weakest kept is -79 (the four below it were dropped)", s.ap[15].rssi_dbm == -79);
    bool none_filtered = true;
    for (int i = 0; i < s.count; i++)
        if (s.ap[i].bssid[5] == 50 || s.ap[i].bssid[5] == 51) none_filtered = false;
    CHECK("neither filtered access point survives, though both were the strongest", none_filtered);
    CHECK("BSSID 1 is kept once, at -33 (its stronger reading)",
          s.ap[1].bssid[5] == 1);
    CHECK("reserved flag bits are cleared", s.flags == CAIRN_WIFI_SCAN_LEARNING);
    CHECK("scan_ms carried", s.scan_ms == 2480);

    /* Ties are ordered by BSSID, so the record is deterministic. */
    cairn_wifiscan_raw_t tie[3] = { ap(0x88, 9, -50, 1, "a"), ap(0x88, 2, -50, 1, "b"),
                                    ap(0x88, 5, -50, 1, "c") };
    cairn_wifiscan_select(tie, 3, 0, 1, &s, NULL);
    CHECK("equal signals are ordered by BSSID", s.count == 3 && s.ap[0].bssid[5] == 2 &&
          s.ap[1].bssid[5] == 5 && s.ap[2].bssid[5] == 9);

    cairn_wifiscan_select(NULL, 0, 0, 70000, &s, &st);
    CHECK("nothing heard: count 0, still a record", s.count == 0 && st.kept == 0);
    CHECK("scan_ms saturates rather than wrapping", s.scan_ms == 0xFFFF);

    /* The SSID never leaves the function: not in the struct, not in the bytes. */
    cairn_wifiscan_raw_t one = ap(0x88, 7, -44, 11, "SecretHomeNetwork");
    cairn_wifiscan_select(&one, 1, 0, 2000, &s, NULL);
    uint8_t buf[CAIRN_WIFI_SCAN_MAX_PAYLOAD];
    size_t len = 0;
    cairn_err_t e = cairn_encode_wifi_scan(&s, buf, sizeof(buf), &len);
    bool leaked = false;
    for (size_t i = 0; i + 6 <= len; i++)
        if (memcmp(buf + i, "Secret", 6) == 0) leaked = true;
    CHECK("one access point encodes to 12 bytes", e == CAIRN_OK && len == 12);
    CHECK("and its SSID appears nowhere in them", !leaked);
    CHECK("the selected record has no room for an SSID",
          sizeof(cairn_wifi_ap_t) == 8);
}

/* ── the record ───────────────────────────────────────────────────────────── */

static void fill(cairn_wifi_scan_t *s, uint8_t count, uint8_t flags, uint16_t ms)
{
    memset(s, 0, sizeof(*s));
    s->count = count; s->flags = flags; s->scan_ms = ms;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t b[6] = { 0x88, 0xdc, 0x97, 0x07, (uint8_t)(0x10 + i), 0xdb };
        memcpy(s->ap[i].bssid, b, 6);
        s->ap[i].rssi_dbm = (int8_t)(-40 - 3 * i);
        s->ap[i].channel  = (uint8_t)(1 + i % 13);
    }
}

static void test_record(void)
{
    uint8_t buf[CAIRN_WIFI_SCAN_MAX_PAYLOAD + 8];
    size_t len = 0;
    cairn_wifi_scan_t s, d;

    CHECK("WIFI_SCAN is record type 0x0C", CAIRN_REC_WIFI_SCAN == 0x0C);
    CHECK("and a known one", cairn_record_type_known(0x0C));
    CHECK("named WIFI_SCAN", strcmp(cairn_record_type_name(0x0C), "WIFI_SCAN") == 0);
    CHECK("0x0D is still unknown", !cairn_record_type_known(0x0D));

    fill(&s, 0, CAIRN_WIFI_SCAN_ACTIVE, 2140);
    CHECK("count 0 encodes to the 4-byte header", cairn_encode_wifi_scan(&s, buf, sizeof(buf), &len) == CAIRN_OK && len == 4);
    CHECK("count, flags, scan_ms little-endian", buf[0] == 0 && buf[1] == 0x02 &&
          buf[2] == (2140 & 0xFF) && buf[3] == (2140 >> 8));
    CHECK("and decodes back", cairn_decode_wifi_scan(buf, len, &d) == CAIRN_OK &&
          d.count == 0 && d.flags == 0x02 && d.scan_ms == 2140);

    fill(&s, 3, 0, 1870);
    CHECK("count 3 encodes to 28 bytes", cairn_encode_wifi_scan(&s, buf, sizeof(buf), &len) == CAIRN_OK && len == 28);
    CHECK("an entry is bssid in transmission order, then rssi as i8, then channel",
          memcmp(buf + 4, s.ap[0].bssid, 6) == 0 && (int8_t)buf[10] == -40 && buf[11] == 1 &&
          buf[4 + 8] == 0x88 && buf[4 + 8 + 4] == 0x11);
    CHECK("count 3 round-trips", cairn_decode_wifi_scan(buf, len, &d) == CAIRN_OK &&
          memcmp(&d, &s, sizeof(d)) == 0);

    fill(&s, 16, CAIRN_WIFI_SCAN_LEARNING | CAIRN_WIFI_SCAN_ACTIVE, 2310);
    CHECK("count 16 encodes to 132 bytes", cairn_encode_wifi_scan(&s, buf, sizeof(buf), &len) == CAIRN_OK &&
          len == CAIRN_WIFI_SCAN_MAX_PAYLOAD && len == 132);
    CHECK("count 16 round-trips", cairn_decode_wifi_scan(buf, len, &d) == CAIRN_OK &&
          memcmp(&d, &s, sizeof(d)) == 0);
    CHECK("the last entry's signal is -85 dBm", (int8_t)buf[4 + 15 * 8 + 6] == -85);

    CHECK("too small a buffer is refused", cairn_encode_wifi_scan(&s, buf, 131, &len) == CAIRN_ERR_BUFFER_TOO_SMALL);
    s.count = 17;
    CHECK("count 17 is refused by the encoder", cairn_encode_wifi_scan(&s, buf, sizeof(buf), &len) == CAIRN_ERR_MALFORMED);

    fill(&s, 3, 0, 1870);
    cairn_encode_wifi_scan(&s, buf, sizeof(buf), &len);
    CHECK("a payload shorter than its count says is malformed", cairn_decode_wifi_scan(buf, 27, &d) == CAIRN_ERR_MALFORMED);
    CHECK("so is one with trailing bytes", cairn_decode_wifi_scan(buf, 29, &d) == CAIRN_ERR_MALFORMED);
    CHECK("under the header is truncated", cairn_decode_wifi_scan(buf, 3, &d) == CAIRN_ERR_TRUNCATED);
    buf[0] = 17;
    CHECK("a count over 16 is malformed", cairn_decode_wifi_scan(buf, 4 + 17 * 8, &d) == CAIRN_ERR_MALFORMED);
    buf[0] = 3; buf[1] = 0xFD;
    CHECK("unknown flag bits are kept, not refused", cairn_decode_wifi_scan(buf, 28, &d) == CAIRN_OK &&
          d.flags == 0xFD);
}

int main(void)
{
    printf("wifiscan\n");
    test_cold_start();
    test_garage();
    test_fix_age();
    test_learning();
    test_edges();
    test_uplink_owns_radio();
    test_filters();
    test_selection();
    test_record();
    printf("wifiscan: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
