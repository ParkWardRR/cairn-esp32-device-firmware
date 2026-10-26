/* Assisted GNSS on the device. See gnss_assist.h; the logic is lib/cairn_agnss. */

#include "gnss_assist.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <string.h>

#include "board_config.h"
#include "cairn_agnss.h"
#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_log.h"
#include "sensors.h"

static const char *TAG = "AGNSS";

/* On the card. Each is written beside its final name and renamed into place, so a
 * reset mid-write leaves the old copy or the new one; a stray .tmp is finished at
 * boot if it validates and removed if it does not. */
#define PATH_ORBITS     CAIRN_DIR_STATE "/agnss.ubx"
#define PATH_ORBITS_TMP CAIRN_DIR_STATE "/agnss.tmp"
#define PATH_ID         CAIRN_DIR_STATE "/gnss-id.bin"
#define PATH_ID_TMP     CAIRN_DIR_STATE "/gnss-id.tmp"

/* The device routes (format/v3 §6.4). */
#define ROUTE_RECEIVER "/api/v2/gnss-assist/receiver"
#define ROUTE_DATA     "/api/v2/gnss-assist"

/*
 * Bounds on the exchange. The download is ~77 KB, which over LTE (~3 KB/s, measured
 * for bundles) is ~26 s; 90 s leaves room for a slow cell without letting one
 * request hold the session indefinitely, and the transfer stops at once if the
 * upload's abort check fires (the car moves, the engine starts, the slot ends).
 */
#define REPORT_TIMEOUT_MS   20000u
#define DOWNLOAD_TIMEOUT_MS 90000u

/* Asking the receiver who it is: three attempts a boot, each the two polls and then
 * ten ATGRR reads 200 ms apart, which is two seconds of the 5 Hz NMEA path's buffer
 * per attempt, spent only while nothing is being injected. */
#define PROBE_ATTEMPTS     3u
#define PROBE_READS        10u
#define PROBE_FIRST_MS     150u
#define PROBE_EVERY_MS     200u

/* A run that has sent what it can and is waiting for a time that never comes is
 * reported after this long. */
#define RUN_REPORT_AFTER_MS (10u * 60u * 1000u)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── the best wall clock we have (written by the controller, read by both) ───── */

static struct {
    bool     have;
    uint64_t utc_ms;
    uint32_t acc_ms;
    uint32_t at_ms;
    uint32_t asleep_ms;
    uint8_t  source;
} s_time;

static bool time_now(uint32_t now, uint64_t *utc, uint32_t *acc, uint8_t *src)
{
    portENTER_CRITICAL(&s_lock);
    bool     have = s_time.have;
    uint64_t base = s_time.utc_ms;
    uint32_t bacc = s_time.acc_ms, at = s_time.at_ms, asleep = s_time.asleep_ms;
    uint8_t  source = s_time.source;
    portEXIT_CRITICAL(&s_lock);
    if (!have) return false;
    uint32_t elapsed = now - at;
    *utc = base + elapsed;
    *acc = cairn_agnss_time_acc(bacc, elapsed, asleep);
    if (src) *src = source;
    return true;
}

static const char *source_name(uint8_t src)
{
    switch (src) {
    case CAIRN_TIME_SRC_PHONE:         return "the phone";
    case CAIRN_TIME_SRC_MODEM_NETWORK: return "the cellular network";
    default:                           return "an unknown source";
    }
}

void gnss_assist_note_time(uint64_t utc_ms, uint32_t acc_ms, uint8_t source, uint32_t at_ms)
{
    if (source != CAIRN_TIME_SRC_PHONE && source != CAIRN_TIME_SRC_MODEM_NETWORK) return;
    uint64_t cur_utc;
    uint32_t cur_acc;
    bool have = time_now(at_ms, &cur_utc, &cur_acc, nullptr);
    /* The tighter of the two, judged at the new observation's instant. */
    if (have && cur_acc < acc_ms) return;
    portENTER_CRITICAL(&s_lock);
    s_time.have = true;
    s_time.utc_ms = utc_ms;
    s_time.acc_ms = acc_ms;
    s_time.at_ms = at_ms;
    s_time.asleep_ms = 0;
    s_time.source = source;
    portEXIT_CRITICAL(&s_lock);
}

void gnss_assist_note_standby(uint32_t ms)
{
    portENTER_CRITICAL(&s_lock);
    if (s_time.have) {
        uint32_t a = s_time.asleep_ms + ms;
        s_time.asleep_ms = (a < s_time.asleep_ms) ? 0xFFFFFFFFu : a;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* ── the orbits in PSRAM (replaced by the controller, read by the sensing task) ─ */

static SemaphoreHandle_t  s_blob_mu;
static uint8_t           *s_blob;
static size_t             s_blob_len;
static uint32_t           s_blob_ver;
static cairn_agnss_info_t s_blob_info;

static void blob_publish(uint8_t *buf, size_t len, const cairn_agnss_info_t *info)
{
    xSemaphoreTake(s_blob_mu, portMAX_DELAY);
    uint8_t *old = s_blob;
    s_blob = buf;
    s_blob_len = len;
    s_blob_info = *info;
    s_blob_ver++;
    xSemaphoreGive(s_blob_mu);
    heap_caps_free(old);
}

typedef enum { JOB_NONE, JOB_SMALL, JOB_ANO, JOB_ALM } job_t;

/* One injection run: everything sent since the receiver last started. */
typedef struct {
    uint32_t gen;
    bool     active, logged, fixed;
    uint32_t started_ms, fixed_ms;

    bool     time_sent;
    uint8_t  time_src;
    uint32_t time_acc;
    bool     pos_sent;

    bool     ano_pending, ano_tried, alm_tried;
    int32_t  ano_lo, ano_hi, data_first;
    uint32_t ano_msgs, ano_bytes, alm_msgs, alm_bytes;
    uint32_t lines_failed;
    uint32_t lines, line_ms_total, line_ms_max;   /* what each ATGDS round trip cost */
    const char *ano_note;

    job_t               job;
    cairn_agnss_pacer_t pacer;
    uint32_t            job_ver;
    uint8_t             small[60];
    size_t              small_len;
} run_t;

/* ── buffers ─────────────────────────────────────────────────────────────────
 *
 * In PSRAM, taken once at boot: the static DRAM segment is the scarce one (it
 * overflowed by 65 KB once, see net_upload.cpp, and overflowed by 2.7 KB when this
 * module first kept these in .bss, 2026-10-25). Every entry point does nothing
 * without them. */
typedef struct {
    run_t                  run;       /* the sensing task's */
    cairn_agnss_identity_t id;        /* published; never written after s_id_ready */
    cairn_agnss_identity_t probe_id;  /* the sensing task's working copy */
    uint8_t                raw[640];  /* one ATGRR reply */
    char                   json[2 * (CAIRN_AGNSS_SECID_MAX + CAIRN_AGNSS_MONVER_MAX) + 40];
    char                   desc[160]; /* controller */
    char                   text[512]; /* sensing task */
} big_t;

static big_t *B;
static run_t *R;   /* &B->run */

/* ── the receiver's identity ───────────────────────────────────────────────── */

static volatile bool          s_id_ready;
static bool                   s_id_on_card;  /* controller only */
static uint8_t                s_id_write_tries;

static bool id_ready(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_id_ready;
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

/* ── the card ──────────────────────────────────────────────────────────────── */

static uint8_t *read_file(const char *path, size_t max, size_t *len)
{
    uint64_t size = 0;
    *len = 0;
    if (!cairn_fs_file_size(path, &size) || size == 0 || size > max) return nullptr;
    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) return nullptr;
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    size_t got = f ? cairn_fs_read(f, buf, (size_t)size) : 0;
    if (f) cairn_fs_close(f);
    if (got != (size_t)size) { heap_caps_free(buf); return nullptr; }
    *len = got;
    return buf;
}

static bool write_atomic(const char *path, const char *tmp, const uint8_t *buf, size_t len)
{
    cairn_fs_remove(tmp);
    cairn_file_t *f = cairn_fs_open(tmp, CAIRN_FS_WRITE);
    if (f == nullptr) return false;
    bool ok = cairn_fs_write(f, buf, len) == len && cairn_fs_flush(f);
    cairn_fs_close(f);
    uint64_t size = 0;
    ok = ok && cairn_fs_file_size(tmp, &size) && size == len;
    if (!ok) { cairn_fs_remove(tmp); return false; }
    if (cairn_fs_exists(path) && !cairn_fs_remove(path)) return false;
    return cairn_fs_rename(tmp, path);
}

static bool valid_orbits(const uint8_t *b, size_t n)
{
    cairn_agnss_info_t i;
    return cairn_agnss_validate(b, n, &i);
}

static bool valid_identity(const uint8_t *b, size_t n)
{
    cairn_agnss_identity_t id;
    return cairn_agnss_identity_decode(b, n, &id);
}

/* A .tmp beside a missing final name is a write that stopped between removing the
 * old copy and renaming the new one into place: finish it if it is whole. */
static void recover(const char *path, const char *tmp, size_t max, bool (*valid)(const uint8_t *, size_t))
{
    if (!cairn_fs_exists(tmp)) return;
    if (!cairn_fs_exists(path)) {
        size_t len = 0;
        uint8_t *b = read_file(tmp, max, &len);
        bool ok = b != nullptr && valid(b, len);
        heap_caps_free(b);
        if (ok && cairn_fs_rename(tmp, path)) {
            CAIRN_LOGI(TAG, "finished an interrupted write of %s", path);
            return;
        }
    }
    cairn_fs_remove(tmp);
}

static void day_str(int32_t day, char out[11])
{
    if (day == CAIRN_AGNSS_NO_DAY) { snprintf(out, 11, "none"); return; }
    int y, m, d;
    cairn_agnss_civil_from_days(day, &y, &m, &d);
    snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}

void gnss_assist_begin(void)
{
    if (B == nullptr) B = (big_t *)heap_caps_calloc(1, sizeof(big_t), MALLOC_CAP_SPIRAM);
    if (B == nullptr) {
        CAIRN_LOGE(TAG, "no PSRAM for %u bytes of buffers; GNSS assistance off", (unsigned)sizeof(big_t));
        return;
    }
    R = &B->run;
    if (s_blob_mu == nullptr) s_blob_mu = xSemaphoreCreateMutex();
    cairn_fs_mkdir(CAIRN_DIR_STATE);

    recover(PATH_ORBITS, PATH_ORBITS_TMP, CAIRN_AGNSS_MAX_BYTES, valid_orbits);
    recover(PATH_ID, PATH_ID_TMP, CAIRN_AGNSS_SECID_MAX + CAIRN_AGNSS_MONVER_MAX, valid_identity);

    size_t len = 0;
    uint8_t *buf = read_file(PATH_ORBITS, CAIRN_AGNSS_MAX_BYTES, &len);
    cairn_agnss_info_t info;
    if (buf != nullptr && cairn_agnss_validate(buf, len, &info)) {
        blob_publish(buf, len, &info);
        char a[11], b[11];
        day_str(info.first_day, a);
        day_str(info.last_day, b);
        CAIRN_LOGI(TAG, "orbits on the card: %u bytes, %u predictions for %s..%s, %u almanac messages",
                   (unsigned)len, (unsigned)info.ano, a, b, (unsigned)info.almanac);
    } else {
        if (buf != nullptr || cairn_fs_exists(PATH_ORBITS)) {
            CAIRN_LOGW(TAG, "the orbits on the card do not validate; removed");
            cairn_fs_remove(PATH_ORBITS);
        } else {
            CAIRN_LOGI(TAG, "no orbits on the card yet");
        }
        heap_caps_free(buf);
    }

    uint8_t idb[CAIRN_AGNSS_SECID_MAX + CAIRN_AGNSS_MONVER_MAX];
    cairn_file_t *f = cairn_fs_open(PATH_ID, CAIRN_FS_READ);
    size_t n = f ? cairn_fs_read(f, idb, sizeof(idb)) : 0;
    if (f) cairn_fs_close(f);
    if (n > 0 && cairn_agnss_identity_decode(idb, n, &B->id)) {
        cairn_agnss_identity_describe(&B->id, B->desc, sizeof(B->desc));
        s_id_on_card = true;
        portENTER_CRITICAL(&s_lock);
        s_id_ready = true;
        portEXIT_CRITICAL(&s_lock);
        CAIRN_LOGI(TAG, "receiver identity on the card: %s", B->desc);
    }
}

void gnss_assist_controller_tick(void)
{
    if (B == nullptr || s_id_on_card || s_id_write_tries >= 3 || !id_ready()) return;
    s_id_write_tries++;
    uint8_t idb[CAIRN_AGNSS_SECID_MAX + CAIRN_AGNSS_MONVER_MAX];
    memcpy(idb, B->id.sec, B->id.sec_len);
    memcpy(idb + B->id.sec_len, B->id.mon, B->id.mon_len);
    if (write_atomic(PATH_ID, PATH_ID_TMP, idb, (size_t)B->id.sec_len + B->id.mon_len)) {
        s_id_on_card = true;
        CAIRN_LOGI(TAG, "receiver identity saved to the card");
    } else {
        CAIRN_LOGW(TAG, "could not save the receiver identity (attempt %u)", (unsigned)s_id_write_tries);
    }
}

/* ── the server ────────────────────────────────────────────────────────────── */

static bool     s_reported;       /* this boot */
static bool     s_want_report;    /* the server answered 404 for data */
static bool     s_server_off;     /* the server has no such routes */
static bool     s_checked;
static uint32_t s_checked_ms;

void gnss_assist_sync(cairn_http_t *h, const char *label,
                      bool (*should_abort)(void *), void *abort_ctx)
{
    if (B == nullptr || s_server_off) return;
    if (should_abort != nullptr && should_abort(abort_ctx)) return;
    uint32_t now = millis();
    int      st = 0;

    /* 1. Who the receiver is, once a boot, or again when the server had no data. */
    if (id_ready() && (!s_reported || s_want_report)) {
        char  *json = B->json;
        size_t jn = cairn_agnss_identity_json(&B->id, json, sizeof(B->json));
        uint8_t resp[160];
        size_t  rl = 0;
        if (jn == 0) return;
        if (!cairn_http_request(h, "POST", ROUTE_RECEIVER, "application/json", nullptr,
                                (const uint8_t *)json, jn, &st, resp, sizeof(resp), &rl,
                                REPORT_TIMEOUT_MS, should_abort, abort_ctx)) {
            CAIRN_LOGW(TAG, "[%s] reporting the receiver failed in transport", label);
            return;
        }
        if (st == 404) {
            /* The routes exist only when the server holds an AssistNow credential. */
            CAIRN_LOGI(TAG, "[%s] the server offers no GNSS assistance (HTTP 404); "
                            "not asking again this boot", label);
            s_server_off = true;
            return;
        }
        if (st == 200 || st == 202) {
            s_reported = true;
            s_want_report = false;
            CAIRN_LOGI(TAG, "[%s] receiver reported (HTTP %d: %s)", label, st,
                       st == 200 ? "registered with AssistNow" : "registration pending on the server");
        } else {
            CAIRN_LOGW(TAG, "[%s] reporting the receiver: HTTP %d", label, st);
        }
    }

    if (should_abort != nullptr && should_abort(abort_ctx)) return;

    /* 2. Fresh orbits, when the copy is missing or getting short. */
    uint64_t utc;
    uint32_t acc;
    int32_t  today = CAIRN_AGNSS_NO_DAY;
    if (time_now(now, &utc, &acc, nullptr) && acc < 6u * 3600u * 1000u) today = cairn_agnss_day_of_ms(utc);

    const bool have = s_blob != nullptr;   /* only this task replaces it */
    if (!cairn_agnss_fetch_due(have, &s_blob_info, today, s_checked, now - s_checked_ms)) return;

    char inm[96] = "";
    if (have) {
        uint8_t sum[32];
        cairn_sha256(s_blob, s_blob_len, sum);
        int w = snprintf(inm, sizeof(inm), "If-None-Match: \"");
        for (int i = 0; i < 32; i++) w += snprintf(inm + w, sizeof(inm) - (size_t)w, "%02x", sum[i]);
        snprintf(inm + w, sizeof(inm) - (size_t)w, "\"");
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(CAIRN_AGNSS_MAX_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
        CAIRN_LOGW(TAG, "[%s] no PSRAM for the orbit download", label);
        return;
    }
    size_t   len = 0;
    uint32_t t0 = millis();
    s_checked = true;
    s_checked_ms = now;
    bool ok = cairn_http_request(h, "GET", ROUTE_DATA, nullptr, have ? inm : nullptr,
                                 nullptr, 0, &st, buf, CAIRN_AGNSS_MAX_BYTES + 1, &len,
                                 DOWNLOAD_TIMEOUT_MS, should_abort, abort_ctx);
    uint32_t ms = millis() - t0;
    if (!ok) {
        CAIRN_LOGW(TAG, "[%s] orbit download failed in transport after %u ms", label, (unsigned)ms);
        heap_caps_free(buf);
        return;
    }
    if (st == 304) {
        CAIRN_LOGI(TAG, "[%s] orbits unchanged on the server (304, %u ms)", label, (unsigned)ms);
        heap_caps_free(buf);
        return;
    }
    if (st == 404) {
        CAIRN_LOGI(TAG, "[%s] the server has no orbits for this receiver yet (HTTP 404)", label);
        s_want_report = true;
        heap_caps_free(buf);
        return;
    }
    cairn_agnss_info_t info;
    if (st != 200 || len > CAIRN_AGNSS_MAX_BYTES || !cairn_agnss_validate(buf, len, &info)) {
        CAIRN_LOGW(TAG, "[%s] orbit download refused: HTTP %d, %u bytes%s", label, st, (unsigned)len,
                   st == 200 ? ", not a valid AssistNow stream" : "");
        heap_caps_free(buf);
        return;
    }
    bool stored = write_atomic(PATH_ORBITS, PATH_ORBITS_TMP, buf, len);
    char a[11], b[11];
    day_str(info.first_day, a);
    day_str(info.last_day, b);
    CAIRN_LOGI(TAG, "[%s] orbits downloaded: %u bytes in %u ms, %u predictions for %s..%s, "
                    "%u almanac messages%s",
               label, (unsigned)len, (unsigned)ms, (unsigned)info.ano, a, b, (unsigned)info.almanac,
               stored ? "; on the card" : "; NOT saved to the card (kept for this boot)");
    blob_publish(buf, len, &info);
}

/* ── the sensing task: injection ───────────────────────────────────────────── */



/* Append to a log line, clamped: snprintf returns what it would have written, and
 * adding that blindly walks past the end once the line is full. */
static void ap(size_t *w, char *line, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
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

static void log_run(const char *ending)
{
    char  *line = B->text;
    const size_t cap = sizeof(B->text);
    char d1[11], d2[11];
    size_t w = 0;
    ap(&w, line, cap, "injection for receiver start %u:", (unsigned)R->gen);

    if (R->ano_msgs > 0) {
        day_str(R->ano_lo, d1);
        ap(&w, line, cap, " %u predictions (%u B) for %s",
           (unsigned)R->ano_msgs, (unsigned)R->ano_bytes, d1);
        if (R->ano_hi != R->ano_lo) {
            day_str(R->ano_hi, d2);
            ap(&w, line, cap, "..%s", d2);
        }
        if (R->data_first != CAIRN_AGNSS_NO_DAY) {
            day_str(R->data_first, d2);
            ap(&w, line, cap, " from a copy starting %s (%d day(s) old)",
               d2, (int)(R->ano_lo - R->data_first));
        }
    } else {
        ap(&w, line, cap, " no predictions (%s)",
           R->ano_note ? R->ano_note : "no time to choose a day by");
    }
    ap(&w, line, cap, ", %u almanac messages (%u B)",
       (unsigned)R->alm_msgs, (unsigned)R->alm_bytes);
    if (R->time_sent) {
        ap(&w, line, cap, ", time from %s +/- %u ms then position %s",
           source_name(R->time_src), (unsigned)R->time_acc, R->pos_sent ? "hint" : "(none saved)");
    } else {
        ap(&w, line, cap, ", no time aiding (no phone or network time yet)");
    }
    if (R->fixed) {
        ap(&w, line, cap, "; first fix %u ms after start",
           (unsigned)(R->fixed_ms - R->started_ms));
        /* How good the claimed accuracy was: the receiver's own UTC against our
         * estimate of the same moment (the summary is up to ~1 s behind). */
        uint64_t rx, est;
        uint32_t rx_acc, est_acc;
        if (R->time_sent && sensors_gnss_utc(&rx, &rx_acc) && time_now(millis(), &est, &est_acc, nullptr)) {
            ap(&w, line, cap, " (our clock %+lld ms from the receiver's)",
               (long long)((int64_t)est - (int64_t)rx));
        }
    }
    if (R->lines > 0) {
        /* The link cost, which the 25 ms pacing assumed and nobody has measured. */
        ap(&w, line, cap, "; %u ATGDS lines, %u ms each on average, %u at most",
           (unsigned)R->lines, (unsigned)(R->line_ms_total / R->lines), (unsigned)R->line_ms_max);
    }
    if (R->lines_failed) {
        ap(&w, line, cap, "; %u line(s) not sent", (unsigned)R->lines_failed);
    }
    ap(&w, line, cap, "; %s", ending);
    CAIRN_LOGI(TAG, "%s", line);
    R->logged = true;
}

static void finish_job(void)
{
    switch (R->job) {
    case JOB_ANO: R->ano_msgs += R->pacer.msgs; R->ano_bytes += R->pacer.bytes; break;
    case JOB_ALM: R->alm_msgs += R->pacer.msgs; R->alm_bytes += R->pacer.bytes; break;
    default: break;
    }
    if (R->pacer.malformed) CAIRN_LOGW(TAG, "the stored orbits are malformed part-way; that part was not sent");
    R->job = JOB_NONE;
}

static void start_run(uint32_t gen, uint32_t now)
{
    memset(R, 0, sizeof(*R));
    R->gen = gen;
    R->active = true;
    R->started_ms = now;
    R->ano_lo = R->ano_hi = R->data_first = CAIRN_AGNSS_NO_DAY;
}

static void job_step(uint32_t now)
{
    if (R->fixed) {           /* the receiver has what it needs */
        finish_job();
        return;
    }
    char   line[CAIRN_AGNSS_LINE_CAP];
    size_t n = 0;
    if (R->job == JOB_SMALL) {
        n = cairn_agnss_pacer_next(&R->pacer, R->small, R->small_len, now, line, sizeof(line));
    } else {
        /* Never wait on the controller: if it is swapping the data in, try next loop. */
        if (xSemaphoreTake(s_blob_mu, 0) != pdTRUE) return;
        if (s_blob_ver != R->job_ver || s_blob == nullptr) {
            R->pacer.done = true;   /* replaced under us; the next start uses the new copy */
        } else {
            n = cairn_agnss_pacer_next(&R->pacer, s_blob, s_blob_len, now, line, sizeof(line));
        }
        xSemaphoreGive(s_blob_mu);
    }
    if (n > 0) {
        uint32_t t0 = millis();
        if (!sensors_gnss_send_line(line)) R->lines_failed++;
        uint32_t took = millis() - t0;
        R->lines++;
        R->line_ms_total += took;
        if (took > R->line_ms_max) R->line_ms_max = took;
    }
    if (R->pacer.done) finish_job();
}

/* Start the next thing to send, in u-blox's order: time, position, orbits; the
 * almanacs (good for weeks and not time-critical) last, or first while waiting for
 * a time to choose the orbits' day by. */
static bool next_job(uint32_t now)
{
    if (!R->time_sent) {
        uint64_t utc;
        uint32_t acc;
        uint8_t  src;
        if (time_now(now, &utc, &acc, &src) && acc <= CAIRN_AGNSS_TIME_MAX_ACC_MS) {
            R->small_len = cairn_agnss_time_utc_frame(utc, acc, R->small);
            /* The hint again after the time: init_gnss sent it before there was one. */
            if (sensors_gnss_pos_hint(R->small + R->small_len)) {
                R->small_len += 28;
                R->pos_sent = true;
            }
            R->time_sent = true;
            R->time_src = src;
            R->time_acc = acc;
            R->ano_pending = true;
            cairn_agnss_day_window(utc, acc, &R->ano_lo, &R->ano_hi);
            cairn_agnss_pacer_start(&R->pacer, CAIRN_AGNSS_PICK_ALL, 0, 0, now);
            R->job = JOB_SMALL;
            return true;
        }
    }

    if (R->ano_pending) {
        R->ano_pending = false;
        R->ano_tried = true;
        bool started = false;
        if (xSemaphoreTake(s_blob_mu, pdMS_TO_TICKS(5)) == pdTRUE) {
            int32_t lo, hi;
            if (s_blob == nullptr) {
                R->ano_note = "no orbits on the card";
            } else if (!cairn_agnss_ano_window(s_blob, s_blob_len, R->ano_lo, R->ano_hi, &lo, &hi)) {
                R->ano_note = "the stored orbits do not cover today";
            } else {
                R->ano_lo = lo;
                R->ano_hi = hi;
                R->data_first = s_blob_info.first_day;
                cairn_agnss_pacer_start(&R->pacer, CAIRN_AGNSS_PICK_ANO, lo, hi, now);
                R->job_ver = s_blob_ver;
                R->job = JOB_ANO;
                started = true;
            }
            xSemaphoreGive(s_blob_mu);
        }
        if (started) return true;
    }

    if (!R->alm_tried) {
        R->alm_tried = true;
        if (xSemaphoreTake(s_blob_mu, pdMS_TO_TICKS(5)) == pdTRUE) {
            bool have = s_blob != nullptr;
            if (have) {
                cairn_agnss_pacer_start(&R->pacer, CAIRN_AGNSS_PICK_ALMANAC, 0, 0, now);
                R->job_ver = s_blob_ver;
                R->job = JOB_ALM;
            }
            xSemaphoreGive(s_blob_mu);
            if (have) return true;
        }
    }
    return false;
}

/* ── the sensing task: who the receiver is ─────────────────────────────────── */

static struct {
    uint8_t  attempts;
    bool     polling;
    uint8_t  reads_left;
    uint32_t next_ms;
    uint32_t bytes_seen, frames_seen, replies;
    bool     gave_up;
} s_probe;


static void probe_step(uint32_t now)
{
    if (s_probe.gave_up) return;
    if (!s_probe.polling) {
        if (s_probe.attempts >= PROBE_ATTEMPTS) return;
        char line[CAIRN_AGNSS_LINE_CAP];
        cairn_agnss_atgds_line(CAIRN_UBX_POLL_SEC_UNIQID, 8, line, sizeof(line));
        sensors_gnss_send_line(line);
        cairn_agnss_atgds_line(CAIRN_UBX_POLL_MON_VER, 8, line, sizeof(line));
        sensors_gnss_send_line(line);
        s_probe.attempts++;
        s_probe.polling = true;
        s_probe.reads_left = PROBE_READS;
        s_probe.next_ms = now + PROBE_FIRST_MS;
        return;
    }
    if ((int32_t)(now - s_probe.next_ms) < 0) return;

    int n = sensors_gnss_read_raw(B->raw, sizeof(B->raw));
    if (n > 0) {
        s_probe.bytes_seen += (uint32_t)n;
        size_t off = 0;
        cairn_ubx_t f;
        while (cairn_ubx_find(B->raw, (size_t)n, &off, &f)) s_probe.frames_seen++;
        s_probe.replies += (uint32_t)cairn_agnss_identity_scan(B->raw, (size_t)n, &B->probe_id);
    }
    if (cairn_agnss_identity_complete(&B->probe_id)) {
        memcpy(&B->id, &B->probe_id, sizeof(B->id));
        portENTER_CRITICAL(&s_lock);
        s_id_ready = true;
        portEXIT_CRITICAL(&s_lock);
        s_probe.polling = false;
        cairn_agnss_identity_describe(&B->id, B->text, sizeof(B->text));
        CAIRN_LOGI(TAG, "receiver identity read through the co-processor (attempt %u): %s",
                   (unsigned)s_probe.attempts, B->text);
        return;
    }
    s_probe.next_ms = now + PROBE_EVERY_MS;
    if (--s_probe.reads_left > 0) return;
    s_probe.polling = false;
    if (s_probe.attempts >= PROBE_ATTEMPTS) {
        s_probe.gave_up = true;
        /*
         * The line that answers whether the co-processor passes UBX replies through.
         * Any UBX frame at all (an ACK, a NAK) means it does and the receiver did not
         * answer these polls; none means ATGRR carries NMEA only and the identity has
         * to come another way.
         */
        CAIRN_LOGW(TAG, "receiver identity not read: %u attempts, %u bytes over ATGRR, %u UBX "
                        "frame(s), %u identity repl(ies)%s",
                   (unsigned)s_probe.attempts, (unsigned)s_probe.bytes_seen,
                   (unsigned)s_probe.frames_seen, (unsigned)s_probe.replies,
                   s_probe.frames_seen == 0 ? "; the co-processor appears to pass NMEA only" : "");
    }
}

/* ── the sensing task: the step ────────────────────────────────────────────── */

void gnss_assist_on_fix(void)
{
    if (R == nullptr) return;
    if (R->active && !R->fixed) {
        R->fixed = true;
        R->fixed_ms = millis();
    }
}

void gnss_assist_sense_step(uint32_t now)
{
    if (B == nullptr || s_blob_mu == nullptr || !sensors_gnss_assistable()) return;
    uint32_t gen = sensors_gnss_generation();
    if (gen == 0) return;

    if (gen != R->gen) {
        if (R->active && !R->logged) log_run("cut short: the receiver restarted");
        start_run(gen, now);
    }

    if (R->job != JOB_NONE) {
        job_step(now);
        return;
    }
    if (!R->fixed && next_job(now)) {
        R->logged = false;   /* a run reported as waiting reports again when it moves on */
        return;
    }

    if (!id_ready()) {
        probe_step(now);
        if (s_probe.polling) return;
    }

    if (!R->logged) {
        if (R->fixed) {
            log_run(R->time_sent && !R->ano_pending && R->alm_tried ? "complete" : "stopped at the first fix");
        } else if (R->time_sent && !R->ano_pending && R->alm_tried) {
            log_run("complete");
        } else if (now - R->started_ms >= RUN_REPORT_AFTER_MS) {
            log_run("still waiting for a time to choose the day's orbits by");
        }
    }
}
