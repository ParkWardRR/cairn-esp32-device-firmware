/* The Wi-Fi uplink. See wifi_link.h. */

#include "wifi_link.h"

#if CAIRN_WIFI_UPLINK

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>

#include "board_config.h"
#include "cairn_bundle.h"
#include "cairn_fs.h"
#include "cairn_intake.h"
#include "cairn_log.h"
#include "cairn_prune.h"
#include "cairn_store.h"
#include "config.h"
#include "net_http.h"

static const char *TAG = "WIFI";

#if !defined(CAIRN_WIFI_SSID) || !defined(CAIRN_SERVER_HOST)
#error "CAIRN_WIFI_UPLINK needs CAIRN_WIFI_SSID and CAIRN_SERVER_HOST in secrets.h"
#endif
#if !defined(CAIRN_SERVER_CA_PEM) || !defined(CAIRN_CLIENT_CERT_PEM)
#error "CAIRN_WIFI_UPLINK needs CAIRN_SERVER_CA_PEM and a client certificate in secrets.h"
#endif

/*
 * Static, not stack or heap-per-transfer. WiFiClientSecure carries an mbedTLS
 * context, and the scratch buffer is read-path memory used on every block; a
 * transfer that allocated them would be the allocation most likely to fail,
 * at the moment it matters most.
 */
static WiFiClientSecure s_tls;
static cairn_http_t     s_http;
static cairn_bundle_t   s_bundle;

/* Big enough that a chunk moves in a few reads, small enough to be affordable
 * alongside an mbedTLS session. */
static uint8_t s_scratch[4096];
static uint8_t s_receipt[CAIRN_INTAKE_MAX_RECEIPT];

/* ── association ──────────────────────────────────────────────────────────── */

bool wifi_link_connect(uint32_t timeout_ms)
{
    CAIRN_LOGI(TAG, "joining \"%s\"", CAIRN_WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);          /* latency over the handful of mA, in a slot */
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
    cairn_http_disconnect(&s_http);
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
}

bool wifi_link_connected(void)
{
    return WiFi.status() == WL_CONNECTED;
}

int wifi_link_rssi(void)
{
    return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
}

/* ── the pinned key, for the prune gate ───────────────────────────────────── */

static bool hex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char    c = hex[i];
        uint8_t v;
        if      (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
        else return false;
        if (i % 2 == 0) out[i / 2] = (uint8_t)(v << 4);
        else            out[i / 2] |= v;
    }
    return true;
}

/* ── one bundle ───────────────────────────────────────────────────────────── */

typedef enum {
    UP_DELIVERED,
    UP_RETAINED,
    UP_REFUSED,
    UP_FAILED,
    UP_ABORTED
} upload_outcome_t;

static upload_outcome_t upload_one(const char *ulid,
                                   bool (*should_abort)(void *), void *abort_ctx,
                                   uint32_t *bytes_up, uint32_t *bytes_down)
{
    char dir[CAIRN_BUNDLE_DIR_MAX];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, ulid);

    if (!cairn_bundle_open(&s_bundle, dir)) {
        /* Not the server's fault and not permanent: a marginal card read may
         * succeed next time. */
        return UP_FAILED;
    }

    cairn_intake_http_t   http;
    cairn_intake_bundle_t src;
    cairn_http_as_intake(&s_http, &http);
    cairn_bundle_as_intake_source(&s_bundle, &src);

    size_t               receipt_len = 0;
    cairn_intake_stats_t st;

    cairn_intake_outcome_t r = cairn_intake_deliver(
        &http, &src, s_bundle.m.chunks, s_bundle.m.chunk_count,
        s_scratch, sizeof(s_scratch), should_abort, abort_ctx,
        s_receipt, sizeof(s_receipt), &receipt_len, &st);

    *bytes_up   += st.bytes_up;
    *bytes_down += st.bytes_down;

    cairn_bundle_close(&s_bundle);

    if (r != CAIRN_INTAKE_RECEIPT) {
        CAIRN_LOGW(TAG, "%s: %s (HTTP %d)", ulid, cairn_intake_outcome_name(r),
                   st.last_status);
        switch (r) {
        case CAIRN_INTAKE_REFUSED: return UP_REFUSED;
        case CAIRN_INTAKE_ABORTED: return UP_ABORTED;
        default:                   return UP_FAILED;
        }
    }

    /*
     * A receipt in hand is not permission to delete. The gate verifies it
     * against the key pinned in firmware AND against this bundle's own content
     * root, so a genuine receipt for different content prunes nothing — which
     * is what stops a misconfigured or hostile server inducing deletion of data
     * it never received.
     */
    uint8_t pinned[32];
    if (!hex32(CAIRN_SERVER_RECEIPT_KEY_HEX, pinned)) {
        CAIRN_LOGE(TAG, "the pinned receipt key is not 64 hex characters; "
                        "nothing will be pruned");
        return UP_RETAINED;
    }

    cairn_prune_result_t pr = cairn_prune_if_receipted(ulid, s_receipt, receipt_len,
                                                      pinned, s_bundle.m.content_root);
    if (pr == CAIRN_PRUNE_OK) {
        CAIRN_LOGI(TAG, "%s: delivered and pruned (%u up, %u chunks sent, "
                        "%u already held)",
                   ulid, (unsigned)st.bytes_up, (unsigned)st.chunks_sent,
                   (unsigned)st.chunks_skipped);
        return UP_DELIVERED;
    }

    CAIRN_LOGW(TAG, "%s: receipted but NOT pruned: %s", ulid,
               cairn_prune_result_name(pr));
    return UP_RETAINED;
}

/* ── the pending set ──────────────────────────────────────────────────────── */

void wifi_link_upload_pending(uint32_t max_bundles,
                              bool (*should_abort)(void *), void *abort_ctx,
                              wifi_link_result_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t t0 = millis();

    if (!wifi_link_connected()) {
        CAIRN_LOGW(TAG, "asked to upload with no association");
        return;
    }

    /* Pin the CA and present this device's certificate. Both are required:
     * verifying the server stops a hostile endpoint collecting trips, and the
     * client certificate is what the server matches against the device id
     * inside the signed manifest. */
    s_tls.setCACert(CAIRN_SERVER_CA_PEM);
    s_tls.setCertificate(CAIRN_CLIENT_CERT_PEM);
    s_tls.setPrivateKey(CAIRN_CLIENT_KEY_PEM);
    s_tls.setTimeout(CAIRN_HTTP_REPLY_TIMEOUT_MS / 1000);

    cairn_http_init(&s_http, &s_tls, CAIRN_SERVER_HOST, CAIRN_SERVER_PORT);

    /*
     * Walk the directory rather than taking a snapshot list: a prune removes a
     * directory mid-walk, and re-reading the name each iteration is simpler
     * than keeping a list consistent with the card.
     */
    char names[16][28];
    uint32_t found = 0;

    cairn_dir_t *d = cairn_fs_opendir(CAIRN_DIR_BUNDLES);
    if (d == NULL) {
        CAIRN_LOGW(TAG, "cannot list %s", CAIRN_DIR_BUNDLES);
        return;
    }

    char name[64];
    bool is_dir = false;
    while (found < (uint32_t)(sizeof(names) / sizeof(names[0])) &&
           cairn_fs_readdir(d, name, sizeof(name), &is_dir, NULL)) {
        if (!is_dir) continue;
        if (strlen(name) != 26) continue;         /* a ULID directory, not a stray */
        snprintf(names[found], sizeof(names[0]), "%s", name);
        found++;
    }
    cairn_fs_closedir(d);

    out->considered = found;
    CAIRN_LOGI(TAG, "%u sealed bundle(s) on the card; uploading up to %u",
               (unsigned)found, (unsigned)max_bundles);

    uint32_t done = 0;
    for (uint32_t i = 0; i < found && done < max_bundles; i++) {
        if (should_abort != NULL && should_abort(abort_ctx)) {
            CAIRN_LOGI(TAG, "abort ordered; stopping with %u delivered",
                       (unsigned)out->delivered);
            break;
        }

        uint32_t before = out->bytes_up;
        switch (upload_one(names[i], should_abort, abort_ctx,
                           &out->bytes_up, &out->bytes_down)) {
        case UP_DELIVERED: out->delivered++; done++; break;
        case UP_RETAINED:  out->retained++;  done++; break;
        case UP_REFUSED:   out->refused++;   done++; break;
        case UP_ABORTED:   i = found;                break;   /* stop the session */
        case UP_FAILED:
        default:
            out->failed++;
            /*
             * Only count it against the budget if something actually went over
             * the air. A bundle that could not be opened did no network work,
             * and the card holds legacy v2 bundles that sort first, so charging
             * them would exhaust the budget before a uploadable bundle is
             * reached.
             */
            if (out->bytes_up != before) done++;
            break;
        }
    }

    cairn_http_disconnect(&s_http);
    out->ms = millis() - t0;

    CAIRN_LOGI(TAG, "session: %u delivered, %u retained, %u refused, %u failed, "
                    "%u bytes up, %u down, %u ms",
               (unsigned)out->delivered, (unsigned)out->retained,
               (unsigned)out->refused, (unsigned)out->failed,
               (unsigned)out->bytes_up, (unsigned)out->bytes_down,
               (unsigned)out->ms);
}

#endif /* CAIRN_WIFI_UPLINK */
