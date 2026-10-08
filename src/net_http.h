/*
 * HTTP/1.1 over an Arduino Client, shaped as a cairn_intake_http_t.
 *
 * This is the half of the network path that is the same for Wi-Fi and LTE. The
 * two differ only in what Client they hand over — WiFiClientSecure in one case,
 * mbedTLS over a modem socket in the other — so the request framing, the
 * response parsing and the connection reuse live here once.
 *
 * Connection reuse is the point worth stating. A bundle is one offer, one
 * commit and up to CAIRN_MAX_CHUNKS chunk PUTs, so around thirty requests. A
 * fresh TLS handshake per request would cost thirty handshakes: several seconds
 * of ECDSA on this chip, and on a metered cellular link a few kilobytes of
 * certificate exchange each time, which is real money against a prepaid bucket.
 * So the socket is opened once per transfer and held across requests, and only
 * reconnected if the server closes it.
 *
 * Deliberately not an HTTPClient: that library owns the connection lifetime and
 * reopens per request, which is the behaviour this needs to avoid.
 */

#ifndef CAIRN_NET_HTTP_H
#define CAIRN_NET_HTTP_H

#include <Client.h>
#include <stdint.h>

#include "cairn_intake.h"

/* How long to wait for a response once a request is sent. The server may be
 * hashing a 8 KiB chunk and writing it to the CAS, so this is generous. */
#define CAIRN_HTTP_REPLY_TIMEOUT_MS 20000

/* How long a connect may take before it is abandoned. */
#define CAIRN_HTTP_CONNECT_TIMEOUT_MS 15000

typedef struct {
    Client     *client;
    const char *host;
    uint16_t    port;

    /* Set when the server said Connection: close, or a read failed: the next
     * request reconnects rather than writing into a dead socket. */
    bool        must_reconnect;

    /* Per-request state. */
    bool        request_open;
    uint32_t    requests;        /* for the log: how much reuse actually happened */
    uint32_t    connects;
} cairn_http_t;

/* Bind to a Client and a destination. Does not connect; the first request does. */
void cairn_http_init(cairn_http_t *h, Client *client, const char *host, uint16_t port);

/* Fill in the callbacks cairn_intake_deliver expects. */
void cairn_http_as_intake(cairn_http_t *h, cairn_intake_http_t *out);

/* Close the socket. Call once per transfer, after cairn_intake_deliver returns;
 * the intake module's own close() ends a request, not the connection. */
void cairn_http_disconnect(cairn_http_t *h);

#endif /* CAIRN_NET_HTTP_H */
