/* A position from one Wi-Fi scan and the learned anchors. See cairn_wifiloc.h. */

#include "cairn_wifiloc.h"

#include <math.h>
#include <string.h>

#define E7_PER_DEG   10000000.0
#define M_PER_DEG    111320.0          /* the server's figure (internal/tsdb views) */
#define LON_SPAN_E7  3600000000LL
#define PI           3.14159265358979323846

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *cairn_wifiloc_err_name(cairn_wifiloc_err_t e)
{
    switch (e) {
    case CAIRN_WIFILOC_OK:          return "ok";
    case CAIRN_WIFILOC_ERR_SHORT:   return "length does not match the header";
    case CAIRN_WIFILOC_ERR_MAGIC:   return "not a CWA1 table";
    case CAIRN_WIFILOC_ERR_VERSION: return "unknown version";
    case CAIRN_WIFILOC_ERR_RECORD:  return "record size under 16";
    case CAIRN_WIFILOC_ERR_COUNT:   return "more records than the cap";
    case CAIRN_WIFILOC_ERR_ORDER:   return "BSSIDs not strictly ascending";
    case CAIRN_WIFILOC_ERR_RANGE:   return "a position or accuracy out of range";
    }
    return "?";
}

const char *cairn_wifiloc_verdict_name(cairn_wifiloc_verdict_t v)
{
    switch (v) {
    case CAIRN_WIFILOC_LOCATED:    return "located";
    case CAIRN_WIFILOC_NONE_KNOWN: return "no known access point";
    case CAIRN_WIFILOC_ONE_WEAK:   return "one known access point, heard too weakly";
    case CAIRN_WIFILOC_DISAGREE:   return "the known access points disagree";
    }
    return "?";
}

static void record_at(const cairn_wifiloc_table_t *t, uint32_t i, const uint8_t **bssid,
                      cairn_wifiloc_anchor_t *a)
{
    const uint8_t *r = t->records + (size_t)i * t->record_size;
    if (bssid) *bssid = r;
    if (a) {
        a->lat_e7 = (int32_t)rd32(r + 6);
        a->lon_e7 = (int32_t)rd32(r + 10);
        a->acc_m  = rd16(r + 14);
    }
}

cairn_wifiloc_err_t cairn_wifiloc_parse(const uint8_t *buf, size_t len, cairn_wifiloc_table_t *t)
{
    memset(t, 0, sizeof(*t));
    if (buf == NULL || len < CAIRN_WIFILOC_HEADER) return CAIRN_WIFILOC_ERR_SHORT;
    if (memcmp(buf, CAIRN_WIFILOC_MAGIC, 4) != 0) return CAIRN_WIFILOC_ERR_MAGIC;
    if (rd16(buf + 4) != CAIRN_WIFILOC_VERSION) return CAIRN_WIFILOC_ERR_VERSION;
    uint16_t rs = rd16(buf + 6);
    if (rs < CAIRN_WIFILOC_RECORD_MIN) return CAIRN_WIFILOC_ERR_RECORD;
    uint32_t count = rd32(buf + 8);
    if (count > CAIRN_WIFILOC_MAX_RECORDS) return CAIRN_WIFILOC_ERR_COUNT;
    if ((uint64_t)len != CAIRN_WIFILOC_HEADER + (uint64_t)count * rs) return CAIRN_WIFILOC_ERR_SHORT;

    cairn_wifiloc_table_t tt;
    tt.records      = buf + CAIRN_WIFILOC_HEADER;
    tt.count        = count;
    tt.record_size  = rs;
    tt.generated_at = rd32(buf + 12);

    const uint8_t *prev = NULL;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *b;
        cairn_wifiloc_anchor_t a;
        record_at(&tt, i, &b, &a);
        if (prev != NULL && memcmp(prev, b, 6) >= 0) return CAIRN_WIFILOC_ERR_ORDER;
        if (a.lat_e7 < -900000000 || a.lat_e7 > 900000000 ||
            a.lon_e7 < -1800000000 || a.lon_e7 > 1800000000 || a.acc_m == 0)
            return CAIRN_WIFILOC_ERR_RANGE;
        prev = b;
    }
    *t = tt;
    return CAIRN_WIFILOC_OK;
}

bool cairn_wifiloc_lookup(const cairn_wifiloc_table_t *t, const uint8_t bssid[6],
                          cairn_wifiloc_anchor_t *out)
{
    if (t == NULL || t->records == NULL) return false;
    uint32_t lo = 0, hi = t->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *b;
        record_at(t, mid, &b, NULL);
        int c = memcmp(b, bssid, 6);
        if (c == 0) {
            record_at(t, mid, NULL, out);
            return true;
        }
        if (c < 0) lo = mid + 1;
        else       hi = mid;
    }
    return false;
}

/* A longitude difference folded into one turn either way of zero. */
static int64_t dlon_e7(int32_t a, int32_t b)
{
    int64_t d = (int64_t)a - (int64_t)b;
    if (d > LON_SPAN_E7 / 2)  d -= LON_SPAN_E7;
    if (d < -LON_SPAN_E7 / 2) d += LON_SPAN_E7;
    return d;
}

double cairn_wifiloc_distance_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7)
{
    double mean_lat = ((double)lat1_e7 + (double)lat2_e7) / 2.0 / E7_PER_DEG;
    double dy = ((double)lat1_e7 - (double)lat2_e7) / E7_PER_DEG * M_PER_DEG;
    double dx = (double)dlon_e7(lon1_e7, lon2_e7) / E7_PER_DEG * M_PER_DEG * cos(mean_lat * PI / 180.0);
    return sqrt(dx * dx + dy * dy);
}

typedef struct {
    int32_t  lat_e7, lon_e7;
    uint16_t acc_m;
    int8_t   rssi;
    double   w;
    bool     kept;
} match_t;

/* The signal-weighted centre of the kept matches, offsets taken from a reference so a
 * cluster astride the antimeridian does not average to the far side of the earth. */
static void centre(const match_t *m, size_t n, int32_t *lat, int32_t *lon)
{
    int32_t ref_lat = 0, ref_lon = 0;
    bool    have_ref = false;
    double  sw = 0, sy = 0, sx = 0;
    for (size_t i = 0; i < n; i++) {
        if (!m[i].kept) continue;
        if (!have_ref) { ref_lat = m[i].lat_e7; ref_lon = m[i].lon_e7; have_ref = true; }
        sw += m[i].w;
        sy += m[i].w * (double)((int64_t)m[i].lat_e7 - ref_lat);
        sx += m[i].w * (double)dlon_e7(m[i].lon_e7, ref_lon);
    }
    *lat = ref_lat;
    *lon = ref_lon;
    if (sw <= 0) return;
    *lat = (int32_t)llround((double)ref_lat + sy / sw);
    int64_t lo = (int64_t)llround((double)ref_lon + sx / sw);
    if (lo > 1800000000LL)  lo -= LON_SPAN_E7;
    if (lo < -1800000000LL) lo += LON_SPAN_E7;
    *lon = (int32_t)lo;
}

bool cairn_wifiloc_locate(const cairn_wifiloc_table_t *t, const cairn_wifiloc_obs_t *obs,
                          size_t n, cairn_wifiloc_estimate_t *out)
{
    memset(out, 0, sizeof(*out));
    out->verdict = CAIRN_WIFILOC_NONE_KNOWN;

    /* Unique BSSIDs, each at its strongest. */
    cairn_wifiloc_obs_t u[CAIRN_WIFILOC_MAX_OBS];
    size_t nu = 0;
    for (size_t i = 0; obs != NULL && i < n; i++) {
        size_t j = 0;
        while (j < nu && memcmp(u[j].bssid, obs[i].bssid, 6) != 0) j++;
        if (j < nu) {
            if (obs[i].rssi_dbm > u[j].rssi_dbm) u[j].rssi_dbm = obs[i].rssi_dbm;
        } else if (nu < CAIRN_WIFILOC_MAX_OBS) {
            u[nu++] = obs[i];
        }
    }
    out->heard = (uint8_t)nu;

    match_t m[CAIRN_WIFILOC_MAX_OBS];
    size_t  k = 0;
    for (size_t i = 0; i < nu; i++) {
        cairn_wifiloc_anchor_t a;
        if (!cairn_wifiloc_lookup(t, u[i].bssid, &a)) continue;
        m[k].lat_e7 = a.lat_e7;
        m[k].lon_e7 = a.lon_e7;
        m[k].acc_m  = a.acc_m;
        m[k].rssi   = u[i].rssi_dbm;
        m[k].w      = pow(10.0, (double)u[i].rssi_dbm / 20.0);
        m[k].kept   = true;
        k++;
    }
    out->known = (uint8_t)k;
    if (k == 0) return false;

    if (k == 1) {
        if (m[0].rssi < CAIRN_WIFILOC_SINGLE_MIN_DBM) {
            out->verdict = CAIRN_WIFILOC_ONE_WEAK;
            return false;
        }
        out->verdict = CAIRN_WIFILOC_LOCATED;
        out->lat_e7  = m[0].lat_e7;
        out->lon_e7  = m[0].lon_e7;
        out->used    = 1;
        out->rms_m   = 0;
        out->acc_m   = m[0].acc_m > CAIRN_WIFILOC_MIN_ACC_M ? m[0].acc_m : CAIRN_WIFILOC_MIN_ACC_M;
        return true;
    }

    /*
     * Two or more. First the place most of them agree on, unweighted, so one strong
     * stray (a hotspot on the next car, an access point the table has wrong) cannot
     * drag the weighted centre to itself and push the true ones out: the seed is the
     * match with the most others within twice the agreement radius (two points each
     * within 300 m of one centre are at most 600 m apart), the strongest on a tie.
     */
    const double pair = 2.0 * CAIRN_WIFILOC_AGREE_M;
    size_t seed = 0, best = 0;
    for (size_t i = 0; i < k; i++) {
        size_t near = 0;
        for (size_t j = 0; j < k; j++)
            if (cairn_wifiloc_distance_m(m[i].lat_e7, m[i].lon_e7, m[j].lat_e7, m[j].lon_e7) <= pair) near++;
        if (near > best || (near == best && m[i].rssi > m[seed].rssi)) { best = near; seed = i; }
    }
    size_t kept = 0;
    for (size_t i = 0; i < k; i++) {
        m[i].kept = cairn_wifiloc_distance_m(m[i].lat_e7, m[i].lon_e7, m[seed].lat_e7, m[seed].lon_e7) <= pair;
        if (m[i].kept) kept++;
    }

    /* Then the weighted centre of those, dropping the farthest while any is more than
     * the agreement radius from it. */
    int32_t clat = 0, clon = 0;
    for (;;) {
        centre(m, k, &clat, &clon);
        size_t far = k;
        double far_d = 0;
        for (size_t i = 0; i < k; i++) {
            if (!m[i].kept) continue;
            double d = cairn_wifiloc_distance_m(m[i].lat_e7, m[i].lon_e7, clat, clon);
            if (d > far_d) { far_d = d; far = i; }
        }
        if (far_d <= CAIRN_WIFILOC_AGREE_M || kept < 2 || far == k) break;
        m[far].kept = false;
        kept--;
    }

    /* At least two, and a strict majority of everything known: two access points here
     * and two a town away is two places, not one. */
    if (kept < 2 || kept * 2 <= k) {
        out->verdict = CAIRN_WIFILOC_DISAGREE;
        out->used = (uint8_t)kept;
        return false;
    }

    double   ss = 0;
    uint32_t worst = 0;
    for (size_t i = 0; i < k; i++) {
        if (!m[i].kept) continue;
        double d = cairn_wifiloc_distance_m(m[i].lat_e7, m[i].lon_e7, clat, clon);
        ss += d * d;
        if (m[i].acc_m > worst) worst = m[i].acc_m;
    }
    uint32_t rms = (uint32_t)ceil(sqrt(ss / (double)kept));
    uint32_t acc = CAIRN_WIFILOC_MIN_ACC_M;
    if (worst > acc) acc = worst;
    if (rms > acc) acc = rms;

    out->verdict = CAIRN_WIFILOC_LOCATED;
    out->lat_e7  = clat;
    out->lon_e7  = clon;
    out->used    = (uint8_t)kept;
    out->rms_m   = rms;
    out->acc_m   = acc;
    return true;
}

void cairn_wifiloc_hint(const cairn_wifiloc_estimate_t *e, bool have_last_fix,
                        int32_t last_lat_e7, int32_t last_lon_e7, int32_t last_alt_cm,
                        int32_t *alt_cm, uint32_t *acc_cm)
{
    uint32_t v = CAIRN_WIFILOC_ALT_NONE_ACC_M;
    *alt_cm = 0;
    if (have_last_fix && !(last_lat_e7 == 0 && last_lon_e7 == 0)) {
        double d = cairn_wifiloc_distance_m(e->lat_e7, e->lon_e7, last_lat_e7, last_lon_e7);
        if (d <= CAIRN_WIFILOC_ALT_NEAR_M) {
            *alt_cm = last_alt_cm;
            v = CAIRN_WIFILOC_ALT_NEAR_ACC_M;
        } else if (d <= CAIRN_WIFILOC_ALT_FAR_M) {
            *alt_cm = last_alt_cm;
            v = CAIRN_WIFILOC_ALT_FAR_ACC_M;
        }
    }
    double h = (double)e->acc_m;
    double m3 = ceil(sqrt(h * h + (double)v * (double)v));
    *acc_cm = (uint32_t)(m3 * 100.0);
}

bool cairn_wifiloc_fetch_due(bool have_copy, uint32_t last_check_s, uint32_t now_s,
                             bool attempted, uint32_t since_attempt_ms)
{
    if (attempted && since_attempt_ms < CAIRN_WIFILOC_MIN_RETRY_MS) return false;
    if (!have_copy) return true;
    if (now_s != 0 && last_check_s != 0) {
        /* A clock that went back an hour or more is a clock that was wrong one of the
         * two times: ask rather than trust either. */
        if ((uint64_t)now_s + 3600u < last_check_s) return true;
        return now_s - last_check_s >= CAIRN_WIFILOC_RECHECK_S;
    }
    return !attempted || since_attempt_ms >= CAIRN_WIFILOC_RECHECK_S * 1000u;
}
