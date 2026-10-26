#include "cairn_gnss.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── the plausibility gate ────────────────────────────────────────────────── */

void cairn_gnss_gate_reset(cairn_gnss_gate_t *g)
{
    memset(g, 0, sizeof(*g));
}

const char *cairn_gnss_verdict_name(cairn_gnss_verdict_t v)
{
    switch (v) {
    case CAIRN_GNSS_ACCEPT:          return "accept";
    case CAIRN_GNSS_REJECT_SETTLING: return "settling";
    case CAIRN_GNSS_REJECT_OBD:      return "obd";
    case CAIRN_GNSS_REJECT_STILL:    return "still";
    case CAIRN_GNSS_REJECT_ACCEL:    return "accel";
    case CAIRN_GNSS_REJECT_FEW_SATS: return "few-sats";
    default:                         return "?";
    }
}

cairn_gnss_verdict_t cairn_gnss_gate(cairn_gnss_gate_t *g, const cairn_gnss_sample_t *s,
                                     uint32_t fix_ms, const cairn_gnss_context_t *ctx)
{
    cairn_gnss_verdict_t v = CAIRN_GNSS_ACCEPT;
    const uint32_t speed  = s->speed_cmps;
    const bool     moving = speed >= CAIRN_GNSS_MOVING_CMPS;

    /* A receiver coming back from a silence has to show enough of the sky first. */
    if (!g->have_accepted || (int32_t)(fix_ms - g->accepted_ms) > (int32_t)CAIRN_GNSS_SETTLE_GAP_MS) {
        if (s->sats_used == CAIRN_U8_UNKNOWN || s->sats_used < CAIRN_GNSS_SETTLE_MIN_SATS)
            v = CAIRN_GNSS_REJECT_SETTLING;
    }

    /* Under a roof the receiver keeps reporting, from too little of the sky. */
    if (v == CAIRN_GNSS_ACCEPT && s->sats_used != CAIRN_U8_UNKNOWN &&
        s->sats_used < CAIRN_GNSS_SETTLE_MIN_SATS)
        v = CAIRN_GNSS_REJECT_FEW_SATS;

    /* At any speed: the garage spur peaked at 7.8 m/s, under the old 8 m/s floor. The
     * margin covers a receiver's jitter at a standstill. */
    if (v == CAIRN_GNSS_ACCEPT && ctx != NULL && ctx->obd_valid &&
        ctx->obd_age_ms <= CAIRN_GNSS_OBD_FRESH_MS && ctx->obd_speed_kph >= 0) {
        /* kph to cm/s is x 1000/36. */
        uint32_t obd_cmps = (uint32_t)ctx->obd_speed_kph * 1000u / 36u;
        uint32_t allowed  = obd_cmps + CAIRN_GNSS_MAX_ACCEL_CMPS2 * ctx->obd_age_ms / 1000u +
                            CAIRN_GNSS_OBD_MARGIN_CMPS;
        if (speed > allowed) v = CAIRN_GNSS_REJECT_OBD;
    }

    if (v == CAIRN_GNSS_ACCEPT && moving && ctx != NULL && ctx->imu_valid &&
        ctx->imu_rms_mg < CAIRN_GNSS_STILL_MG)
        v = CAIRN_GNSS_REJECT_STILL;

    if (v == CAIRN_GNSS_ACCEPT && g->have_accepted) {
        int32_t dt = (int32_t)(fix_ms - g->accepted_ms);
        if (dt > 0 && dt <= (int32_t)CAIRN_GNSS_ACCEL_WINDOW_MS) {
            /* At least a second: two fixes 200 ms apart each carry their own speed
             * noise, and dividing that by 0.2 s would refuse ordinary driving. */
            uint32_t span = dt < 1000 ? 1000u : (uint32_t)dt;
            uint32_t dv   = speed > g->accepted_speed_cmps ? speed - g->accepted_speed_cmps
                                                           : g->accepted_speed_cmps - speed;
            if (dv * 1000u / span > CAIRN_GNSS_MAX_ACCEL_CMPS2) v = CAIRN_GNSS_REJECT_ACCEL;
        }
    }

    if (v == CAIRN_GNSS_ACCEPT) {
        g->have_accepted       = true;
        g->accepted_ms         = fix_ms;
        g->accepted_speed_cmps = (uint16_t)speed;
    } else {
        g->rejected[v]++;
    }
    return v;
}

/* ── NMEA ─────────────────────────────────────────────────────────────────── */

void cairn_nmea_reset(cairn_nmea_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->last_tod_ms = CAIRN_NMEA_NO_TOD;
}

int32_t cairn_nmea_tod_diff(uint32_t a, uint32_t b)
{
    int64_t d = (int64_t)a - (int64_t)b;
    const int64_t day = CAIRN_NMEA_DAY_MS;
    if (d > day / 2) d -= day;
    if (d < -day / 2) d += day;
    return (int32_t)d;
}

static int digits(const char *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

uint32_t cairn_nmea_parse_tod(const char *f)
{
    if (f == NULL || strlen(f) < 6) return CAIRN_NMEA_NO_TOD;
    int h = digits(f, 2), m = digits(f + 2, 2), s = digits(f + 4, 2);
    if (h < 0 || m < 0 || s < 0 || h > 23 || m > 59 || s > 60) return CAIRN_NMEA_NO_TOD;
    uint32_t ms = 0;
    if (f[6] == '.') {
        /* Fractional seconds to milliseconds, however many digits are given. */
        uint32_t scale = 100;
        for (const char *p = f + 7; *p >= '0' && *p <= '9' && scale > 0; p++, scale /= 10)
            ms += (uint32_t)(*p - '0') * scale;
    } else if (f[6] != '\0') {
        return CAIRN_NMEA_NO_TOD;
    }
    return ((uint32_t)h * 3600u + (uint32_t)m * 60u + (uint32_t)s) * 1000u + ms;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

#define MAX_SENTENCE 100
#define MAX_FIELDS   24

/*
 * Copies one sentence ($ to the end of its checksum) into out, NUL-terminated and with
 * the checksum verified. Returns false for a sentence that is cut off, too long, or
 * whose checksum does not match: the buffer is read over a serial link and its last
 * sentence is routinely incomplete.
 */
static bool take_sentence(const char *start, const char *end, char *out, cairn_nmea_state_t *st)
{
    const char *star = NULL;
    uint8_t x = 0;
    for (const char *p = start + 1; p < end; p++) {
        if (*p == '*') { star = p; break; }
        if (*p == '\r' || *p == '\n') break;
        x ^= (uint8_t)*p;
    }
    /* Both checksum digits have to be inside the buffer. */
    if (star == NULL || star + 2 >= end) { st->bad_checksum++; return false; }
    int hi = hexval(star[1]), lo = hexval(star[2]);
    if (hi < 0 || lo < 0 || (uint8_t)(hi * 16 + lo) != x) { st->bad_checksum++; return false; }
    size_t n = (size_t)(star - start);
    if (n >= MAX_SENTENCE) { st->bad_checksum++; return false; }
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

/* Splits "$GNGGA,a,b,,c" in place into fields; field 0 is the talker and type. */
static int split(char *s, char **f)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < MAX_FIELDS; p++) {
        if (*p == ',') { *p = '\0'; f[n++] = p + 1; }
    }
    return n;
}

static bool is_type(const char *tag, const char *type)
{
    /* "$GNGGA", "$GPGGA", "$GLGGA": any talker. */
    return strlen(tag) == 6 && tag[0] == '$' && strcmp(tag + 3, type) == 0;
}

static bool coord(const char *v, const char *hemi, double *out)
{
    if (*v == '\0' || *hemi == '\0') return false;
    double raw = strtod(v, NULL);
    int deg = (int)(raw / 100.0);
    double d = deg + (raw - deg * 100.0) / 60.0;
    if (*hemi == 'S' || *hemi == 'W') d = -d;
    else if (*hemi != 'N' && *hemi != 'E') return false;
    *out = d;
    return true;
}

typedef struct {
    uint32_t tod;
    uint16_t speed_cmps;
    uint16_t heading_cdeg;
} rmc_t;

#define MAX_RMC 16

int cairn_nmea_collect(const char *buf, size_t len, uint32_t ref_tod_ms,
                       cairn_nmea_state_t *st, cairn_nmea_fix_t *out, int max_out)
{
    if (buf == NULL || out == NULL || max_out <= 0) return 0;
    const char *end = buf + len;

    /* Pass 1: every valid RMC, which is where speed and course live. */
    rmc_t rmc[MAX_RMC];
    int n_rmc = 0;
    char line[MAX_SENTENCE];
    char *f[MAX_FIELDS];
    for (const char *p = buf; p < end; p++) {
        if (*p != '$') continue;
        if (!take_sentence(p, end, line, st)) continue;
        int nf = split(line, f);
        if (!is_type(f[0], "RMC") || nf < 9 || n_rmc >= MAX_RMC) continue;
        if (f[2][0] != 'A') continue; /* V: the receiver says the fix is not valid */
        uint32_t tod = cairn_nmea_parse_tod(f[1]);
        if (tod == CAIRN_NMEA_NO_TOD) continue;
        double knots  = f[7][0] ? strtod(f[7], NULL) : -1.0;
        if (knots < 0 || knots > 1000) continue;
        double course = f[8][0] ? strtod(f[8], NULL) : 0.0;
        if (course < 0 || course >= 360) course = 0;
        rmc[n_rmc].tod          = tod;
        rmc[n_rmc].speed_cmps   = (uint16_t)lround(knots * 51.4444);
        rmc[n_rmc].heading_cdeg = (uint16_t)lround(course * 100.0);
        if (rmc[n_rmc].heading_cdeg > 35999) rmc[n_rmc].heading_cdeg = 0;
        n_rmc++;
    }
    /* take_sentence counted the same bad sentences in this pass; count them once. */
    uint32_t bad_after_pass1 = st->bad_checksum;

    /* Pass 2: each GGA with a fix and an RMC of the same instant. */
    int n = 0;
    uint32_t newest = CAIRN_NMEA_NO_TOD;
    for (const char *p = buf; p < end && n < max_out; p++) {
        if (*p != '$') continue;
        if (!take_sentence(p, end, line, st)) continue;
        int nf = split(line, f);
        if (!is_type(f[0], "GGA") || nf < 10) continue;
        uint32_t tod = cairn_nmea_parse_tod(f[1]);
        if (tod == CAIRN_NMEA_NO_TOD) continue;
        int quality = atoi(f[6]);
        if (quality <= 0) continue;
        double lat, lon;
        if (!coord(f[2], f[3], &lat) || !coord(f[4], f[5], &lon)) continue;

        /* Already written in an earlier poll, or out of order within this one. */
        if (st->last_tod_ms != CAIRN_NMEA_NO_TOD && cairn_nmea_tod_diff(tod, st->last_tod_ms) <= 0) {
            st->repeats++;
            continue;
        }
        if (newest != CAIRN_NMEA_NO_TOD && cairn_nmea_tod_diff(tod, newest) <= 0) {
            st->repeats++;
            continue;
        }

        const rmc_t *r = NULL;
        for (int i = 0; i < n_rmc; i++) if (rmc[i].tod == tod) { r = &rmc[i]; break; }
        if (r == NULL) {
            /* GGA has no speed, and the format has no unknown value for one: a fix
             * written with speed 0 says the car was standing still. */
            st->unpaired++;
            continue;
        }

        int sats = atoi(f[7]);
        double hdop = f[8][0] ? strtod(f[8], NULL) : -1.0;
        double alt  = f[9][0] ? strtod(f[9], NULL) : 0.0;

        cairn_nmea_fix_t *o = &out[n];
        memset(o, 0, sizeof(*o));
        cairn_gnss_sample_t *s = &o->sample;
        s->lat_e7       = (int32_t)lround(lat * 1e7);
        s->lon_e7       = (int32_t)lround(lon * 1e7);
        s->alt_cm       = (int32_t)lround(alt * 100.0);
        s->speed_cmps   = r->speed_cmps;
        s->heading_cdeg = r->heading_cdeg;
        s->hdop_e2      = (hdop > 0 && hdop < 655.0) ? (uint16_t)lround(hdop * 100.0) : CAIRN_U16_UNKNOWN;
        s->h_acc_cm     = CAIRN_U16_UNKNOWN;
        s->v_acc_cm     = CAIRN_U16_UNKNOWN;
        s->sats_used    = (sats >= 0 && sats < 255) ? (uint8_t)sats : CAIRN_U8_UNKNOWN;
        s->sats_visible = CAIRN_U8_UNKNOWN;
        /* The same convention as the co-processor's summary (sensors_read_gnss), so
         * one drive does not mix two meanings of the field. */
        s->fix_type     = (sats >= 4 && alt != 0) ? 3 : (sats >= 3 ? 2 : 0);
        s->utc_acc_ms   = CAIRN_U16_UNKNOWN;
        if (s->fix_type == 0 || (s->lat_e7 == 0 && s->lon_e7 == 0)) continue;
        o->tod_ms = tod;
        newest = tod;
        n++;
    }
    st->bad_checksum = bad_after_pass1;
    if (n == 0) return 0;

    uint32_t ref = newest;
    if (ref_tod_ms != CAIRN_NMEA_NO_TOD) {
        int32_t lag = cairn_nmea_tod_diff(ref_tod_ms, newest);
        if (lag > (int32_t)CAIRN_NMEA_STALE_MS) {
            /* The newest fix here is seconds behind the receiver: the co-processor
             * returned a buffer it filled a while ago. Stamping it "now" would put old
             * positions at the current time. */
            st->stale_buffers++;
            return 0;
        }
        if (lag >= 0) ref = ref_tod_ms;
    }
    for (int i = 0; i < n; i++) {
        int32_t age = cairn_nmea_tod_diff(ref, out[i].tod_ms);
        out[i].age_ms = age > 0 ? (uint32_t)age : 0;
    }
    st->last_tod_ms = newest;
    return n;
}
