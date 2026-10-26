/*
 * Wi-Fi position scans: when to listen for access points, and which ones to keep.
 *
 * GNSS is the only position source the dongle has, and it is absent exactly where a
 * trip starts: a cold receiver takes a minute or two (the 2026-10-07 drive had its first
 * fix 130 s in), and a garage gives none at all. The access points around the car are
 * heard in all of those places. A WIFI_SCAN record (format v3 section 4.13) carries what
 * one scan heard, and the server turns it into a position estimate -- from the owner's
 * known places first, then from access-point positions it learned beside trusted fixes.
 *
 * So two kinds of scan, and this module decides both:
 *
 *   POSITION  no accepted fix for CAIRN_WIFISCAN_NO_FIX_MS, or none since boot:
 *               - every CAIRN_WIFISCAN_TRIP_INTERVAL_MS while a trip is suspected,
 *                 confirmed or trailing (the garage exit, the cold start);
 *               - every CAIRN_WIFISCAN_PARKED_INTERVAL_MS while idle.
 *   LEARNING  an accepted fix no older than CAIRN_WIFISCAN_LEARN_FIX_MS:
 *               - every CAIRN_WIFISCAN_LEARN_INTERVAL_MS while driving;
 *               - once at trip start (confirmation) and once at trip end (the car
 *                 stopping, i.e. entering the trailing dwell -- the moment it is parked
 *                 at its destination, and while the record still lands in the trip's
 *                 own bundle rather than the next one's pre-roll).
 *
 * And never while the uplink owns the Wi-Fi radio (a slot or its home scan): the radio
 * has one owner at a time, and the uplink's transfer has a deadline a scan does not.
 *
 * "Accepted" means what lib/cairn_gnss means: a fix that passed the plausibility gate.
 * The phone's fixes are deliberately not counted. A phone in a garage reports a position
 * computed from the very access points this scan hears, and learning their positions
 * from it would be circular.
 *
 * Privacy is enforced here, at selection, before anything reaches the payload: no SSID is
 * ever copied out of cairn_wifiscan_select, and an access point is dropped when its BSSID
 * is locally administered (bit 1 of the first octet: phone hotspots and randomised
 * addresses, which move with their owner) or its SSID ends in "_nomap" or "_optout" (its
 * owner's request not to be mapped).
 *
 * Portable C, the clock passed in; host-tested in test/host/wifiscan_test.c. The radio
 * itself is src/wifiscan_runner.cpp.
 */
#ifndef CAIRN_WIFISCAN_H
#define CAIRN_WIFISCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cairn_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAIRN_WIFISCAN_NO_FIX_MS            5000u    /* no accepted fix this long: scan for position */
#define CAIRN_WIFISCAN_TRIP_INTERVAL_MS     10000u   /* ...this often during a trip */
#define CAIRN_WIFISCAN_PARKED_INTERVAL_MS   300000u  /* ...and this often parked */
#define CAIRN_WIFISCAN_LEARN_FIX_MS         2000u    /* a fix this fresh makes a learning scan */
#define CAIRN_WIFISCAN_LEARN_INTERVAL_MS    60000u   /* learning cadence while driving */
/* A trip-start or trip-end learning scan still owed this long after the edge is dropped:
 * five minutes into a drive is not its start, and the cadence covers it anyway. */
#define CAIRN_WIFISCAN_EDGE_WINDOW_MS       30000u
/* No two scans closer than this, whatever asks for them. The phone link shares the radio. */
#define CAIRN_WIFISCAN_MIN_GAP_MS           10000u

/* The capture state, as far as scanning cares. Matches the lifecycle's CaptureState. */
typedef enum {
    CAIRN_WIFISCAN_IDLE = 0,
    CAIRN_WIFISCAN_PRETRIP,
    CAIRN_WIFISCAN_ACTIVE,
    CAIRN_WIFISCAN_TRAILING,
} cairn_wifiscan_capture_t;

typedef struct {
    cairn_wifiscan_capture_t capture;
    bool     have_fix;        /* an accepted GNSS fix exists since boot */
    uint32_t fix_ms;          /* monotonic time that fix was taken */
    bool     radio_busy;      /* the uplink owns the Wi-Fi radio, or BLE is down for it */
} cairn_wifiscan_evidence_t;

typedef enum {
    CAIRN_WIFISCAN_NONE = 0,
    CAIRN_WIFISCAN_NO_FIX_TRIP,     /* position: no fix, trip suspected/confirmed/trailing */
    CAIRN_WIFISCAN_NO_FIX_PARKED,   /* position: no fix, idle */
    CAIRN_WIFISCAN_LEARN_CADENCE,   /* learning: fresh fix, driving */
    CAIRN_WIFISCAN_LEARN_TRIP_START,
    CAIRN_WIFISCAN_LEARN_TRIP_END,
} cairn_wifiscan_reason_t;

typedef struct {
    cairn_wifiscan_capture_t last_capture;
    bool     scanned_ever;
    uint32_t last_start_ms;
    bool     in_progress;

    bool     start_owed;          /* a trip-start learning scan is owed */
    uint32_t start_edge_ms;
    bool     end_owed;            /* a trip-end learning scan is owed */
    uint32_t end_edge_ms;

    /* audit counters */
    uint32_t scans;
    uint32_t learning_scans;
    uint32_t cancelled;
    uint32_t refused_busy;        /* a scan was due and the uplink had the radio */
} cairn_wifiscan_t;

void cairn_wifiscan_init(cairn_wifiscan_t *s);

/* The one decision. Call it every lifecycle tick; NONE when nothing is due. Tracks the
 * trip edges itself from ev->capture, so it must see every tick's state. */
cairn_wifiscan_reason_t cairn_wifiscan_tick(cairn_wifiscan_t *s,
                                            const cairn_wifiscan_evidence_t *ev,
                                            uint32_t now_ms);

/* Report back: the scan the tick asked for has started / completed / been cancelled.
 * A cancelled scan still counts as the last scan for the cadence, so an uplink slot that
 * cancels one does not cause an immediate retry the moment it ends. */
void cairn_wifiscan_started(cairn_wifiscan_t *s, cairn_wifiscan_reason_t why, uint32_t now_ms);
void cairn_wifiscan_finished(cairn_wifiscan_t *s, bool learning);
void cairn_wifiscan_cancelled(cairn_wifiscan_t *s);

/* Whether a scan completing at now_ms is a learning scan: flag bit 0 of the record.
 * Judged at completion, the instant the frame's monotonic_ms names. */
bool cairn_wifiscan_is_learning(const cairn_wifiscan_evidence_t *ev, uint32_t now_ms);

bool cairn_wifiscan_reason_is_learning(cairn_wifiscan_reason_t r);
const char *cairn_wifiscan_reason_name(cairn_wifiscan_reason_t r);

/* ── selection ────────────────────────────────────────────────────────────── */

/* One access point as the radio reported it. The SSID is read, never copied. */
typedef struct {
    uint8_t        bssid[6];
    int8_t         rssi_dbm;
    uint8_t        channel;
    const uint8_t *ssid;      /* not necessarily NUL-terminated; may be NULL */
    size_t         ssid_len;
} cairn_wifiscan_raw_t;

typedef struct {
    uint16_t heard;           /* raw entries offered */
    uint16_t dropped_filter;  /* locally administered, _nomap, _optout */
    uint16_t dropped_dup;     /* the same BSSID twice; the stronger one is kept */
    uint16_t dropped_cap;     /* weaker than the 16 kept */
    uint16_t kept;
} cairn_wifiscan_stats_t;

/* Whether this access point may be recorded at all. */
bool cairn_wifiscan_ap_allowed(const cairn_wifiscan_raw_t *ap);

/*
 * Filter, de-duplicate, order strongest first (ties by BSSID, so the record is
 * deterministic) and keep CAIRN_WIFI_SCAN_MAX_APS. `out` receives no SSID: it has no
 * field to hold one. stats may be NULL.
 */
void cairn_wifiscan_select(const cairn_wifiscan_raw_t *raw, size_t n, uint8_t flags,
                           uint32_t scan_ms, cairn_wifi_scan_t *out,
                           cairn_wifiscan_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
