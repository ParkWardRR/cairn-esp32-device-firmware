/*
 * The Wi-Fi position for the receiver's hint (lib/cairn_wifiloc): the table the server
 * sends (format/v3 §6.4) and every way it can be refused, the lookup, the acceptance
 * rules (what is located and, more to the point, what is not), the altitude the hint
 * carries, and when to ask the server again.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_wifiloc.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [wifiloc] %s\n", name); }        \
        else      { g_fail++; printf("  FAIL  [wifiloc] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* ── building tables ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  bssid[6];
    int32_t  lat, lon;
    uint16_t acc;
} rec_t;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A table as the server encodes it; record_size may exceed 16 (zero padding). */
static size_t build(uint8_t *out, const rec_t *r, uint32_t n, uint16_t rs)
{
    memcpy(out, "CWA1", 4);
    put16(out + 4, 1);
    put16(out + 6, rs);
    put32(out + 8, n);
    put32(out + 12, 1792951200u);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = out + 16 + (size_t)i * rs;
        memset(p, 0, rs);
        memcpy(p, r[i].bssid, 6);
        put32(p + 6, (uint32_t)r[i].lat);
        put32(p + 10, (uint32_t)r[i].lon);
        put16(p + 14, r[i].acc);
    }
    return 16 + (size_t)n * rs;
}

static rec_t R(uint8_t last, int32_t lat, int32_t lon, uint16_t acc)
{
    rec_t r = { { 0x10, 0x20, 0x30, 0x40, 0x50, last }, lat, lon, acc };
    return r;
}

static cairn_wifiloc_obs_t O(uint8_t last, int rssi)
{
    cairn_wifiloc_obs_t o = { { 0x10, 0x20, 0x30, 0x40, 0x50, last }, (int8_t)rssi };
    return o;
}

/* Metres north/east of a point near the owner's area, as e7 offsets (equirectangular). */
#define HOME_LAT 377749000
#define HOME_LON (-1224194000)
static int32_t north(double m) { return HOME_LAT + (int32_t)lround(m / 111320.0 * 1e7); }
static int32_t east(double m)
{
    return HOME_LON + (int32_t)lround(m / (111320.0 * cos(37.7749 * 3.14159265358979323846 / 180.0)) * 1e7);
}

/* ── the table ───────────────────────────────────────────────────────────── */

static void test_parse(void)
{
    static uint8_t buf[CAIRN_WIFILOC_MAX_BYTES + 64];
    cairn_wifiloc_table_t t;

    /* Byte for byte the record the server's TestEncodeLayout pins
     * (internal/wifianchors): 37.7749295, -122.4194155, spread 12.2 m -> acc 38. */
    rec_t one = { { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 }, 377749295, -1224194155, 38 };
    size_t n = build(buf, &one, 1, 16);
    static const uint8_t want[16] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
                                      0x2F, 0xFF, 0x83, 0x16, 0x95, 0x47, 0x08, 0xB7, 0x26, 0x00 };
    CHECK("the record layout is the server's, byte for byte", n == 32 && !memcmp(buf + 16, want, 16));
    CHECK("a one-record table parses", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_OK && t.count == 1 &&
                                       t.generated_at == 1792951200u && t.record_size == 16);
    cairn_wifiloc_anchor_t a;
    CHECK("and its record reads back", cairn_wifiloc_lookup(&t, one.bssid, &a) && a.lat_e7 == 377749295 &&
                                       a.lon_e7 == -1224194155 && a.acc_m == 38);

    n = build(buf, NULL, 0, 16);
    CHECK("an empty table (the server's count 0) parses", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_OK && t.count == 0);
    CHECK("and finds nothing", !cairn_wifiloc_lookup(&t, one.bssid, &a));

    rec_t two[2] = { R(1, HOME_LAT, HOME_LON, 50), R(2, HOME_LAT, HOME_LON, 50) };
    n = build(buf, two, 2, 16);
    CHECK("short by a byte", cairn_wifiloc_parse(buf, n - 1, &t) == CAIRN_WIFILOC_ERR_SHORT);
    CHECK("long by a byte", cairn_wifiloc_parse(buf, n + 1, &t) == CAIRN_WIFILOC_ERR_SHORT);
    CHECK("under a header", cairn_wifiloc_parse(buf, 15, &t) == CAIRN_WIFILOC_ERR_SHORT);
    CHECK("a refused table leaves nothing to search", t.records == NULL && !cairn_wifiloc_lookup(&t, two[0].bssid, &a));
    buf[0] = 'X';
    CHECK("bad magic", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_MAGIC);
    n = build(buf, two, 2, 16);
    put16(buf + 4, 2);
    CHECK("version 2", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_VERSION);
    n = build(buf, two, 2, 16);
    put16(buf + 6, 15);
    CHECK("record size 15", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_RECORD);
    n = build(buf, two, 2, 16);
    put32(buf + 8, CAIRN_WIFILOC_MAX_RECORDS + 1);
    CHECK("over the cap", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_COUNT);

    rec_t dup[2] = { R(1, HOME_LAT, HOME_LON, 50), R(1, HOME_LAT, HOME_LON, 50) };
    n = build(buf, dup, 2, 16);
    CHECK("a repeated BSSID", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_ORDER);
    rec_t desc[2] = { R(2, HOME_LAT, HOME_LON, 50), R(1, HOME_LAT, HOME_LON, 50) };
    n = build(buf, desc, 2, 16);
    CHECK("descending BSSIDs", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_ORDER);

    rec_t bad[2] = { R(1, 900000001, HOME_LON, 50), R(2, HOME_LAT, HOME_LON, 50) };
    n = build(buf, bad, 2, 16);
    CHECK("latitude past the pole", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_RANGE);
    bad[0] = R(1, HOME_LAT, -1800000001, 50);
    n = build(buf, bad, 2, 16);
    CHECK("longitude past the antimeridian", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_RANGE);
    bad[0] = R(1, HOME_LAT, HOME_LON, 0);
    n = build(buf, bad, 2, 16);
    CHECK("an accuracy of zero", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_ERR_RANGE);

    /* A newer server may lengthen the record; the reader skips what it does not know. */
    n = build(buf, two, 2, 20);
    CHECK("a 20-byte record parses", cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_OK && t.record_size == 20);
    CHECK("and the second record is found past the padding",
          cairn_wifiloc_lookup(&t, two[1].bssid, &a) && a.acc_m == 50);
}

static void test_lookup(void)
{
    /* The server's cap, every BSSID even, so the odd ones fall between. */
    static rec_t r[CAIRN_WIFILOC_MAX_RECORDS];
    static uint8_t buf[CAIRN_WIFILOC_MAX_BYTES];
    for (uint32_t i = 0; i < CAIRN_WIFILOC_MAX_RECORDS; i++) {
        uint32_t v = 2 * i + 2;
        rec_t x = { { 0x00, 0x11, 0x22, (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v },
                    HOME_LAT + (int32_t)i, HOME_LON - (int32_t)i, (uint16_t)(25 + i % 400) };
        r[i] = x;
    }
    size_t n = build(buf, r, CAIRN_WIFILOC_MAX_RECORDS, 16);
    cairn_wifiloc_table_t t;
    CHECK("a full 4096-record table (64 KiB) parses",
          n == CAIRN_WIFILOC_MAX_BYTES && cairn_wifiloc_parse(buf, n, &t) == CAIRN_WIFILOC_OK);

    bool all = true, none = true;
    for (uint32_t i = 0; i < CAIRN_WIFILOC_MAX_RECORDS; i++) {
        cairn_wifiloc_anchor_t a;
        if (!cairn_wifiloc_lookup(&t, r[i].bssid, &a) || a.lat_e7 != r[i].lat || a.lon_e7 != r[i].lon ||
            a.acc_m != r[i].acc)
            all = false;
        uint8_t miss[6];
        memcpy(miss, r[i].bssid, 6);
        miss[5] = (uint8_t)(miss[5] - 1); /* odd: between this one and the last */
        if (cairn_wifiloc_lookup(&t, miss, &a)) none = false;
    }
    CHECK("every record is found, first to last", all);
    CHECK("nothing between them is", none);
    uint8_t above[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, below[6] = { 0 };
    cairn_wifiloc_anchor_t a;
    CHECK("nothing above the last or below the first",
          !cairn_wifiloc_lookup(&t, above, &a) && !cairn_wifiloc_lookup(&t, below, &a));
}

/* ── locating ────────────────────────────────────────────────────────────── */

static cairn_wifiloc_table_t g_t;
static uint8_t g_buf[4096];

static void table(const rec_t *r, uint32_t n)
{
    size_t len = build(g_buf, r, n, 16);
    if (cairn_wifiloc_parse(g_buf, len, &g_t) != CAIRN_WIFILOC_OK) {
        printf("  FAIL  [wifiloc] fixture table did not parse\n");
        g_fail++;
    }
}

static double off_m(const cairn_wifiloc_estimate_t *e, int32_t lat, int32_t lon)
{
    return cairn_wifiloc_distance_m(e->lat_e7, e->lon_e7, lat, lon);
}

static void test_locate(void)
{
    cairn_wifiloc_estimate_t e;

    CHECK("1e-3 degree of latitude is 111.3 m", fabs(cairn_wifiloc_distance_m(0, 0, 10000, 0) - 111.32) < 0.01);

    /* Home: three access points within 60 m, learned at 30..60 m. A shop 5 km east. */
    rec_t r[8] = {
        R(0x01, north(0), east(0), 30),
        R(0x02, north(40), east(10), 45),
        R(0x03, north(-20), east(40), 60),
        R(0x04, north(0), east(5000), 40),     /* the shop */
        R(0x05, north(0), east(5050), 40),     /* the shop's other one */
        R(0x06, north(0), east(1000), 250),    /* down the road, loosely learned */
        R(0x07, north(200), east(200), 25),
        R(0x08, north(-200), east(-200), 25),
    };
    table(r, 8);

    cairn_wifiloc_obs_t none[2] = { O(0xA0, -40), O(0xA1, -50) };
    CHECK("no known access point: nothing",
          !cairn_wifiloc_locate(&g_t, none, 2, &e) && e.verdict == CAIRN_WIFILOC_NONE_KNOWN && e.heard == 2 && e.known == 0);
    CHECK("an empty scan: nothing", !cairn_wifiloc_locate(&g_t, NULL, 0, &e) && e.verdict == CAIRN_WIFILOC_NONE_KNOWN);

    /* One. */
    cairn_wifiloc_obs_t one = O(0x01, -55);
    CHECK("one strong access point: its position",
          cairn_wifiloc_locate(&g_t, &one, 1, &e) && e.lat_e7 == r[0].lat && e.lon_e7 == r[0].lon && e.used == 1);
    CHECK("at no better than 100 m", e.acc_m == 100);
    one = O(0x06, -50);
    CHECK("one strong but loosely learned: its own accuracy", cairn_wifiloc_locate(&g_t, &one, 1, &e) && e.acc_m == 250);
    one = O(0x01, -60);
    CHECK("one at exactly -60 dBm: located", cairn_wifiloc_locate(&g_t, &one, 1, &e));
    one = O(0x01, -61);
    CHECK("one at -61 dBm: nothing", !cairn_wifiloc_locate(&g_t, &one, 1, &e) && e.verdict == CAIRN_WIFILOC_ONE_WEAK);
    cairn_wifiloc_obs_t weak_plus_unknown[3] = { O(0x01, -75), O(0xA0, -30), O(0xA1, -35) };
    CHECK("one weak known among strong unknown ones: nothing",
          !cairn_wifiloc_locate(&g_t, weak_plus_unknown, 3, &e) && e.verdict == CAIRN_WIFILOC_ONE_WEAK && e.heard == 3 && e.known == 1);

    /* Home, three agreeing. */
    cairn_wifiloc_obs_t home[4] = { O(0x01, -50), O(0x02, -70), O(0x03, -80), O(0xA0, -40) };
    bool ok = cairn_wifiloc_locate(&g_t, home, 4, &e);
    CHECK("three agreeing: located", ok && e.used == 3 && e.known == 3 && e.heard == 4);
    CHECK("weighted to the strongest (within 10 m of it)", off_m(&e, r[0].lat, r[0].lon) < 10.0);
    CHECK("accuracy is the worst learned one, 60 m, raised to 100", e.acc_m == 100 && e.rms_m < 60);

    /* Home plus the shop's access point heard loud (a phone hotspot named like it, a
     * table that has it wrong): the stray is dropped, not averaged in. */
    cairn_wifiloc_obs_t stray[4] = { O(0x01, -75), O(0x02, -78), O(0x03, -80), O(0x04, -35) };
    ok = cairn_wifiloc_locate(&g_t, stray, 4, &e);
    CHECK("a loud stray 5 km away: dropped, home located", ok && e.used == 3 && e.known == 4);
    CHECK("and the estimate is at home, not pulled toward the stray", off_m(&e, r[0].lat, r[0].lon) < 60.0);

    /* Two and two: two places. */
    cairn_wifiloc_obs_t split[4] = { O(0x01, -60), O(0x02, -60), O(0x04, -60), O(0x05, -60) };
    CHECK("two here and two 5 km away: nothing",
          !cairn_wifiloc_locate(&g_t, split, 4, &e) && e.verdict == CAIRN_WIFILOC_DISAGREE);
    cairn_wifiloc_obs_t pair_apart[2] = { O(0x01, -40), O(0x06, -40) };
    CHECK("two a kilometre apart: nothing",
          !cairn_wifiloc_locate(&g_t, pair_apart, 2, &e) && e.verdict == CAIRN_WIFILOC_DISAGREE);
    cairn_wifiloc_obs_t two_one[3] = { O(0x01, -70), O(0x02, -70), O(0x04, -40) };
    CHECK("two here, one loud stray: two of three agree, located",
          cairn_wifiloc_locate(&g_t, two_one, 3, &e) && e.used == 2 && off_m(&e, r[0].lat, r[0].lon) < 60.0);
    cairn_wifiloc_obs_t two_two_strays[4] = { O(0x01, -70), O(0x02, -70), O(0x04, -40), O(0x06, -40) };
    CHECK("two here and two strays in different places: not a majority, nothing",
          !cairn_wifiloc_locate(&g_t, two_two_strays, 4, &e) && e.verdict == CAIRN_WIFILOC_DISAGREE);

    /* Spread wide but within the radius: the RMS is the accuracy. */
    cairn_wifiloc_obs_t wide[3] = { O(0x07, -60), O(0x08, -60), O(0x01, -60) };
    ok = cairn_wifiloc_locate(&g_t, wide, 3, &e);
    CHECK("three across 560 m: located, accuracy is their RMS (> 100 m)",
          ok && e.used == 3 && e.acc_m == e.rms_m && e.rms_m > 200 && e.rms_m < 300);

    /* The same BSSID twice counts once, at its strongest. */
    cairn_wifiloc_obs_t twice[2] = { O(0x01, -80), O(0x01, -55) };
    CHECK("a repeated BSSID counts once, at its strongest",
          cairn_wifiloc_locate(&g_t, twice, 2, &e) && e.heard == 1 && e.known == 1 && e.used == 1);

    /* Astride the antimeridian: 222 m apart, not 40,000 km. */
    rec_t am[2] = { R(0x01, 0, 1799990000, 30), R(0x02, 0, -1799990000, 30) };
    table(am, 2);
    cairn_wifiloc_obs_t amo[2] = { O(0x01, -60), O(0x02, -60) };
    ok = cairn_wifiloc_locate(&g_t, amo, 2, &e);
    CHECK("two astride the antimeridian: located on it",
          ok && (e.lon_e7 >= 1799990000 || e.lon_e7 <= -1799990000) && e.acc_m >= 111 && e.acc_m <= 112);
}

static void test_hint(void)
{
    cairn_wifiloc_estimate_t e;
    memset(&e, 0, sizeof(e));
    e.verdict = CAIRN_WIFILOC_LOCATED;
    e.lat_e7 = HOME_LAT;
    e.lon_e7 = HOME_LON;
    e.acc_m = 100;
    int32_t alt;
    uint32_t acc;

    cairn_wifiloc_hint(&e, true, north(3000), east(0), 1234, &alt, &acc);
    CHECK("last fix 3 km off: its altitude, +/- sqrt(100^2 + 200^2) = 224 m", alt == 1234 && acc == 22400);
    cairn_wifiloc_hint(&e, true, north(50000), east(0), 1234, &alt, &acc);
    CHECK("50 km off: its altitude, +/- 1005 m", alt == 1234 && acc == 100500);
    cairn_wifiloc_hint(&e, true, north(500000), east(0), 1234, &alt, &acc);
    CHECK("500 km off: sea level, +/- 3002 m", alt == 0 && acc == 300200);
    cairn_wifiloc_hint(&e, false, 0, 0, 0, &alt, &acc);
    CHECK("no last fix: sea level, +/- 3002 m", alt == 0 && acc == 300200);
}

static void test_fetch_due(void)
{
    const uint32_t H = 3600u, NOW = 1792951200u;
    CHECK("no copy: ask", cairn_wifiloc_fetch_due(false, 0, 0, false, 0));
    CHECK("no copy, but asked 10 min ago this boot: wait",
          !cairn_wifiloc_fetch_due(false, 0, 0, true, 600000));
    CHECK("no copy, asked an hour ago: ask", cairn_wifiloc_fetch_due(false, 0, 0, true, 3600000));
    CHECK("checked 23 h ago by the clock: no", !cairn_wifiloc_fetch_due(true, NOW - 23 * H, NOW, false, 0));
    CHECK("checked 24 h ago: ask", cairn_wifiloc_fetch_due(true, NOW - 24 * H, NOW, false, 0));
    CHECK("a clock two hours behind the last check: ask", cairn_wifiloc_fetch_due(true, NOW + 2 * H, NOW, false, 0));
    CHECK("no clock: once a boot", cairn_wifiloc_fetch_due(true, NOW, 0, false, 0));
    CHECK("no clock, asked 2 h ago this boot: no", !cairn_wifiloc_fetch_due(true, NOW, 0, true, 2 * H * 1000u));
    CHECK("no clock, asked a day ago this boot: ask", cairn_wifiloc_fetch_due(true, 0, 0, true, 24 * H * 1000u));
    CHECK("a clock but no recorded check: once a boot", cairn_wifiloc_fetch_due(true, 0, NOW, false, 0) &&
                                                       !cairn_wifiloc_fetch_due(true, 0, NOW, true, 2 * H * 1000u));
}

static bool contains(const uint8_t *hay, size_t n, const uint8_t *needle, size_t k)
{
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(hay + i, needle, k)) return true;
    return false;
}

static void test_card(void)
{
    uint8_t root[32], other[32], key[32], key2[32], again[32];
    for (int i = 0; i < 32; i++) { root[i] = (uint8_t)i; other[i] = (uint8_t)(i + 1); }
    cairn_wifiloc_card_key(root, key);
    cairn_wifiloc_card_key(root, again);
    cairn_wifiloc_card_key(other, key2);
    CHECK("the card key is derived, the same each boot, and differs by root",
          !memcmp(key, again, 32) && memcmp(key, key2, 32) != 0 && memcmp(key, root, 32) != 0);

    static uint8_t table[16 + 3 * 16], sealed[sizeof(table) + CAIRN_WIFILOC_CARD_OVERHEAD], back[sizeof(table)];
    rec_t r[3] = { R(1, HOME_LAT, HOME_LON, 30), R(2, HOME_LAT, HOME_LON, 40), R(3, HOME_LAT, HOME_LON, 50) };
    size_t n = build(table, r, 3, 16);
    uint8_t nonce[24];
    memset(nonce, 0xA5, sizeof(nonce));
    size_t s = cairn_wifiloc_card_seal(key, nonce, table, n, sealed, sizeof(sealed));
    CHECK("sealed: 44 bytes more, and no BSSID left in the clear",
          s == n + 44 && !memcmp(sealed, "CWS1", 4) && !contains(sealed, s, r[0].bssid, 6));
    CHECK("a short buffer is refused", cairn_wifiloc_card_seal(key, nonce, table, n, sealed, n + 43) == 0);
    size_t got = 0;
    CHECK("opens to the table it was", cairn_wifiloc_card_open(key, sealed, s, back, sizeof(back), &got) &&
                                       got == n && !memcmp(back, table, n));
    memset(back, 0, sizeof(back));
    CHECK("not under another root's key", !cairn_wifiloc_card_open(key2, sealed, s, back, sizeof(back), &got) &&
                                          got == 0 && back[0] == 0);
    sealed[60] ^= 1;
    CHECK("not with a flipped bit", !cairn_wifiloc_card_open(key, sealed, s, back, sizeof(back), &got));
    sealed[60] ^= 1;
    sealed[0] = 'X';
    CHECK("not with another magic", !cairn_wifiloc_card_open(key, sealed, s, back, sizeof(back), &got));
    sealed[0] = 'C';
    CHECK("not truncated", !cairn_wifiloc_card_open(key, sealed, s - 1, back, sizeof(back), &got));
    CHECK("not into a buffer too small", !cairn_wifiloc_card_open(key, sealed, s, back, n - 1, &got));
}

int main(void)
{
    printf("wifiloc\n");
    test_parse();
    test_lookup();
    test_locate();
    test_hint();
    test_fetch_due();
    test_card();
    printf("wifiloc: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
