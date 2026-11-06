/* Running the uplink schedule against the real modem. See uplink_runner.h. */

#include "uplink_runner.h"

#if CAIRN_UPLINK_RUNNER

#include <Arduino.h>
#include <string.h>

#include "ble_companion.h"
#include "cairn_log.h"
#include "cairn_uplink.h"
#include "config.h"
#include "lifecycle.h"
#include "sensor_task.h"
#include "sensors.h"
#if CAIRN_LTE_UPLINK
#include "lte_link.h"
#include "net_upload.h"
#endif

static const char *TAG = "UPLINK";

static cairn_uplink_t s_u;
static bool           s_busy;

/*
 * The state the abort check needs, captured when a send starts.
 *
 * It cannot consult the Lifecycle: the tick that would refresh it is the one
 * blocked on this transfer, so every field in there is frozen at the moment the
 * send began — including the motion score that is supposed to stop it.
 */
static struct {
    uint32_t started_ms;
    uint32_t limit_ms;
    uint16_t supply_at_start_mv;
    bool     aborting;
    const char *why;
} s_slot;

/* ── evidence ─────────────────────────────────────────────────────────────── */

static bool parked_enough(const Lifecycle *lc)
{
    return lc->capture == CaptureState::Idle &&
           lc->idle_since_ms != 0 &&
           (millis() - lc->idle_since_ms) >= CAIRN_PENDING_MIN_IDLE_MS;
}

static void gather(const Lifecycle *lc, cairn_uplink_evidence_t *ev)
{
    memset(ev, 0, sizeof(*ev));

    ev->trip_active     = (lc->capture != CaptureState::Idle);
    ev->parked          = parked_enough(lc);
    /* A bundle no session can open is not waiting to be sent (net_upload_unreadable_last). */
#if CAIRN_LTE_UPLINK
    uint32_t unreadable = net_upload_unreadable_last();
#else
    uint32_t unreadable = 0;
#endif
    ev->bundles_waiting = lc->pending_bundles > unreadable ? lc->pending_bundles - unreadable : 0;
    ev->phone_connected = ble_companion_connected();

    /*
     * Battery. Unknown is treated as acceptable rather than as a refusal: on
     * the bench and on a unit whose co-processor has not answered yet the
     * reading is the unavailable sentinel, and refusing to ever upload because
     * a voltage could not be read is the wrong direction — the caps and the
     * send bound are what actually protect the car's battery.
     */
    ev->battery_ok = (lc->last_battery_mv == CAIRN_U16_UNKNOWN) ||
                     (lc->last_battery_mv >= CAIRN_UPLINK_BATTERY_FLOOR_MV);

    /*
     * Home is not asserted yet. The phone-asserted trigger is a field in a BLE
     * contract that is not released; until it is wired, CAIRN_HOME_NONE keeps
     * the manager honest about which trigger it actually had. Its only effect
     * now is to hold LTE back when the phone says the car is home.
     */
    ev->home = CAIRN_HOME_NONE;

    /*
     * Whole bundles over cellular, not digests. A digest earns no receipt, so
     * it can never prune the card: a transport that only sends digests
     * postpones the problem it exists to solve. The measured cost is about
     * 230 KB a trip, which a 2 GB plan carries some nine thousand times.
     */
    ev->lte_full_bundles_ok = true;

    ev->path[CAIRN_PATH_BLE].configured = true;
    ev->path[CAIRN_PATH_BLE].available  = ev->phone_connected;
    ev->path[CAIRN_PATH_BLE].allowed    = true;

#if CAIRN_LTE_UPLINK
    ev->path[CAIRN_PATH_LTE].configured = true;
    ev->path[CAIRN_PATH_LTE].available  = !ev->trip_active;
    ev->path[CAIRN_PATH_LTE].allowed    = true;
#endif
}

/* ── the abort check ──────────────────────────────────────────────────────── */

/*
 * Should the transfer in progress stop?
 *
 * Polled by cairn_intake between chunks and between scratch-sized writes, so
 * this is the only thing standing between a long upload and a delayed capture.
 * It reads the sensors directly for the reason in the header.
 */
static bool should_abort(void *)
{
    if (s_slot.aborting) return true;

    uint32_t elapsed = millis() - s_slot.started_ms;
    if (elapsed >= s_slot.limit_ms) {
        s_slot.aborting = true;
        s_slot.why      = "the send limit";
        return true;
    }

    /*
     * A rising supply rail is the earliest honest sign of an engine start, and
     * it is what the lifecycle itself latches a drive on. Checked against the
     * value when the send began so a unit sitting on a steady 12.6 V rail is
     * not mistaken for one being started.
     */
    uint16_t supply = 0;
    sensor_task_supply(nullptr, &supply, nullptr);
    if (supply >= CAIRN_ENGINE_ON_MV &&
        s_slot.supply_at_start_mv < CAIRN_ENGINE_ON_MV) {
        s_slot.aborting = true;
        s_slot.why      = "the supply rail rose to engine-on";
        return true;
    }

    /* Real movement, from the accelerometer the sensing task is still reading
     * on the other core. */
    if (sensors_recent_accel_rms_mg() >= CAIRN_UPLINK_ABORT_RMS_MG) {
        s_slot.aborting = true;
        s_slot.why      = "the car moved";
        return true;
    }

    return false;
}

static void slot_begin(uint32_t limit_ms)
{
    s_slot.started_ms         = millis();
    s_slot.limit_ms           = limit_ms;
    s_slot.supply_at_start_mv = 0;
    sensor_task_supply(nullptr, &s_slot.supply_at_start_mv, nullptr);
    s_slot.aborting           = false;
    s_slot.why                = nullptr;
}

/* ── the actions ──────────────────────────────────────────────────────────── */

#if CAIRN_LTE_UPLINK
static void run_lte_send(Lifecycle *lc)
{
    CAIRN_LOGI(TAG, "LTE send");

    slot_begin(CAIRN_UPLINK_LTE_LIMIT_MS);
    s_busy = true;

    /* LTE has its own radio and its own antenna, so BLE is left up: the phone
     * can stay connected throughout, and the modem's attach is unaffected by
     * the 2.4 GHz side. */
    bool ok = false;
    cairn_lte_status_t st = lte_link_up();
    if (st == CAIRN_LTE_OK) {
        /*
         * Record what the network says the time is, while the modem is up.
         *
         * This send runs parked, after the trip's bundle has sealed, so the
         * observation cannot date the bundle about to be uploaded -- it lands in
         * the pre-roll and is written into the *next* capture, which means the
         * next trip is dated from its first frame instead of waiting on a GNSS
         * fix. That is the whole gap: the 2026-10-07 drive attached to LTE
         * perfectly and still decoded to 1970, because GNSS was the only clock
         * and its fix came 130 s in.
         *
         * Two seconds of claimed accuracy, not zero: NITZ is good to about a
         * second and the AT round trip adds more, and a source that overstates
         * itself would win comparisons it should lose.
         */
        uint64_t net_ms = lte_link_network_unix_ms();
        if (net_ms != 0) {
            fact_t t;
            memset(&t, 0, sizeof(t));
            t.kind = FACT_TIME_OBSERVATION;
            t.monotonic_ms = millis();
            t.data.utc.utc_ms = net_ms;
            t.data.utc.acc_ms = 2000;
            t.data.utc.source = CAIRN_TIME_SRC_MODEM_NETWORK;
            sensor_task_post_fact(&t);
        }

        lte_link_result_t l;
        lte_link_upload_pending(CAIRN_UPLINK_BUNDLES_PER_SLOT, should_abort,
                                nullptr, &l);
        ok = (l.delivered > 0);
    } else {
        CAIRN_LOGW(TAG, "LTE not usable: %s", cairn_lte_status_name(st));
    }
    lte_link_down();

    s_busy = false;
    cairn_uplink_lte_finished(&s_u, ok, millis());
    lc->next_pending_check_ms = millis();
}
#endif

/* ── the loop ─────────────────────────────────────────────────────────────── */

void uplink_runner_begin(void)
{
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);

    cfg.lte_after_trip_ms = CAIRN_UPLINK_LTE_AFTER_TRIP_MS;
    cfg.lte_auto          = true;

    cairn_uplink_init(&s_u, &cfg);

    CAIRN_LOGI(TAG, "uplink schedule armed: %u bundle(s) per send, LTE %s, %u ms after a trip",
               (unsigned)CAIRN_UPLINK_BUNDLES_PER_SLOT,
               cfg.lte_auto ? "automatic" : "manual",
               (unsigned)cfg.lte_after_trip_ms);
}

bool uplink_runner_busy(void) { return s_busy; }

void uplink_runner_tick(Lifecycle *lc)
{
    cairn_uplink_evidence_t ev;
    gather(lc, &ev);

    cairn_uplink_action_t a = cairn_uplink_tick(&s_u, &ev, millis());

    switch (a.kind) {
    case CAIRN_UL_ACT_NONE:
        break;

    case CAIRN_UL_ACT_LTE_SEND:
#if CAIRN_LTE_UPLINK
        cairn_uplink_lte_started(&s_u);
        run_lte_send(lc);
#else
        /* No LTE in this build: report the send finished with nothing done so
         * the manager spaces its retries instead of asking again every tick. */
        cairn_uplink_lte_started(&s_u);
        cairn_uplink_lte_finished(&s_u, false, millis());
#endif
        break;
    }
}

#endif /* CAIRN_UPLINK_RUNNER */
