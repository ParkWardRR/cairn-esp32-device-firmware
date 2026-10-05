/*
 * Manual-transmission probe. Compiled only when CAIRN_MTPROBE is defined.
 *
 * What it is for. Gear and clutch are not standard OBD-II values. Gear can be
 * inferred from RPM over road speed, but the clutch pedal, the neutral switch
 * and engine torque are only visible if this particular DME offers them, and
 * nothing written about an F32 says whether it does. So rather than design a
 * record around a guess, this build asks for everything that might describe a
 * gearbox and writes the raw answers to the SD log, and a drive says which of
 * them exist.
 *
 * Three things, in the order they pay off:
 *
 *   1. The Mode 01 support bitmaps, walked in full. This is the ECU's own list
 *      of what it will answer, logged verbatim.
 *   2. A rotating set of candidate PIDs — engine and driver-demand torque,
 *      accelerator pedal, friction torque, J1979-2 transmission gear, and a
 *      handful of engine-state values — each asked alongside RPM, speed and
 *      throttle. The anchors are the point: with them on every request, one log
 *      line has what a shift is read against (RPM, speed, pedal) next to the
 *      candidate values, and nothing has to be joined back to the bundle to be
 *      useful.
 *   3. Optionally, short passive CAN sniff windows, to see whether the OBD
 *      connector carries anything beyond our own conversation. On an F32 it
 *      probably does not — pins 6 and 14 are the diagnostic CAN behind the
 *      gateway, silent without a tester — but that is cheap to find out and
 *      expensive to assume.
 *
 * What it does not do. It sends only standard Mode 01 requests, the same kind
 * production already sends. Nothing is written to the car, no diagnostic
 * session is opened, and no manufacturer-specific service is touched. The
 * BMW-specific route (UDS reads on the DME) may be the only way to the clutch
 * switch, but guessing identifiers at an engine ECU from a moving car is not
 * something to ship without a bench test, so it is deliberately absent.
 *
 * Output is one INFO line per request with tag MTP:
 *
 *   t=<ms> req=<01 + PIDs> dt=<ms> ans=<PIDs found> rx=<reply, lines joined by |>
 *
 * grep MTP over the log and decode offline; the reply text is kept raw so a
 * wrong length in mtprobe_parse.c cannot corrupt what was recorded.
 */

#include "config.h"

#if CAIRN_MTPROBE

#include <Arduino.h>
#include <FreematicsPlus.h>
#include <string.h>

#include "cairn_log.h"
#include "mtprobe.h"
#include "mtprobe_parse.h"
#include "sensors.h"

static const char *TAG = "MTP";

/* Carried on every request. */
static const uint8_t k_anchor[] = { 0x0C, 0x0D, 0x11 };
#define ANCHORS ((int)sizeof(k_anchor))

/* A multi-PID request carries at most six. */
#define REQ_PIDS 6
#define CAND_PER_REQ (REQ_PIDS - ANCHORS)

/*
 * Ordered by how directly each bears on a gearbox, so the first discovery round
 * reaches the interesting ones first on a short drive.
 */
static const uint8_t k_cand[] = {
    0xA4,                                  /* transmission actual gear */
    0x62, 0x61, 0x63, 0x64, 0x8E,          /* torque */
    0x49, 0x4A, 0x4B, 0x5A, 0x45, 0x47, 0x4C, /* pedal and throttle */
    0x5E, 0x5D, 0x5C, 0x42, 0x22, 0x23,    /* fuel rate, timing, oil, voltage, rail */
    0x03, 0x2C, 0x2E, 0x3C,                /* fuel system, EGR, purge, catalyst */
    0x1F, 0x21, 0x31, 0x4D,                /* counters */
};
#define NCAND ((int)(sizeof(k_cand) / sizeof(k_cand[0])))

static uint8_t  s_tries[NCAND];
static bool     s_answered[NCAND];
static uint32_t s_last_try_ms[NCAND];
static int      s_cursor;

static const uint8_t k_bases[] = { 0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0 };
#define NBASES ((int)sizeof(k_bases))

static uint8_t s_bitmap[NBASES][4];
static bool    s_bitmap_have[NBASES];
static int     s_walk_idx;
static bool    s_walk_done;

/*
 * Whether the ECU answers a request carrying several PIDs. Production proved
 * it does for six supported ones; this asks for PIDs that may not be supported,
 * and an ECU is allowed to refuse the whole request over one of them. So until
 * a reply has shown the anchors coming back, three failures in a row switch the
 * probe to one candidate per request, which always works and costs more bus
 * time.
 */
static bool s_multi_proven;
static bool s_multi_ok = true;
static int  s_multi_misses;

static uint32_t s_next_ms;
static uint32_t s_requests;

/* One reply as a single log-safe line. */
static void flatten(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    for (size_t i = 0; in[i] != '\0' && w + 1 < cap; i++) {
        char c = in[i];
        if (c == '\r' || c == '\n') {
            if (w == 0 || out[w - 1] == '|') continue;
            c = '|';
        } else if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) {
            c = '?';
        }
        out[w++] = c;
    }
    while (w > 0 && (out[w - 1] == '|' || out[w - 1] == ' ' || out[w - 1] == '>')) w--;
    out[w] = '\0';
}

static bool bitmap_claims(uint8_t pid, bool *known)
{
    int slot = pid / 0x20;
    if (slot >= NBASES || !s_bitmap_have[slot]) {
        *known = false;
        return false;
    }
    *known = true;
    int n = pid - k_bases[slot];            /* 1..32 within this bitmap */
    if (n < 1) return false;
    return (s_bitmap[slot][(n - 1) >> 3] >> (7 - ((n - 1) & 7))) & 1u;
}

/* What the ECU's own bitmaps say about each candidate, once the walk ends. */
static void log_claims(void)
{
    char line[160];
    size_t w = 0;
    line[0] = '\0';

    for (int i = 0; i < NCAND && w + 8 < sizeof(line); i++) {
        bool known;
        bool claim = bitmap_claims(k_cand[i], &known);
        w += (size_t)snprintf(line + w, sizeof(line) - w, "%02X=%c ",
                              (unsigned)k_cand[i], known ? (claim ? 'Y' : 'n') : '?');
    }
    CAIRN_LOGI(TAG, "claims (Y supported, n not, ? bitmap unread): %s", line);
}

static void walk_step(uint32_t now)
{
    uint8_t base = k_bases[s_walk_idx];

    char cmd[8];
    snprintf(cmd, sizeof(cmd), "01%02X\r", (unsigned)base);

    char rx[128];
    int  n = sensors_obd_command(cmd, rx, sizeof(rx), OBD_TIMEOUT_SHORT);

    char flat[96];
    flatten(n > 0 ? rx : "", flat, sizeof(flat));
    CAIRN_LOGI(TAG, "t=%lu bitmap %02X rx=%s", (unsigned long)now,
               (unsigned)base, flat[0] ? flat : "(none)");

    bool chain = false;

    uint8_t bytes[32];
    int     nb = (n > 0) ? mtprobe_reply_bytes(rx, bytes, (int)sizeof(bytes)) : -1;
    mtprobe_hit_t hit[1];
    if (nb > 0 && mtprobe_split(bytes, nb, hit, 1) == 1 && hit[0].pid == base) {
        memcpy(s_bitmap[s_walk_idx], hit[0].data, 4);
        s_bitmap_have[s_walk_idx] = true;
        /* Bit 0 of the last byte says whether the next bitmap exists. */
        chain = (hit[0].data[3] & 1u) != 0;
    }

    s_walk_idx++;
    if (!chain || s_walk_idx >= NBASES) {
        s_walk_done = true;
        log_claims();
    }
}

/* Pick up to `want` candidates that are due, advancing the shared cursor. */
static int pick(uint32_t now, int want, int *idx)
{
    int found = 0;
    for (int scanned = 0; scanned < NCAND && found < want; scanned++) {
        int i = s_cursor;
        s_cursor = (s_cursor + 1) % NCAND;

        bool due = s_answered[i] ||
                   s_tries[i] < CAIRN_MTPROBE_DISCOVERY_TRIES ||
                   (uint32_t)(now - s_last_try_ms[i]) >= CAIRN_MTPROBE_RETRY_MS;
        if (due) idx[found++] = i;
    }
    return found;
}

static void probe_step(uint32_t now)
{
    int idx[CAND_PER_REQ];
    int want  = s_multi_ok ? CAND_PER_REQ : 1;
    int count = pick(now, want, idx);
    if (count == 0) return;

    char cmd[24];
    size_t k = (size_t)snprintf(cmd, sizeof(cmd), "01");


    if (s_multi_ok) {
        for (int i = 0; i < ANCHORS; i++) {
            k += (size_t)snprintf(cmd + k, sizeof(cmd) - k, "%02X", (unsigned)k_anchor[i]);
        }
    }
    for (int i = 0; i < count; i++) {
        k += (size_t)snprintf(cmd + k, sizeof(cmd) - k, "%02X", (unsigned)k_cand[idx[i]]);
        s_last_try_ms[idx[i]] = now;
    }
    char req_label[24];
    snprintf(req_label, sizeof(req_label), "%s", cmd);
    cmd[k++] = '\r';
    cmd[k]   = '\0';

    char     rx[200];
    uint32_t t0 = millis();
    int      n  = sensors_obd_command(cmd, rx, sizeof(rx), OBD_TIMEOUT_SHORT);
    uint32_t dt = millis() - t0;

    uint8_t bytes[48];
    int     nb = (n > 0) ? mtprobe_reply_bytes(rx, bytes, (int)sizeof(bytes)) : -1;

    mtprobe_hit_t hits[MTPROBE_MAX_HITS];
    int nh = (nb > 0) ? mtprobe_split(bytes, nb, hits, MTPROBE_MAX_HITS) : 0;

    bool anchors_back = false;
    char ans[40];
    size_t aw = 0;
    ans[0] = '\0';
    for (int h = 0; h < nh && aw + 4 < sizeof(ans); h++) {
        aw += (size_t)snprintf(ans + aw, sizeof(ans) - aw, "%02X,", (unsigned)hits[h].pid);
        if (hits[h].pid == k_anchor[0]) anchors_back = true;
    }
    if (aw > 0) ans[aw - 1] = '\0';

    /* Which of the candidates in this request came back. */
    bool trustworthy;
    if (s_multi_ok) {
        if (anchors_back) {
            s_multi_proven = true;
            s_multi_misses = 0;
        } else if (!s_multi_proven && ++s_multi_misses >= 3) {
            s_multi_ok = false;
            CAIRN_LOGW(TAG, "multi-PID request never answered with candidates "
                            "attached; falling back to one PID per request");
        }
        /* Without the anchors the reply says nothing about the candidates. */
        trustworthy = anchors_back;
    } else {
        trustworthy = true;
    }

    if (trustworthy) {
        for (int i = 0; i < count; i++) {
            bool got = false;
            for (int h = 0; h < nh; h++) {
                if (hits[h].pid == k_cand[idx[i]]) got = true;
            }
            if (got) {
                if (!s_answered[idx[i]]) {
                    CAIRN_LOGI(TAG, "PID %02X answers", (unsigned)k_cand[idx[i]]);
                }
                s_answered[idx[i]] = true;
            } else if (s_tries[idx[i]] < 255) {
                s_tries[idx[i]]++;
            }
        }
    }

    char flat[128];
    flatten(n > 0 ? rx : "", flat, sizeof(flat));
    CAIRN_LOGI(TAG, "t=%lu req=%s dt=%lu ans=%s rx=%s", (unsigned long)now, req_label,
               (unsigned long)dt, ans[0] ? ans : "-", flat[0] ? flat : "(none)");

    s_requests++;
}

#if CAIRN_MTPROBE_SNIFF
static bool     s_sniff_dead;
static uint32_t s_sniff_next_ms;
static bool     s_sniff_scheduled;

/* One AT command, with the adapter's reply logged, because its dialect is
 * exactly what this first drive is meant to reveal. */
static void at_logged(const char *cmd, uint32_t timeout_ms)
{
    char rx[96];
    int  n = sensors_obd_command(cmd, rx, sizeof(rx), timeout_ms);

    char label[16];
    flatten(cmd, label, sizeof(label));

    char flat[80];
    flatten(n > 0 ? rx : "", flat, sizeof(flat));
    CAIRN_LOGI(TAG, "sniff cmd %s -> %s", label, flat[0] ? flat : "(none)");
}

static void sniff_window(uint32_t now)
{
    CAIRN_LOGI(TAG, "t=%lu sniff window begins (%d ms, up to %d chunks)",
               (unsigned long)now, CAIRN_MTPROBE_SNIFF_WINDOW_MS,
               CAIRN_MTPROBE_SNIFF_MAX_CHUNKS);

    /* Mask zero: no header bits must match, so every frame is accepted. */
    at_logged("ATCF 0\r", 500);
    at_logged("ATCM 0\r", 500);
    at_logged("ATM1\r", 500);

    int chunks = 0;
    uint32_t t0 = millis();
    char buf[240];

    while ((uint32_t)(millis() - t0) < CAIRN_MTPROBE_SNIFF_WINDOW_MS &&
           chunks < CAIRN_MTPROBE_SNIFF_MAX_CHUNKS) {
        /* The accelerometer is local I2C, so it keeps sampling while the OBD
         * link is busy listening. */
        sensors_imu_accumulate();

        int n = sensors_obd_receive(buf, sizeof(buf), 100);
        if (n <= 0) continue;

        char flat[200];
        flatten(buf, flat, sizeof(flat));
        if (flat[0] == '\0') continue;

        CAIRN_LOGI(TAG, "t=%lu sniff=%s", (unsigned long)millis(), flat);
        chunks++;
    }

    at_logged("ATM0\r", 500);

    /* Whatever was still in flight when the mode changed. */
    sensors_obd_receive(buf, sizeof(buf), 100);

    CAIRN_LOGI(TAG, "sniff window ended: %d chunk(s)", chunks);

    /*
     * Prove the link is back before returning to production polling. A plain
     * request is the test; if it fails the adapter is still in the wrong mode or
     * has lost its protocol, and a full ECU re-initialisation is the way out.
     * If even that fails, sniffing is switched off for the rest of the boot —
     * one lost window is a diagnostic, a link that stays broken is a lost trip.
     */
    char rx[96];
    int  n = sensors_obd_command("010C\r", rx, sizeof(rx), OBD_TIMEOUT_SHORT);
    uint8_t b[8];
    bool ok = n > 0 && mtprobe_reply_bytes(rx, b, (int)sizeof(b)) > 0;

    if (!ok) {
        CAIRN_LOGW(TAG, "no ECU reply after leaving sniff mode; re-initialising");
        ok = sensors_obd_recover();
        CAIRN_LOGW(TAG, "re-initialisation %s", ok ? "succeeded" : "FAILED");
    }
    if (!ok) {
        s_sniff_dead = true;
        CAIRN_LOGE(TAG, "sniffing disabled for this boot");
    }

    cairn_log_flush();
}
#endif /* CAIRN_MTPROBE_SNIFF */

void mtprobe_tick(void)
{
    uint32_t now = millis();

    if ((int32_t)(now - s_next_ms) < 0) return;
    s_next_ms = now + CAIRN_MTPROBE_PERIOD_MS;

    if (!sensors_obd_ready()) return;

    if (!s_walk_done) {
        walk_step(now);
        return;
    }

#if CAIRN_MTPROBE_SNIFF
    if (!s_sniff_dead) {
        if (!s_sniff_scheduled) {
            s_sniff_scheduled = true;
            s_sniff_next_ms   = now + CAIRN_MTPROBE_SNIFF_FIRST_MS;
        } else if ((int32_t)(now - s_sniff_next_ms) >= 0) {
            s_sniff_next_ms = now + CAIRN_MTPROBE_SNIFF_EVERY_MS;
            sniff_window(now);
            return;
        }
    }
#endif

    probe_step(now);
}

#endif /* CAIRN_MTPROBE */
