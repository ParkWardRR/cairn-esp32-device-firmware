/* Running the uplink schedule against the real radios. See uplink_runner.h. */

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

#if CAIRN_WIFI_UPLINK
#include <WiFi.h>
#include "wifi_link.h"
#endif
#if CAIRN_LTE_UPLINK
#include "lte_link.h"
#endif

static const char *TAG = "UPLINK";

static cairn_uplink_t s_u;
static bool           s_busy;

/*
 * The state the abort check needs, captured when a slot starts.
 *
 * It cannot consult the Lifecycle: the tick that would refresh it is the one
 * blocked on this transfer, so every field in there is frozen at the moment the
 * slot began — including the motion score that is supposed to stop it.
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
    ev->bundles_waiting = lc->pending_bundles;
    ev->phone_connected = ble_companion_connected();

    /*
     * Battery. Unknown is treated as acceptable rather than as a refusal: on
     * the bench and on a unit whose co-processor has not answered yet the
     * reading is the unavailable sentinel, and refusing to ever upload because
     * a voltage could not be read is the wrong direction — the caps and the
     * slot bound are what actually protect the car's battery.
     */
    ev->battery_ok = (lc->last_battery_mv == CAIRN_U16_UNKNOWN) ||
                     (lc->last_battery_mv >= CAIRN_UPLINK_BATTERY_FLOOR_MV);

    /*
     * Home is not asserted yet. The phone-asserted trigger is a field in a BLE
     * contract that is not released, and the no-phone scan is Wi-Fi radio use
     * that the schedule gates on its own rules. Until one of those exists, a
     * Wi-Fi slot is driven by the configured network being reachable, which is
     * what the bench proved; CAIRN_HOME_NONE keeps the manager honest about
     * which trigger it actually had.
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

#if CAIRN_WIFI_UPLINK
    /*
     * Wi-Fi is configured and works -- but not while BLE is initialised, which
     * in the production image is always.
     *
     * Measured on this unit, three times: with the Bluetooth controller up,
     * association fails in the WPA2 four-way handshake (STA_DISCONNECTED reason
     * 204, HANDSHAKE_TIMEOUT) after 25 s, every attempt. The same code
     * associates in ~2 s and has delivered four bundles in env:cairn-wifiup,
     * which halts before BLE starts. Two mitigations were tried and neither
     * helped: leaving Wi-Fi power save at its default (needed anyway -- turning
     * it off aborts under coexistence) and biasing the arbiter with
     * esp_coex_preference_set(ESP_COEX_PREFER_WIFI). Deinitialising NimBLE for
     * the slot does let Wi-Fi associate, but the next scan panics with
     * InstrFetchProhibited at PC 0, a call through a pointer into the freed
     * GATT objects.
     *
     * So the path is marked unavailable rather than left to burn 25 s and stop
     * BLE advertising once per parked session for nothing. LTE carries the data
     * in the car; Wi-Fi remains proven and usable through env:cairn-wifiup.
     * Re-enabling this needs either a safe NimBLE teardown or a slot scheduled
     * where BLE is already down -- the standby path is the obvious candidate.
     */
    ev->path[CAIRN_PATH_WIFI].configured = true;
    ev->path[CAIRN_PATH_WIFI].available  = false;
    ev->path[CAIRN_PATH_WIFI].allowed    = true;
#endif

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
        s_slot.why      = "the slot limit";
        return true;
    }

    /*
     * A rising supply rail is the earliest honest sign of an engine start, and
     * it is what the lifecycle itself latches a drive on. Checked against the
     * value when the slot began so a unit sitting on a steady 12.6 V rail is
     * not mistaken for one being started.
     */
    uint16_t supply = sensors_supply_mv_raw();
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
    s_slot.supply_at_start_mv = sensors_supply_mv_raw();
    s_slot.aborting           = false;
    s_slot.why                = nullptr;
}

/* ── the actions ──────────────────────────────────────────────────────────── */

#if CAIRN_WIFI_UPLINK
/*
 * Is the configured network in range?
 *
 * This is what makes a Wi-Fi slot possible at all. The schedule refuses to open
 * one unless home is established — by a phone assertion, which is an unreleased
 * BLE contract field, or by this scan. With neither, effective_home() is always
 * NONE and no slot ever starts, which is how a first attempt at this wiring
 * ended up with a transport that could never run.
 *
 * A scan is Wi-Fi radio use like any other, so BLE goes down for it, and the
 * manager rate-limits and bounds it (it runs only when parked with bundles
 * waiting, never during a trip, never with a phone connected).
 *
 * Only the configured SSID is looked for, and the result is a single bit. The
 * device learns nothing about where it is and stores no survey.
 */
__attribute__((unused)) static bool run_home_scan(uint32_t max_ms)
{
    CAIRN_LOGI(TAG, "home scan: looking for the configured network");

    ble_companion_radio_off();
    WiFi.mode(WIFI_STA);

    uint32_t t0 = millis();
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);

    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        if (WiFi.SSID(i) == CAIRN_WIFI_SSID) found = true;
    }

    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    ble_companion_radio_on();

    CAIRN_LOGI(TAG, "home scan: %s after %u ms (%d network(s) seen, bound %u ms)",
               found ? "FOUND the configured network" : "not at home",
               (unsigned)(millis() - t0), n, (unsigned)max_ms);
    return found;
}

static void run_wifi_slot(Lifecycle *lc, uint8_t slot_no, uint32_t limit_ms)
{
    CAIRN_LOGI(TAG, "Wi-Fi slot %u: up to %u ms", (unsigned)slot_no,
               (unsigned)limit_ms);

    cairn_slot_result_t r;
    memset(&r, 0, sizeof(r));

    slot_begin(limit_ms);
    s_busy = true;

    /*
     * One radio. BLE goes down first and comes back up after, and the phone has
     * already been told (the announce action ran before this one).
     */
    ble_companion_radio_off();

    uint32_t t0 = millis();
    if (wifi_link_connect(CAIRN_UPLINK_ASSOCIATE_MS)) {
        wifi_link_result_t w;
        wifi_link_upload_pending(CAIRN_UPLINK_BUNDLES_PER_SLOT, should_abort,
                                 nullptr, &w);
        wifi_link_disconnect();

        r.committed = w.delivered;
        r.failed    = w.failed + w.retained + w.refused;
        r.bytes     = w.bytes_up;
    } else {
        /* Could not associate: nothing attempted, and the manager's backoff is
         * what keeps this from being retried immediately. */
        r.failed = 1;
    }
    r.ms = millis() - t0;

    if (s_slot.aborting) {
        r.aborted_by = CAIRN_UL_REASON_TRIP;
        CAIRN_LOGI(TAG, "slot %u aborted: %s", (unsigned)slot_no,
                   s_slot.why ? s_slot.why : "unspecified");
    }

    ble_companion_radio_on();
    s_busy = false;

    cairn_uplink_slot_finished(&s_u, &r, millis());

    /* The count on the card changed; let the next tick see it promptly. */
    lc->next_pending_check_ms = millis();
}
#endif

#if CAIRN_LTE_UPLINK
static void run_lte_send(Lifecycle *lc)
{
    CAIRN_LOGI(TAG, "LTE send");

    slot_begin(CAIRN_UPLINK_LTE_LIMIT_MS);
    s_busy = true;

    /* LTE has its own radio and its own antenna, so BLE is left up: the phone
     * can stay connected throughout, and the modem's attach is unaffected by
     * the 2.4 GHz side. That is also why LTE succeeded on a bundle Wi-Fi lost. */
    bool ok = false;
    cairn_lte_status_t st = lte_link_up();
    if (st == CAIRN_LTE_OK) {
        /*
         * Record what the network says the time is, while the modem is up.
         *
         * This slot runs parked, after the trip's bundle has sealed, so the
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

    /*
     * The defaults are placeholders the module's own documentation says must
     * come from measurement (issue #19). Two are now measured rather than
     * guessed: cellular moves about 3 KB/s on this hardware, so a bundle of
     * around 230 KB needs roughly 80 seconds and a slot shorter than that would
     * never finish one.
     */
    cfg.slot_max_ms      = CAIRN_UPLINK_SLOT_MAX_MS;
    cfg.lte_after_trip_ms = CAIRN_UPLINK_LTE_AFTER_TRIP_MS;
    cfg.lte_auto         = true;

    cairn_uplink_init(&s_u, &cfg);

    CAIRN_LOGI(TAG, "uplink schedule armed: slot %u ms, %u bundle(s) per slot, "
                    "LTE %s, %u ms after a trip",
               (unsigned)cfg.slot_max_ms,
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

    case CAIRN_UL_ACT_ANNOUNCE_SLOT:
        /*
         * The wire format for this is an unreleased field in the BLE contract,
         * so there is nothing to send yet. Reported as done rather than left
         * pending: the alternative is a schedule that never advances, and the
         * cost of not announcing is a phone that treats the disconnect as a
         * failure, not lost data.
         */
        CAIRN_LOGI(TAG, "slot %u pending; the announce field is not in the "
                        "released BLE contract, so the phone is not told",
                   (unsigned)a.slot_no);
        cairn_uplink_slot_announced(&s_u);
        break;

    case CAIRN_UL_ACT_START_WIFI_SLOT:
#if CAIRN_WIFI_UPLINK
        cairn_uplink_slot_started(&s_u, millis());
        run_wifi_slot(lc, a.slot_no, a.max_ms ? a.max_ms : CAIRN_UPLINK_SLOT_MAX_MS);
#else
        /* No Wi-Fi in this build: report the slot finished with nothing done so
         * the manager backs off instead of asking again every tick. */
        cairn_uplink_slot_started(&s_u, millis());
        {
            cairn_slot_result_t r;
            memset(&r, 0, sizeof(r));
            r.failed = 1;
            cairn_uplink_slot_finished(&s_u, &r, millis());
        }
#endif
        break;

    case CAIRN_UL_ACT_ABORT_SLOT:
    case CAIRN_UL_ACT_FORCE_RADIO_OFF:
        /*
         * Reached only if a slot is somehow still believed to be running after
         * run_wifi_slot returned — it reports the slot finished itself, so the
         * manager normally has nothing to abort. Cutting the radio is still the
         * right response: the invariant is that Wi-Fi is down whenever BLE is
         * meant to be up.
         */
        CAIRN_LOGW(TAG, "radio forced off by the schedule");
#if CAIRN_WIFI_UPLINK
        wifi_link_disconnect();
#endif
        ble_companion_radio_on();
        {
            cairn_slot_result_t r;
            memset(&r, 0, sizeof(r));
            r.aborted_by = a.reason;
            cairn_uplink_slot_finished(&s_u, &r, millis());
        }
        break;

    case CAIRN_UL_ACT_CHECKIN:
        /* Same as the announce: the check-in message is contract surface that
         * is not released. The state transition still has to happen. */
        cairn_uplink_checkin_done(&s_u, millis());
        break;

    case CAIRN_UL_ACT_SCAN_HOME:
        /*
         * Reported as a miss, and deliberately so while the Wi-Fi path is
         * unavailable (see gather()). A scan hit marks the device "at home",
         * and being at home is what suppresses the LTE trigger -- so a
         * truthful scan would disable the only working network path to enable
         * one that cannot associate. run_home_scan is kept and tested: it
         * works, finding the configured network in about 1.7 s.
         */
        cairn_uplink_scan_started(&s_u, millis());
        cairn_uplink_scan_done(&s_u, false, millis());
        break;

    case CAIRN_UL_ACT_LTE_SEND:
#if CAIRN_LTE_UPLINK
        cairn_uplink_lte_started(&s_u);
        run_lte_send(lc);
#else
        cairn_uplink_lte_started(&s_u);
        cairn_uplink_lte_finished(&s_u, false, millis());
#endif
        break;
    }
}

#endif /* CAIRN_UPLINK_RUNNER */
