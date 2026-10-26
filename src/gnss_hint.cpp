/* Position and time hints for the receiver, and its first fix. See gnss_hint.h. */

#include "gnss_hint.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cairn_agnss.h"
#include "cairn_fs.h"
#include "cairn_kv.h"
#include "cairn_log.h"
#include "cairn_platform.h"
#include "cairn_wifiloc.h"
#include "config.h"
#include "sensors.h"

static const char *TAG = "HINT";

#define PATH_ANCHORS     CAIRN_DIR_STATE "/wifi-anchors.bin"
#define PATH_ANCHORS_TMP CAIRN_DIR_STATE "/wifi-anchors.tmp"
#define ROUTE_ANCHORS    "/api/v2/wifi-anchors"

/* Unix seconds of the last successful check (200 or 304), when the clock was known. */
#define KV_ANCHORS_CHECK "wanchor_chk"

#define CARD_MAX (CAIRN_WIFILOC_MAX_BYTES + CAIRN_WIFILOC_CARD_OVERHEAD)

/* Why no Wi-Fi hint went, for the first-fix line. Short tokens: it is meant to be grepped
 * and compared across drives. */
static const char *verdict_token(cairn_wifiloc_verdict_t v)
{
    switch (v) {
    case CAIRN_WIFILOC_NONE_KNOWN: return "none-known";
    case CAIRN_WIFILOC_ONE_WEAK:   return "one-weak";
    case CAIRN_WIFILOC_DISAGREE:   return "disagree";
    default:                       return "located";
    }
}

static const char *source_name(uint8_t src)
{
    switch (src) {
    case CAIRN_TIME_SRC_PHONE:         return "phone";
    case CAIRN_TIME_SRC_MODEM_NETWORK: return "network";
    case CAIRN_TIME_SRC_GNSS:          return "gnss";
    default:                           return "other";
    }
}

/*
 * Everything this module keeps, in one PSRAM allocation: dram0 had ~500 bytes to spare
 * when the gnss-assist branch measured it (2026-10-25), and a static copy of this would
 * be a third of that for state touched a few times a minute.
 */
typedef struct {
    /* The table: the server's bytes, in PSRAM, and a view over them. */
    uint8_t              *blob;
    size_t                blob_len;
    cairn_wifiloc_table_t table;
    uint8_t               card_key[32];
    bool                  have_key;

    /* Fetching. */
    bool     attempted;
    uint32_t attempted_ms;

    /* The latest time observation of any source, for "is the copy a day old". */
    bool     have_clock;
    uint64_t clock_utc_ms;
    uint32_t clock_acc_ms;
    uint32_t clock_mono_ms;

    /* The latest phone or network time, the time hint's source. */
    bool     have_aid;
    uint64_t aid_utc_ms;
    uint32_t aid_acc_ms;
    uint32_t aid_mono_ms;
    uint32_t aid_standby_ms;   /* lifecycle's standby total when it was observed */
    uint8_t  aid_source;

    /* The current receiver start. */
    uint32_t gen;
    uint32_t started_ms;
    bool     lastfix_hint;
    bool     fixed;

    uint8_t  scans;            /* scans without a fix since the start */
    bool     wifi_sent;
    bool     wifi_logged_miss;
    const char *wifi_why;      /* when not sent: a token */
    uint32_t wifi_at_ms;
    uint32_t wifi_acc_m;       /* 3-D, as sent */
    uint32_t wifi_hacc_m;      /* horizontal */
    uint8_t  wifi_used, wifi_known, wifi_heard;
    int32_t  wifi_from_lastfix_m;   /* -1: no last fix */

    bool     time_sent;
    const char *time_why;
    uint32_t time_at_ms;
    uint32_t time_acc_ms;
    uint8_t  time_source;
} hint_t;

static hint_t *S;

/* ── the card ──────────────────────────────────────────────────────────────── */

static uint8_t *read_card(const char *path, size_t *len)
{
    uint64_t size = 0;
    *len = 0;
    if (!cairn_fs_file_size(path, &size) || size == 0 || size > CARD_MAX) return nullptr;
    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) return nullptr;
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    size_t got = f ? cairn_fs_read(f, buf, (size_t)size) : 0;
    if (f) cairn_fs_close(f);
    if (got != (size_t)size) { heap_caps_free(buf); return nullptr; }
    *len = got;
    return buf;
}

/* Beside the final name, then renamed into place: a reset mid-write leaves the old copy
 * or the new one. cairn_fs_rename refuses to overwrite (a sealed bundle is never
 * replaced), so the old copy goes first, and a .tmp alone at boot is a write that
 * stopped between the two. */
static bool write_atomic(const uint8_t *buf, size_t len)
{
    cairn_fs_remove(PATH_ANCHORS_TMP);
    cairn_file_t *f = cairn_fs_open(PATH_ANCHORS_TMP, CAIRN_FS_WRITE);
    if (f == nullptr) return false;
    bool ok = cairn_fs_write(f, buf, len) == len && cairn_fs_flush(f);
    cairn_fs_close(f);
    uint64_t size = 0;
    ok = ok && cairn_fs_file_size(PATH_ANCHORS_TMP, &size) && size == len;
    if (!ok) { cairn_fs_remove(PATH_ANCHORS_TMP); return false; }
    if (cairn_fs_exists(PATH_ANCHORS) && !cairn_fs_remove(PATH_ANCHORS)) return false;
    return cairn_fs_rename(PATH_ANCHORS_TMP, PATH_ANCHORS);
}

/* The card copy opened into a fresh PSRAM buffer and checked; nullptr if it is not one. */
static uint8_t *open_card_copy(const uint8_t *sealed, size_t len, size_t *pt_len,
                               cairn_wifiloc_table_t *t, const char **why)
{
    *pt_len = 0;
    *why = "unreadable";
    if (len < CAIRN_WIFILOC_CARD_OVERHEAD) return nullptr;
    size_t   cap = len - CAIRN_WIFILOC_CARD_OVERHEAD;
    uint8_t *pt  = (uint8_t *)heap_caps_malloc(cap ? cap : 1, MALLOC_CAP_SPIRAM);
    if (pt == nullptr) { *why = "no PSRAM"; return nullptr; }
    if (!cairn_wifiloc_card_open(S->card_key, sealed, len, pt, cap, pt_len)) {
        *why = "does not open under this device's key";
        heap_caps_free(pt);
        return nullptr;
    }
    cairn_wifiloc_err_t e = cairn_wifiloc_parse(pt, *pt_len, t);
    if (e != CAIRN_WIFILOC_OK) {
        *why = cairn_wifiloc_err_name(e);
        heap_caps_free(pt);
        return nullptr;
    }
    return pt;
}

static void day_of(uint32_t unix_s, char out[11])
{
    if (unix_s == 0) { snprintf(out, 11, "never"); return; }
    int y, m, d;
    cairn_agnss_civil_from_days((int32_t)(unix_s / 86400u), &y, &m, &d);
    snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}

static void adopt(uint8_t *blob, size_t len, const cairn_wifiloc_table_t *t)
{
    if (S->blob != nullptr) heap_caps_free(S->blob);
    S->blob     = blob;
    S->blob_len = len;
    S->table    = *t;
}

void gnss_hint_begin(const cairn_storage_identity_t *storage)
{
    if (S == nullptr) S = (hint_t *)heap_caps_calloc(1, sizeof(hint_t), MALLOC_CAP_SPIRAM);
    if (S == nullptr) {
        CAIRN_LOGW(TAG, "no PSRAM for %u bytes of hint state: no Wi-Fi or time hints this boot, "
                        "no first-fix line", (unsigned)sizeof(hint_t));
        return;
    }
    S->wifi_from_lastfix_m = -1;

    if (storage != nullptr) {
        cairn_wifiloc_card_key(storage->root_key, S->card_key);
        S->have_key = true;
    }

    /* A .tmp beside a missing copy is a write that stopped between removing the old
     * copy and renaming the new one: finish it if it opens. */
    if (cairn_fs_exists(PATH_ANCHORS_TMP)) {
        if (!cairn_fs_exists(PATH_ANCHORS) && S->have_key) {
            size_t len = 0, n = 0;
            uint8_t *b = read_card(PATH_ANCHORS_TMP, &len);
            cairn_wifiloc_table_t t;
            const char *why;
            uint8_t *pt = b ? open_card_copy(b, len, &n, &t, &why) : nullptr;
            const bool whole = pt != nullptr;
            if (b) heap_caps_free(b);
            if (pt) heap_caps_free(pt);
            if (whole && cairn_fs_rename(PATH_ANCHORS_TMP, PATH_ANCHORS))
                CAIRN_LOGI(TAG, "finished an interrupted write of the Wi-Fi anchors");
        }
        cairn_fs_remove(PATH_ANCHORS_TMP);
    }

    if (!S->have_key || !cairn_fs_exists(PATH_ANCHORS)) {
        CAIRN_LOGI(TAG, "no Wi-Fi anchors on the card yet; the next upload session asks for them");
        return;
    }
    size_t len = 0, n = 0;
    uint8_t *sealed = read_card(PATH_ANCHORS, &len);
    cairn_wifiloc_table_t t;
    const char *why = "unreadable";
    uint8_t *pt = sealed ? open_card_copy(sealed, len, &n, &t, &why) : nullptr;
    if (sealed) heap_caps_free(sealed);
    if (pt == nullptr) {
        CAIRN_LOGW(TAG, "the Wi-Fi anchors on the card are not usable (%s); removed, and "
                        "the next upload session downloads them again", why);
        cairn_fs_remove(PATH_ANCHORS);
        return;
    }
    adopt(pt, n, &t);
    char day[11];
    day_of(t.generated_at, day);
    CAIRN_LOGI(TAG, "Wi-Fi anchors on the card: %u access points, learned up to %s, %u bytes in PSRAM",
               (unsigned)t.count, day, (unsigned)n);
}

/* ── the receiver's starts ─────────────────────────────────────────────────── */

/* Notice a new receiver start; true while there is one to hint. */
static bool refresh(void)
{
    if (S == nullptr) return false;
    uint32_t started = 0;
    bool     lastfix = false;
    uint32_t gen = sensors_gnss_generation(&started, &lastfix);
    if (gen != S->gen) {
        S->gen              = gen;
        S->started_ms       = started;
        S->lastfix_hint     = lastfix;
        S->fixed            = false;
        S->scans            = 0;
        S->wifi_sent        = false;
        S->wifi_logged_miss = false;
        S->wifi_why         = "no-scan";
        S->time_sent        = false;
        S->time_why         = "none";
    }
    return S->gen != 0;
}

/* The time hint, if one is owed to this start and still honest. */
static void try_time(uint32_t standby_total_ms)
{
    if (!refresh() || S->fixed || S->time_sent || !S->have_aid) return;
    if (!CAIRN_GNSS_HINTS) { S->time_why = "off"; return; }
    if (!sensors_gnss_assistable()) return;

    uint32_t now     = millis();
    uint32_t elapsed = now - S->aid_mono_ms;
    uint32_t asleep  = standby_total_ms - S->aid_standby_ms;
    uint32_t acc     = cairn_agnss_time_acc(S->aid_acc_ms, elapsed, asleep);
    if (acc > CAIRN_AGNSS_TIME_MAX_ACC_MS) {
        S->time_why = "too-loose";
        return;
    }
    uint8_t frame[32];
    if (cairn_agnss_time_utc_frame(S->aid_utc_ms + elapsed, acc, frame) != sizeof(frame)) return;
    if (!sensors_gnss_send_ubx(frame, sizeof(frame))) return;

    S->time_sent   = true;
    S->time_at_ms  = now;
    S->time_acc_ms = acc;
    S->time_source = S->aid_source;
    CAIRN_LOGI(TAG, "time hint sent: %s time +/- %u ms, %u ms after receiver start %u (MGA-INI-TIME_UTC; "
                    "unacknowledged)",
               source_name(S->aid_source), (unsigned)acc, (unsigned)(now - S->started_ms), (unsigned)S->gen);
}

void gnss_hint_tick(uint32_t standby_total_ms)
{
    try_time(standby_total_ms);
}

void gnss_hint_note_time(uint64_t utc_ms, uint32_t acc_ms, uint8_t source, uint32_t mono_ms,
                         uint32_t standby_total_ms)
{
    if (S == nullptr) return;
    S->have_clock    = true;
    S->clock_utc_ms  = utc_ms;
    S->clock_acc_ms  = acc_ms;
    S->clock_mono_ms = mono_ms;

    /* The receiver's own time is no hint to itself. */
    if (source != CAIRN_TIME_SRC_PHONE && source != CAIRN_TIME_SRC_MODEM_NETWORK) return;
    S->have_aid       = true;
    S->aid_utc_ms     = utc_ms;
    S->aid_acc_ms     = acc_ms;
    S->aid_mono_ms    = mono_ms;
    S->aid_standby_ms = standby_total_ms;
    S->aid_source     = source;
    try_time(standby_total_ms);
}

/* ── the Wi-Fi position ────────────────────────────────────────────────────── */

void gnss_hint_on_scan(const cairn_wifi_scan_t *scan, bool learning)
{
    /* A learning scan is taken beside a fix: nothing to hint. */
    if (learning || scan == nullptr || !refresh() || S->fixed || S->wifi_sent) return;
    if (S->scans < 0xFF) S->scans++;

    if (!CAIRN_GNSS_HINTS) { S->wifi_why = "off"; return; }
    if (S->blob == nullptr) { S->wifi_why = "no-table"; return; }

    cairn_wifiloc_obs_t obs[CAIRN_WIFI_SCAN_MAX_APS];
    size_t n = 0;
    for (uint8_t i = 0; i < scan->count && i < CAIRN_WIFI_SCAN_MAX_APS; i++) {
        memcpy(obs[n].bssid, scan->ap[i].bssid, 6);
        obs[n].rssi_dbm = scan->ap[i].rssi_dbm;
        n++;
    }
    cairn_wifiloc_estimate_t e;
    if (!cairn_wifiloc_locate(&S->table, obs, n, &e)) {
        S->wifi_why = verdict_token(e.verdict);
        S->wifi_known = e.known;
        S->wifi_heard = e.heard;
        /* Once a start: a drive with no fix scans every ten seconds. */
        if (!S->wifi_logged_miss) {
            S->wifi_logged_miss = true;
            CAIRN_LOGI(TAG, "Wi-Fi hint: not from this scan (%s; %u heard, %u known, %u agreeing); "
                            "later scans retry until a fix", cairn_wifiloc_verdict_name(e.verdict),
                       (unsigned)e.heard, (unsigned)e.known, (unsigned)e.used);
        }
        return;
    }
    if (!sensors_gnss_assistable()) { S->wifi_why = "no-receiver"; return; }

    int32_t lat = 0, lon = 0, alt_saved = 0;
    bool have_last = sensors_gnss_saved_fix(&lat, &lon, &alt_saved);
    int32_t  alt = 0;
    uint32_t acc_cm = 0;
    cairn_wifiloc_hint(&e, have_last, lat, lon, alt_saved, &alt, &acc_cm);

    uint8_t frame[28];
    if (cairn_agnss_pos_llh_frame(e.lat_e7, e.lon_e7, alt, acc_cm, frame) != sizeof(frame)) return;
    if (!sensors_gnss_send_ubx(frame, sizeof(frame))) { S->wifi_why = "no-receiver"; return; }

    S->wifi_sent   = true;
    S->wifi_at_ms  = millis();
    S->wifi_acc_m  = acc_cm / 100u;
    S->wifi_hacc_m = e.acc_m;
    S->wifi_used   = e.used;
    S->wifi_known  = e.known;
    S->wifi_heard  = e.heard;
    S->wifi_from_lastfix_m =
        have_last ? (int32_t)cairn_wifiloc_distance_m(e.lat_e7, e.lon_e7, lat, lon) : -1;

    /* No position and no BSSID in the log: the card log is not sealed. The distance from
     * the last fix is the sanity check -- a hint kilometres from where the car was parked
     * means it moved without a fix, or the table is wrong. */
    char from[48];
    if (have_last) snprintf(from, sizeof(from), "%d m from the last saved fix", (int)S->wifi_from_lastfix_m);
    else           snprintf(from, sizeof(from), "no saved fix to compare");
    CAIRN_LOGI(TAG, "Wi-Fi hint sent: +/- %u m (3-D; %u m across, %u of %u known access points, "
                    "%u heard), %s, %u ms after receiver start %u (MGA-INI-POS_LLH; unacknowledged)",
               (unsigned)S->wifi_acc_m, (unsigned)e.acc_m, (unsigned)e.used, (unsigned)e.known,
               (unsigned)e.heard, from, (unsigned)(S->wifi_at_ms - S->started_ms), (unsigned)S->gen);
}

/* ── the first fix ─────────────────────────────────────────────────────────── */

/* Append to a line, clamped: snprintf returns what it would have written, and adding
 * that blindly walks past the end once the line is full. */
static void ap(size_t *w, char *line, size_t cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void ap(size_t *w, char *line, size_t cap, const char *fmt, ...)
{
    if (*w + 1 >= cap) return;
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(line + *w, cap - *w, fmt, a);
    va_end(a);
    if (n < 0) return;
    *w = (*w + (size_t)n >= cap) ? cap - 1 : *w + (size_t)n;
}

static void secs(char out[16], uint32_t ms)
{
    snprintf(out, 16, "%u.%us", (unsigned)(ms / 1000u), (unsigned)((ms % 1000u) / 100u));
}

void gnss_hint_on_fix(uint32_t mono_ms, uint8_t sats)
{
    if (!refresh() || S->fixed) return;
    /* A fix stamped before this start belongs to the last one (an NMEA buffer read late). */
    if ((int32_t)(mono_ms - S->started_ms) < 0) return;
    S->fixed = true;

    /*
     * One line, key=value, so drives can be compared with grep (README, "Reading the
     * TTFF line"):
     *   TTFF start=<n> ttff=<s since this start> boot=<s since boot> sats=<n>
     *        lastfix=<sent|none> wifi=<sent@s|reason> [acc= used= from_lastfix=]
     *        time=<source@s|reason> [tacc=]
     */
    char line[320];
    size_t w = 0;
    char a[16], b[16];
    secs(a, mono_ms - S->started_ms);
    secs(b, mono_ms);
    ap(&w, line, sizeof(line), "TTFF start=%u ttff=%s boot=%s sats=%u lastfix=%s", (unsigned)S->gen, a, b,
       (unsigned)sats, S->lastfix_hint ? "sent" : "none");
    if (S->wifi_sent) {
        secs(a, S->wifi_at_ms - S->started_ms);
        ap(&w, line, sizeof(line), " wifi=sent@%s acc=%um hacc=%um aps=%u/%u/%u", a, (unsigned)S->wifi_acc_m,
           (unsigned)S->wifi_hacc_m, (unsigned)S->wifi_used, (unsigned)S->wifi_known, (unsigned)S->wifi_heard);
        if (S->wifi_from_lastfix_m >= 0) ap(&w, line, sizeof(line), " from_lastfix=%dm", (int)S->wifi_from_lastfix_m);
    } else {
        ap(&w, line, sizeof(line), " wifi=%s scans=%u", S->wifi_why ? S->wifi_why : "no-scan", (unsigned)S->scans);
    }
    if (S->time_sent) {
        secs(a, S->time_at_ms - S->started_ms);
        ap(&w, line, sizeof(line), " time=%s@%s tacc=%ums", source_name(S->time_source), a, (unsigned)S->time_acc_ms);
    } else {
        ap(&w, line, sizeof(line), " time=%s", S->have_aid ? (S->time_why ? S->time_why : "none") : "none");
    }
    ap(&w, line, sizeof(line), " anchors=%u", (unsigned)(S->blob ? S->table.count : 0));
    CAIRN_LOGI(TAG, "%s", line);
}

/* ── the download ──────────────────────────────────────────────────────────── */

#if CAIRN_WIFI_UPLINK || CAIRN_LTE_UPLINK

static uint32_t clock_now_s(void)
{
    /* An hour of slack is plenty for "is it a day old". */
    if (!S->have_clock || S->clock_acc_ms > 3600u * 1000u) return 0;
    uint64_t now = S->clock_utc_ms + (uint32_t)(millis() - S->clock_mono_ms);
    return (uint32_t)(now / 1000u);
}

static void note_checked(void)
{
    uint32_t now_s = clock_now_s();
    if (now_s != 0) cairn_kv_set_u32(KV_ANCHORS_CHECK, now_s);
}

void gnss_hint_sync(cairn_http_t *h, const char *label, bool (*should_abort)(void *), void *abort_ctx)
{
    if (S == nullptr || !S->have_key) return;
    if (should_abort != nullptr && should_abort(abort_ctx)) return;

    const bool have = S->blob != nullptr;
    uint32_t last = cairn_kv_get_u32(KV_ANCHORS_CHECK, 0);
    if (!cairn_wifiloc_fetch_due(have, last, clock_now_s(), S->attempted, millis() - S->attempted_ms)) return;

    /* The ETag is the SHA-256 of the body (format/v3 §6.4), so the copy names itself. */
    char inm[96] = "";
    if (have) {
        uint8_t sum[32];
        cairn_sha256(S->blob, S->blob_len, sum);
        int w = snprintf(inm, sizeof(inm), "If-None-Match: \"");
        for (int i = 0; i < 32; i++) w += snprintf(inm + w, sizeof(inm) - (size_t)w, "%02x", sum[i]);
        snprintf(inm + w, sizeof(inm) - (size_t)w, "\"");
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(CAIRN_WIFILOC_MAX_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
        CAIRN_LOGW(TAG, "[%s] no PSRAM for the Wi-Fi anchor download", label);
        return;
    }
    S->attempted    = true;
    S->attempted_ms = millis();

    int      st = 0;
    size_t   len = 0;
    uint32_t t0 = millis();
    bool ok = cairn_http_request(h, "GET", ROUTE_ANCHORS, nullptr, have ? inm : nullptr, nullptr, 0, &st, buf,
                                 CAIRN_WIFILOC_MAX_BYTES + 1, &len, CAIRN_WIFI_ANCHORS_TIMEOUT_MS,
                                 should_abort, abort_ctx);
    uint32_t ms = millis() - t0;
    if (!ok) {
        CAIRN_LOGW(TAG, "[%s] Wi-Fi anchors: no answer after %u ms (transport, timeout or abort); "
                        "asked again in an hour at the soonest", label, (unsigned)ms);
        heap_caps_free(buf);
        return;
    }
    if (st == 304) {
        note_checked();
        CAIRN_LOGI(TAG, "[%s] Wi-Fi anchors unchanged (304, %u ms)", label, (unsigned)ms);
        heap_caps_free(buf);
        return;
    }
    if (st != 200) {
        CAIRN_LOGW(TAG, "[%s] Wi-Fi anchors: HTTP %d after %u ms%s", label, st, (unsigned)ms,
                   st == 404 ? " (a server without the route, or without -app-snapshot-url)"
                   : st == 503 ? " (the server cannot read its store now)" : "");
        heap_caps_free(buf);
        return;
    }
    cairn_wifiloc_table_t t;
    cairn_wifiloc_err_t e = len > CAIRN_WIFILOC_MAX_BYTES ? CAIRN_WIFILOC_ERR_COUNT : cairn_wifiloc_parse(buf, len, &t);
    if (e != CAIRN_WIFILOC_OK) {
        CAIRN_LOGW(TAG, "[%s] Wi-Fi anchors refused: %u bytes, %s", label, (unsigned)len, cairn_wifiloc_err_name(e));
        heap_caps_free(buf);
        return;
    }
    note_checked();

    /* Sealed onto the card (cairn_wifiloc.h), then the plaintext kept as the table. */
    bool stored = false;
    uint8_t *sealed = (uint8_t *)heap_caps_malloc(len + CAIRN_WIFILOC_CARD_OVERHEAD, MALLOC_CAP_SPIRAM);
    if (sealed != nullptr) {
        uint8_t nonce[24];
        cairn_rng_fill(nonce, sizeof(nonce));
        size_t sl = cairn_wifiloc_card_seal(S->card_key, nonce, buf, len, sealed, len + CAIRN_WIFILOC_CARD_OVERHEAD);
        stored = sl != 0 && write_atomic(sealed, sl);
        heap_caps_free(sealed);
    }
    adopt(buf, len, &t);
    char day[11];
    day_of(t.generated_at, day);
    CAIRN_LOGI(TAG, "[%s] Wi-Fi anchors downloaded: %u access points learned up to %s, %u bytes in %u ms%s",
               label, (unsigned)t.count, day, (unsigned)len, (unsigned)ms,
               stored ? "; sealed onto the card" : "; NOT saved to the card (kept for this boot)");
}

#endif /* CAIRN_WIFI_UPLINK || CAIRN_LTE_UPLINK */
