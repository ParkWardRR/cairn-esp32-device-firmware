/* TLS over any Arduino Client. See tls_client.h. */

#include "tls_client.h"

#include <Arduino.h>
#include <string.h>

/* For MBEDTLS_ERR_NET_CONN_RESET, the error a BIO returns when the pipe is
 * gone. Only the constant is used; no socket code from this header runs. */
#include <mbedtls/net_sockets.h>

#include "cairn_log.h"
#include "cairn_platform.h"

static const char *TAG = "TLS";

/* The same source the device uses to generate its own signing key
 * (lib/cairn_fs/cairn_platform_esp.cpp, esp_fill_random), rather than a second
 * opinion about randomness in the same firmware. */
static int cairn_mbedtls_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    cairn_rng_fill(out, len);
    return 0;
}

TlsClient::TlsClient(Client *transport) : _transport(transport)
{
    mbedtls_ssl_init(&_ssl);
    mbedtls_ssl_config_init(&_conf);
    mbedtls_x509_crt_init(&_ca);
    mbedtls_x509_crt_init(&_cert);
    mbedtls_pk_init(&_key);
    _inited = true;
}

TlsClient::~TlsClient()
{
    stop();
    if (_inited) {
        mbedtls_pk_free(&_key);
        mbedtls_x509_crt_free(&_cert);
        mbedtls_x509_crt_free(&_ca);
        mbedtls_ssl_config_free(&_conf);
        mbedtls_ssl_free(&_ssl);
        _inited = false;
    }
}

/* ── the byte pipe ────────────────────────────────────────────────────────── */

int TlsClient::bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    Client *t = (Client *)ctx;
    if (!t->connected()) return MBEDTLS_ERR_NET_CONN_RESET;

    size_t n = t->write(buf, len);
    if (n == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)n;
}

int TlsClient::bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    Client *t = (Client *)ctx;

    int avail = t->available();
    if (avail <= 0) {
        /*
         * WANT_READ rather than an error: the transport is a modem socket that
         * polls, so "nothing yet" is the normal case and must not look like a
         * reset. The deadline lives in the caller's loop.
         */
        if (!t->connected()) return MBEDTLS_ERR_NET_CONN_RESET;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    size_t want = ((size_t)avail < len) ? (size_t)avail : len;
    int    n    = t->read(buf, want);
    if (n <= 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return n;
}

/* ── connect ──────────────────────────────────────────────────────────────── */

int TlsClient::connect(const char *host, uint16_t port)
{
    if (_ca_pem == nullptr || _cert_pem == nullptr || _key_pem == nullptr) {
        CAIRN_LOGE(TAG, "refusing to connect without a CA, a client "
                        "certificate and a key");
        return 0;
    }

    stop();

    if (_transport->connect(host, port) != 1) {
        CAIRN_LOGW(TAG, "the transport could not reach %s:%u", host, (unsigned)port);
        return 0;
    }

    if (!handshake(host)) {
        _transport->stop();
        return 0;
    }

    _up = true;
    return 1;
}

int TlsClient::connect(IPAddress ip, uint16_t port)
{
    /*
     * Refused on purpose. Verification is bound to the name — and the Funnel
     * ingress needs that name as SNI to route at all — so connecting by address
     * would either fail verification or tempt someone into disabling it.
     */
    (void)ip; (void)port;
    CAIRN_LOGE(TAG, "connect by IP is not supported: the certificate and the "
                    "SNI routing are both bound to the hostname");
    return 0;
}

bool TlsClient::handshake(const char *host)
{
    int rc;

    mbedtls_ssl_free(&_ssl);
    mbedtls_ssl_config_free(&_conf);
    mbedtls_ssl_init(&_ssl);
    mbedtls_ssl_config_init(&_conf);

    rc = mbedtls_ssl_config_defaults(&_conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { _last_err = rc; return false; }

    /* Parse the pinned CA. */
    mbedtls_x509_crt_free(&_ca);
    mbedtls_x509_crt_init(&_ca);
    rc = mbedtls_x509_crt_parse(&_ca, (const unsigned char *)_ca_pem,
                                strlen(_ca_pem) + 1);
    if (rc != 0) {
        CAIRN_LOGE(TAG, "the pinned CA does not parse (-0x%04x)", -rc);
        _last_err = rc;
        return false;
    }

    /* And this device's own certificate and key. */
    mbedtls_x509_crt_free(&_cert);
    mbedtls_x509_crt_init(&_cert);
    rc = mbedtls_x509_crt_parse(&_cert, (const unsigned char *)_cert_pem,
                                strlen(_cert_pem) + 1);
    if (rc != 0) {
        CAIRN_LOGE(TAG, "the client certificate does not parse (-0x%04x)", -rc);
        _last_err = rc;
        return false;
    }

    mbedtls_pk_free(&_key);
    mbedtls_pk_init(&_key);
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    rc = mbedtls_pk_parse_key(&_key, (const unsigned char *)_key_pem,
                              strlen(_key_pem) + 1, nullptr, 0,
                              cairn_mbedtls_rng, nullptr);
#else
    rc = mbedtls_pk_parse_key(&_key, (const unsigned char *)_key_pem,
                              strlen(_key_pem) + 1, nullptr, 0);
#endif
    if (rc != 0) {
        CAIRN_LOGE(TAG, "the client key does not parse (-0x%04x)", -rc);
        _last_err = rc;
        return false;
    }

    /*
     * REQUIRED. With VERIFY_OPTIONAL the handshake succeeds against any
     * certificate and the result is only readable afterwards, which is how a
     * device ends up trusting anyone while every log line says success.
     */
    mbedtls_ssl_conf_authmode(&_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&_conf, &_ca, nullptr);

    rc = mbedtls_ssl_conf_own_cert(&_conf, &_cert, &_key);
    if (rc != 0) {
        CAIRN_LOGE(TAG, "could not install the client certificate (-0x%04x)", -rc);
        _last_err = rc;
        return false;
    }

    mbedtls_ssl_conf_rng(&_conf, cairn_mbedtls_rng, nullptr);

    /* No SSLv3/TLS1.0/TLS1.1. The server offers TLS 1.2 and this mbedTLS has
     * no 1.3, so 1.2 is both floor and ceiling. */
    mbedtls_ssl_conf_min_version(&_conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);

    rc = mbedtls_ssl_setup(&_ssl, &_conf);
    if (rc != 0) { _last_err = rc; return false; }

    /*
     * Sets SNI *and* the name hostname verification checks. Both matter: the
     * Funnel edge demultiplexes on SNI, and without the name a certificate for
     * any host the CA signed would be accepted.
     */
    rc = mbedtls_ssl_set_hostname(&_ssl, host);
    if (rc != 0) { _last_err = rc; return false; }

    mbedtls_ssl_set_bio(&_ssl, _transport, bio_send, bio_recv, nullptr);

    uint32_t t0       = millis();
    uint32_t deadline = t0 + 30000;

    for (;;) {
        rc = mbedtls_ssl_handshake(&_ssl);
        if (rc == 0) break;

        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            _last_err = rc;
            if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                uint32_t flags = mbedtls_ssl_get_verify_result(&_ssl);
                char why[256];
                mbedtls_x509_crt_verify_info(why, sizeof(why), "  ", flags);
                CAIRN_LOGE(TAG, "the server's certificate was REJECTED "
                                "(flags 0x%08x):\n%s", (unsigned)flags, why);
            } else {
                CAIRN_LOGE(TAG, "handshake failed (-0x%04x)", -rc);
            }
            return false;
        }

        if ((int32_t)(millis() - deadline) >= 0) {
            CAIRN_LOGE(TAG, "handshake did not finish within 30 s");
            _last_err = MBEDTLS_ERR_SSL_TIMEOUT;
            return false;
        }
        delay(5);
    }

    CAIRN_LOGI(TAG, "handshake done in %u ms: %s, %s",
               (unsigned)(millis() - t0),
               mbedtls_ssl_get_version(&_ssl),
               mbedtls_ssl_get_ciphersuite(&_ssl));
    return true;
}

/* ── Client ───────────────────────────────────────────────────────────────── */

size_t TlsClient::write(uint8_t b) { return write(&b, 1); }

size_t TlsClient::write(const uint8_t *buf, size_t size)
{
    if (!_up) return 0;

    size_t   sent     = 0;
    uint32_t deadline = millis() + 30000;

    while (sent < size) {
        int rc = mbedtls_ssl_write(&_ssl, buf + sent, size - sent);
        if (rc > 0) {
            sent += (size_t)rc;
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if ((int32_t)(millis() - deadline) >= 0) break;
            delay(2);
            continue;
        }
        _last_err = rc;
        CAIRN_LOGW(TAG, "write failed (-0x%04x)", -rc);
        _up = false;
        break;
    }
    return sent;
}

int TlsClient::fill(uint32_t budget_ms)
{
    if (!_up) return -1;

    /* Compact anything already consumed. */
    if (_pos > 0 && _pos == _len) { _pos = 0; _len = 0; }
    if (_len >= BUF) return 0;

    uint32_t deadline = millis() + budget_ms;

    for (;;) {
        int rc = mbedtls_ssl_read(&_ssl, _buf + _len, BUF - _len);
        if (rc > 0) { _len += (size_t)rc; return rc; }

        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if ((int32_t)(millis() - deadline) >= 0) return 0;
            delay(2);
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            _up = false;
            return -1;
        }
        _last_err = rc;
        _up = false;
        return -1;
    }
}

int TlsClient::available()
{
    if (_pos < _len) return (int)(_len - _pos);
    if (!_up) return 0;

    /* A short poll: the HTTP layer loops on available() and tolerates zero. */
    (void)fill(5);
    return (int)(_len - _pos);
}

int TlsClient::read()
{
    if (_pos >= _len && available() <= 0) return -1;
    if (_pos >= _len) return -1;
    return _buf[_pos++];
}

int TlsClient::read(uint8_t *buf, size_t size)
{
    if (_pos >= _len && available() <= 0) return -1;

    size_t have = _len - _pos;
    size_t take = (have < size) ? have : size;
    if (take == 0) return -1;

    memcpy(buf, _buf + _pos, take);
    _pos += take;
    return (int)take;
}

int TlsClient::peek()
{
    if (_pos >= _len && available() <= 0) return -1;
    if (_pos >= _len) return -1;
    return _buf[_pos];
}

void TlsClient::stop()
{
    if (_up) {
        /* Best effort: tell the peer rather than vanishing, so the server does
         * not log a truncated request for a transfer that completed. */
        (void)mbedtls_ssl_close_notify(&_ssl);
        _up = false;
    }
    _len = 0;
    _pos = 0;
    if (_transport != nullptr && _transport->connected()) _transport->stop();
}

uint8_t TlsClient::connected()
{
    if (!_up) return 0;
    if (_pos < _len) return 1;          /* buffered data outlives the socket */
    return _transport->connected() ? 1 : 0;
}
