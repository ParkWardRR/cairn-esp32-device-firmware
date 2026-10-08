/* Bench network probe. See netprobe.h for why it exists. */

#include "netprobe.h"

#include <Arduino.h>
#include <WiFi.h>

#include <FreematicsPlus.h>

#include "cairn_log.h"
#include "config.h"

static const char *TAG = "PROBE";

/* ── Wi-Fi ────────────────────────────────────────────────────────────────── */

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI-PSK";
    default:                        return "?";
    }
}

/*
 * The scan is the authority on the SSID. An access point's name as written down
 * by a person is a guess until the radio has seen it: a trailing space, a
 * non-breaking hyphen or a band suffix all produce a name that looks right and
 * never associates. Printing the exact bytes, with the length, settles it.
 */
static void probe_wifi(void)
{
    CAIRN_LOGI(TAG, "--- Wi-Fi scan ---");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    delay(100);

    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
    if (n <= 0) {
        CAIRN_LOGW(TAG, "scan found no networks (result %d)", n);
        WiFi.mode(WIFI_OFF);
        return;
    }

    CAIRN_LOGI(TAG, "%d networks:", n);
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        CAIRN_LOGI(TAG, "  %2d. %-32s len %2u  %4d dBm  ch %2d  %s",
                   i + 1, ssid.length() ? ssid.c_str() : "<hidden>",
                   (unsigned)ssid.length(), WiFi.RSSI(i), WiFi.channel(i),
                   auth_name(WiFi.encryptionType(i)));
    }

    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
}

/*
 * Associate, then open a TCP connection to the ingest listener.
 *
 * Association is the only proof a pre-shared key is the right one; a key is
 * otherwise indistinguishable from a wrong one until the moment it matters. The
 * TCP connect then separates "the credential is wrong" from "the server is
 * unreachable from this network", which are the two failures that look
 * identical in an upload that simply never completes.
 *
 * The TLS handshake is deliberately not attempted here. It needs the client
 * certificate from the card, and a handshake failure at this stage would say
 * nothing about the credential this function exists to check.
 */
static void probe_wifi_join(void)
{
#if defined(CAIRN_WIFI_SSID)
    CAIRN_LOGI(TAG, "--- Wi-Fi association ---");
    CAIRN_LOGI(TAG, "joining \"%s\" (%u characters)", CAIRN_WIFI_SSID,
               (unsigned)strlen(CAIRN_WIFI_SSID));

    WiFi.mode(WIFI_STA);
    WiFi.begin(CAIRN_WIFI_SSID, CAIRN_WIFI_PSK);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) {
        delay(250);
    }

    if (WiFi.status() != WL_CONNECTED) {
        /*
         * status() distinguishes the two cases that matter: the network was not
         * found at all, versus found and the key refused.
         */
        CAIRN_LOGE(TAG, "association FAILED after %u ms, status %d (%s)",
                   (unsigned)(millis() - t0), (int)WiFi.status(),
                   WiFi.status() == WL_NO_SSID_AVAIL
                       ? "SSID not found — the name is wrong"
                       : "found, but not associated — the key is likely wrong");
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    CAIRN_LOGI(TAG, "associated in %u ms: ip %s, gw %s, rssi %d dBm, ch %d",
               (unsigned)(millis() - t0), WiFi.localIP().toString().c_str(),
               WiFi.gatewayIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
    CAIRN_LOGI(TAG, "dns %s", WiFi.dnsIP().toString().c_str());

#if defined(CAIRN_SERVER_HOST)
    IPAddress addr;
    if (!WiFi.hostByName(CAIRN_SERVER_HOST, addr)) {
        CAIRN_LOGE(TAG, "DNS lookup of %s failed", CAIRN_SERVER_HOST);
    } else {
        CAIRN_LOGI(TAG, "%s resolves to %s", CAIRN_SERVER_HOST,
                   addr.toString().c_str());

        WiFiClient c;
        t0 = millis();
        if (c.connect(addr, CAIRN_SERVER_PORT, 8000)) {
            CAIRN_LOGI(TAG, "TCP connect to %s:%d succeeded in %u ms — the "
                            "ingest listener is reachable from this network",
                       CAIRN_SERVER_HOST, (int)CAIRN_SERVER_PORT,
                       (unsigned)(millis() - t0));
            c.stop();
        } else {
            CAIRN_LOGE(TAG, "TCP connect to %s:%d failed after %u ms",
                       CAIRN_SERVER_HOST, (int)CAIRN_SERVER_PORT,
                       (unsigned)(millis() - t0));
        }
    }
#endif

    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
#else
    CAIRN_LOGW(TAG, "no CAIRN_WIFI_SSID compiled in; skipping association");
#endif
}

/* ── modem ────────────────────────────────────────────────────────────────── */

static FreematicsESP32 s_sys;

/*
 * One AT exchange, printed verbatim.
 *
 * Verbatim matters here. A probe that reported "SIM ok" would be interpreting,
 * and the interesting outcomes on an unknown socket are the ones no parser
 * expects: a bare echo with no terminator (wrong baud), framing noise (no
 * module, floating RX), or +CME ERROR with a number that names the real fault.
 * Returns the byte count read.
 */
static int at(const char *cmd, unsigned int timeout_ms)
{
    static char buf[768];

    s_sys.xbPurge();
    s_sys.xbWrite(cmd);
    s_sys.xbWrite("\r\n");

    int got = s_sys.xbRead(buf, sizeof(buf) - 1, timeout_ms);
    if (got <= 0) {
        CAIRN_LOGW(TAG, "  %-14s -> (nothing in %u ms)", cmd, timeout_ms);
        return 0;
    }
    buf[got] = '\0';

    /* Collapse the reply onto one line so the log stays greppable, and escape
     * anything unprintable rather than letting it corrupt the terminal. */
    char out[768];
    size_t o = 0;
    for (int i = 0; i < got && o + 5 < sizeof(out); i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r' || c == '\n') {
            if (o > 0 && out[o - 1] != '|') { out[o++] = '|'; }
        } else if (c >= 0x20 && c < 0x7f) {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "\\x%02x", c);
        }
    }
    out[o] = '\0';

    CAIRN_LOGI(TAG, "  %-14s -> %s", cmd, out);
    return got;
}

static void probe_modem(void)
{
    CAIRN_LOGI(TAG, "--- cellular modem (BEE socket, PWR %d, RX %d, TX %d) ---",
               PIN_BEE_PWR, PIN_BEE_UART_RXD, PIN_BEE_UART_TXD);

    /* xbBegin opens UART1 on the BEE pins and toggles PIN_BEE_PWR itself. */
    if (!s_sys.xbBegin(BEE_BAUDRATE)) {
        CAIRN_LOGE(TAG, "xbBegin failed: the UART could not be opened");
        return;
    }

    /*
     * SIMCom modules need a few seconds after the power pulse before the UART
     * answers, and an impatient probe reads back nothing on a module that is
     * merely still booting. Poll bare AT rather than sleeping blind.
     */
    bool alive = false;
    for (int i = 1; i <= 12 && !alive; i++) {
        CAIRN_LOGI(TAG, "attempt %d of 12 to raise the module", i);
        if (at("AT", 1200) > 0) alive = true;
        if (!alive) delay(1000);
    }

    if (!alive) {
        CAIRN_LOGW(TAG, "no reply on the BEE UART after ~13 s.");
        CAIRN_LOGW(TAG, "Either no module is fitted, or it sits at a different "
                        "baud rate, or it needs a longer power pulse.");
        CAIRN_LOGW(TAG, "Trying other common SIMCom baud rates before giving up.");

        static const unsigned long bauds[] = { 9600, 19200, 38400, 57600, 230400, 460800 };
        for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]) && !alive; i++) {
            s_sys.xbEnd();
            delay(50);
            if (!s_sys.xbBegin(bauds[i])) continue;
            CAIRN_LOGI(TAG, "retrying at %lu baud", bauds[i]);
            for (int k = 0; k < 3 && !alive; k++) {
                if (at("AT", 800) > 0) {
                    alive = true;
                    CAIRN_LOGI(TAG, "module answers at %lu baud", bauds[i]);
                }
            }
        }
    }

    if (!alive) {
        CAIRN_LOGE(TAG, "VERDICT: no cellular modem responding in the BEE socket.");
        CAIRN_LOGE(TAG, "The SIM cannot be tested on this unit, and the LTE path "
                        "has no hardware. Wi-Fi is unaffected.");
        s_sys.xbEnd();
        return;
    }

    CAIRN_LOGI(TAG, "a module is present and answering. Identifying it:");

    at("ATE0", 1000);          /* echo off, so replies are not doubled */

    /* Identity. */
    at("ATI", 2000);           /* manufacturer and model, free-form */
    at("AT+CGMI", 1000);       /* manufacturer */
    at("AT+CGMM", 1000);       /* model */
    at("AT+CGMR", 1000);       /* firmware revision */
    at("AT+CGSN", 1000);       /* IMEI */

    /* The SIM the owner has just installed. */
    at("AT+CPIN?", 5000);      /* READY / SIM PIN / SIM PUK / not inserted */
    at("AT+CCID", 3000);       /* ICCID, SIM7600 spelling */
    at("AT+ICCID", 3000);      /* ICCID, SIM7070/7670 spelling */
    at("AT+CIMI", 3000);       /* IMSI: proves the SIM is actually readable */

    /* Radio and network. */
    at("AT+CSQ", 2000);        /* signal: 99,99 means no measurement yet */
    at("AT+CREG?", 2000);      /* circuit-switched registration */
    at("AT+CGREG?", 2000);     /* packet registration — the one that matters */
    at("AT+CEREG?", 2000);     /* LTE registration */
    at("AT+COPS?", 10000);     /* operator, once registered */
    at("AT+CNMP?", 2000);      /* preferred mode (SIMCom) */
    at("AT+CGDCONT?", 2000);   /* the APN table as the module has it */

    CAIRN_LOGI(TAG, "VERDICT: a modem is fitted. Read CPIN/CIMI above for the "
                    "SIM and CGREG/CEREG for registration.");

    /*
     * Left powered deliberately. Registration can take a minute on a cold SIM,
     * so a second look at CSQ and CGREG after the probe prints is worth more
     * than a tidy power-down, and the next reflash resets it anyway.
     */
    CAIRN_LOGI(TAG, "re-reading signal and registration after a 20 s settle:");
    delay(20000);
    at("AT+CSQ", 2000);
    at("AT+CGREG?", 2000);
    at("AT+CEREG?", 2000);
    at("AT+COPS?", 10000);
}

/* ── entry point ──────────────────────────────────────────────────────────── */

void netprobe_run(void)
{
    CAIRN_LOGI(TAG, "=== network probe ===");
    CAIRN_LOGI(TAG, "This build captures nothing and writes nothing to NVS.");

    probe_wifi();
    probe_wifi_join();
    probe_modem();

    CAIRN_LOGI(TAG, "=== network probe complete ===");
    cairn_log_flush();
}
