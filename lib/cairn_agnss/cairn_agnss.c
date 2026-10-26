/* Aiding messages for the receiver, the portable half. See cairn_agnss.h. */

#include "cairn_agnss.h"

#include <string.h>

#define UBX_SYNC1 0xB5u
#define UBX_SYNC2 0x62u

#define CLS_MGA    0x13u
#define ID_MGA_INI 0x40u

void cairn_ubx_checksum(const uint8_t *body, size_t n, uint8_t *a, uint8_t *b)
{
    uint8_t x = 0, y = 0;
    for (size_t i = 0; i < n; i++) {
        x = (uint8_t)(x + body[i]);
        y = (uint8_t)(y + x);
    }
    *a = x;
    *b = y;
}

size_t cairn_ubx_encode(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len,
                        uint8_t *out, size_t cap)
{
    size_t n = (size_t)len + CAIRN_UBX_OVERHEAD;
    if (out == NULL || cap < n) return 0;
    out[0] = UBX_SYNC1;
    out[1] = UBX_SYNC2;
    out[2] = cls;
    out[3] = id;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)(len >> 8);
    if (len > 0) memcpy(out + 6, payload, len);
    cairn_ubx_checksum(out + 2, (size_t)len + 4u, &out[6 + len], &out[7 + len]);
    return n;
}

/* ── dates (Howard Hinnant's days_from_civil) ───────────────────────────── */

int32_t cairn_agnss_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + doe - 719468);
}

void cairn_agnss_civil_from_days(int32_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int32_t doe = z - era * 146097;
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int32_t mp  = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* ── messages ───────────────────────────────────────────────────────────── */

uint32_t cairn_agnss_time_acc(uint32_t obs_acc_ms, uint32_t elapsed_ms, uint32_t asleep_ms)
{
    if (asleep_ms > elapsed_ms) asleep_ms = elapsed_ms;
    uint64_t awake = (uint64_t)(elapsed_ms - asleep_ms);
    uint64_t acc = (uint64_t)obs_acc_ms + CAIRN_AGNSS_TIME_TRANSIT_MS
                 + (awake * CAIRN_AGNSS_AWAKE_DRIFT_PPM + 999999u) / 1000000u
                 + ((uint64_t)asleep_ms * CAIRN_AGNSS_SLEEP_DRIFT_PPM + 999999u) / 1000000u;
    return acc > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)acc;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

size_t cairn_agnss_time_utc_frame(uint64_t utc_ms, uint32_t acc_ms, uint8_t out[32])
{
    /* UBX-MGA-INI-TIME_UTC, 24 bytes: type 0x10, version 0, ref 0 (on receipt of this
     * message), leapSecs -128 (unknown), year u16, month, day, hour, minute, second,
     * reserved, ns u32, tAccS u16, reserved u16, tAccNs u32. */
    uint8_t p[24];
    memset(p, 0, sizeof(p));
    int32_t days = (int32_t)(utc_ms / 86400000ull);
    uint32_t ms_of_day = (uint32_t)(utc_ms % 86400000ull);
    int y, m, d;
    cairn_agnss_civil_from_days(days, &y, &m, &d);
    p[0] = 0x10;
    p[1] = 0x00;
    p[2] = 0x00;
    p[3] = 0x80;
    put_u16(&p[4], (uint16_t)y);
    p[6]  = (uint8_t)m;
    p[7]  = (uint8_t)d;
    p[8]  = (uint8_t)(ms_of_day / 3600000u);
    p[9]  = (uint8_t)((ms_of_day / 60000u) % 60u);
    p[10] = (uint8_t)((ms_of_day / 1000u) % 60u);
    put_u32(&p[12], (ms_of_day % 1000u) * 1000000u);
    uint32_t acc_s = acc_ms / 1000u;
    put_u16(&p[16], (uint16_t)(acc_s > 0xFFFFu ? 0xFFFFu : acc_s));
    put_u32(&p[20], (acc_ms % 1000u) * 1000000u);
    return cairn_ubx_encode(CLS_MGA, ID_MGA_INI, p, sizeof(p), out, 32);
}

size_t cairn_agnss_pos_llh_frame(int32_t lat_e7, int32_t lon_e7, int32_t alt_cm,
                                 uint32_t acc_cm, uint8_t out[28])
{
    /* UBX-MGA-INI-POS_LLH, 20 bytes: type 0x01, version 0, reserved[2], lat, lon
     * (1e-7 deg), alt (cm), posAcc (cm). */
    uint8_t p[20];
    memset(p, 0, sizeof(p));
    p[0] = 0x01;
    put_u32(&p[4], (uint32_t)lat_e7);
    put_u32(&p[8], (uint32_t)lon_e7);
    put_u32(&p[12], (uint32_t)alt_cm);
    put_u32(&p[16], acc_cm);
    return cairn_ubx_encode(CLS_MGA, ID_MGA_INI, p, sizeof(p), out, 28);
}

size_t cairn_agnss_atgds_line(const uint8_t *bytes, size_t n, char *line, size_t cap)
{
    if (n == 0 || n > CAIRN_AGNSS_LINE_BYTES || cap < 5u + 3u * n + 1u) return 0;
    static const char hexd[] = "0123456789ABCDEF";
    memcpy(line, "ATGDS", 5);
    size_t w = 5;
    for (size_t i = 0; i < n; i++) {
        line[w++] = hexd[bytes[i] >> 4];
        line[w++] = hexd[bytes[i] & 0xF];
        line[w++] = ' ';
    }
    line[w - 1] = '\r';
    line[w] = '\0';
    return w;
}

size_t cairn_agnss_frame_line(const uint8_t *frame, size_t len, unsigned i, char *line, size_t cap)
{
    size_t off = (size_t)i * CAIRN_AGNSS_LINE_BYTES;
    if (frame == NULL || off >= len) return 0;
    size_t n = len - off;
    if (n > CAIRN_AGNSS_LINE_BYTES) n = CAIRN_AGNSS_LINE_BYTES;
    return cairn_agnss_atgds_line(frame + off, n, line, cap);
}
