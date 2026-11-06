/*
 * Uplink manager: which path moves a bundle, and when LTE may spend the owner's data.
 *
 * Two transports (front door commit 2003c62): BLE to the phone, which is the home
 * state and the everyday path, and LTE for a car parked away from its phone. This
 * module is the policy, as portable C with the clock and every outside fact passed
 * in, so the whole schedule runs on the host against simulated links. It does no
 * I/O itself: it returns what to do, and the caller (the lifecycle) does it and
 * reports back.
 *
 * There was a third transport. Wi-Fi slots -- the dongle leaving BLE for a bounded
 * visit to a home network -- were deleted with the rest of the Wi-Fi stack: the
 * ESP32 has one 2.4 GHz radio, so every slot tore the BLE stack down and the phone
 * watched the dongle vanish every ~30 s. The slot/scan/check-in state machine,
 * the home scan and the session backoff went with it. LTE has its own modem and
 * never contends with BLE, so what remains needs no radio arbitration at all:
 * BLE may always advertise.
 *
 * What this module does not decide: what a path's bytes look like, or when a
 * bundle may be pruned. Pruning happens only when the injected prune callback
 * accepts a receipt, and that callback is cairn_prune_if_receipted in the
 * product: this module cannot delete anything on its own authority.
 *
 * LTE is subject to the caps in lib/cairn_usage and sends digests by default; here
 * it is only a path with a gate (`allowed`) and a trigger (away from home, no
 * phone, a while after the trip). The proposed default trigger is the owner's
 * (issue #17), but "partly open": it is a config flag, not a constant.
 */

#ifndef CAIRN_UPLINK_H
#define CAIRN_UPLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * In-memory path indices; never serialized. (The wire constants for uplink
 * events are CAIRN_UE_PATH_* in lib/cairn_devinfo, where Wi-Fi keeps its
 * contract value.) Wi-Fi used to sit between these two.
 */
typedef enum {
    CAIRN_PATH_BLE = 0,
    CAIRN_PATH_LTE,
    CAIRN_PATH_COUNT
} cairn_path_t;

const char *cairn_path_name(cairn_path_t p);

/* ── configuration ────────────────────────────────────────────────────────── */

typedef struct {
    bool     lte_auto;               /* LTE fires by itself after a trip, when away */
    uint32_t lte_after_trip_ms;      /* ...this long after the trip ended */
    uint32_t lte_retry_ms;           /* minimum spacing of LTE attempts */
} cairn_uplink_config_t;

/* Defaults chosen to be cautious; the numbers are to come from measurement (issue #19). */
void cairn_uplink_config_defaults(cairn_uplink_config_t *c);

/* ── evidence ─────────────────────────────────────────────────────────────── */

/*
 * "The phone says the car is home" (the HOME_TRIGGER message, ble/v1/checkin.md
 * §3). It used to open Wi-Fi slots; now it only holds LTE back -- at home the
 * phone fetches bundles for free, so cellular would spend data for nothing.
 * Value 2 was CAIRN_HOME_SCAN_HIT, the dongle's own Wi-Fi scan: removed.
 */
typedef enum {
    CAIRN_HOME_NONE = 0,
    CAIRN_HOME_PHONE_ASSERTED = 1
} cairn_home_trigger_t;

typedef struct {
    bool     configured;       /* the path has what it needs (APN, pinned keys) */
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

/* ── the schedule ─────────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_UL_ACT_NONE     = 0,
    CAIRN_UL_ACT_LTE_SEND = 7   /* explicit: the value it had beside the Wi-Fi slot actions */
} cairn_uplink_action_kind_t;

typedef struct {
    cairn_uplink_action_kind_t kind;
} cairn_uplink_action_t;

typedef struct {
    cairn_uplink_config_t cfg;

    /* trip edge, for the LTE trigger */
    bool     trip_was_active;
    bool     trip_ended_known;
    uint32_t trip_ended_ms;

    /* LTE */
    bool     lte_in_flight;
    uint32_t lte_next_ok_ms;
    bool     lte_sent_since_trip;
} cairn_uplink_t;

void cairn_uplink_init(cairn_uplink_t *u, const cairn_uplink_config_t *cfg);

/* The one decision function. Call it from the lifecycle tick. */
cairn_uplink_action_t cairn_uplink_tick(cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                        uint32_t now_ms);

/*
 * Report back. Every action tick() returns is a request: the caller does it and reports
 * it before the next tick, otherwise the same action is returned again.
 */
void cairn_uplink_lte_started(cairn_uplink_t *u);
void cairn_uplink_lte_finished(cairn_uplink_t *u, bool ok, uint32_t now_ms);

/* ── per-bundle delivery with fallback ────────────────────────────────────── */

typedef struct {
    uint8_t root[32];          /* the bundle's content root */
} cairn_bundle_ref_t;

typedef enum {
    CAIRN_XFER_RECEIPT = 0,    /* committed, and a receipt for this bundle came back */
    CAIRN_XFER_PARTIAL,        /* some chunks accepted; try again or elsewhere */
    CAIRN_XFER_FAILED,         /* nothing usable happened on this path */
    CAIRN_XFER_ABORTED         /* the caller's abort fired (trip start) */
} cairn_xfer_result_t;

typedef struct {
    uint32_t bytes;
    uint32_t chunks_sent;
} cairn_xfer_stats_t;

typedef struct {
    /*
     * Move one bundle on this path until a receipt is in hand, it fails, or `should_abort`
     * says stop. Resumes from the chunks the other side already holds (the offer returns
     * the missing set), so a second path or a second attempt never starts over. `receipt`
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

/* Which paths to try for a bundle now, best first. Returns the count (0..2):
 * BLE when a phone is connected, then LTE when the user allows whole bundles. */
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
