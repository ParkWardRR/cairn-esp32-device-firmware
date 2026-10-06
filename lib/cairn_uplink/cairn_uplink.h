/*
 * Uplink manager: which path moves a bundle, and when the radio may leave BLE.
 *
 * With three transports the dongle needs a policy, not three code paths. This module is
 * that policy, as portable C with the clock and every outside fact passed in, so the
 * whole schedule runs on the host against simulated links. It does no I/O itself: it
 * returns what to do, and the caller (the lifecycle) does it and reports back.
 *
 * The schedule is the owner's (issue #17, decided):
 *
 *   BLE is the home state. The dongle leaves it only for a bounded Wi-Fi slot, and
 *   always comes back to tell the phone what happened.
 *
 *     BLE -> [at a known home network, bundles waiting, parked, battery ok]
 *         -> Wi-Fi slot 1 -> BLE check-in
 *         -> Wi-Fi slot 2 (only if slot 1 left bundles uncommitted) -> BLE check-in
 *
 *   - One 2.4 GHz radio, time-sliced: BLE and Wi-Fi are NEVER used at once. LTE has its
 *     own modem and is not part of the slicing.
 *   - Capture and BLE-ready are never delayed. Wi-Fi is not on the boot path; a trip
 *     start (or low battery, or the slot limit) aborts a slot, and the radio is forced
 *     off if the caller has not returned within a bounded time.
 *   - The dongle announces a slot to the phone BEFORE leaving, so the phone does not
 *     treat the disconnect as a failure and start a reconnect storm.
 *   - Home is decided by the phone ("you may use Wi-Fi now", the dongle stores no
 *     location) or, with no phone present, by a very short scan for a provisioned
 *     network. A GNSS geofence on the chip is deliberately not an option.
 *
 * What this module does not decide: whether a credential may be stored (nothing secret
 * is written to flash until flash and NVS encryption exist, issue #18), what a path's
 * bytes look like (the uplink contract is not released), or when a bundle may be
 * pruned. Pruning happens only when the injected prune callback accepts a receipt, and
 * that callback is cairn_prune_if_receipted in the product: this module cannot delete
 * anything on its own authority.
 *
 * LTE is subject to the caps in lib/cairn_usage and sends digests by default; here it is
 * only a path with a gate (`allowed`) and a trigger (away from home, no phone, a while
 * after the trip). The proposed default trigger is the owner's, but "partly open" in the
 * issue: it is a config flag, not a constant.
 */

#ifndef CAIRN_UPLINK_H
#define CAIRN_UPLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAIRN_PATH_BLE = 0,
    CAIRN_PATH_WIFI,
    CAIRN_PATH_LTE,
    CAIRN_PATH_COUNT
} cairn_path_t;

const char *cairn_path_name(cairn_path_t p);

/* ── configuration ────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t slot_max_ms;            /* a Wi-Fi slot never outlasts this */
    uint8_t  slots_per_session;      /* slots in one parked session (the owner's: 2) */
    uint32_t min_slot_gap_ms;        /* between slots of one session, after the check-in */
    uint32_t checkin_wait_ms;        /* how long to wait for a phone to take the report */
    uint32_t abort_return_ms;        /* abort ordered -> back on BLE, or the radio is forced off */
    uint32_t backoff_initial_ms;     /* after a session that left bundles uncommitted */
    uint32_t backoff_max_ms;
    uint32_t scan_min_interval_ms;   /* the no-phone home scan is rate limited */
    uint32_t scan_max_ms;            /* and bounded: a few seconds of radio, not minutes */
    uint32_t scan_hit_ttl_ms;        /* how long a scan hit stays a valid home trigger */
    bool     lte_auto;               /* LTE fires by itself after a trip, when away */
    uint32_t lte_after_trip_ms;      /* ...this long after the trip ended */
    uint32_t lte_retry_ms;           /* minimum spacing of LTE attempts */
} cairn_uplink_config_t;

/* Defaults chosen to be cautious; the numbers are to come from measurement (issue #19). */
void cairn_uplink_config_defaults(cairn_uplink_config_t *c);

/* ── evidence ─────────────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_HOME_NONE = 0,
    CAIRN_HOME_PHONE_ASSERTED,   /* the phone says "you may use Wi-Fi now" */
    CAIRN_HOME_SCAN_HIT          /* the dongle's own short scan found a provisioned network */
} cairn_home_trigger_t;

typedef struct {
    bool     configured;       /* the path has what it needs (network list, APN) */
    bool     available;        /* usable now: phone connected / modem may attach */
    bool     allowed;          /* policy gate: user setting, caps, pause, roaming */
} cairn_path_info_t;

typedef struct {
    bool     trip_active;      /* any capture state but idle, including the trailing dwell */
    bool     parked;           /* idle long enough to count as parked */
    bool     battery_ok;       /* above the floor for radio work */
    uint32_t bundles_waiting;  /* sealed, not yet receipted */
    bool     phone_connected;  /* an enrolled phone is in a BLE session */
    cairn_home_trigger_t home; /* the phone's assertion, if any */
    bool     lte_full_bundles_ok; /* the user allows whole bundles over LTE (default: digests only) */
    cairn_path_info_t path[CAIRN_PATH_COUNT];
} cairn_uplink_evidence_t;

/* ── slot state machine ───────────────────────────────────────────────────── */

typedef enum {
    CAIRN_UL_BLE_HOME = 0,     /* advertising; the phone may pull bundles */
    CAIRN_UL_WIFI_SLOT,        /* the radio is on Wi-Fi; BLE is down */
    CAIRN_UL_WIFI_SCAN,        /* the radio is on a bounded scan for a provisioned network */
    CAIRN_UL_BLE_CHECKIN       /* back on BLE, reporting to the phone */
} cairn_uplink_state_t;

typedef enum {
    CAIRN_UL_ACT_NONE = 0,
    CAIRN_UL_ACT_ANNOUNCE_SLOT,   /* tell the phone: leaving for Wi-Fi for at most N s */
    CAIRN_UL_ACT_START_WIFI_SLOT, /* drop BLE and start the slot */
    CAIRN_UL_ACT_ABORT_SLOT,      /* stop the slot or scan and return to BLE now */
    CAIRN_UL_ACT_FORCE_RADIO_OFF, /* the abort was not honoured in time: cut Wi-Fi */
    CAIRN_UL_ACT_CHECKIN,         /* report to the phone and take its instructions */
    CAIRN_UL_ACT_SCAN_HOME,       /* a bounded Wi-Fi scan for a provisioned network */
    CAIRN_UL_ACT_LTE_SEND         /* start an LTE send (digest by default) */
} cairn_uplink_action_kind_t;

typedef enum {
    CAIRN_UL_REASON_NONE = 0,
    CAIRN_UL_REASON_HOME_PHONE,   /* a slot: the phone asserted home */
    CAIRN_UL_REASON_HOME_SCAN,    /* a slot: a scan found the network */
    CAIRN_UL_REASON_TRIP,         /* an abort: a trip started or the car woke */
    CAIRN_UL_REASON_BATTERY,
    CAIRN_UL_REASON_SLOT_LIMIT
} cairn_uplink_reason_t;

typedef struct {
    cairn_uplink_action_kind_t kind;
    cairn_uplink_reason_t      reason;
    uint8_t                    slot_no;      /* 1-based, for slot actions */
    uint32_t                   max_ms;       /* the bound announced to the phone / the scan */
} cairn_uplink_action_t;

typedef struct {
    uint32_t committed;        /* bundles that ended with a verified receipt this slot */
    uint32_t failed;
    uint32_t bytes;
    uint32_t ms;
    cairn_uplink_reason_t aborted_by;   /* NONE when the slot ended on its own */
} cairn_slot_result_t;

typedef struct {
    cairn_uplink_config_t cfg;
    cairn_uplink_state_t  state;

    /* session */
    bool     session_open;
    uint8_t  slots_used;
    bool     announced;
    bool     checkin_due;
    uint32_t checkin_since_ms;
    uint32_t slot_started_ms;
    bool     abort_ordered;
    bool     checkin_in_flight;
    uint32_t abort_ordered_ms;
    cairn_slot_result_t last_result;
    uint32_t next_slot_ok_ms;      /* min_slot_gap within a session */
    uint32_t next_session_ok_ms;   /* backoff between sessions */
    uint32_t backoff_ms;
    bool     progress_this_session;

    /* home scan */
    bool     scanned_ever;
    uint32_t last_scan_ms;
    uint32_t scan_started_ms;
    uint32_t scan_hit_until_ms;
    bool     scan_hit_valid;

    /* trip edge, for the LTE trigger */
    bool     trip_was_active;
    bool     trip_ended_known;
    uint32_t trip_ended_ms;

    /* LTE */
    bool     lte_in_flight;
    uint32_t lte_next_ok_ms;
    bool     lte_sent_since_trip;

    /* audit counters, for the reported state */
    uint32_t slots_total;
    uint32_t slots_aborted;
} cairn_uplink_t;

void cairn_uplink_init(cairn_uplink_t *u, const cairn_uplink_config_t *cfg);

/* The one decision function. Call it from the lifecycle tick. */
cairn_uplink_action_t cairn_uplink_tick(cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                        uint32_t now_ms);

/*
 * Report back. Every action tick() returns is a request: the caller does it and reports
 * it before the next tick, otherwise the same action is returned again.
 */
void cairn_uplink_slot_announced(cairn_uplink_t *u);
void cairn_uplink_slot_started(cairn_uplink_t *u, uint32_t now_ms);
void cairn_uplink_slot_finished(cairn_uplink_t *u, const cairn_slot_result_t *r, uint32_t now_ms);
void cairn_uplink_checkin_done(cairn_uplink_t *u, uint32_t now_ms);
void cairn_uplink_scan_started(cairn_uplink_t *u, uint32_t now_ms);
void cairn_uplink_scan_done(cairn_uplink_t *u, bool hit, uint32_t now_ms);
void cairn_uplink_lte_started(cairn_uplink_t *u);
void cairn_uplink_lte_finished(cairn_uplink_t *u, bool ok, uint32_t now_ms);

/* Whether the no-phone home scan may run now, and why not. The scan is a Wi-Fi radio
 * use like any other, so it obeys the same gates as a slot, plus its own rate limit. */
bool cairn_uplink_scan_allowed(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                               uint32_t now_ms);
const char *cairn_uplink_scan_blocker(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                      uint32_t now_ms);

/* True from the moment an abort is ordered until the slot or scan is reported finished.
 * A transfer's should_abort callback returns this, so one ordered abort stops every
 * bundle still queued in the slot, not only the one in flight. */
bool cairn_uplink_abort_pending(const cairn_uplink_t *u);

/* The radio is owned by exactly one of BLE and Wi-Fi at any moment. */
bool cairn_uplink_ble_may_advertise(const cairn_uplink_t *u);
bool cairn_uplink_wifi_may_run(const cairn_uplink_t *u);

const char *cairn_uplink_state_name(cairn_uplink_state_t s);

/* ── per-bundle delivery with fallback ────────────────────────────────────── */

typedef struct {
    uint8_t root[32];          /* the bundle's content root */
} cairn_bundle_ref_t;

typedef enum {
    CAIRN_XFER_RECEIPT = 0,    /* committed, and a receipt for this bundle came back */
    CAIRN_XFER_PARTIAL,        /* some chunks accepted; try again or elsewhere */
    CAIRN_XFER_FAILED,         /* nothing usable happened on this path */
    CAIRN_XFER_ABORTED         /* the caller's abort fired (trip, slot limit) */
} cairn_xfer_result_t;

typedef struct {
    uint32_t bytes;
    uint32_t chunks_sent;
} cairn_xfer_stats_t;

typedef struct {
    /*
     * Move one bundle on this path until a receipt is in hand, it fails, or `should_abort`
     * says stop. Resumes from the chunks the other side already holds (the offer returns
     * the missing set), so a second path or a second slot never starts over. `receipt`
     * receives the receipt bytes on CAIRN_XFER_RECEIPT; their meaning is the prune
     * callback's business, not this module's.
     */
    cairn_xfer_result_t (*transfer)(void *ctx, const cairn_bundle_ref_t *b,
                                    bool (*should_abort)(void *), void *abort_ctx,
                                    uint8_t *receipt, size_t receipt_cap, size_t *receipt_len,
                                    cairn_xfer_stats_t *st);
    void *ctx;
} cairn_transport_t;

typedef struct {
    cairn_transport_t path[CAIRN_PATH_COUNT];   /* transfer == NULL: path not present */

    /*
     * Verify the receipt against the pinned key AND the bundle's own content root, then
     * prune. True only when the bundle is verifiably gone from the card because a
     * genuine receipt for it said so. Anything else (forged, wrong root, no pinned key)
     * must return false and delete nothing: this is the receipt gate, and the delivery
     * loop trusts no transport's word that a bundle was committed.
     */
    bool (*prune_if_receipted)(void *ctx, const cairn_bundle_ref_t *b,
                               const uint8_t *receipt, size_t receipt_len);
    void *prune_ctx;
} cairn_uplink_io_t;

/* Which paths to try for a bundle now, best first. Returns the count (0..3). BLE and
 * Wi-Fi are never both in the list: the radio is time-sliced. */
size_t cairn_uplink_path_order(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                               cairn_path_t out[CAIRN_PATH_COUNT]);

typedef struct {
    bool     delivered;        /* a verified receipt pruned the bundle */
    bool     aborted;
    uint8_t  paths_tried;
    uint8_t  failures;
    uint32_t bytes;
} cairn_delivery_t;

/*
 * Try each usable path in order until the bundle ends with a receipt that the prune
 * callback accepts. A path that fails or returns a receipt the gate refuses is passed
 * over; the next resumes from the accepted chunks. The callback runs at most once per
 * bundle and only after a transport produced a receipt.
 */
cairn_delivery_t cairn_uplink_deliver(cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                      const cairn_uplink_io_t *io, const cairn_bundle_ref_t *b,
                                      bool (*should_abort)(void *), void *abort_ctx);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_UPLINK_H */
