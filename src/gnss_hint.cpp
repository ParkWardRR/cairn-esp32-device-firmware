/* Position and time hints for the receiver, and its first fix. See gnss_hint.h. */

#include "gnss_hint.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cairn_agnss.h"
#include "cairn_format.h"
#include "cairn_log.h"
#include "cairn_phonehint.h"
#include "cairn_platform.h"
#include "config.h"
#include "sensors.h"

static const char *TAG = "HINT";

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
    /* The latest time observation of any source. */
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

    bool     time_sent;
    const char *time_why;
    uint32_t time_at_ms;
    uint32_t time_acc_ms;
    uint8_t  time_source;

    /* The phone's hint, and the accuracy of the best position hint sent this start, in
     * centimetres (0 when none has gone). Every position hint is compared against it, so
     * the receiver ends a start holding the best one rather than the last one to arrive. */
    bool     phone_sent;
    const char *phone_why;
    uint32_t phone_at_ms;
    uint32_t phone_acc_m;
    uint32_t phone_hacc_m;
    uint32_t best_acc_cm;
} hint_t;

static hint_t *S;

void gnss_hint_begin(void)
{
    if (S == nullptr) S = (hint_t *)heap_caps_calloc(1, sizeof(hint_t), MALLOC_CAP_SPIRAM);
    if (S == nullptr) {
        CAIRN_LOGW(TAG, "no PSRAM for %u bytes of hint state: no position or time hints "
                        "this boot, no first-fix line", (unsigned)sizeof(hint_t));
    }
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
        S->time_sent        = false;
        S->time_why         = "none";
        S->phone_sent       = false;
        S->phone_why        = "none";
        /* The last saved fix goes out at +/- 10 km before anything else can (sensors.cpp),
         * so a start that had one begins with that as the bar to beat. */
        S->best_acc_cm      = lastfix ? 1000000u : 0u;
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

/* Defined below, beside the mailbox it drains. */
static void try_phone(void);

void gnss_hint_tick(uint32_t standby_total_ms)
{
    try_phone();
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

/* ── the phone's fix ───────────────────────────────────────────────────────── */

/*
 * The one entry point that is not on the controller task: the BLE server's write
 * callback runs on NimBLE's. Everything in `hint_t` is controller-task-only by design
 * and takes no lock, and `sensors_gnss_send_ubx` wants the link lock, so a write cannot
 * be acted on where it lands. It goes into this one-slot mailbox instead and the next
 * controller pass drains it.
 *
 * One slot, newest wins: the phone sends exactly one hint per link, and if it ever sent
 * two the later one is the better one.
 */
static portMUX_TYPE s_mail_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_mail_full;
static struct {
    int32_t  lat_e7, lon_e7, alt_cm;
    uint16_t h_acc_cm;
    uint8_t  validity_flags;
    uint8_t  companion_flags;
} s_mail;

bool gnss_hint_note_phone_fix(int32_t lat_e7, int32_t lon_e7, int32_t alt_cm,
                              uint16_t h_acc_cm, uint8_t validity_flags,
                              uint8_t companion_flags)
{
    /* Cheap checks here, where they cost the BLE task nothing and where saying no at
     * once is more useful to the phone than saying nothing. The decision that depends on
     * what the receiver has already been told is made on the controller task. */
    if (!cairn_phonehint_is_hint(companion_flags)) return false;
    if ((validity_flags & CAIRN_GNSS_VALID_POSITION) == 0u) return false;

    portENTER_CRITICAL(&s_mail_lock);
    s_mail.lat_e7          = lat_e7;
    s_mail.lon_e7          = lon_e7;
    s_mail.alt_cm          = alt_cm;
    s_mail.h_acc_cm        = h_acc_cm;
    s_mail.validity_flags  = validity_flags;
    s_mail.companion_flags = companion_flags;
    s_mail_full            = true;
    portEXIT_CRITICAL(&s_mail_lock);
    return true;
}

/* Drain the mailbox. Controller task, like everything else that touches S. */
static void try_phone(void)
{
    if (!s_mail_full) return;

    portENTER_CRITICAL(&s_mail_lock);
    int32_t  lat = s_mail.lat_e7, lon = s_mail.lon_e7, alt = s_mail.alt_cm;
    uint16_t hacc = s_mail.h_acc_cm;
    uint8_t  validity = s_mail.validity_flags, flags = s_mail.companion_flags;
    s_mail_full = false;
    portEXIT_CRITICAL(&s_mail_lock);

    if (!refresh()) return;
    if (!CAIRN_GNSS_HINTS) { S->phone_why = "off"; return; }

    cairn_phonehint_state_t st;
    st.receiver_fixed      = S->fixed;
    st.best_acc_cm         = S->best_acc_cm;
    st.receiver_assistable = sensors_gnss_assistable();

    cairn_phonehint_frame_t fr;
    fr.companion_flags = flags;
    fr.validity_flags  = validity;
    fr.h_acc_cm        = hacc;

    uint32_t acc_cm = 0;
    cairn_phonehint_verdict_t v = cairn_phonehint_decide(&st, &fr, &acc_cm);
    if (v != CAIRN_PHONEHINT_SEND) {
        S->phone_why = cairn_phonehint_verdict_name(v);
        return;
    }

    uint8_t frame[28];
    if (cairn_agnss_pos_llh_frame(lat, lon, alt, acc_cm, frame) != sizeof(frame)) {
        S->phone_why = "encode-failed";
        return;
    }
    if (!sensors_gnss_send_ubx(frame, sizeof(frame))) {
        S->phone_why = "no-receiver";
        return;
    }

    S->phone_sent   = true;
    S->phone_at_ms  = millis();
    S->phone_acc_m  = acc_cm / 100u;
    S->phone_hacc_m = (hacc == CAIRN_PHONEHINT_ACC_UNKNOWN) ? 0u : (uint32_t)hacc / 100u;
    S->best_acc_cm  = acc_cm;

    /* No position in the log: the card log is not sealed. What is worth recording is how
     * good the phone said its fix was, what the receiver was told instead, and when --
     * because the hint cannot be acknowledged and the next TTFF line is the only answer. */
    CAIRN_LOGI(TAG, "phone hint sent: +/- %u m (the phone said %u m; floored at %u m), "
                    "%u ms after receiver start %u (MGA-INI-POS_LLH; unacknowledged)",
               (unsigned)S->phone_acc_m, (unsigned)S->phone_hacc_m,
               (unsigned)(CAIRN_PHONEHINT_FLOOR_CM / 100u),
               (unsigned)(S->phone_at_ms - S->started_ms), (unsigned)S->gen);
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
     *        lastfix=<sent|none>
     *        phone=<sent@s|reason> [pacc= phacc=]
     *        time=<source@s|reason> [tacc=]
     */
    char line[384];
    size_t w = 0;
    char a[16], b[16];
    secs(a, mono_ms - S->started_ms);
    secs(b, mono_ms);
    ap(&w, line, sizeof(line), "TTFF start=%u ttff=%s boot=%s sats=%u lastfix=%s", (unsigned)S->gen, a, b,
       (unsigned)sats, S->lastfix_hint ? "sent" : "none");
    if (S->phone_sent) {
        secs(a, S->phone_at_ms - S->started_ms);
        ap(&w, line, sizeof(line), " phone=sent@%s pacc=%um phacc=%um", a,
           (unsigned)S->phone_acc_m, (unsigned)S->phone_hacc_m);
    } else {
        ap(&w, line, sizeof(line), " phone=%s", S->phone_why ? S->phone_why : "none");
    }
    if (S->time_sent) {
        secs(a, S->time_at_ms - S->started_ms);
        ap(&w, line, sizeof(line), " time=%s@%s tacc=%ums", source_name(S->time_source), a, (unsigned)S->time_acc_ms);
    } else {
        ap(&w, line, sizeof(line), " time=%s", S->have_aid ? (S->time_why ? S->time_why : "none") : "none");
    }
    CAIRN_LOGI(TAG, "%s", line);
}
