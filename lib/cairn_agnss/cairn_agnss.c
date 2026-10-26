/* Assisted GNSS, the portable half. See cairn_agnss.h. */

#include "cairn_agnss.h"

#include <stdio.h>
#include <string.h>

#define UBX_SYNC1 0xB5u
#define UBX_SYNC2 0x62u

#define CLS_MGA 0x13u
#define CLS_MON 0x0Au
#define CLS_SEC 0x27u
#define ID_MGA_ANO 0x20u
#define ID_MGA_INI 0x40u
#define ID_MON_VER 0x04u
#define ID_SEC_UNIQID 0x03u

const uint8_t CAIRN_UBX_POLL_SEC_UNIQID[8] = { 0xB5, 0x62, 0x27, 0x03, 0x00, 0x00, 0x2A, 0xA5 };
const uint8_t CAIRN_UBX_POLL_MON_VER[8]    = { 0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34 };

/* ── UBX framing ────────────────────────────────────────────────────────── */

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

/* A whole valid frame at buf[off]? Fills out. */
static bool frame_at(const uint8_t *buf, size_t len, size_t off, cairn_ubx_t *out)
{
    if (off > len || len - off < CAIRN_UBX_OVERHEAD) return false;
    const uint8_t *p = buf + off;
    if (p[0] != UBX_SYNC1 || p[1] != UBX_SYNC2) return false;
    size_t n = (size_t)p[4] | ((size_t)p[5] << 8);
    if (len - off < n + CAIRN_UBX_OVERHEAD) return false;
    uint8_t a, b;
    cairn_ubx_checksum(p + 2, n + 4u, &a, &b);
    if (p[6 + n] != a || p[7 + n] != b) return false;
    out->cls       = p[2];
    out->id        = p[3];
    out->len       = (uint16_t)n;
    out->payload   = p + 6;
    out->frame     = p;
    out->frame_len = n + CAIRN_UBX_OVERHEAD;
    return true;
}

int cairn_ubx_next(const uint8_t *buf, size_t len, size_t *off, cairn_ubx_t *out)
{
    if (*off >= len) return 0;
    if (!frame_at(buf, len, *off, out)) return -1;
    *off += out->frame_len;
    return 1;
}

bool cairn_ubx_find(const uint8_t *buf, size_t len, size_t *off, cairn_ubx_t *out)
{
    for (size_t i = *off; i + CAIRN_UBX_OVERHEAD <= len; i++) {
        if (buf[i] != UBX_SYNC1) continue;
        if (frame_at(buf, len, i, out)) {
            *off = i + out->frame_len;
            return true;
        }
    }
    *off = len;
    return false;
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

int32_t cairn_agnss_day_of_ms(uint64_t utc_ms)
{
    return (int32_t)(utc_ms / 86400000ull);
}

/* ── a downloaded stream ────────────────────────────────────────────────── */

int32_t cairn_agnss_ano_day(const cairn_ubx_t *f)
{
    /* type, version, svId, gnssId, year since 2000, month, day, ... */
    if (f->cls != CLS_MGA || f->id != ID_MGA_ANO || f->len < 7) return CAIRN_AGNSS_NO_DAY;
    int y = 2000 + f->payload[4], m = f->payload[5], d = f->payload[6];
    if (m < 1 || m > 12 || d < 1 || d > 31) return CAIRN_AGNSS_NO_DAY;
    return cairn_agnss_days_from_civil(y, m, d);
}

static bool is_ini(const cairn_ubx_t *f) { return f->cls == CLS_MGA && f->id == ID_MGA_INI; }

bool cairn_agnss_validate(const uint8_t *buf, size_t len, cairn_agnss_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->first_day = CAIRN_AGNSS_NO_DAY;
    info->last_day  = CAIRN_AGNSS_NO_DAY;
    if (buf == NULL || len == 0 || len > CAIRN_AGNSS_MAX_BYTES) return false;

    size_t      off = 0;
    cairn_ubx_t f;
    int         r;
    bool        only_mga = true;
    while ((r = cairn_ubx_next(buf, len, &off, &f)) == 1) {
        info->frames++;
        info->bytes += f.frame_len;
        if (f.cls != CLS_MGA) { only_mga = false; continue; }
        if (is_ini(&f)) { info->ini++; continue; }
        int32_t day = cairn_agnss_ano_day(&f);
        if (day != CAIRN_AGNSS_NO_DAY) {
            info->ano++;
            if (info->first_day == CAIRN_AGNSS_NO_DAY || day < info->first_day) info->first_day = day;
            if (info->last_day == CAIRN_AGNSS_NO_DAY || day > info->last_day) info->last_day = day;
        } else if (f.id == ID_MGA_ANO) {
            only_mga = false;   /* an ANO with an impossible date */
        } else {
            info->almanac++;
        }
    }
    return r == 0 && only_mga && info->ano > 0 && info->ini == 0;
}

bool cairn_agnss_wanted(const cairn_ubx_t *f, cairn_agnss_pick_t pick, int32_t lo, int32_t hi)
{
    switch (pick) {
    case CAIRN_AGNSS_PICK_ALL:
        return true;
    case CAIRN_AGNSS_PICK_ANO: {
        int32_t day = cairn_agnss_ano_day(f);
        return day != CAIRN_AGNSS_NO_DAY && day >= lo && day <= hi;
    }
    case CAIRN_AGNSS_PICK_ALMANAC:
        return f->cls == CLS_MGA && f->id != ID_MGA_ANO && f->id != ID_MGA_INI;
    }
    return false;
}

void cairn_agnss_count(const uint8_t *buf, size_t len, cairn_agnss_pick_t pick,
                       int32_t lo, int32_t hi, uint32_t *msgs, uint32_t *bytes)
{
    *msgs = 0;
    *bytes = 0;
    size_t off = 0;
    cairn_ubx_t f;
    while (cairn_ubx_next(buf, len, &off, &f) == 1) {
        if (!cairn_agnss_wanted(&f, pick, lo, hi)) continue;
        (*msgs)++;
        *bytes += (uint32_t)f.frame_len;
    }
}

void cairn_agnss_day_window(uint64_t utc_ms, uint32_t acc_ms, int32_t *lo, int32_t *hi)
{
    uint64_t early = utc_ms > acc_ms ? utc_ms - acc_ms : 0;
    *lo = cairn_agnss_day_of_ms(early);
    *hi = cairn_agnss_day_of_ms(utc_ms + acc_ms);
}

bool cairn_agnss_ano_window(const uint8_t *buf, size_t len, int32_t lo, int32_t hi,
                            int32_t *out_lo, int32_t *out_hi)
{
    /* Which days around the window does the stream carry? */
    bool has_in = false, has_before = false, has_after = false;
    size_t off = 0;
    cairn_ubx_t f;
    while (cairn_ubx_next(buf, len, &off, &f) == 1) {
        int32_t day = cairn_agnss_ano_day(&f);
        if (day == CAIRN_AGNSS_NO_DAY) continue;
        if (day >= lo && day <= hi) has_in = true;
        else if (day == lo - 1) has_before = true;
        else if (day == hi + 1) has_after = true;
    }
    if (has_in) { *out_lo = lo; *out_hi = hi; return true; }
    /* Nothing for the window itself: the nearest neighbouring day, later first
     * (a copy whose first day is tomorrow is a copy fetched near midnight). */
    if (has_after)  { *out_lo = *out_hi = hi + 1; return true; }
    if (has_before) { *out_lo = *out_hi = lo - 1; return true; }
    return false;
}

/* ── messages the device builds ─────────────────────────────────────────── */

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
    /* UBX-MGA-INI-TIME_UTC, 24 bytes: type 0x10, version 0, ref 0 (on receipt of
     * this message), leapSecs -128 (unknown), year u16, month, day, hour, minute,
     * second, reserved, ns u32, tAccS u16, reserved u16, tAccNs u32. */
    uint8_t p[24];
    memset(p, 0, sizeof(p));
    int32_t days = cairn_agnss_day_of_ms(utc_ms);
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

/* ── pacing ─────────────────────────────────────────────────────────────── */

void cairn_agnss_pacer_start(cairn_agnss_pacer_t *p, cairn_agnss_pick_t pick,
                             int32_t lo, int32_t hi, uint32_t now_ms)
{
    memset(p, 0, sizeof(*p));
    p->pick    = pick;
    p->lo      = lo;
    p->hi      = hi;
    p->next_ms = now_ms;
}

size_t cairn_agnss_pacer_next(cairn_agnss_pacer_t *p, const uint8_t *buf, size_t len,
                              uint32_t now_ms, char *line, size_t cap)
{
    if (p->done) return 0;
    if ((int32_t)(now_ms - p->next_ms) < 0) return 0;

    cairn_ubx_t f;
    if (p->in_frame) {
        if (!frame_at(buf, len, p->off, &f)) { p->done = p->malformed = true; return 0; }
    } else {
        /* The next wanted frame. */
        for (;;) {
            size_t at = p->off;
            int r = cairn_ubx_next(buf, len, &at, &f);
            if (r == 0) { p->done = true; return 0; }
            if (r < 0)  { p->done = p->malformed = true; return 0; }
            if (cairn_agnss_wanted(&f, p->pick, p->lo, p->hi)) break;
            p->off = at;
        }
        p->in_frame = true;
        p->pos = 0;
    }

    size_t left = f.frame_len - p->pos;
    size_t n = left > CAIRN_AGNSS_LINE_BYTES ? CAIRN_AGNSS_LINE_BYTES : left;
    size_t w = cairn_agnss_atgds_line(f.frame + p->pos, n, line, cap);
    if (w == 0) { p->done = true; return 0; }

    p->pos   += n;
    p->bytes += (uint32_t)n;
    p->lines++;
    p->next_ms = now_ms + CAIRN_AGNSS_LINE_INTERVAL_MS;
    if (p->pos >= f.frame_len) {
        p->msgs++;
        p->off += f.frame_len;
        p->in_frame = false;
    }
    return w;
}

/* ── the receiver's identity ────────────────────────────────────────────── */

static bool is_sec_reply(const cairn_ubx_t *f)
{
    return f->cls == CLS_SEC && f->id == ID_SEC_UNIQID && (f->len == 9 || f->len == 10);
}

static bool is_mon_reply(const cairn_ubx_t *f)
{
    return f->cls == CLS_MON && f->id == ID_MON_VER && f->len >= 40 && (f->len - 40) % 30 == 0 &&
           f->frame_len <= CAIRN_AGNSS_MONVER_MAX;
}

int cairn_agnss_identity_scan(const uint8_t *buf, size_t len, cairn_agnss_identity_t *id)
{
    int taken = 0;
    size_t off = 0;
    cairn_ubx_t f;
    while (cairn_ubx_find(buf, len, &off, &f)) {
        if (is_sec_reply(&f)) {
            memcpy(id->sec, f.frame, f.frame_len);
            id->sec_len = (uint8_t)f.frame_len;
            taken++;
        } else if (is_mon_reply(&f)) {
            memcpy(id->mon, f.frame, f.frame_len);
            id->mon_len = (uint16_t)f.frame_len;
            taken++;
        }
    }
    return taken;
}

bool cairn_agnss_identity_complete(const cairn_agnss_identity_t *id)
{
    return id->sec_len > 0 && id->mon_len > 0;
}

static size_t put_hex(char *out, size_t cap, size_t w, const uint8_t *b, size_t n)
{
    static const char hexd[] = "0123456789ABCDEF";
    if (w + 2 * n >= cap) return 0;
    for (size_t i = 0; i < n; i++) {
        out[w++] = hexd[b[i] >> 4];
        out[w++] = hexd[b[i] & 0xF];
    }
    out[w] = '\0';
    return w;
}

static size_t put_str(char *out, size_t cap, size_t w, const char *s)
{
    size_t n = strlen(s);
    if (w + n >= cap) return 0;
    memcpy(out + w, s, n + 1);
    return w + n;
}

size_t cairn_agnss_identity_json(const cairn_agnss_identity_t *id, char *out, size_t cap)
{
    if (!cairn_agnss_identity_complete(id) || cap == 0) return 0;
    size_t w = 0;
    if (!(w = put_str(out, cap, w, "{\"sec_uniqid\":\""))) return 0;
    if (!(w = put_hex(out, cap, w, id->sec, id->sec_len))) return 0;
    if (!(w = put_str(out, cap, w, "\",\"mon_ver\":\""))) return 0;
    if (!(w = put_hex(out, cap, w, id->mon, id->mon_len))) return 0;
    if (!(w = put_str(out, cap, w, "\"}"))) return 0;
    return w;
}

/* A fixed-width, NUL-padded string field, trimmed. */
static void field(const uint8_t *p, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i < n && p[i] != 0 && k + 1 < cap; i++) {
        char c = (char)p[i];
        out[k++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

size_t cairn_agnss_identity_describe(const cairn_agnss_identity_t *id, char *out, size_t cap)
{
    if (cap == 0) return 0;
    out[0] = '\0';
    size_t w = 0;
    char tmp[40];
    if (id->sec_len > CAIRN_UBX_OVERHEAD + 4) {
        const uint8_t *uid = id->sec + 6 + 4;
        size_t n = (size_t)id->sec_len - CAIRN_UBX_OVERHEAD - 4;
        if (!(w = put_str(out, cap, w, "id="))) return 0;
        static const char hexd[] = "0123456789abcdef";
        for (size_t i = 0; i < n && w + 3 < cap; i++) {
            out[w++] = hexd[uid[i] >> 4];
            out[w++] = hexd[uid[i] & 0xF];
        }
        out[w] = '\0';
    }
    if (id->mon_len >= CAIRN_UBX_OVERHEAD + 40) {
        const uint8_t *p = id->mon + 6;
        size_t plen = (size_t)id->mon_len - CAIRN_UBX_OVERHEAD;
        field(p, 30, tmp, sizeof(tmp));
        snprintf(out + w, cap - w, "%ssw=%s", w ? " " : "", tmp);
        w = strlen(out);
        field(p + 30, 10, tmp, sizeof(tmp));
        snprintf(out + w, cap - w, " hw=%s", tmp);
        w = strlen(out);
        for (size_t o = 40; o + 30 <= plen; o += 30) {
            field(p + o, 30, tmp, sizeof(tmp));
            if (tmp[0] == '\0') continue;
            snprintf(out + w, cap - w, " %s", tmp);
            w = strlen(out);
        }
    }
    return w;
}

bool cairn_agnss_identity_decode(const uint8_t *buf, size_t len, cairn_agnss_identity_t *id)
{
    memset(id, 0, sizeof(*id));
    size_t off = 0;
    cairn_ubx_t f;
    int r;
    while ((r = cairn_ubx_next(buf, len, &off, &f)) == 1) {
        if (is_sec_reply(&f)) {
            memcpy(id->sec, f.frame, f.frame_len);
            id->sec_len = (uint8_t)f.frame_len;
        } else if (is_mon_reply(&f)) {
            memcpy(id->mon, f.frame, f.frame_len);
            id->mon_len = (uint16_t)f.frame_len;
        } else {
            return false;
        }
    }
    return r == 0 && cairn_agnss_identity_complete(id);
}

/* ── when to ask the server ─────────────────────────────────────────────── */

bool cairn_agnss_fetch_due(bool have_copy, const cairn_agnss_info_t *info, int32_t today,
                           bool checked, uint32_t since_check_ms)
{
    if (checked && since_check_ms < CAIRN_AGNSS_MIN_RECHECK_MS) return false;
    if (!have_copy || !checked) return true;
    if (since_check_ms >= CAIRN_AGNSS_RECHECK_MS) return true;
    if (today != CAIRN_AGNSS_NO_DAY && info != NULL && info->last_day != CAIRN_AGNSS_NO_DAY &&
        info->last_day - today < CAIRN_AGNSS_AHEAD_DAYS)
        return true;
    return false;
}
