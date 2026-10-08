/*
 * TLS over any Arduino Client, as a Client.
 *
 * This exists so the LTE path can reuse the Wi-Fi path's TLS and HTTP code. The
 * modem's own TLS stack was the obvious choice and is the wrong one here:
 *
 *   - Tailscale Funnel, which is how this device reaches the server over
 *     cellular, requires SNI. Verified against the public ingress: without SNI
 *     the edge drops the handshake; with it, the chain verifies and the request
 *     succeeds. The SIM7600's `enableSNI` is undocumented on this firmware
 *     revision, so relying on it would be relying on an unknown.
 *   - The module caps a certificate file at 10240 bytes, caps a send at 2048,
 *     tops out at TLS 1.2, and defaults `ignorelocaltime` to 1 — which silently
 *     accepts an expired server certificate.
 *   - Doing it here means one TLS implementation and one HTTP implementation
 *     for both transports instead of two of each.
 *
 * The modem is then only a byte pipe, which is all the AT socket commands are
 * good at. Throughput is bounded by the 115200 UART either way, so the module's
 * hardware TLS would have bought nothing.
 *
 * Verification is REQUIRED, not optional. A CA loaded without
 * MBEDTLS_SSL_VERIFY_REQUIRED is a CA that is never checked, and the handshake
 * still succeeds — which is the worst possible failure here, because every log
 * line would read as success while the device talked to anyone. The same
 * mistake is documented in the wild against this modem family's AT stack, where
 * uploading a CA does nothing unless authmode is set explicitly.
 */

#ifndef CAIRN_TLS_CLIENT_H
#define CAIRN_TLS_CLIENT_H

#include <Client.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

class TlsClient : public Client {
public:
    explicit TlsClient(Client *transport);
    ~TlsClient() override;

    /* All three are required for mTLS to this server. PEM, NUL-terminated. */
    void setCACert(const char *pem)      { _ca_pem   = pem; }
    void setCertificate(const char *pem) { _cert_pem = pem; }
    void setPrivateKey(const char *pem)  { _key_pem  = pem; }

    /* Client */
    int    connect(const char *host, uint16_t port) override;
    int    connect(IPAddress ip, uint16_t port) override;
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;
    int    available() override;
    int    read() override;
    int    read(uint8_t *buf, size_t size) override;
    int    peek() override;
    void   flush() override {}
    void   stop() override;
    uint8_t connected() override;
    operator bool() override { return connected() != 0; }

    /* The last mbedTLS error, for the log. 0 when none. */
    int lastError() const { return _last_err; }

private:
    bool handshake(const char *host);
    void teardown();
    /* Move decrypted bytes into the buffer. Returns bytes added, or -1. */
    int  fill(uint32_t budget_ms);

    static int bio_send(void *ctx, const unsigned char *buf, size_t len);
    static int bio_recv(void *ctx, unsigned char *buf, size_t len);

    Client     *_transport;
    const char *_ca_pem   = nullptr;
    const char *_cert_pem = nullptr;
    const char *_key_pem  = nullptr;

    bool _up        = false;
    bool _inited    = false;
    int  _last_err  = 0;

    mbedtls_ssl_context       _ssl;
    mbedtls_ssl_config        _conf;
    mbedtls_x509_crt          _ca;
    mbedtls_x509_crt          _cert;
    mbedtls_pk_context        _key;

    /* Decrypted bytes waiting for the caller. mbedTLS hands back whole records,
     * and the HTTP layer reads a line at a time, so something has to hold the
     * remainder. */
    static const size_t BUF = 1024;
    uint8_t _buf[BUF];
    size_t  _len = 0;
    size_t  _pos = 0;
};

#endif /* CAIRN_TLS_CLIENT_H */
