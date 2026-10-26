/*
 * Assisted GNSS (lib/cairn_agnss): UBX framing, the AssistNow stream, what is
 * injected for a given day and how it is cut into ATGDS lines, MGA-INI-TIME_UTC
 * against bytes computed independently (Python struct, 2026-10-25), the receiver
 * identity found among NMEA, and when to ask the server again.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_agnss.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [agnss] %s\n", name); }          \
        else      { g_fail++; printf("  FAIL  [agnss] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* 2026-10-25, the day the slow starts were measured. */
#define DAY0 20751

static size_t ano(uint8_t *out, int32_t day, uint8_t gnss, uint8_t sv)
{
    uint8_t p[76];
    int y, m, d;
    memset(p, 0, sizeof(p));
    cairn_agnss_civil_from_days(day, &y, &m, &d);
    p[2] = sv; p[3] = gnss;
    p[4] = (uint8_t)(y - 2000); p[5] = (uint8_t)m; p[6] = (uint8_t)d;
    for (int i = 8; i < 72; i++) p[i] = (uint8_t)(i * sv + day);
    return cairn_ubx_encode(0x13, 0x20, p, sizeof(p), out, 84);
}

static size_t alm(uint8_t *out, uint8_t sv)
{
    uint8_t p[36];
    memset(p, 0, sizeof(p));
    p[0] = 0x02; p[2] = sv;
    return cairn_ubx_encode(0x13, 0x00, p, sizeof(p), out, 44);
}

/* Almanacs, then three days of predictions (yesterday, today, tomorrow) for
 * two constellations of three satellites. */
static size_t stream(uint8_t *buf)
{
    size_t n = 0;
    for (uint8_t sv = 1; sv <= 4; sv++) n += alm(buf + n, sv);
    for (int32_t d = DAY0 - 1; d <= DAY0 + 1; d++)
        for (uint8_t g = 0; g <= 2; g += 2)
            for (uint8_t sv = 1; sv <= 3; sv++) n += ano(buf + n, d, g, sv);
    return n;
}

static int unhex(char c) { return c <= '9' ? c - '0' : c - 'A' + 10; }

/* Bytes back out of an ATGDS line; -1 if the line is malformed. */
static int line_bytes(const char *line, size_t len, uint8_t *out)
{
    if (len < 8 || memcmp(line, "ATGDS", 5) != 0 || line[len - 1] != '\r') return -1;
    int n = 0;
    for (size_t i = 5; i + 2 < len + 1; i += 3) {
        out[n++] = (uint8_t)(unhex(line[i]) << 4 | unhex(line[i + 1]));
        char sep = line[i + 2];
        if (sep == '\r') return i + 3 == len ? n : -1;
        if (sep != ' ') return -1;
    }
    return -1;
}

static void test_framing(void)
{
    uint8_t b[8];
    CHECK("UBX-SEC-UNIQID poll is the guide's B5 62 27 03 00 00 2A A5",
          cairn_ubx_encode(0x27, 0x03, NULL, 0, b, sizeof(b)) == 8 && !memcmp(b, CAIRN_UBX_POLL_SEC_UNIQID, 8));
    CHECK("UBX-MON-VER poll is the guide's B5 62 0A 04 00 00 0E 34",
          cairn_ubx_encode(0x0A, 0x04, NULL, 0, b, sizeof(b)) == 8 && !memcmp(b, CAIRN_UBX_POLL_MON_VER, 8));
    CHECK("encode refuses a short buffer", cairn_ubx_encode(0x27, 0x03, NULL, 0, b, 7) == 0);

    /* A frame among NMEA, another cut off by the end of the buffer. */
    uint8_t buf[256];
    size_t n = 0;
    const char *nmea = "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    memcpy(buf, nmea, strlen(nmea)); n += strlen(nmea);
    n += alm(buf + n, 7);
    memcpy(buf + n, "\xB5\x62\x13", 3); n += 3;          /* a stray sync */
    size_t whole = n;
    n += alm(buf + n, 8);
    n -= 5;                                               /* cut the last one */
    size_t off = 0;
    cairn_ubx_t f;
    CHECK("find skips NMEA to the frame", cairn_ubx_find(buf, n, &off, &f) && f.cls == 0x13 && f.payload[2] == 7);
    CHECK("find does not take a truncated frame", !cairn_ubx_find(buf, n, &off, &f) && off == n);
    off = 0;
    CHECK("next refuses bytes that are not a frame", cairn_ubx_next(buf, whole, &off, &f) == -1);
}

static void test_dates_and_time(void)
{
    int y, m, d;
    cairn_agnss_civil_from_days(DAY0, &y, &m, &d);
    CHECK("2026-10-25 is day 20751", cairn_agnss_days_from_civil(2026, 10, 25) == DAY0 && y == 2026 && m == 10 && d == 25);
    bool round = true;
    for (int32_t z = 0; z < 40000; z += 7) {
        cairn_agnss_civil_from_days(z, &y, &m, &d);
        if (cairn_agnss_days_from_civil(y, m, d) != z) round = false;
    }
    CHECK("civil dates round-trip 1970..2079", round);

    /* 2026-10-25T13:45:30.250Z, +/- 2.7 s, against Python's struct. */
    static const uint8_t want[32] = {
        0xB5, 0x62, 0x13, 0x40, 0x18, 0x00, 0x10, 0x00, 0x00, 0x80, 0xEA, 0x07, 0x0A, 0x19, 0x0D, 0x2D,
        0x1E, 0x00, 0x80, 0xB2, 0xE6, 0x0E, 0x02, 0x00, 0x00, 0x00, 0x00, 0x27, 0xB9, 0x29, 0x98, 0xC5 };
    uint8_t t[32];
    CHECK("MGA-INI-TIME_UTC matches the independent encoding byte for byte",
          cairn_agnss_time_utc_frame(1792935930250ull, 2700, t) == 32 && !memcmp(t, want, 32));

    /* The position hint, the same bytes send_gnss_aiding has always built. */
    static const uint8_t pos[28] = {
        0xB5, 0x62, 0x13, 0x40, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0xFE, 0x83, 0x16,
        0x30, 0x48, 0x08, 0xB7, 0xD2, 0x04, 0x00, 0x00, 0x40, 0x42, 0x0F, 0x00, 0xA5, 0xA5 };
    uint8_t p[28];
    CHECK("MGA-INI-POS_LLH matches the independent encoding",
          cairn_agnss_pos_llh_frame(377749000, -1224194000, 1234, 1000000, p) == 28 && !memcmp(p, pos, 28));

    CHECK("phone time an hour later, awake: 500 + 200 transit + 180 drift",
          cairn_agnss_time_acc(500, 3600000, 0) == 880);
    CHECK("network time after an hour of light sleep: 2 % of it added",
          cairn_agnss_time_acc(2000, 3600000, 3600000) == 2000 + 200 + 72000);
    CHECK("sleep longer than elapsed is clamped", cairn_agnss_time_acc(0, 1000, 5000) == 200 + 20);
    CHECK("a day of sleep is past the 15-minute limit",
          cairn_agnss_time_acc(500, 86400000u, 86400000u) > CAIRN_AGNSS_TIME_MAX_ACC_MS);
    CHECK("accuracy saturates", cairn_agnss_time_acc(0xFFFFFFF0u, 0xFFFFFFFFu, 0) == 0xFFFFFFFFu);

    int32_t lo, hi;
    uint64_t noon = (uint64_t)DAY0 * 86400000ull + 43200000ull;
    cairn_agnss_day_window(noon, 900000, &lo, &hi);
    CHECK("noon: one day", lo == DAY0 && hi == DAY0);
    cairn_agnss_day_window((uint64_t)(DAY0 + 1) * 86400000ull - 120000ull, 300000, &lo, &hi);
    CHECK("23:58 +/- 5 min: today and tomorrow", lo == DAY0 && hi == DAY0 + 1);
}

static void test_stream(void)
{
    static uint8_t buf[8192];
    size_t n = stream(buf);
    cairn_agnss_info_t info;
    CHECK("a clean stream validates", cairn_agnss_validate(buf, n, &info) && info.ano == 18 &&
          info.almanac == 4 && info.first_day == DAY0 - 1 && info.last_day == DAY0 + 1 && info.bytes == n);

    uint8_t ini[32];
    cairn_agnss_time_utc_frame(1792935930250ull, 10000, ini);
    static uint8_t with_ini[8192];
    memcpy(with_ini, ini, 32);
    memcpy(with_ini + 32, buf, n);
    CHECK("a stream holding INI-TIME_UTC is refused", !cairn_agnss_validate(with_ini, n + 32, &info) && info.ini == 1);
    CHECK("a truncated stream is refused", !cairn_agnss_validate(buf, n - 1, &info));
    static uint8_t text[] = "403003 Access is blocked";
    CHECK("an error string is refused", !cairn_agnss_validate(text, sizeof(text) - 1, &info));
    CHECK("almanacs alone are refused", !cairn_agnss_validate(buf, 4 * 44, &info));
    uint8_t mon[48];
    size_t mn = cairn_ubx_encode(0x0A, 0x04, buf, 40, mon, sizeof(mon));
    static uint8_t mixed[8192];
    memcpy(mixed, buf, n);
    memcpy(mixed + n, mon, mn);
    CHECK("a non-MGA frame is refused", !cairn_agnss_validate(mixed, n + mn, &info));
    CHECK("an oversize stream is refused", !cairn_agnss_validate(buf, CAIRN_AGNSS_MAX_BYTES + 1, &info));

    uint32_t msgs, bytes;
    cairn_agnss_count(buf, n, CAIRN_AGNSS_PICK_ANO, DAY0, DAY0, &msgs, &bytes);
    CHECK("today's predictions: 6 messages, 504 bytes", msgs == 6 && bytes == 6 * 84);
    cairn_agnss_count(buf, n, CAIRN_AGNSS_PICK_ALMANAC, 0, 0, &msgs, &bytes);
    CHECK("the almanacs: 4 messages", msgs == 4 && bytes == 4 * 44);

    int32_t lo, hi;
    CHECK("a day the stream has is used as is",
          cairn_agnss_ano_window(buf, n, DAY0, DAY0, &lo, &hi) && lo == DAY0 && hi == DAY0);
    CHECK("the day after the stream ends falls back to its last day",
          cairn_agnss_ano_window(buf, n, DAY0 + 2, DAY0 + 2, &lo, &hi) && lo == DAY0 + 1 && hi == DAY0 + 1);
    CHECK("the day before it starts uses its first day",
          cairn_agnss_ano_window(buf, n, DAY0 - 2, DAY0 - 2, &lo, &hi) && lo == DAY0 - 1);
    CHECK("a week past the stream: nothing", !cairn_agnss_ano_window(buf, n, DAY0 + 7, DAY0 + 7, &lo, &hi));
}

/* Every byte of the pick arrives, in order, in lines of at most 28 bytes that never
 * straddle two frames, no faster than one per interval. */
static void test_pacer(void)
{
    static uint8_t buf[8192], want[8192], got[8192];
    size_t n = stream(buf);

    size_t wn = 0, off = 0;
    cairn_ubx_t f;
    size_t frame_ends[64];
    int nf = 0;
    while (cairn_ubx_next(buf, n, &off, &f) == 1) {
        if (!cairn_agnss_wanted(&f, CAIRN_AGNSS_PICK_ANO, DAY0, DAY0)) continue;
        memcpy(want + wn, f.frame, f.frame_len);
        wn += f.frame_len;
        frame_ends[nf++] = wn;
    }

    cairn_agnss_pacer_t p;
    cairn_agnss_pacer_start(&p, CAIRN_AGNSS_PICK_ANO, DAY0, DAY0, 1000);
    char line[CAIRN_AGNSS_LINE_CAP];
    size_t gn = 0;
    uint32_t now = 1000;
    bool short_lines = true, aligned = true, paced = true;
    int lines = 0;
    for (int guard = 0; guard < 10000 && !p.done; guard++) {
        size_t len = cairn_agnss_pacer_next(&p, buf, n, now, line, sizeof(line));
        if (len > 0) {
            int b = line_bytes(line, len, got + gn);
            if (b <= 0 || b > (int)CAIRN_AGNSS_LINE_BYTES) short_lines = false;
            /* A line ends inside one frame or exactly at its end. */
            size_t start = gn, end = gn + (size_t)b;
            for (int i = 0; i < nf; i++)
                if (frame_ends[i] > start && frame_ends[i] < end) aligned = false;
            gn = end;
            lines++;
            if (cairn_agnss_pacer_next(&p, buf, n, now + CAIRN_AGNSS_LINE_INTERVAL_MS - 1, line, sizeof(line)) != 0)
                paced = false;
        }
        now += CAIRN_AGNSS_LINE_INTERVAL_MS;
    }
    CHECK("the pacer finishes", p.done && !p.malformed);
    CHECK("it sends exactly today's frames, in order", gn == wn && !memcmp(got, want, wn));
    CHECK("6 messages of 84 bytes in 3 lines each (28 + 28 + 28)", p.msgs == 6 && lines == 18 && p.bytes == 504);
    CHECK("no line carries more than 28 bytes", short_lines);
    CHECK("no line straddles two frames", aligned);
    CHECK("no line comes sooner than the interval", paced);
    CHECK("a finished pacer stays quiet", cairn_agnss_pacer_next(&p, buf, n, now + 100000, line, sizeof(line)) == 0);

    /* The time and position pair, as the glue sends it: 32 + 28 bytes. */
    uint8_t small[60];
    cairn_agnss_time_utc_frame(1792935930250ull, 880, small);
    cairn_agnss_pos_llh_frame(377749000, -1224194000, 1234, 1000000, small + 32);
    cairn_agnss_pacer_start(&p, CAIRN_AGNSS_PICK_ALL, 0, 0, 0);
    size_t l1 = cairn_agnss_pacer_next(&p, small, 60, 0, line, sizeof(line));
    int b1 = line_bytes(line, l1, got);
    size_t l2 = cairn_agnss_pacer_next(&p, small, 60, 25, line, sizeof(line));
    int b2 = line_bytes(line, l2, got);
    size_t l3 = cairn_agnss_pacer_next(&p, small, 60, 50, line, sizeof(line));
    int b3 = line_bytes(line, l3, got);
    cairn_agnss_pacer_next(&p, small, 60, 75, line, sizeof(line));
    CHECK("TIME_UTC goes as 28 + 4, then POS_LLH as 28", b1 == 28 && b2 == 4 && b3 == 28 && p.done && p.msgs == 2);
    CHECK("a 28-byte line is 89 characters, as long as the POS_LLH hint always was", l1 == 89);

    /* Data that turns out malformed ends the pick rather than sending garbage. */
    static uint8_t bad[8192];
    memcpy(bad, buf, n);
    bad[4 * 44 + 84 * 6 + 10] ^= 0xFF;   /* inside a later frame */
    cairn_agnss_pacer_start(&p, CAIRN_AGNSS_PICK_ANO, DAY0, DAY0, 0);
    for (int guard = 0; guard < 1000 && !p.done; guard++)
        cairn_agnss_pacer_next(&p, bad, n, (uint32_t)guard * 25u, line, sizeof(line));
    CHECK("a corrupt stream stops the pacer", p.done && p.malformed);
}

static size_t sec_reply(uint8_t *out, bool m10)
{
    static const uint8_t p10[10] = { 2, 0, 0, 0, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };
    static const uint8_t p9[9]   = { 1, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55 };
    return m10 ? cairn_ubx_encode(0x27, 0x03, p10, 10, out, 18) : cairn_ubx_encode(0x27, 0x03, p9, 9, out, 17);
}

static size_t mon_reply(uint8_t *out, const char *mod)
{
    uint8_t p[100];
    memset(p, 0, sizeof(p));
    memcpy(p, "ROM SPG 5.10 (7b202e)", 21);
    memcpy(p + 30, "000A0000", 8);
    memcpy(p + 40, "PROTVER=34.10", 13);
    snprintf((char *)p + 70, 30, "MOD=%s", mod);
    return cairn_ubx_encode(0x0A, 0x04, p, sizeof(p), out, 108);
}

static void test_identity(void)
{
    /* What an ATGRR reply might hold if the co-processor passes UBX through:
     * NMEA, the two replies, a third truncated one. */
    uint8_t buf[512];
    size_t n = 0;
    const char *gga = "$GNGGA,,,,,,0,00,99.99,,,,,,*56\r\n";
    memcpy(buf + n, gga, strlen(gga)); n += strlen(gga);
    n += sec_reply(buf + n, true);
    memcpy(buf + n, gga, strlen(gga)); n += strlen(gga);
    n += mon_reply(buf + n, "NEO-M10S");
    n += sec_reply(buf + n, false) - 3;

    cairn_agnss_identity_t id;
    memset(&id, 0, sizeof(id));
    CHECK("both replies are found among NMEA", cairn_agnss_identity_scan(buf, n, &id) == 2 &&
          cairn_agnss_identity_complete(&id) && id.sec_len == 18 && id.mon_len == 108);

    char json[600];
    size_t jl = cairn_agnss_identity_json(&id, json, sizeof(json));
    CHECK("the report is the two frames in upper-case hex",
          jl > 0 && !strncmp(json, "{\"sec_uniqid\":\"B56227030A0002000000A1B2C3D4E5F6", 47) &&
          strstr(json, "\",\"mon_ver\":\"B5620A046400524F4D") != NULL && json[jl - 1] == '}' && jl == strlen(json));
    CHECK("a short buffer gives no partial JSON", cairn_agnss_identity_json(&id, json, 100) == 0);

    char desc[200];
    cairn_agnss_identity_describe(&id, desc, sizeof(desc));
    CHECK("the log line names the chip and the module",
          strstr(desc, "id=a1b2c3d4e5f6") && strstr(desc, "sw=ROM SPG 5.10 (7b202e)") &&
          strstr(desc, "hw=000A0000") && strstr(desc, "MOD=NEO-M10S"));

    uint8_t card[CAIRN_AGNSS_SECID_MAX + CAIRN_AGNSS_MONVER_MAX];
    memcpy(card, id.sec, id.sec_len);
    memcpy(card + id.sec_len, id.mon, id.mon_len);
    cairn_agnss_identity_t back;
    CHECK("the card copy decodes", cairn_agnss_identity_decode(card, (size_t)id.sec_len + id.mon_len, &back) &&
          back.sec_len == id.sec_len && !memcmp(back.mon, id.mon, id.mon_len));
    CHECK("a torn card copy does not", !cairn_agnss_identity_decode(card, (size_t)id.sec_len + id.mon_len - 1, &back));

    cairn_agnss_identity_t m9;
    memset(&m9, 0, sizeof(m9));
    uint8_t s9[17];
    sec_reply(s9, false);
    cairn_agnss_identity_scan(s9, sizeof(s9), &m9);
    CHECK("an M9's 5-byte id is taken too", m9.sec_len == 17 && !cairn_agnss_identity_complete(&m9));

    /* The poll itself (empty payload) is not a reply. */
    cairn_agnss_identity_t none;
    memset(&none, 0, sizeof(none));
    CHECK("an echoed poll is not mistaken for a reply",
          cairn_agnss_identity_scan(CAIRN_UBX_POLL_SEC_UNIQID, 8, &none) == 0 &&
          cairn_agnss_identity_scan(CAIRN_UBX_POLL_MON_VER, 8, &none) == 0);
}

static void test_fetch_due(void)
{
    cairn_agnss_info_t info;
    memset(&info, 0, sizeof(info));
    info.first_day = DAY0;
    info.last_day = DAY0 + 6;
    const uint32_t H = 3600000u;
    CHECK("no copy: ask", cairn_agnss_fetch_due(false, NULL, CAIRN_AGNSS_NO_DAY, false, 0));
    CHECK("first session of a boot: ask", cairn_agnss_fetch_due(true, &info, DAY0, false, 0));
    CHECK("asked ten minutes ago: not again, even with no copy",
          !cairn_agnss_fetch_due(false, NULL, DAY0, true, H / 6));
    CHECK("a fresh copy, asked two hours ago: no", !cairn_agnss_fetch_due(true, &info, DAY0, true, 2 * H));
    CHECK("a day since asking: ask", cairn_agnss_fetch_due(true, &info, DAY0, true, 24 * H));
    CHECK("four days of predictions left: ask", cairn_agnss_fetch_due(true, &info, DAY0 + 3, true, 2 * H));
    CHECK("time unknown, fresh-ish: no", !cairn_agnss_fetch_due(true, &info, CAIRN_AGNSS_NO_DAY, true, 2 * H));
}

int main(void)
{
    printf("agnss\n");
    test_framing();
    test_dates_and_time();
    test_stream();
    test_pacer();
    test_identity();
    test_fetch_due();
    printf("agnss: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
