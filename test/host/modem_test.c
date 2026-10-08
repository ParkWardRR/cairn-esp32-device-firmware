/*
 * The modem reply parsers, against replies this modem really produced.
 *
 * Every string below marked "observed" was captured from the car's dongle by
 * env:cairn-netprobe on 2026-10-07. That matters more than it sounds: the
 * interesting AT failures are the ones no invented fixture contains — an echo
 * with no terminator, a reply arriving before the previous command's OK, an
 * error delivered as +CME text rather than a field, a SIM absence that is not a
 * +CPIN value at all.
 *
 * Three behaviours here are load-bearing for the LTE path, and each one has
 * already been a shipped bug somewhere:
 *
 *   - reading CREG instead of CEREG/CGREG, which reports "no network" on a
 *     perfectly healthy data-only link;
 *   - treating registration state 5 as unhealthy, which refuses everything
 *     forever on a permanently-roaming SIM;
 *   - trusting the operator *name*, which this SIM stores on the card and
 *     reports regardless of which network it is attached to.
 */

#include <stdio.h>
#include <string.h>

#include "cairn_modem.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [modem] %s\n", name); }           \
        else      { g_fail++; printf("  FAIL  [modem] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* ── observed replies ─────────────────────────────────────────────────────── */

static const char R_ATI[] =
    "Manufacturer: SIMCOM INCORPORATED\r\n"
    "Model: SIMCOM_SIM7600A-H\r\n"
    "Revision: SIM7600A-H_V1.1\r\n"
    "SVN: 03\r\nIMEI: 868022030638220\r\n"
    "+GCAP: +CGSM\r\n\r\nOK\r\n\r\n+CPIN: READY\r\n\r\nSMS DONE\r\n";

static const char R_CPIN_ABSENT[] = "\r\n+CME ERROR: SIM not inserted\r\n";
static const char R_CPIN_READY[]  = "\r\n+CPIN: READY\r\n\r\nOK\r\n";
static const char R_CREG_DENIED[] = "\r\n+CREG: 0,3\r\n\r\nOK\r\n";
static const char R_CGREG_ROAM[]  = "\r\n+CGREG: 0,5\r\n\r\nOK\r\n";
static const char R_CEREG_ROAM[]  = "\r\n+CEREG: 0,5\r\n\r\nOK\r\n";
static const char R_CEREG_SEARCH[]= "\r\n+CEREG: 0,2\r\n\r\nOK\r\n";
static const char R_COPS_NAME[]   = "\r\n+COPS: 0,0,\"T-Mobile EIOTCLUB\",7\r\n\r\nOK\r\n";
static const char R_COPS_NUM[]    = "\r\n+COPS: 0,2,\"310260\",7\r\n\r\nOK\r\n";
static const char R_CSQ_18[]      = "\r\n+CSQ: 18,99\r\n\r\nOK\r\n";
static const char R_CSQ_15[]      = "\r\n+CSQ: 15,99\r\n\r\nOK\r\n";
static const char R_CSQ_NONE[]    = "\r\n+CSQ: 99,99\r\n\r\nOK\r\n";
static const char R_CPSI[] =
    "\r\n+CPSI: LTE,Online,310-260,0x3F29,89815063,125,EUTRAN-BAND12,"
    "5035,2,2,-196,-1167,-832,7\r\n\r\nOK\r\n";
static const char R_CGDCONT[] =
    "\r\n+CGDCONT: 1,\"IPV4V6\",\"nxtgenphone\",\"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\",0,0,0,0\r\n"
    "+CGDCONT: 2,\"IPV4V6\",\"ims\",\"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\",0,0,0,0\r\n"
    "+CGDCONT: 3,\"IPV4V6\",\"sos\",\"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\",0,0,0,1\r\n\r\nOK\r\n";
static const char R_SIMCARD_GONE[] = "\r\n+SIMCARD: NOT AVAILABLE\r\n\r\nOK\r\n";

static void test_classify(void)
{
    int cme = 0;

    CHECK("classify: OK on its own line terminates",
          cairn_at_classify("\r\nOK\r\n", 6, &cme) == CAIRN_AT_OK);

    CHECK("classify: observed ATI reply terminates with OK",
          cairn_at_classify(R_ATI, sizeof(R_ATI) - 1, &cme) == CAIRN_AT_OK);

    CHECK("classify: a bare ERROR is an error",
          cairn_at_classify("\r\nERROR\r\n", 9, &cme) == CAIRN_AT_ERROR);

    CHECK("classify: observed +CME ERROR text is an error",
          cairn_at_classify(R_CPIN_ABSENT, sizeof(R_CPIN_ABSENT) - 1, &cme)
              == CAIRN_AT_ERROR);

    cme = 0;
    CHECK("classify: a numeric +CME ERROR yields its code",
          cairn_at_classify("\r\n+CME ERROR: 13\r\n", 17, &cme) == CAIRN_AT_ERROR &&
          cme == 13);

    CHECK("classify: nothing yet is pending, not a failure",
          cairn_at_classify("\r\n+CSQ: 18,99\r\n", 14, &cme) == CAIRN_AT_PENDING);

    CHECK("classify: an empty buffer is pending",
          cairn_at_classify("", 0, &cme) == CAIRN_AT_PENDING);

    CHECK("classify: the send prompt is recognised",
          cairn_at_classify("\r\n> ", 4, &cme) == CAIRN_AT_PROMPT);

    CHECK("classify: a prompt with nothing after it is still a prompt",
          cairn_at_classify(">", 1, &cme) == CAIRN_AT_PROMPT);

    /*
     * The one that substring matching gets wrong. An operator name containing
     * "OK" must not end the command: the real reply has not arrived yet.
     */
    CHECK("classify: \"OK\" inside an operator name does NOT terminate",
          cairn_at_classify("\r\n+COPS: 0,0,\"OKLAHOMA MOBILE\",7\r\n", 33, &cme)
              == CAIRN_AT_PENDING);

    CHECK("classify: NO CARRIER is an error",
          cairn_at_classify("\r\nNO CARRIER\r\n", 14, &cme) == CAIRN_AT_ERROR);
}

static void test_registration(void)
{
    /* The trap. Both of these came off the same modem at the same moment. */
    CHECK("reg: observed CREG is 3, registration DENIED on the CS domain",
          cairn_modem_reg_state(R_CREG_DENIED, "+CREG:") == 3);
    CHECK("reg: and CREG 3 is correctly NOT usable",
          !cairn_modem_reg_usable(cairn_modem_reg_state(R_CREG_DENIED, "+CREG:")));
    CHECK("reg: observed CGREG is 5 at the same time, and IS usable",
          cairn_modem_reg_usable(cairn_modem_reg_state(R_CGREG_ROAM, "+CGREG:")));
    CHECK("reg: observed CEREG is 5, and IS usable",
          cairn_modem_reg_usable(cairn_modem_reg_state(R_CEREG_ROAM, "+CEREG:")));

    CHECK("reg: state 1 (home) is usable too",
          cairn_modem_reg_usable(1));
    CHECK("reg: searching (2) is not usable",
          !cairn_modem_reg_usable(cairn_modem_reg_state(R_CEREG_SEARCH, "+CEREG:")));

    CHECK("reg: a missing tag reports -1 rather than guessing",
          cairn_modem_reg_state(R_CSQ_18, "+CEREG:") == -1);

    /*
     * CPSI is full of commas and digits, including ",2" twice. A parser that
     * searched the buffer instead of the field would read one of them.
     */
    CHECK("reg: CPSI's many comma-separated numbers are not mistaken for a state",
          cairn_modem_reg_state(R_CPSI, "+CEREG:") == -1);

    /* A combined buffer, as happens when a URC arrives mid-command. */
    static char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", R_CPSI, R_CEREG_ROAM);
    CHECK("reg: with CPSI and CEREG in one buffer, CEREG's field is read",
          cairn_modem_reg_state(combined, "+CEREG:") == 5);

    CHECK("reg: reason text names the denial",
          strstr(cairn_modem_reg_reason(3), "DENIED") != NULL);
    CHECK("reg: reason text names roaming for 5",
          strstr(cairn_modem_reg_reason(5), "roaming") != NULL);
}

static void test_sim(void)
{
    CHECK("sim: the observed absence is detected from the +CME text",
          cairn_modem_sim_state(R_CPIN_ABSENT) == CAIRN_SIM_ABSENT);
    CHECK("sim: the observed ready reply is ready",
          cairn_modem_sim_state(R_CPIN_READY) == CAIRN_SIM_READY);
    CHECK("sim: READY arriving as a URC inside another reply is seen",
          cairn_modem_sim_state(R_ATI) == CAIRN_SIM_READY);
    CHECK("sim: a PIN request is distinguished",
          cairn_modem_sim_state("\r\n+CPIN: SIM PIN\r\n\r\nOK\r\n")
              == CAIRN_SIM_PIN_REQUIRED);
    CHECK("sim: a PUK request is not mistaken for a PIN request",
          cairn_modem_sim_state("\r\n+CPIN: SIM PUK\r\n\r\nOK\r\n")
              == CAIRN_SIM_PUK_REQUIRED);
    CHECK("sim: an unrelated reply is unknown, not ready",
          cairn_modem_sim_state(R_CSQ_18) == CAIRN_SIM_UNKNOWN);
    CHECK("sim: the observed CFUN=0 side effect is not read as ready",
          cairn_modem_sim_state(R_SIMCARD_GONE) != CAIRN_SIM_READY);
}

static void test_operator(void)
{
    char plmn[8];

    /*
     * The default format carries the SIM's own stored name. This card says
     * "T-Mobile EIOTCLUB" whatever it is attached to, so policy must refuse it.
     */
    CHECK("cops: the observed name format is refused for policy",
          !cairn_modem_cops_numeric(R_COPS_NAME, plmn, sizeof(plmn)));

    CHECK("cops: the observed numeric format yields 310260",
          cairn_modem_cops_numeric(R_COPS_NUM, plmn, sizeof(plmn)) &&
          strcmp(plmn, "310260") == 0);

    CHECK("plmn: T-Mobile US (310260) is allowed",
          cairn_modem_plmn_allowed("310260"));
    CHECK("plmn: AT&T US (310410) is allowed",
          cairn_modem_plmn_allowed("310410"));
    CHECK("plmn: MCC 311 and 312 are allowed",
          cairn_modem_plmn_allowed("311480") && cairn_modem_plmn_allowed("312250"));

    /* The SIM's own home network. Attaching there would be genuine
     * international roaming, which is what the allow-list exists to refuse. */
    CHECK("plmn: the SIM's Hong Kong home network (45400) is NOT allowed",
          !cairn_modem_plmn_allowed("45400"));
    CHECK("plmn: a Mexican network (334020) is NOT allowed",
          !cairn_modem_plmn_allowed("334020"));
    CHECK("plmn: a malformed value is refused",
          !cairn_modem_plmn_allowed("31") && !cairn_modem_plmn_allowed("") &&
          !cairn_modem_plmn_allowed(NULL));
}

static void test_signal(void)
{
    CHECK("csq: the observed 18 is -77 dBm", cairn_modem_csq_dbm(R_CSQ_18) == -77);
    CHECK("csq: the observed 15 is -83 dBm", cairn_modem_csq_dbm(R_CSQ_15) == -83);
    CHECK("csq: 99 means no measurement, reported as 0, not as -113+198",
          cairn_modem_csq_dbm(R_CSQ_NONE) == 0);
    CHECK("csq: an unrelated reply yields 0",
          cairn_modem_csq_dbm(R_CEREG_ROAM) == 0);
}

static void test_pdp(void)
{
    char ip[40];

    CHECK("cgpaddr: a quoted address is read",
          cairn_modem_cgpaddr("\r\n+CGPADDR: 1,\"10.172.4.91\"\r\n\r\nOK\r\n",
                              ip, sizeof(ip)) &&
          strcmp(ip, "10.172.4.91") == 0);

    CHECK("cgpaddr: an unquoted address is read too",
          cairn_modem_cgpaddr("\r\n+CGPADDR: 1,10.172.4.91\r\n\r\nOK\r\n",
                              ip, sizeof(ip)) &&
          strcmp(ip, "10.172.4.91") == 0);

    /*
     * The gate. 0.0.0.0 means the context never activated, and reporting it as
     * an address would send the caller hunting for blocked ports instead of a
     * wrong APN or a dead plan.
     */
    CHECK("cgpaddr: 0.0.0.0 is reported as NO address",
          !cairn_modem_cgpaddr("\r\n+CGPADDR: 1,\"0.0.0.0\"\r\n\r\nOK\r\n",
                               ip, sizeof(ip)));

    CHECK("cgpaddr: an unrelated reply yields nothing",
          !cairn_modem_cgpaddr(R_CGDCONT, ip, sizeof(ip)));
}

static void test_sockets(void)
{
    int err = -1, req = 0, cnf = 0;

    CHECK("cipopen: success on link 0",
          cairn_modem_cipopen_result("\r\n+CIPOPEN: 0,0\r\n", 0, &err) && err == 0);

    err = -1;
    CHECK("cipopen: a connect error is reported with its code",
          cairn_modem_cipopen_result("\r\n+CIPOPEN: 0,4\r\n", 0, &err) && err == 4);

    CHECK("cipopen: another link's result is not mistaken for ours",
          !cairn_modem_cipopen_result("\r\n+CIPOPEN: 1,0\r\n", 0, &err));

    err = -1;
    CHECK("cipopen: ours is found even when another link's reply is also present",
          cairn_modem_cipopen_result("\r\n+CIPOPEN: 1,4\r\n+CIPOPEN: 0,0\r\n", 0, &err) &&
          err == 0);

    CHECK("cipopen: OK alone is not a connection",
          !cairn_modem_cipopen_result("\r\nOK\r\n", 0, &err));

    CHECK("cipsend: a full write confirms the requested length",
          cairn_modem_cipsend_result("\r\n+CIPSEND: 0,1024,1024\r\n", 0, &req, &cnf) &&
          req == 1024 && cnf == 1024);

    /* A short write must be visible, or the body would be silently truncated
     * and the server would reject the chunk's digest. */
    CHECK("cipsend: a short write reports both lengths so it can be detected",
          cairn_modem_cipsend_result("\r\n+CIPSEND: 0,1024,512\r\n", 0, &req, &cnf) &&
          req == 1024 && cnf == 512);

    int link = -1, have = 0, rest = 0;
    size_t off = 0;
    static const char rx[] = "\r\n+CIPRXGET: 2,0,6,10\r\nABCDEF";
    CHECK("ciprxget: the header parses and names where the payload starts",
          cairn_modem_ciprxget_header(rx, sizeof(rx) - 1, &link, &have, &rest, &off) &&
          link == 0 && have == 6 && rest == 10 &&
          memcmp(rx + off, "ABCDEF", 6) == 0);

    CHECK("ciprxget: mode 1 (the data-waiting notice) carries no payload",
          !cairn_modem_ciprxget_header("\r\n+CIPRXGET: 1,0\r\n", 17, &link, &have,
                                       &rest, &off));

    CHECK("closed: an +IPCLOSE for our link is seen",
          cairn_modem_socket_closed("\r\n+IPCLOSE: 0,1\r\n", 0));
    CHECK("closed: an +IPCLOSE for another link is not ours",
          !cairn_modem_socket_closed("\r\n+IPCLOSE: 1,1\r\n", 0));
    CHECK("closed: a healthy reply is not a close",
          !cairn_modem_socket_closed("\r\n+CIPSEND: 0,8,8\r\n\r\nOK\r\n", 0));
}

/*
 * AT+CCLK? -- the network clock.
 *
 * The dangerous case is not a malformed reply but a well-formed lie: a SIM7600
 * that has had no NITZ answers with its build default in 1980, which parses
 * perfectly and would be adopted as a bundle's time basis.
 */
static void test_cclk(void)
{
    uint64_t ms = 0;

    /* 2026-10-08 21:30:00 UTC, reported as 14:30 local with a -7 h offset
     * (-28 quarter hours), which is what this car's timezone looks like. */
    CHECK("cclk: a local reading converts to UTC",
          cairn_modem_cclk_unix_ms("\r\n+CCLK: \"26/10/08,14:30:00-28\"\r\n\r\nOK\r\n", &ms));
    CHECK("cclk: ...and the offset is subtracted, not added",
          ms == 1791495000000ULL);

    /*
     * East of UTC, and a fractional zone: India is +5:30 = +22 quarter hours.
     * The same instant is 03:00 on the 9th there, so this also covers the
     * subtraction carrying back across a date boundary.
     */
    ms = 0;
    CHECK("cclk: a positive fractional offset parses",
          cairn_modem_cclk_unix_ms("+CCLK: \"26/10/09,03:00:00+22\"", &ms));
    CHECK("cclk: ...to the same instant, carrying back a day", ms == 1791495000000ULL);

    /* No zone field at all: the reading is already UTC. */
    ms = 0;
    CHECK("cclk: a reply with no zone is taken as UTC",
          cairn_modem_cclk_unix_ms("+CCLK: \"26/10/08,21:30:00\"", &ms));
    CHECK("cclk: ...unshifted", ms == 1791495000000ULL);

    /* The build default. Parses cleanly, and must still be refused. */
    ms = 0;
    CHECK("cclk: the module's unset 1980 default is refused",
          !cairn_modem_cclk_unix_ms("+CCLK: \"80/01/06,00:00:00+00\"", &ms));
    CHECK("cclk: ...and writes nothing", ms == 0);

    CHECK("cclk: a missing reply is not a time",
          !cairn_modem_cclk_unix_ms("\r\nOK\r\n", &ms));
    CHECK("cclk: a truncated reply is refused",
          !cairn_modem_cclk_unix_ms("+CCLK: \"26/10/0", &ms));
    CHECK("cclk: an out-of-range month is refused",
          !cairn_modem_cclk_unix_ms("+CCLK: \"26/13/08,21:30:00+00\"", &ms));
    CHECK("cclk: an out-of-range hour is refused",
          !cairn_modem_cclk_unix_ms("+CCLK: \"26/10/08,24:30:00+00\"", &ms));
    CHECK("cclk: an absurd zone is refused",
          !cairn_modem_cclk_unix_ms("+CCLK: \"26/10/08,21:30:00+99\"", &ms));
    CHECK("cclk: a leap second is accepted rather than rejected",
          cairn_modem_cclk_unix_ms("+CCLK: \"26/10/08,21:29:60+00\"", &ms));
}

int main(void)
{
    printf("modem: SIMCom reply parsing, against replies this unit produced\n");

    test_classify();
    test_registration();
    test_sim();
    test_operator();
    test_signal();
    test_pdp();
    test_cclk();
    test_sockets();

    printf("modem: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
