/* Bench network probe. See netprobe.h for why it exists. */

#include "netprobe.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

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

/* Defined below: the data-session test runs at the end of the modem probe, once
 * the module has had time to register. */
static void probe_lte_data(void);

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

    /*
     * Descriptive errors, before anything that can fail.
     *
     * Without this the module answers a refused command with a bare "ERROR",
     * which names neither the cause nor the subsystem. With it the same refusal
     * arrives as "+CME ERROR: SIM not inserted" or similar, which is the
     * difference between a diagnosis and another flash cycle.
     */
    at("AT+CMEE=2", 1000);

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

    probe_lte_data();
}

/* ── the LTE data path ────────────────────────────────────────────────────── */

/*
 * One AT exchange, with the reply kept so it can be inspected.
 *
 * `want` is a substring that must appear for this to count as success. Using a
 * substring rather than requiring a bare "OK" is deliberate: several of the
 * commands below answer with the information first and OK afterwards, and some
 * answer OK while reporting a failure in the payload.
 */
static bool at_expect(const char *cmd, const char *want, unsigned int timeout_ms,
                      char *reply, size_t reply_cap)
{
    static char buf[1024];

    s_sys.xbPurge();
    s_sys.xbWrite(cmd);
    s_sys.xbWrite("\r\n");

    int got = s_sys.xbRead(buf, sizeof(buf) - 1, timeout_ms);
    if (got < 0) got = 0;
    buf[got] = '\0';

    if (reply != NULL && reply_cap > 0) {
        snprintf(reply, reply_cap, "%s", buf);
    }

    /* One line, printable, for the log. */
    char out[1024];
    size_t o = 0;
    for (int i = 0; i < got && o + 5 < sizeof(out); i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r' || c == '\n') {
            if (o > 0 && out[o - 1] != '|') out[o++] = '|';
        } else if (c >= 0x20 && c < 0x7f) {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "\\x%02x", c);
        }
    }
    out[o] = '\0';

    /*
     * "A reply arrived and the module did not refuse" — not merely "a reply
     * arrived". An earlier version returned true whenever `want` was NULL,
     * which made a timeout and an ERROR both read as success; the APN write
     * that silently did not happen was exactly that bug's shape.
     */
    bool ok = (got > 0) && (strstr(buf, "ERROR") == NULL);
    if (ok && want != NULL) ok = (strstr(buf, want) != NULL);

    CAIRN_LOGI(TAG, "  %-34s -> %s%s", cmd, out, ok ? "" : "   [FAILED]");
    return ok;
}

/*
 * The APN candidates, best first.
 *
 * EIOTCLUB keys the APN off the ICCID prefix rather than publishing one value,
 * and this card's prefix (891030) matches their "North America" row, which
 * names altanwifi. That is the documented answer but not a proven one — the
 * same prefix maps elsewhere by region with no finer discriminator — so the
 * device tries a list rather than trusting a single string. An empty APN is
 * last: letting the network choose does work on some cards and is worth having
 * as a fallback, but it is the least diagnosable outcome.
 */
static const char *const APN_CANDIDATES[] = {
    "altanwifi",      /* EIOTCLUB's documented value for the 891030 North America row */
    "bicsapn",        /* their documented all-regions fallback */
    "america.bics",
    "mobile",
    "globaldata",
    ""                /* network-chosen */
};

/* Registered on the packet domain? Accepts 1 (home) and 5 (roaming).
 *
 * Both are usable data states, and on this SIM 5 is the ONLY reachable one: the
 * home network is in Hong Kong, so "registered, home" cannot happen in the US.
 * Note that AT+CREG? is deliberately not consulted — it reports the
 * circuit-switched domain, which this data-only plan has no subscription for,
 * so it answers 0,3 ("denied") on a perfectly healthy link. Gating on CREG is a
 * real and already-shipped class of bug.
 */
/*
 * The <stat> field of a "+CxREG: <n>,<stat>" reply, or -1.
 *
 * Parsed rather than substring-matched: searching the whole reply for ",5"
 * would also match a cell id, a timestamp or the echo of another command, and a
 * registration check that can be fooled by unrelated digits is worse than none.
 */
static int reg_state(const char *reply, const char *tag)
{
    const char *p = strstr(reply, tag);
    if (p == NULL) return -1;

    p = strchr(p, ',');
    if (p == NULL) return -1;
    p++;

    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;
    return *p - '0';
}

static bool packet_registered(void)
{
    char reply[256];

    if (at_expect("AT+CEREG?", NULL, 3000, reply, sizeof(reply))) {
        int s = reg_state(reply, "+CEREG:");
        if (s == 1 || s == 5) return true;
        if (s == 3) CAIRN_LOGW(TAG, "EPS registration DENIED (stat 3)");
    }
    if (at_expect("AT+CGREG?", NULL, 3000, reply, sizeof(reply))) {
        int s = reg_state(reply, "+CGREG:");
        if (s == 1 || s == 5) return true;
    }
    return false;
}

/* Wait for the SIM to finish initialising after an RF cycle.
 *
 * The module answers AT long before the SIM is ready, and an attach that races
 * SIM init is refused by the network — which shows up as registration state 3
 * (denied) and looks exactly like a subscription problem. */
static bool wait_sim_ready(unsigned int budget_ms)
{
    char reply[256];
    uint32_t t0 = millis();

    while (millis() - t0 < budget_ms) {
        if (at_expect("AT+CPIN?", NULL, 3000, reply, sizeof(reply)) &&
            strstr(reply, "READY") != NULL) {
            return true;
        }
        delay(1000);
    }
    CAIRN_LOGW(TAG, "SIM did not report READY within %u ms", budget_ms);
    return false;
}

/* True when AT+CGPADDR reports an address that is not the unassigned 0.0.0.0.
 * An address here is the gate for everything else: without one the problem is
 * the APN, the plan or the subscription, and no amount of socket work will
 * help. */
static bool have_ip(char *ip, size_t cap)
{
    char reply[256];
    if (!at_expect("AT+CGPADDR=1", "+CGPADDR", 5000, reply, sizeof(reply))) return false;

    /* +CGPADDR: 1,"10.1.2.3" — take what is between the quotes. */
    const char *q = strchr(reply, '"');
    if (q == NULL) {
        /* Some builds answer unquoted: +CGPADDR: 1,10.1.2.3 */
        q = strstr(reply, ",");
        if (q == NULL) return false;
        q++;
    } else {
        q++;
    }

    size_t n = 0;
    while (*q != '\0' && *q != '"' && *q != '\r' && *q != '\n' && n + 1 < cap) {
        ip[n++] = *q++;
    }
    ip[n] = '\0';

    if (n == 0) return false;
    if (strcmp(ip, "0.0.0.0") == 0) return false;
    return true;
}

/* Try one TCP connect through the modem's own stack, to settle whether the
 * carrier passes this port at all. */
static bool tcp_reaches(const char *host, int port)
{
    char cmd[160], reply[320];

    snprintf(cmd, sizeof(cmd), "AT+CIPOPEN=0,\"TCP\",\"%s\",%d", host, port);

    /* The connect result arrives as +CIPOPEN: 0,0 (0 = success), which may come
     * well after the OK, so allow a generous window. */
    bool sent = at_expect(cmd, NULL, 30000, reply, sizeof(reply));
    bool ok   = sent && strstr(reply, "+CIPOPEN: 0,0") != NULL;

    if (!ok && sent && strstr(reply, "+CIPOPEN: 0,") != NULL) {
        CAIRN_LOGW(TAG, "port %d: the modem reported a connect error", port);
    }

    at_expect("AT+CIPCLOSE=0", NULL, 10000, NULL, 0);
    return ok;
}

/*
 * Bring up a data session and prove the server is reachable over cellular.
 *
 * This exists because every question that actually blocks the LTE transport is
 * one no datasheet can answer: which APN this card wants, whether the prepaid
 * plan is live (an expired plan still registers, so the attach above proves
 * nothing about data), and whether the carrier passes the ingest port. Finding
 * out takes one flash; guessing costs a redesign.
 */
static void probe_lte_data(void)
{
#if !defined(CAIRN_SERVER_HOST)
    CAIRN_LOGW(TAG, "no CAIRN_SERVER_HOST compiled in; skipping the LTE data test");
#else
    CAIRN_LOGI(TAG, "--- LTE data path ---");

    char reply[320];

    /* Identity of the card, from the modem rather than the printed label: the
     * APN is keyed off this prefix. */
    at_expect("AT+CICCID", NULL, 3000, reply, sizeof(reply));

    /*
     * LTE only. The sole pre-LTE radio on this part is WCDMA B2/B5, and every
     * US network that carried WCDMA there is switched off, so leaving the
     * default automatic mode only spends time and power scanning technologies
     * that cannot succeed anywhere in the country.
     */
    at_expect("AT+CNMP=38", "OK", 5000, NULL, 0);

    /* Who are we actually on? The alphanumeric name is a string stored on the
     * SIM and is not evidence of the serving network. */
    at_expect("AT+COPS=3,2", "OK", 3000, NULL, 0);
    if (at_expect("AT+COPS?", NULL, 10000, reply, sizeof(reply))) {
        if (strstr(reply, "\"310") != NULL || strstr(reply, "\"311") != NULL ||
            strstr(reply, "\"312") != NULL) {
            CAIRN_LOGI(TAG, "serving PLMN is a US one, as expected");
        } else {
            CAIRN_LOGW(TAG, "serving PLMN is not a US MCC — check the numeric "
                            "value above before trusting any allow-list");
        }
    }

    at_expect("AT+CPSI?", NULL, 5000, NULL, 0);   /* band, cell, RSRP, RSRQ */

    bool got_session = false;

    for (size_t i = 0; i < sizeof(APN_CANDIDATES) / sizeof(APN_CANDIDATES[0]) &&
                       !got_session; i++) {
        const char *apn = APN_CANDIDATES[i];
        CAIRN_LOGI(TAG, "trying APN \"%s\" (%u of %u)",
                   apn[0] ? apn : "<network-chosen>", (unsigned)(i + 1),
                   (unsigned)(sizeof(APN_CANDIDATES) / sizeof(APN_CANDIDATES[0])));

        /*
         * Detach to edit the context: AT+CGDCONT is refused while the context
         * is active. Context 1 currently holds nxtgenphone, the AT&T
         * certification APN burned into this module's firmware at the factory —
         * stale, and an APN the home network will not accept fails the default
         * bearer while registration still reads healthy. Contexts 2 (ims) and 3
         * (sos) are left alone; they carry no user data and editing them can
         * destabilise an IMS-certified build.
         *
         * "IP" rather than "IPV4V6": a dual-stack request that only one side
         * honours is a known source of a context that activates but passes no
         * traffic on wholesale roaming APNs.
         */
        /*
         * CFUN=4, not CFUN=0. Minimum functionality powers the SIM interface
         * down — the module answers "+SIMCARD: NOT AVAILABLE" and then refuses
         * AT+CGDCONT, so the APN write silently does not happen and the next
         * attach uses the stale one. CFUN=4 turns the radio off and leaves the
         * SIM alive, which is what editing a context actually needs.
         */
        at_expect("AT+CFUN=4", "OK", 10000, NULL, 0);
        delay(500);

        char cmd[160];
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
        if (!at_expect(cmd, "OK", 5000, NULL, 0)) {
            /* Continuing here would test the previous APN again and report the
             * result against this one. */
            CAIRN_LOGE(TAG, "the APN write was refused; not testing this "
                            "candidate, because the context still holds the "
                            "previous value");
            at_expect("AT+CFUN=1", "OK", 10000, NULL, 0);
            continue;
        }
        at_expect("AT+CGDCONT?", NULL, 5000, NULL, 0);   /* prove it landed */

        at_expect("AT+CFUN=1", "OK", 10000, NULL, 0);
        wait_sim_ready(15000);

        /* Re-attach. A cold attach under steering of roaming can take a while,
         * so this waits rather than deciding early. */
        bool reg = false;
        for (int k = 0; k < 15 && !reg; k++) {
            delay(2000);
            reg = packet_registered();
        }
        if (!reg) {
            CAIRN_LOGW(TAG, "did not re-register on this APN");
            continue;
        }

        at_expect("AT+CGACT=1,1", NULL, 30000, NULL, 0);

        char ip[64];
        if (have_ip(ip, sizeof(ip))) {
            CAIRN_LOGI(TAG, "APN \"%s\" WORKS: assigned IP %s",
                       apn[0] ? apn : "<network-chosen>", ip);
            at_expect("AT+CGCONTRDP=1", NULL, 5000, NULL, 0);  /* DNS, gateway, MTU */
            got_session = true;
        } else {
            CAIRN_LOGW(TAG, "APN \"%s\": no address assigned",
                       apn[0] ? apn : "<network-chosen>");
        }
    }

    if (!got_session) {
        CAIRN_LOGE(TAG, "VERDICT: no APN produced a data session.");
        CAIRN_LOGE(TAG, "The attach is healthy, so this is very likely the "
                        "prepaid plan, not the radio: an expired or unpurchased "
                        "plan still registers while all data fails. Check the "
                        "EIOTCLUB portal for an active plan on the ICCID above "
                        "before changing any code.");
        return;
    }

    /*
     * The port question. 8443 is the configured ingest listener; 443 is the
     * fallback the deployment also serves precisely because carrier filtering
     * of a non-standard port cannot be settled from documentation.
     */
    at_expect("AT+NETOPEN", NULL, 30000, NULL, 0);

    bool p8443 = tcp_reaches(CAIRN_SERVER_HOST, CAIRN_SERVER_PORT);
    CAIRN_LOGI(TAG, "port %d over LTE: %s", (int)CAIRN_SERVER_PORT,
               p8443 ? "REACHABLE" : "not reachable");

    bool p443 = tcp_reaches(CAIRN_SERVER_HOST, 443);
    CAIRN_LOGI(TAG, "port 443 over LTE: %s", p443 ? "REACHABLE" : "not reachable");

    at_expect("AT+NETCLOSE", NULL, 15000, NULL, 0);

    if (p8443) {
        CAIRN_LOGI(TAG, "VERDICT: LTE carries data and the ingest port is open. "
                        "The transport can use %d directly.", (int)CAIRN_SERVER_PORT);
    } else if (p443) {
        CAIRN_LOGW(TAG, "VERDICT: LTE carries data but %d is blocked; the "
                        "transport must fall back to 443.", (int)CAIRN_SERVER_PORT);
    } else {
        CAIRN_LOGE(TAG, "VERDICT: a data session exists but neither port "
                        "connected. Check the server is reachable from outside "
                        "the home network at all.");
    }
#endif
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
