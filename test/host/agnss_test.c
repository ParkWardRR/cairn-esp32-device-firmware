/*
 * The receiver's aiding messages (lib/cairn_agnss): UBX framing, MGA-INI-TIME_UTC and
 * MGA-INI-POS_LLH against bytes computed independently (Python struct, 2026-10-25, on
 * the parked feat/gnss-assist branch), the time accuracy's growth, and how a frame is
 * cut into ATGDS lines.
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
    static const uint8_t sec_poll[8] = { 0xB5, 0x62, 0x27, 0x03, 0x00, 0x00, 0x2A, 0xA5 };
    uint8_t b[8];
    CHECK("an empty UBX-SEC-UNIQID poll is the guide's B5 62 27 03 00 00 2A A5",
          cairn_ubx_encode(0x27, 0x03, NULL, 0, b, sizeof(b)) == 8 && !memcmp(b, sec_poll, 8));
    CHECK("encode refuses a short buffer", cairn_ubx_encode(0x27, 0x03, NULL, 0, b, 7) == 0);
}

static void test_messages(void)
{
    int y, m, d;
    cairn_agnss_civil_from_days(20751, &y, &m, &d);
    CHECK("2026-10-25 is day 20751", cairn_agnss_days_from_civil(2026, 10, 25) == 20751 && y == 2026 && m == 10 && d == 25);
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
    CHECK("network time sent at once: 2000 + 200 transit + 1 ms drift",
          cairn_agnss_time_acc(2000, 5, 0) == 2201);
    CHECK("sleep longer than elapsed is clamped", cairn_agnss_time_acc(0, 1000, 5000) == 200 + 20);
    CHECK("a day of sleep is past the 15-minute limit",
          cairn_agnss_time_acc(500, 86400000u, 86400000u) > CAIRN_AGNSS_TIME_MAX_ACC_MS);
    CHECK("accuracy saturates", cairn_agnss_time_acc(0xFFFFFFF0u, 0xFFFFFFFFu, 0) == 0xFFFFFFFFu);
}

static void test_lines(void)
{
    uint8_t pos[28];
    cairn_agnss_pos_llh_frame(377749000, -1224194000, 1234, 1000000, pos);
    char line[CAIRN_AGNSS_LINE_CAP];
    uint8_t back[64];

    /* The hint goes as one line, exactly as the firmware has always sent it. */
    size_t n = cairn_agnss_frame_line(pos, 28, 0, line, sizeof(line));
    CHECK("POS_LLH is one 89-character line", n == 5 + 3 * 28 && line_bytes(line, n, back) == 28 && !memcmp(back, pos, 28));
    CHECK("and there is no second", cairn_agnss_frame_line(pos, 28, 1, line, sizeof(line)) == 0);
    CHECK("the line ends in CR, no trailing space", line[n - 1] == '\r' && line[n - 2] != ' ');

    /* TIME_UTC is 32 bytes: 28 and 4, and the receiver reads them as one stream. */
    uint8_t t[32];
    cairn_agnss_time_utc_frame(1792935930250ull, 2700, t);
    uint8_t joined[64];
    int total = 0;
    unsigned lines = 0;
    for (unsigned i = 0;; i++) {
        size_t k = cairn_agnss_frame_line(t, 32, i, line, sizeof(line));
        if (k == 0) break;
        int got = line_bytes(line, k, joined + total);
        if (got < 0) { total = -1; break; }
        total += got;
        lines++;
    }
    CHECK("TIME_UTC goes as two lines, 28 + 4, that rejoin to the frame",
          lines == 2 && total == 32 && !memcmp(joined, t, 32));
    CHECK("a line refuses more than 28 bytes", cairn_agnss_atgds_line(t, 29, line, sizeof(line)) == 0);
    CHECK("or a buffer too short for it", cairn_agnss_atgds_line(t, 28, line, 5 + 3 * 28) == 0);
}

int main(void)
{
    printf("agnss\n");
    test_framing();
    test_messages();
    test_lines();
    printf("agnss: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
