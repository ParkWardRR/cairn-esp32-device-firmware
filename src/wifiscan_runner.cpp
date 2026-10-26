/* Wi-Fi position scans against the real radio. See wifiscan_runner.h. */

#include "wifiscan_runner.h"

#if CAIRN_WIFI_SCAN

#include <Arduino.h>
#include <WiFi.h>
#include <esp_coexist.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <string.h>

#include "cairn_log.h"
#include "cairn_wifiscan.h"
#include "config.h"
#include "lifecycle.h"
#include "uplink_runner.h"

static const char *TAG = "WSCAN";

static cairn_wifiscan_t        s_ws;
static bool                    s_running;
static cairn_wifiscan_reason_t s_reason;
static uint32_t                s_started_ms;
static uint32_t                s_heap_at_start;
static const char             *s_last_refusal;

/*
 * When the driver said the scan was done, stamped in the Wi-Fi event task. The tick that
 * notices can be seconds later -- a seal blocks the controller for about 2.5 s -- and the
 * frame's monotonic_ms is defined as the scan's completion, not the moment it was read.
 */
static volatile uint32_t s_done_event_ms;

static void on_scan_done(arduino_event_id_t)
{
    uint32_t t = millis();
    s_done_event_ms = t ? t : 1;
}

/*
 * The most access points one scan is read for. A city street can hear more; the strongest
 * sixteen are what the record keeps, and the radio's list is not promised to be in signal
 * order, so this is sized well above that rather than at it.
 *
 * Allocated per scan, from PSRAM where there is any, not static: the image's DRAM .bss had
 * under 2 KB to spare on 2026-10-25, and a static 2.3 KB array overflowed it.
 */
#define WSCAN_RAW_MAX 96

/*
 * Arduino keeps its own "a scan is running" bit, set by scanNetworks and cleared only by
 * the driver's SCAN_DONE event or by scanComplete's timeout. A scan this file stops early
 * may never post SCAN_DONE, and the bit left set makes the uplink's next scanNetworks()
 * return "already running" -- a home scan that silently never looks. Clearing it is
 * protected, so this reaches it the one way C++ allows.
 */
struct WiFiScanBits : public WiFiGenericClass {
    static void clear_scanning() { clearStatusBits(WIFI_SCANNING_BIT); }
};

static cairn_wifiscan_capture_t capture_of(const Lifecycle *lc)
{
    switch (lc->capture) {
    case CaptureState::Pretrip:  return CAIRN_WIFISCAN_PRETRIP;
    case CaptureState::Active:   return CAIRN_WIFISCAN_ACTIVE;
    case CaptureState::Trailing: return CAIRN_WIFISCAN_TRAILING;
    case CaptureState::Idle:
    default:                     return CAIRN_WIFISCAN_IDLE;
    }
}

static void gather(const Lifecycle *lc, cairn_wifiscan_evidence_t *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->capture  = capture_of(lc);
    /* The gate's accepted fixes only, never the phone's: see cairn_wifiscan.h. */
    ev->have_fix = lc->have_accepted_fix;
    ev->fix_ms   = lc->accepted_fix_ms;
    ev->radio_busy = uplink_runner_owns_wifi();
}

/*
 * Hand the radio back: results freed, the driver stopped, the arbiter on its balanced
 * split. WIFI_OFF is the state the uplink expects to find (wifi_link_connect and
 * run_home_scan both start from it), and it is the state that costs the car nothing.
 */
static void radio_off(void)
{
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
}

static void refuse(cairn_wifiscan_reason_t why, const char *reason, uint32_t now)
{
    /* Reported to the schedule as a scan that started and was cancelled, so the cadence
     * waits a full interval instead of asking again on the next tick. */
    cairn_wifiscan_started(&s_ws, why, now);
    cairn_wifiscan_cancelled(&s_ws);
    if (reason != s_last_refusal) {
        CAIRN_LOGW(TAG, "scan (%s) not started: %s", cairn_wifiscan_reason_name(why), reason);
        s_last_refusal = reason;
    }
}

static void start_scan(cairn_wifiscan_reason_t why)
{
    uint32_t now = millis();

    s_heap_at_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (s_heap_at_start < CAIRN_WIFISCAN_MIN_FREE_HEAP) {
        refuse(why, "internal heap below the floor for bringing Wi-Fi up beside BLE", now);
        return;
    }

    /*
     * BLE is NOT taken down, and the arbiter is held at its balanced split.
     *
     * An uplink slot deinits NimBLE because WPA2's four-way handshake has a deadline the
     * shared radio could not meet (reason 204, HANDSHAKE_TIMEOUT, every time with the
     * controller up, per wifi_link.cpp). A scan has no such deadline: it listens on each
     * channel for a dwell and moves on, and a beacon missed while BLE had the slot costs
     * one access point, not the scan. Tearing BLE down would instead cost the phone link
     * every ten seconds of a drive with no fix, and the deinit/reinit cycle is where the
     * documented InstrFetchProhibited crash lived (ble_companion.cpp).
     *
     * So: ESP_COEX_PREFER_BALANCE, explicitly, for the scan, rather than PREFER_WIFI as a
     * slot uses. Set here in case anything left the arbiter favouring Wi-Fi, and set again
     * by radio_off() afterwards.
     *
     * Power save is left at the Arduino default (modem sleep), as wifi_link.cpp documents:
     * WiFi.setSleep(false) aborts in pm_set_sleep_type whenever the BT controller is up,
     * which is exactly the condition here.
     *
     * UNVERIFIED ON HARDWARE (2026-10-25): that WiFi.mode(WIFI_STA) beside a live NimBLE
     * host is stable on this unit (the pre-deinit slot code did it, so it has run, but it
     * was never run repeatedly); that an encrypted phone connection survives a 2.6 s scan
     * under BALANCE; how many access points a passive scan hears under coexistence; and
     * the heap left with both stacks up.
     */
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

    if (!WiFi.mode(WIFI_STA)) {
        radio_off();
        refuse(why, "the Wi-Fi driver would not start", now);
        return;
    }

    s_done_event_ms = 0;
    s_started_ms = millis();
    int16_t r = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false,
                                  /*passive=*/CAIRN_WIFISCAN_ACTIVE_SCAN == 0,
                                  /*max_ms_per_chan=*/CAIRN_WIFISCAN_DWELL_MS);
    if (r != WIFI_SCAN_RUNNING) {
        radio_off();
        refuse(why, "the driver refused the scan", now);
        return;
    }

    cairn_wifiscan_started(&s_ws, why, now);
    s_running      = true;
    s_reason       = why;
    s_last_refusal = nullptr;
    CAIRN_LOGD(TAG, "scan started (%s), heap %u", cairn_wifiscan_reason_name(why),
               (unsigned)s_heap_at_start);
}

/* Stop a scan the driver may still be running, and wait briefly for it to say so. */
static void stop_scan(void)
{
    esp_wifi_scan_stop();
    uint32_t t0 = millis();
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t0 < 100) delay(5);
    WiFiScanBits::clear_scanning();
    radio_off();
}

/* Read the radio's list into the selection input. The SSID pointer is into the driver's
 * record and is only read by cairn_wifiscan_select, which never copies it. */
static size_t collect(cairn_wifiscan_raw_t *raw, int16_t n, uint16_t *beyond)
{
    size_t count = 0;
    *beyond = 0;
    for (int16_t i = 0; i < n; i++) {
        const wifi_ap_record_t *it =
            reinterpret_cast<const wifi_ap_record_t *>(WiFi.getScanInfoByIndex(i));
        if (it == nullptr) continue;
        if (count >= WSCAN_RAW_MAX) { (*beyond)++; continue; }

        cairn_wifiscan_raw_t *r = &raw[count++];
        memcpy(r->bssid, it->bssid, 6);
        r->rssi_dbm = it->rssi;
        r->channel  = it->primary;
        r->ssid     = it->ssid;
        r->ssid_len = strnlen(reinterpret_cast<const char *>(it->ssid), sizeof(it->ssid));
    }
    return count;
}

void wifiscan_runner_begin(void)
{
    cairn_wifiscan_init(&s_ws);
    WiFi.onEvent(on_scan_done, ARDUINO_EVENT_WIFI_SCAN_DONE);
    CAIRN_LOGI(TAG, "Wi-Fi position scans armed: %s, %u ms a channel; no fix: every %u s "
                    "in a trip, %u s parked; learning every %u s driving",
               CAIRN_WIFISCAN_ACTIVE_SCAN ? "active" : "passive",
               (unsigned)CAIRN_WIFISCAN_DWELL_MS,
               (unsigned)(CAIRN_WIFISCAN_TRIP_INTERVAL_MS / 1000),
               (unsigned)(CAIRN_WIFISCAN_PARKED_INTERVAL_MS / 1000),
               (unsigned)(CAIRN_WIFISCAN_LEARN_INTERVAL_MS / 1000));
}

bool wifiscan_runner_busy(void) { return s_running; }

void wifiscan_runner_cancel(const char *why)
{
    if (!s_running) return;
    stop_scan();
    s_running = false;
    cairn_wifiscan_cancelled(&s_ws);
    CAIRN_LOGI(TAG, "scan (%s) cancelled after %u ms: %s", cairn_wifiscan_reason_name(s_reason),
               (unsigned)(millis() - s_started_ms), why ? why : "unspecified");
}

bool wifiscan_runner_tick(const Lifecycle *lc, uint8_t *payload, size_t cap, size_t *len,
                          uint32_t *done_ms)
{
    cairn_wifiscan_evidence_t ev;
    gather(lc, &ev);
    uint32_t now = millis();

    if (!s_running) {
        cairn_wifiscan_reason_t why = cairn_wifiscan_tick(&s_ws, &ev, now);
        if (why != CAIRN_WIFISCAN_NONE) start_scan(why);
        return false;
    }

    /* Keep the schedule's trip edges current while a scan runs. It returns NONE. */
    (void)cairn_wifiscan_tick(&s_ws, &ev, now);

    /* Belt and braces: the uplink cancels before it takes the radio, but if it ever
     * holds it while a scan is still believed running, the scan yields. */
    if (ev.radio_busy) {
        wifiscan_runner_cancel("the uplink has the radio");
        return false;
    }

    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        if (now - s_started_ms >= CAIRN_WIFISCAN_TIMEOUT_MS)
            wifiscan_runner_cancel("no result within the timeout");
        return false;
    }

    uint32_t done = millis();
    uint32_t ev_ms = s_done_event_ms;
    if (ev_ms != 0 && (int32_t)(ev_ms - s_started_ms) >= 0 && (int32_t)(done - ev_ms) >= 0)
        done = ev_ms;
    uint32_t took = done - s_started_ms;

    if (n < 0) {
        /* The driver failed it, or Arduino's own timeout (20 dwells) fired first. Nothing
         * is recorded: a failed scan heard nothing and says nothing, unlike a scan that
         * completed and heard nothing, which is written with count 0. */
        stop_scan();
        s_running = false;
        cairn_wifiscan_cancelled(&s_ws);
        CAIRN_LOGW(TAG, "scan (%s) failed after %u ms (%d); nothing recorded",
                   cairn_wifiscan_reason_name(s_reason), (unsigned)took, (int)n);
        return false;
    }

    uint16_t beyond = 0;
    size_t   want   = (n < WSCAN_RAW_MAX) ? (size_t)n : WSCAN_RAW_MAX;
    cairn_wifiscan_raw_t *raw = nullptr;
    if (want > 0) {
        raw = static_cast<cairn_wifiscan_raw_t *>(
            heap_caps_malloc(want * sizeof(*raw), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (raw == nullptr)
            raw = static_cast<cairn_wifiscan_raw_t *>(malloc(want * sizeof(*raw)));
        if (raw == nullptr) {
            /* Recording count 0 here would claim the scan heard nothing. */
            stop_scan();
            s_running = false;
            cairn_wifiscan_cancelled(&s_ws);
            CAIRN_LOGW(TAG, "scan (%s): no memory to read %d access points; nothing recorded",
                       cairn_wifiscan_reason_name(s_reason), (int)n);
            return false;
        }
    }
    size_t count = raw ? collect(raw, (int16_t)want, &beyond) : 0;
    if (n > (int16_t)want) beyond = (uint16_t)(beyond + (n - (int16_t)want));

    /* Judged at completion: the instant the frame's monotonic_ms names. */
    bool learning = cairn_wifiscan_is_learning(&ev, done);
    uint8_t flags = (uint8_t)((learning ? CAIRN_WIFI_SCAN_LEARNING : 0) |
                              (CAIRN_WIFISCAN_ACTIVE_SCAN ? CAIRN_WIFI_SCAN_ACTIVE : 0));

    cairn_wifi_scan_t rec;
    cairn_wifiscan_stats_t st;
    cairn_wifiscan_select(raw, count, flags, took, &rec, &st);

    /* The driver's list, and the SSIDs it points at, are freed before anything is
     * written. */
    free(raw);
    radio_off();
    s_running = false;
    cairn_wifiscan_finished(&s_ws, learning);

    /* No BSSID and no SSID in the log: the record is sealed, the card log is not. */
    CAIRN_LOGI(TAG, "scan (%s): %u kept, %u filtered, %u duplicate, %u over the cap%s, "
                    "%u ms, %s, heap %u",
               cairn_wifiscan_reason_name(s_reason), (unsigned)st.kept,
               (unsigned)st.dropped_filter, (unsigned)st.dropped_dup,
               (unsigned)(st.dropped_cap + beyond), beyond ? " (list truncated)" : "",
               (unsigned)took, learning ? "learning" : "position", (unsigned)s_heap_at_start);

    if (cairn_encode_wifi_scan(&rec, payload, cap, len) != CAIRN_OK) {
        CAIRN_LOGE(TAG, "WIFI_SCAN would not encode into %u bytes", (unsigned)cap);
        return false;
    }
    *done_ms = done;
    return true;
}

#endif /* CAIRN_WIFI_SCAN */
