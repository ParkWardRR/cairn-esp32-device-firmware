/*
 * The Wi-Fi uplink: associate, upload sealed bundles over mTLS, prune on a
 * verified receipt.
 *
 * Everything above the socket is shared with the LTE path — lib/cairn_intake
 * speaks the protocol, src/net_http.cpp frames the requests, lib/cairn_bundle
 * reads the card — so this file is only the parts that are genuinely Wi-Fi:
 * joining a network, and a WiFiClientSecure configured with the pinned CA and
 * this device's client certificate.
 *
 * Two rules it must not break, both from the uplink schedule (issue #17):
 *
 *   - BLE and Wi-Fi are never up together. One 2.4 GHz radio, time-sliced.
 *     This module does not decide when it may run; it refuses to start if
 *     asked while the caller says BLE owns the radio, and the caller
 *     (lib/cairn_uplink, via the lifecycle) owns the schedule.
 *   - Nothing is deleted except through cairn_prune_if_receipted, against the
 *     key pinned in firmware. A receipt the gate refuses deletes nothing and
 *     the bundle stays on the card for the next attempt.
 */

#ifndef CAIRN_WIFI_LINK_H
#define CAIRN_WIFI_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if CAIRN_WIFI_UPLINK

/* Associate with the configured network. Returns false on timeout, having left
 * the radio off. */
bool wifi_link_connect(uint32_t timeout_ms);

/* Drop the association and power the radio down, so BLE may have it back. */
void wifi_link_disconnect(void);

bool wifi_link_connected(void);

/* Signal of the current association, in dBm. 0 when not associated. */
int wifi_link_rssi(void);

typedef struct {
    uint32_t considered;     /* sealed bundles seen on the card */
    uint32_t delivered;      /* ended with a receipt the prune gate accepted */
    uint32_t retained;       /* receipted but the prune did not complete */
    uint32_t refused;        /* the server will never accept these bytes */
    uint32_t failed;         /* link or card trouble; try again later */
    uint32_t bytes_up;
    uint32_t bytes_down;
    uint32_t ms;
} wifi_link_result_t;

/*
 * Upload up to `max_bundles` sealed bundles, newest-last, stopping early if
 * `should_abort` fires. Requires an association already up.
 *
 * A bundle is pruned only when the server's receipt verifies against the
 * pinned key AND names that bundle's content root. Every other outcome leaves
 * the card untouched.
 */
void wifi_link_upload_pending(uint32_t max_bundles,
                              bool (*should_abort)(void *), void *abort_ctx,
                              wifi_link_result_t *out);

#endif /* CAIRN_WIFI_UPLINK */

#endif /* CAIRN_WIFI_LINK_H */
