/* The LTE uplink. See lte_link.h. */

#include "lte_link.h"

#if CAIRN_LTE_UPLINK

#include <Arduino.h>
#include <Client.h>
#include <FreematicsPlus.h>
#include <string.h>

#include "board_config.h"
#include "cairn_fs.h"
#include "cairn_intake.h"
#include "cairn_log.h"
#include "cairn_modem.h"
#include "cairn_store.h"
#include "config.h"
#include "net_http.h"
#include "net_upload.h"
#include "tls_client.h"

static const char *TAG = "LTE";

#ifndef CAIRN_LTE_APN
#define CAIRN_LTE_APN "altanwifi"
#endif

/* One socket is enough: the uplink is strictly one request at a time. */
#define LINK 0

static FreematicsESP32 s_sys;
static bool            s_powered;
static char            s_addr[48];
static char            s_plmn[8];
static int             s_rssi;

const char *cairn_lte_status_name(cairn_lte_status_t s)
{
    switch (s) {
    case CAIRN_LTE_OK:             return "ok";
    case CAIRN_LTE_NO_MODEM:       return "no modem answering";
    case CAIRN_LTE_NO_SIM:         return "no SIM";
    case CAIRN_LTE_SIM_LOCKED:     return "SIM locked";
    case CAIRN_LTE_NOT_REGISTERED: return "not registered";
    case CAIRN_LTE_PLMN_REFUSED:   return "serving network not allowed";
    case CAIRN_LTE_NO_ADDRESS:     return "no PDP address";
    case CAIRN_LTE_NET_FAILED:     return "socket stack failed";
    default:                       return "?";
    }
}

/* ── the AT transaction ───────────────────────────────────────────────────── */

static char s_reply[1280];

/*
 * Send a command and read until the module terminates it or the budget runs
 * out. The terminator decision is cairn_at_classify's, so "OK" inside an
 * operator name cannot end a command early.
 */
static cairn_at_status_t at_cmd(const char *cmd, uint32_t timeout_ms, int *cme)
{
    s_sys.xbPurge();
    if (cmd != nullptr) {
        s_sys.xbWrite(cmd);
        s_sys.xbWrite("\r\n");
    }

    size_t   n        = 0;
    uint32_t deadline = millis() + timeout_ms;
    s_reply[0] = '\0';

    for (;;) {
        if (n + 1 < sizeof(s_reply)) {
            int got = s_sys.xbRead(s_reply + n, (int)(sizeof(s_reply) - 1 - n), 50);
            if (got > 0) {
                n += (size_t)got;
                s_reply[n] = '\0';
                cairn_at_status_t st = cairn_at_classify(s_reply, n, cme);
                if (st != CAIRN_AT_PENDING) return st;
            }
        }
        if ((int32_t)(millis() - deadline) >= 0) return CAIRN_AT_PENDING;
    }
}

/* Same, and log the outcome. */
static bool at_ok(const char *cmd, uint32_t timeout_ms)
{
    int cme = -1;
    cairn_at_status_t st = at_cmd(cmd, timeout_ms, &cme);
    if (st == CAIRN_AT_OK) return true;

    CAIRN_LOGW(TAG, "%s -> %s%s", cmd, cairn_at_status_name(st),
               (cme >= 0) ? " (+CME)" : "");
    return false;
}

/* ── bring-up ─────────────────────────────────────────────────────────────── */

static bool raise_module(void)
{
    if (!s_sys.xbBegin(BEE_BAUDRATE)) {
        CAIRN_LOGE(TAG, "the BEE UART would not open");
        return false;
    }
    s_powered = true;

    /*
     * This module answers only around 45 s after the power pulse, printing RDY
     * first. Polling bare AT rather than sleeping blind, because the time
     * varies and an impatient probe reads back nothing from a module that is
     * merely still booting.
     */
    for (int i = 1; i <= 30; i++) {
        if (at_cmd("AT", 1500, nullptr) == CAIRN_AT_OK) {
            CAIRN_LOGI(TAG, "module answering after %d attempt(s)", i);
            return true;
        }
        delay(1000);
    }

    CAIRN_LOGE(TAG, "no reply on the BEE UART after ~45 s");
    return false;
}

cairn_lte_status_t lte_link_up(void)
{
    s_addr[0] = '\0';
    s_plmn[0] = '\0';
    s_rssi    = 0;

    if (!raise_module()) return CAIRN_LTE_NO_MODEM;

    at_ok("ATE0", 2000);
    /* Before anything that can fail, or a refusal arrives as a bare ERROR and
     * names neither its cause nor its subsystem. */
    at_ok("AT+CMEE=2", 2000);

    /*
     * Full functionality first, so bring-up does not depend on the state the
     * last run left behind.
     *
     * This is not belt-and-braces. lte_link_down() ends with AT+CFUN=0, which
     * powers the SIM interface down, and the module stays powered afterwards
     * (closing the UART does not cut its supply). A second bring-up then read
     * the SIM as "unknown" and had its APN write refused — the identical
     * symptom to the CFUN=0 trap, arriving from the other direction.
     */
    at_ok("AT+CFUN=1", 15000);
    for (int i = 0; i < 20; i++) {
        at_cmd("AT+CPIN?", 3000, nullptr);
        if (cairn_modem_sim_state(s_reply) != CAIRN_SIM_UNKNOWN) break;
        delay(1000);
    }

    cairn_sim_state_t sim = cairn_modem_sim_state(s_reply);
    CAIRN_LOGI(TAG, "SIM: %s", cairn_modem_sim_state_name(sim));
    if (sim == CAIRN_SIM_ABSENT) return CAIRN_LTE_NO_SIM;
    if (sim == CAIRN_SIM_PIN_REQUIRED || sim == CAIRN_SIM_PUK_REQUIRED) {
        return CAIRN_LTE_SIM_LOCKED;
    }

    /*
     * LTE only. The sole pre-LTE radio on this part is WCDMA B2/B5 and every US
     * network that carried WCDMA there is switched off, so automatic mode only
     * spends time and power scanning technologies that cannot succeed.
     */
    at_ok("AT+CNMP=38", 6000);

    /*
     * The APN, written with the radio off but the SIM alive. CFUN=0 would power
     * the SIM interface down and AT+CGDCONT would be refused, leaving the
     * factory AT&T profile (nxtgenphone) in place and the write silently undone.
     * "IP" rather than "IPV4V6": a dual-stack request only one side honours is a
     * known cause of a context that activates and passes no traffic.
     */
    at_ok("AT+CFUN=4", 12000);
    delay(500);

    char cmd[160];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", CAIRN_LTE_APN);
    if (!at_ok(cmd, 6000)) {
        CAIRN_LOGE(TAG, "the APN write was refused; the context still holds its "
                        "previous value, so not continuing");
        at_ok("AT+CFUN=1", 12000);
        return CAIRN_LTE_NET_FAILED;
    }
    at_cmd("AT+CGDCONT?", 6000, nullptr);   /* prove it landed */

    at_ok("AT+CFUN=1", 12000);

    /* An attach that races SIM init is refused, and the refusal looks exactly
     * like a subscription problem. */
    for (int i = 0; i < 15; i++) {
        at_cmd("AT+CPIN?", 3000, nullptr);
        if (cairn_modem_sim_state(s_reply) == CAIRN_SIM_READY) break;
        delay(1000);
    }

    /* Registration, judged on the packet domain. CREG reports the
     * circuit-switched domain and answers 0,3 on this voice-less SIM. */
    bool registered = false;
    for (int i = 0; i < 45 && !registered; i++) {
        at_cmd("AT+CEREG?", 3000, nullptr);
        int stat = cairn_modem_reg_state(s_reply, "+CEREG:");
        if (cairn_modem_reg_usable(stat)) { registered = true; break; }

        at_cmd("AT+CGREG?", 3000, nullptr);
        int gstat = cairn_modem_reg_state(s_reply, "+CGREG:");
        if (cairn_modem_reg_usable(gstat)) { registered = true; break; }

        if (i == 10 || i == 30) {
            CAIRN_LOGI(TAG, "still attaching: CEREG %s",
                       cairn_modem_reg_reason(stat));
        }
        delay(2000);
    }
    if (!registered) {
        CAIRN_LOGE(TAG, "not registered on the packet domain");
        return CAIRN_LTE_NOT_REGISTERED;
    }

    at_cmd("AT+CSQ", 3000, nullptr);
    s_rssi = cairn_modem_csq_dbm(s_reply);

    /*
     * Which network, numerically. The alphanumeric name is a string stored on
     * this SIM ("T-Mobile EIOTCLUB") and is reported whatever the module is
     * attached to, so it cannot carry policy.
     */
    at_ok("AT+COPS=3,2", 3000);
    at_cmd("AT+COPS?", 15000, nullptr);
    if (!cairn_modem_cops_numeric(s_reply, s_plmn, sizeof(s_plmn))) {
        CAIRN_LOGE(TAG, "could not read the serving network numerically; "
                        "refusing to transmit rather than guessing");
        return CAIRN_LTE_PLMN_REFUSED;
    }
    if (!cairn_modem_plmn_allowed(s_plmn)) {
        CAIRN_LOGE(TAG, "attached to PLMN %s, which the allow-list forbids; "
                        "not transmitting", s_plmn);
        return CAIRN_LTE_PLMN_REFUSED;
    }
    CAIRN_LOGI(TAG, "serving PLMN %s, %d dBm", s_plmn, s_rssi);

    at_cmd("AT+CPSI?", 5000, nullptr);      /* band and cell, for the log */

    /* Activate and read the address. An address is the gate: without one the
     * problem is the APN, the plan or the subscription. */
    at_cmd("AT+CGACT=1,1", 30000, nullptr);
    at_cmd("AT+CGPADDR=1", 8000, nullptr);
    if (!cairn_modem_cgpaddr(s_reply, s_addr, sizeof(s_addr))) {
        CAIRN_LOGE(TAG, "no PDP address. The attach is healthy, so this is very "
                        "likely the APN (\"%s\") or the prepaid plan, not the "
                        "radio: an expired plan still registers.", CAIRN_LTE_APN);
        return CAIRN_LTE_NO_ADDRESS;
    }
    CAIRN_LOGI(TAG, "PDP address %s via APN \"%s\"", s_addr, CAIRN_LTE_APN);

    /* Manual receive, so data waits for us instead of arriving unsolicited and
     * interleaving with a command's reply. */
    at_ok("AT+CIPRXGET=1", 5000);

    if (!at_ok("AT+NETOPEN", 30000)) {
        /* Already open is not a failure. */
        if (strstr(s_reply, "opened") == nullptr) {
            CAIRN_LOGE(TAG, "AT+NETOPEN failed");
            return CAIRN_LTE_NET_FAILED;
        }
    }

    return CAIRN_LTE_OK;
}

void lte_link_down(void)
{
    if (!s_powered) return;

    at_ok("AT+CIPCLOSE=0", 10000);
    at_ok("AT+NETCLOSE", 15000);

    /*
     * CFUN=4, not CFUN=0: it drops the radio, which is what costs power, while
     * leaving the SIM interface alive. CFUN=0 powers the SIM down and the next
     * bring-up inherits a module whose SIM reads "unknown" and whose context
     * cannot be edited. The module keeps its own supply either way — closing
     * the UART does not cut it — so there is nothing to gain by going further.
     */
    at_ok("AT+CFUN=4", 10000);

    s_sys.xbEnd();
    s_powered = false;
    s_addr[0] = '\0';
    CAIRN_LOGI(TAG, "modem powered down");
}

const char *lte_link_address(void) { return s_addr; }
const char *lte_link_plmn(void)    { return s_plmn; }
int         lte_link_rssi_dbm(void){ return s_rssi; }

/* ── the modem socket, as a Client ────────────────────────────────────────── */

class ModemClient : public Client {
public:
    int connect(const char *host, uint16_t port) override
    {
        stop();

        char cmd[200];
        snprintf(cmd, sizeof(cmd), "AT+CIPOPEN=%d,\"TCP\",\"%s\",%u", LINK, host,
                 (unsigned)port);

        /*
         * The connect result arrives as +CIPOPEN: <link>,<err>, which may come
         * well after the OK — so OK alone is not a connection, and the reply has
         * to be read until the result appears.
         */
        s_sys.xbPurge();
        s_sys.xbWrite(cmd);
        s_sys.xbWrite("\r\n");

        size_t   n        = 0;
        uint32_t deadline = millis() + 40000;
        s_reply[0] = '\0';

        for (;;) {
            if (n + 1 < sizeof(s_reply)) {
                int got = s_sys.xbRead(s_reply + n, (int)(sizeof(s_reply) - 1 - n), 100);
                if (got > 0) {
                    n += (size_t)got;
                    s_reply[n] = '\0';

                    int err = -1;
                    if (cairn_modem_cipopen_result(s_reply, LINK, &err)) {
                        if (err == 0) {
                            _open = true;
                            return 1;
                        }
                        CAIRN_LOGW(TAG, "CIPOPEN refused with error %d", err);
                        return 0;
                    }
                }
            }
            if ((int32_t)(millis() - deadline) >= 0) {
                CAIRN_LOGW(TAG, "no CIPOPEN result within 40 s");
                return 0;
            }
        }
    }

    int connect(IPAddress ip, uint16_t port) override { (void)ip; (void)port; return 0; }

    size_t write(uint8_t b) override { return write(&b, 1); }

    size_t write(const uint8_t *buf, size_t size) override
    {
        if (!_open) return 0;

        size_t sent = 0;
        while (sent < size) {
            /* The module's own ceiling per send. */
            size_t want = size - sent;
            if (want > 1024) want = 1024;

            char cmd[48];
            snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%u", LINK, (unsigned)want);

            if (at_cmd(cmd, 10000, nullptr) != CAIRN_AT_PROMPT) {
                CAIRN_LOGW(TAG, "no send prompt");
                _open = false;
                break;
            }

            s_sys.xbWrite((const char *)(buf + sent), (int)want);

            /* Wait for the confirmation, and compare the two lengths: a short
             * accept must not read as a success, or the body is truncated and
             * the server rejects the chunk's digest. */
            size_t   n        = 0;
            uint32_t deadline = millis() + 20000;
            s_reply[0] = '\0';
            bool done = false;

            while (!done) {
                if (n + 1 < sizeof(s_reply)) {
                    int got = s_sys.xbRead(s_reply + n, (int)(sizeof(s_reply) - 1 - n), 100);
                    if (got > 0) {
                        n += (size_t)got;
                        s_reply[n] = '\0';

                        int req = 0, cnf = 0;
                        if (cairn_modem_cipsend_result(s_reply, LINK, &req, &cnf)) {
                            if (cnf < req) {
                                CAIRN_LOGW(TAG, "short send: %d of %d accepted",
                                           cnf, req);
                                _open = false;
                            }
                            sent += (size_t)(cnf > 0 ? cnf : 0);
                            done = true;
                        } else if (cairn_modem_socket_closed(s_reply, LINK)) {
                            _open = false;
                            done  = true;
                        }
                    }
                }
                if (!done && (int32_t)(millis() - deadline) >= 0) {
                    CAIRN_LOGW(TAG, "no send confirmation within 20 s");
                    _open = false;
                    done  = true;
                }
            }

            if (!_open) break;
        }
        return sent;
    }

    int available() override
    {
        if (_pos < _len) return (int)(_len - _pos);
        if (!_open) return 0;
        pump();
        return (int)(_len - _pos);
    }

    int read() override
    {
        if (_pos >= _len && available() <= 0) return -1;
        if (_pos >= _len) return -1;
        return _rx[_pos++];
    }

    int read(uint8_t *buf, size_t size) override
    {
        if (_pos >= _len && available() <= 0) return -1;
        size_t have = _len - _pos;
        size_t take = (have < size) ? have : size;
        if (take == 0) return -1;
        memcpy(buf, _rx + _pos, take);
        _pos += take;
        return (int)take;
    }

    int  peek() override { return (_pos < _len) ? _rx[_pos] : -1; }
    void flush() override {}

    void stop() override
    {
        if (_open) {
            at_ok("AT+CIPCLOSE=" "0", 10000);
            _open = false;
        }
        _len = 0;
        _pos = 0;
    }

    uint8_t connected() override
    {
        if (_pos < _len) return 1;      /* buffered data outlives the socket */
        return _open ? 1 : 0;
    }

    operator bool() override { return connected() != 0; }

private:
    /* Fetch whatever the module is holding for us. */
    void pump()
    {
        _len = 0;
        _pos = 0;

        /* Mode 4 asks how much is waiting, so a fetch is only issued when there
         * is something to fetch. */
        if (at_cmd("AT+CIPRXGET=4," "0", 5000, nullptr) != CAIRN_AT_OK) return;

        int mode_link = 0, waiting = 0;
        if (!cairn_modem_cipsend_result(s_reply, LINK, &mode_link, &waiting)) {
            /* "+CIPRXGET: 4,<link>,<len>" shares the shape, so the generic
             * tagged parse would need the mode stripped; read it directly. */
            const char *p = strstr(s_reply, "+CIPRXGET:");
            if (p == nullptr) return;
            int m = 0, l = 0, w = 0;
            if (sscanf(p, "+CIPRXGET: %d,%d,%d", &m, &l, &w) != 3) return;
            if (m != 4 || l != LINK) return;
            waiting = w;
        }
        if (waiting <= 0) return;

        size_t want = (size_t)waiting;
        if (want > sizeof(_rx)) want = sizeof(_rx);

        char cmd[48];
        snprintf(cmd, sizeof(cmd), "AT+CIPRXGET=2,%d,%u", LINK, (unsigned)want);

        s_sys.xbPurge();
        s_sys.xbWrite(cmd);
        s_sys.xbWrite("\r\n");

        /* Read header plus payload. The payload is binary, so the terminator
         * cannot be looked for until `have` bytes are in hand. */
        /* Header plus payload. Sized for a 1 KB fetch rather than 2: the
         * responses on this protocol are a few hundred bytes, and DRAM is the
         * segment that binds in this image. */
        static char raw[1024 + 128];
        size_t   n        = 0;
        uint32_t deadline = millis() + 15000;
        raw[0] = '\0';

        for (;;) {
            if (n + 1 < sizeof(raw)) {
                int got = s_sys.xbRead(raw + n, (int)(sizeof(raw) - 1 - n), 100);
                if (got > 0) {
                    n += (size_t)got;
                    raw[n] = '\0';

                    int link = 0, have = 0, rest = 0;
                    size_t off = 0;
                    if (cairn_modem_ciprxget_header(raw, n, &link, &have, &rest, &off) &&
                        link == LINK) {
                        if (have == 0) return;
                        if (n >= off + (size_t)have) {
                            size_t take = (size_t)have;
                            if (take > sizeof(_rx)) take = sizeof(_rx);
                            memcpy(_rx, raw + off, take);
                            _len = take;
                            return;
                        }
                    }
                }
            }
            if ((int32_t)(millis() - deadline) >= 0) return;
        }
    }

    bool    _open = false;
    uint8_t _rx[1024];
    size_t  _len = 0;
    size_t  _pos = 0;
};

/* ── uploading ────────────────────────────────────────────────────────────── */

/*
 * The modem socket and the TLS over it. Static for the same reason as the
 * Wi-Fi side; the bundle, HTTP and scratch buffers are in net_upload.cpp,
 * shared between the two transports because the schedule never runs both.
 */
static ModemClient s_modem;
static TlsClient   s_tls(&s_modem);

void lte_link_upload_pending(uint32_t max_bundles,
                             bool (*should_abort)(void *), void *abort_ctx,
                             lte_link_result_t *out)
{
    memset(out, 0, sizeof(*out));

    s_tls.setCACert(CAIRN_SERVER_CA_PEM);
    s_tls.setCertificate(CAIRN_CLIENT_CERT_PEM);
    s_tls.setPrivateKey(CAIRN_CLIENT_KEY_PEM);

    /*
     * The Funnel hostname, not the LAN one: that name has no public DNS record
     * and resolves only on the home network, to a private address. The
     * hostname is also what mbedTLS sends as SNI, which the Funnel edge needs
     * in order to route at all.
     */
    net_upload_result_t r;
    net_upload_pending(&s_tls, CAIRN_LTE_SERVER_HOST, CAIRN_LTE_SERVER_PORT,
                       "lte", max_bundles, should_abort, abort_ctx, &r);

    out->considered = r.considered;
    out->delivered  = r.delivered;
    out->retained   = r.retained;
    out->refused    = r.refused;
    out->failed     = r.failed + r.unreadable;
    out->bytes_up   = r.bytes_up;
    out->bytes_down = r.bytes_down;
    out->ms         = r.ms;
}

#endif /* CAIRN_LTE_UPLINK */
