/* The Wi-Fi uplink. See wifi_link.h. */

#include "wifi_link.h"

#if CAIRN_WIFI_UPLINK

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_coexist.h>
#include <string.h>

#include "board_config.h"
#include "cairn_fs.h"
#include "cairn_intake.h"
#include "cairn_log.h"
#include "cairn_store.h"
#include "config.h"
#include "net_http.h"
#include "net_upload.h"

static const char *TAG = "WIFI";

#if !defined(CAIRN_WIFI_SSID) || !defined(CAIRN_SERVER_HOST)
#error "CAIRN_WIFI_UPLINK needs CAIRN_WIFI_SSID and CAIRN_SERVER_HOST in secrets.h"
#endif
#if !defined(CAIRN_SERVER_CA_PEM) || !defined(CAIRN_CLIENT_CERT_PEM)
#error "CAIRN_WIFI_UPLINK needs CAIRN_SERVER_CA_PEM and a client certificate in secrets.h"
#endif

/*
 * Static, not per-transfer. WiFiClientSecure carries an mbedTLS context, and a
 * transfer that allocated it would be the allocation most likely to fail at the
 * moment it matters most. The bundle, HTTP and scratch buffers live in
 * net_upload.cpp, shared with the LTE path.
 */
static WiFiClientSecure s_tls;

/* ── association ──────────────────────────────────────────────────────────── */

bool wifi_link_connect(uint32_t timeout_ms)
{
    CAIRN_LOGI(TAG, "joining \"%s\"", CAIRN_WIFI_SSID);

    /*
     * Tell the coexistence arbiter to favour Wi-Fi for the duration.
     *
     * The Bluetooth controller stays initialised during a slot -- tearing it
     * down and rebuilding it panicked on the next scan -- so the radio is
     * genuinely shared, and a shared radio cannot meet the WPA2 four-way
     * handshake's timing on its default balanced split: association failed with
     * reason 204, HANDSHAKE_TIMEOUT, every time. Biasing toward Wi-Fi for the
     * slot and restoring balance afterwards is the least invasive thing that
     * makes the handshake possible. Advertising is already stopped by the
     * caller.
     */
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    WiFi.mode(WIFI_STA);

    /*
     * Power save is left at the default, deliberately.
     *
     * Disabling it (WiFi.setSleep(false)) aborts in the Wi-Fi driver whenever
     * the Bluetooth controller is also up: esp_wifi_set_ps(WIFI_PS_NONE) is not
     * permitted under coexistence, and the panic lands in
     * pm_set_sleep_type <- wifi_set_ps_process <- ppTask on core 0. The bench
     * build never saw it because it halts before BLE starts; in the production
     * image it rebooted the device every time a slot opened.
     *
     * Nothing is lost by leaving it on. The uplink is bounded by the 115200
     * UART, the card and the server, not by Wi-Fi latency, and a parked car has
     * no reason to favour microseconds over milliamps.
     */
    WiFi.begin(CAIRN_WIFI_SSID, CAIRN_WIFI_PSK);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
        delay(100);
    }

    if (WiFi.status() != WL_CONNECTED) {
        CAIRN_LOGW(TAG, "not associated after %u ms (status %d); radio off",
                   (unsigned)(millis() - t0), (int)WiFi.status());
        wifi_link_disconnect();
        return false;
    }

    CAIRN_LOGI(TAG, "associated in %u ms: %s, %d dBm, channel %d",
               (unsigned)(millis() - t0), WiFi.localIP().toString().c_str(),
               WiFi.RSSI(), WiFi.channel());
    return true;
}

void wifi_link_disconnect(void)
{
    /* net_upload closes the HTTP session itself when it returns, so there is
     * nothing to tear down here but the socket and the radio. */
    s_tls.stop();
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);

    /* Hand the radio back to a balanced split, so BLE is not starved while the
     * device is doing nothing in particular. */
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
}

bool wifi_link_connected(void)
{
    return WiFi.status() == WL_CONNECTED;
}

int wifi_link_rssi(void)
{
    return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
}

/* ── uploading ────────────────────────────────────────────────────────────── */

void wifi_link_upload_pending(uint32_t max_bundles,
                              bool (*should_abort)(void *), void *abort_ctx,
                              wifi_link_result_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!wifi_link_connected()) {
        CAIRN_LOGW(TAG, "asked to upload with no association");
        return;
    }

    /*
     * Pin the CA and present this device's certificate. Both are required:
     * verifying the server is what stops a hostile endpoint collecting trips,
     * and the client certificate is what the server matches against the device
     * id inside the signed manifest.
     */
    s_tls.setCACert(CAIRN_SERVER_CA_PEM);
    s_tls.setCertificate(CAIRN_CLIENT_CERT_PEM);
    s_tls.setPrivateKey(CAIRN_CLIENT_KEY_PEM);
    s_tls.setTimeout(CAIRN_HTTP_REPLY_TIMEOUT_MS / 1000);

    net_upload_result_t r;
    net_upload_pending(&s_tls, CAIRN_SERVER_HOST, CAIRN_SERVER_PORT, "wifi",
                       max_bundles, should_abort, abort_ctx, &r);

    out->considered = r.considered;
    out->delivered  = r.delivered;
    out->retained   = r.retained;
    out->refused    = r.refused;
    out->failed     = r.failed + r.unreadable;
    out->bytes_up   = r.bytes_up;
    out->bytes_down = r.bytes_down;
    out->ms         = r.ms;
}

#endif /* CAIRN_WIFI_UPLINK */
