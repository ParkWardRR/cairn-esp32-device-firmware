/* Wi-Fi position scans. See cairn_wifiscan.h. */

#include "cairn_wifiscan.h"

#include <string.h>

void cairn_wifiscan_init(cairn_wifiscan_t *s)
{
    memset(s, 0, sizeof(*s));
    s->last_capture = CAIRN_WIFISCAN_IDLE;
}

/* now - then, or 0 when `then` is (by a tick's worth of skew) in the future: the
 * sensing task stamps a fix on its own core, and the controller reads its clock after. */
static uint32_t age_ms(uint32_t now, uint32_t then)
{
    return ((int32_t)(now - then) < 0) ? 0 : now - then;
}

static void track_edges(cairn_wifiscan_t *s, cairn_wifiscan_capture_t cur, uint32_t now)
{
    cairn_wifiscan_capture_t prev = s->last_capture;

    /* Confirmation, not suspicion: a pretrip that is rejected was never a trip. */
    if (cur == CAIRN_WIFISCAN_ACTIVE &&
        (prev == CAIRN_WIFISCAN_IDLE || prev == CAIRN_WIFISCAN_PRETRIP)) {
        s->start_owed    = true;
        s->start_edge_ms = now;
    }
    if (cur == CAIRN_WIFISCAN_TRAILING && prev == CAIRN_WIFISCAN_ACTIVE) {
        s->end_owed    = true;
        s->end_edge_ms = now;
    }

    /*
     * An owed scan is only worth anything in the state that owes it. A start scan owed
     * when the trip has already stopped is not a start scan; an end scan owed when the
     * car moved off again (a long red light) is not an end; and one owed after the trip
     * sealed would land in the next capture's pre-roll, which evicts it.
     */
    if (cur != CAIRN_WIFISCAN_ACTIVE) s->start_owed = false;
    if (cur != CAIRN_WIFISCAN_TRAILING) s->end_owed = false;

    if (s->start_owed && age_ms(now, s->start_edge_ms) >= CAIRN_WIFISCAN_EDGE_WINDOW_MS)
        s->start_owed = false;
    if (s->end_owed && age_ms(now, s->end_edge_ms) >= CAIRN_WIFISCAN_EDGE_WINDOW_MS)
        s->end_owed = false;

    s->last_capture = cur;
}

cairn_wifiscan_reason_t cairn_wifiscan_tick(cairn_wifiscan_t *s,
                                            const cairn_wifiscan_evidence_t *ev,
                                            uint32_t now_ms)
{
    track_edges(s, ev->capture, now_ms);

    if (s->in_progress) return CAIRN_WIFISCAN_NONE;

    const bool     trip = (ev->capture != CAIRN_WIFISCAN_IDLE);
    const uint32_t gap  = s->scanned_ever ? age_ms(now_ms, s->last_start_ms) : UINT32_MAX;
    const uint32_t fix_age = ev->have_fix ? age_ms(now_ms, ev->fix_ms) : UINT32_MAX;

    cairn_wifiscan_reason_t why = CAIRN_WIFISCAN_NONE;

    if (fix_age >= CAIRN_WIFISCAN_NO_FIX_MS) {
        uint32_t interval = trip ? CAIRN_WIFISCAN_TRIP_INTERVAL_MS
                                 : CAIRN_WIFISCAN_PARKED_INTERVAL_MS;
        if (gap >= interval)
            why = trip ? CAIRN_WIFISCAN_NO_FIX_TRIP : CAIRN_WIFISCAN_NO_FIX_PARKED;
    } else if (fix_age <= CAIRN_WIFISCAN_LEARN_FIX_MS && gap >= CAIRN_WIFISCAN_MIN_GAP_MS) {
        /* Between 2 and 5 s old is neither: too stale to anchor an access point, too
         * fresh to call the position lost. */
        if (s->start_owed)
            why = CAIRN_WIFISCAN_LEARN_TRIP_START;
        else if (s->end_owed)
            why = CAIRN_WIFISCAN_LEARN_TRIP_END;
        else if (ev->capture == CAIRN_WIFISCAN_ACTIVE && gap >= CAIRN_WIFISCAN_LEARN_INTERVAL_MS)
            why = CAIRN_WIFISCAN_LEARN_CADENCE;
    }

    if (why != CAIRN_WIFISCAN_NONE && ev->radio_busy) {
        /* Held, not dropped: still due on the first tick the radio is free. */
        s->refused_busy++;
        return CAIRN_WIFISCAN_NONE;
    }
    return why;
}

void cairn_wifiscan_started(cairn_wifiscan_t *s, cairn_wifiscan_reason_t why, uint32_t now_ms)
{
    s->in_progress   = true;
    s->scanned_ever  = true;
    s->last_start_ms = now_ms;
    s->scans++;

    /* Any learning scan settles what the current state owed: a cadence scan in the first
     * seconds of a drive is the trip-start scan, there is no need for two. A position
     * scan settles nothing, because it had no fix to pair with. */
    if (cairn_wifiscan_reason_is_learning(why)) {
        s->start_owed = false;
        s->end_owed   = false;
    }
}

void cairn_wifiscan_finished(cairn_wifiscan_t *s, bool learning)
{
    s->in_progress = false;
    if (learning) s->learning_scans++;
}

void cairn_wifiscan_cancelled(cairn_wifiscan_t *s)
{
    s->in_progress = false;
    s->cancelled++;
}

bool cairn_wifiscan_is_learning(const cairn_wifiscan_evidence_t *ev, uint32_t now_ms)
{
    return ev->have_fix && age_ms(now_ms, ev->fix_ms) <= CAIRN_WIFISCAN_LEARN_FIX_MS;
}

bool cairn_wifiscan_reason_is_learning(cairn_wifiscan_reason_t r)
{
    return r == CAIRN_WIFISCAN_LEARN_CADENCE || r == CAIRN_WIFISCAN_LEARN_TRIP_START ||
           r == CAIRN_WIFISCAN_LEARN_TRIP_END;
}

const char *cairn_wifiscan_reason_name(cairn_wifiscan_reason_t r)
{
    switch (r) {
    case CAIRN_WIFISCAN_NONE:             return "none";
    case CAIRN_WIFISCAN_NO_FIX_TRIP:      return "no fix, trip";
    case CAIRN_WIFISCAN_NO_FIX_PARKED:    return "no fix, parked";
    case CAIRN_WIFISCAN_LEARN_CADENCE:    return "learning, driving";
    case CAIRN_WIFISCAN_LEARN_TRIP_START: return "learning, trip start";
    case CAIRN_WIFISCAN_LEARN_TRIP_END:   return "learning, trip end";
    default:                              return "?";
    }
}

/* ── selection ────────────────────────────────────────────────────────────── */

static bool ends_with_ci(const uint8_t *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    if (s == NULL || len < n) return false;
    const uint8_t *tail = s + (len - n);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = tail[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
        if (c != (uint8_t)suffix[i]) return false;
    }
    return true;
}

bool cairn_wifiscan_ap_allowed(const cairn_wifiscan_raw_t *ap)
{
    /* Locally administered: a phone hotspot or a randomised address. It moves with its
     * owner, so it says nothing about a place and something about a person. */
    if (ap->bssid[0] & 0x02) return false;

    /*
     * The owner's request not to be mapped. Case-insensitive, which is stricter than the
     * conventions are written: honouring "_NoMap" costs one access point, ignoring it
     * would record a network whose owner asked not to be.
     */
    if (ends_with_ci(ap->ssid, ap->ssid_len, "_nomap")) return false;
    if (ends_with_ci(ap->ssid, ap->ssid_len, "_optout")) return false;
    return true;
}

/* Strongest first; equal signals by BSSID, so one set of access points always encodes
 * to one record. */
static bool better(const cairn_wifi_ap_t *a, const cairn_wifi_ap_t *b)
{
    if (a->rssi_dbm != b->rssi_dbm) return a->rssi_dbm > b->rssi_dbm;
    return memcmp(a->bssid, b->bssid, 6) < 0;
}

/* Whether raw[i] is the occurrence of its BSSID that is kept: the strongest, the first
 * of equals. */
static bool is_representative(const cairn_wifiscan_raw_t *raw, size_t n, size_t i)
{
    for (size_t j = 0; j < n; j++) {
        if (j == i || memcmp(raw[j].bssid, raw[i].bssid, 6) != 0) continue;
        if (!cairn_wifiscan_ap_allowed(&raw[j])) continue;
        if (raw[j].rssi_dbm > raw[i].rssi_dbm) return false;
        if (raw[j].rssi_dbm == raw[i].rssi_dbm && j < i) return false;
    }
    return true;
}

void cairn_wifiscan_select(const cairn_wifiscan_raw_t *raw, size_t n, uint8_t flags,
                           uint32_t scan_ms, cairn_wifi_scan_t *out,
                           cairn_wifiscan_stats_t *stats)
{
    cairn_wifiscan_stats_t st;
    memset(&st, 0, sizeof(st));
    memset(out, 0, sizeof(*out));

    /* Bits 2-7 are reserved and zero in this version of the record. */
    out->flags   = (uint8_t)(flags & (CAIRN_WIFI_SCAN_LEARNING | CAIRN_WIFI_SCAN_ACTIVE));
    out->scan_ms = (scan_ms > 0xFFFFu) ? 0xFFFFu : (uint16_t)scan_ms;

    size_t candidates = 0;
    for (size_t i = 0; i < n; i++) {
        st.heard = (uint16_t)(st.heard + 1);

        if (!cairn_wifiscan_ap_allowed(&raw[i])) {
            st.dropped_filter = (uint16_t)(st.dropped_filter + 1);
            continue;
        }
        if (!is_representative(raw, n, i)) {
            st.dropped_dup = (uint16_t)(st.dropped_dup + 1);
            continue;
        }
        candidates++;

        /* Only the BSSID, the signal and the channel are copied: the SSID stops here. */
        cairn_wifi_ap_t c;
        memcpy(c.bssid, raw[i].bssid, 6);
        c.rssi_dbm = raw[i].rssi_dbm;
        c.channel  = raw[i].channel;

        size_t pos = out->count;
        while (pos > 0 && better(&c, &out->ap[pos - 1])) pos--;
        if (pos >= CAIRN_WIFI_SCAN_MAX_APS) continue;

        size_t last = (out->count < CAIRN_WIFI_SCAN_MAX_APS) ? out->count
                                                             : CAIRN_WIFI_SCAN_MAX_APS - 1;
        for (size_t k = last; k > pos; k--) out->ap[k] = out->ap[k - 1];
        out->ap[pos] = c;
        if (out->count < CAIRN_WIFI_SCAN_MAX_APS) out->count++;
    }

    st.kept        = out->count;
    st.dropped_cap = (uint16_t)(candidates - out->count);
    if (stats != NULL) *stats = st;
}
