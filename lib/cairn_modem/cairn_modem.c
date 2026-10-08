/* Reading a SIMCom modem's replies. See cairn_modem.h. */

#include "cairn_modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *cairn_at_status_name(cairn_at_status_t s)
{
    switch (s) {
    case CAIRN_AT_PENDING: return "pending";
    case CAIRN_AT_OK:      return "ok";
    case CAIRN_AT_ERROR:   return "error";
    case CAIRN_AT_PROMPT:  return "prompt";
    default:               return "?";
    }
}

/* Does `buf` contain `needle` as a whole line? Substring matching is not enough:
 * "NO CARRIER" contains no terminator, but a cell name or an APN could contain
 * "OK", and accepting that would end a command early. */
static bool has_line(const char *buf, const char *needle)
{
    size_t n = strlen(needle);
    const char *p = buf;

    while ((p = strstr(p, needle)) != NULL) {
        bool start_ok = (p == buf) || p[-1] == '\n' || p[-1] == '\r';
        char after    = p[n];
        bool end_ok   = (after == '\0' || after == '\r' || after == '\n');
        if (start_ok && end_ok) return true;
        p += n;
    }
    return false;
}

cairn_at_status_t cairn_at_classify(const char *buf, size_t len, int *cme)
{
    if (cme != NULL) *cme = -1;
    if (buf == NULL || len == 0) return CAIRN_AT_PENDING;

    const char *err = strstr(buf, "+CME ERROR:");
    if (err == NULL) err = strstr(buf, "+CMS ERROR:");
    if (err != NULL) {
        if (cme != NULL) {
            const char *colon = strchr(err, ':');
            if (colon != NULL) {
                /* Numeric with AT+CMEE=1, text with =2. Only a number is
                 * meaningful here; text callers read the buffer. */
                const char *p = colon + 1;
                while (*p == ' ') p++;
                if (*p >= '0' && *p <= '9') *cme = atoi(p);
            }
        }
        return CAIRN_AT_ERROR;
    }

    if (has_line(buf, "ERROR"))      return CAIRN_AT_ERROR;
    if (has_line(buf, "NO CARRIER")) return CAIRN_AT_ERROR;
    if (has_line(buf, "OK"))         return CAIRN_AT_OK;

    /* The send prompt has no terminator and may not be followed by anything. */
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '>' && (buf[i + 1] == ' ' || buf[i + 1] == '\0')) {
            return CAIRN_AT_PROMPT;
        }
    }
    if (len >= 1 && buf[len - 1] == '>') return CAIRN_AT_PROMPT;

    return CAIRN_AT_PENDING;
}

/* ── registration ─────────────────────────────────────────────────────────── */

int cairn_modem_reg_state(const char *buf, const char *tag)
{
    if (buf == NULL || tag == NULL) return -1;

    const char *p = strstr(buf, tag);
    if (p == NULL) return -1;

    /* "+CEREG: <n>,<stat>[,...]" — skip <n>, then read <stat>. */
    p = strchr(p, ',');
    if (p == NULL) return -1;
    p++;
    while (*p == ' ') p++;

    if (*p < '0' || *p > '9') return -1;

    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        if (v > 99) return -1;
    }
    return v;
}

bool cairn_modem_reg_usable(int stat)
{
    return stat == 1 || stat == 5;
}

const char *cairn_modem_reg_reason(int stat)
{
    switch (stat) {
    case 0:  return "not registered, not searching";
    case 1:  return "registered, home";
    case 2:  return "searching for an operator";
    case 3:  return "registration DENIED";
    case 4:  return "unknown (often out of coverage)";
    case 5:  return "registered, roaming";
    case 6:  return "registered, SMS only, home";
    case 7:  return "registered, SMS only, roaming";
    default: return "no registration reply";
    }
}

/* ── SIM ──────────────────────────────────────────────────────────────────── */

cairn_sim_state_t cairn_modem_sim_state(const char *buf)
{
    if (buf == NULL) return CAIRN_SIM_UNKNOWN;

    /* The absent case arrives as a +CME ERROR, not a +CPIN reply, so it has to
     * be recognised from the error text rather than the field. */
    if (strstr(buf, "SIM not inserted") != NULL ||
        strstr(buf, "not inserted") != NULL) {
        return CAIRN_SIM_ABSENT;
    }

    if (strstr(buf, "+CPIN:") == NULL) return CAIRN_SIM_UNKNOWN;

    /* Order matters: "SIM PUK" also contains "PIN" in some firmwares' phrasing,
     * so the more specific check goes first. */
    if (strstr(buf, "PUK") != NULL)   return CAIRN_SIM_PUK_REQUIRED;
    if (strstr(buf, "READY") != NULL) return CAIRN_SIM_READY;
    if (strstr(buf, "PIN") != NULL)   return CAIRN_SIM_PIN_REQUIRED;
    return CAIRN_SIM_UNKNOWN;
}

const char *cairn_modem_sim_state_name(cairn_sim_state_t s)
{
    switch (s) {
    case CAIRN_SIM_READY:        return "ready";
    case CAIRN_SIM_PIN_REQUIRED: return "PIN required";
    case CAIRN_SIM_PUK_REQUIRED: return "PUK required";
    case CAIRN_SIM_ABSENT:       return "not inserted";
    default:                     return "unknown";
    }
}

/* ── serving network ──────────────────────────────────────────────────────── */

bool cairn_modem_cops_numeric(const char *buf, char *out, size_t cap)
{
    if (buf == NULL || out == NULL || cap < 7) return false;
    out[0] = '\0';

    const char *p = strstr(buf, "+COPS:");
    if (p == NULL) return false;

    /* "+COPS: <mode>,<format>,<oper>[,<act>]" — format 2 is numeric. */
    const char *c1 = strchr(p, ',');
    if (c1 == NULL) return false;
    const char *fmt = c1 + 1;
    while (*fmt == ' ') fmt++;
    if (*fmt != '2') return false;        /* a name, not usable for policy */

    const char *c2 = strchr(fmt, ',');
    if (c2 == NULL) return false;
    const char *q = c2 + 1;
    while (*q == ' ') q++;
    if (*q == '"') q++;

    size_t n = 0;
    while (*q >= '0' && *q <= '9' && n + 1 < cap) out[n++] = *q++;
    out[n] = '\0';

    return n == 5 || n == 6;
}

bool cairn_modem_plmn_allowed(const char *plmn)
{
    if (plmn == NULL) return false;
    size_t n = strlen(plmn);
    if (n != 5 && n != 6) return false;

    /* The three mobile country codes assigned to the United States. */
    return strncmp(plmn, "310", 3) == 0 ||
           strncmp(plmn, "311", 3) == 0 ||
           strncmp(plmn, "312", 3) == 0;
}

/* ── signal ───────────────────────────────────────────────────────────────── */

int cairn_modem_csq_dbm(const char *buf)
{
    if (buf == NULL) return 0;

    const char *p = strstr(buf, "+CSQ:");
    if (p == NULL) return 0;
    p += 5;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return 0;

    int rssi = atoi(p);
    if (rssi < 0 || rssi > 31) return 0;    /* 99 means no measurement */
    return -113 + 2 * rssi;
}

/* ── PDP context ──────────────────────────────────────────────────────────── */

bool cairn_modem_cgpaddr(const char *buf, char *out, size_t cap)
{
    if (buf == NULL || out == NULL || cap < 8) return false;
    out[0] = '\0';

    const char *p = strstr(buf, "+CGPADDR:");
    if (p == NULL) return false;

    const char *c = strchr(p, ',');
    if (c == NULL) return false;
    const char *q = c + 1;
    while (*q == ' ') q++;
    if (*q == '"') q++;

    size_t n = 0;
    while (*q != '\0' && *q != '"' && *q != '\r' && *q != '\n' && n + 1 < cap) {
        out[n++] = *q++;
    }
    out[n] = '\0';

    if (n == 0) return false;
    if (strcmp(out, "0.0.0.0") == 0) { out[0] = '\0'; return false; }
    return true;
}

/* ── sockets ──────────────────────────────────────────────────────────────── */

/* "<tag>: <link>,<a>[,<b>]" — returns how many numbers were read after link. */
static int parse_tagged(const char *buf, const char *tag, int link,
                        int *a, int *b)
{
    const char *p = buf;

    /* The reply for another link may be in the buffer; keep looking. */
    while ((p = strstr(p, tag)) != NULL) {
        const char *q = p + strlen(tag);
        while (*q == ' ') q++;
        if (*q < '0' || *q > '9') { p = q; continue; }

        int got_link = atoi(q);
        while (*q >= '0' && *q <= '9') q++;

        if (got_link != link) { p = q; continue; }
        if (*q != ',') { p = q; continue; }
        q++;
        while (*q == ' ') q++;

        bool neg = (*q == '-');
        if (neg) q++;
        if (*q < '0' || *q > '9') { p = q; continue; }
        *a = atoi(q) * (neg ? -1 : 1);
        while (*q >= '0' && *q <= '9') q++;

        if (*q != ',') return 1;
        q++;
        while (*q == ' ') q++;
        if (*q < '0' || *q > '9') return 1;
        if (b != NULL) *b = atoi(q);
        return 2;
    }
    return 0;
}

bool cairn_modem_cipopen_result(const char *buf, int link, int *err)
{
    int a = 0;
    if (parse_tagged(buf, "+CIPOPEN:", link, &a, NULL) < 1) return false;
    if (err != NULL) *err = a;
    return true;
}

bool cairn_modem_cipsend_result(const char *buf, int link, int *requested,
                                int *confirmed)
{
    int a = 0, b = 0;
    if (parse_tagged(buf, "+CIPSEND:", link, &a, &b) < 2) return false;
    if (requested != NULL) *requested = a;
    if (confirmed != NULL) *confirmed = b;
    return true;
}

bool cairn_modem_ciprxget_header(const char *buf, size_t len, int *link,
                                 int *have, int *remaining, size_t *data_offset)
{
    if (buf == NULL) return false;

    const char *p = strstr(buf, "+CIPRXGET:");
    if (p == NULL) return false;

    const char *q = p + strlen("+CIPRXGET:");
    while (*q == ' ') q++;

    /* Mode first; only mode 2 carries payload. */
    if (*q != '2') return false;
    q++;
    if (*q != ',') return false;
    q++;

    int v[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        while (*q == ' ') q++;
        if (*q < '0' || *q > '9') return false;
        v[i] = atoi(q);
        while (*q >= '0' && *q <= '9') q++;
        if (i < 2) {
            if (*q != ',') return false;
            q++;
        }
    }

    /* The payload starts after the header's line ending. */
    while (*q == '\r') q++;
    if (*q != '\n') return false;
    q++;

    size_t off = (size_t)(q - buf);
    if (off > len) return false;

    if (link != NULL)        *link = v[0];
    if (have != NULL)        *have = v[1];
    if (remaining != NULL)   *remaining = v[2];
    if (data_offset != NULL) *data_offset = off;
    return true;
}

bool cairn_modem_socket_closed(const char *buf, int link)
{
    if (buf == NULL) return false;

    int a = 0;
    if (parse_tagged(buf, "+IPCLOSE:", link, &a, NULL) >= 1) return true;

    /* Older firmwares emit a bare "CLOSED" with no link number. */
    return has_line(buf, "CLOSED");
}

bool cairn_modem_cclk_unix_ms(const char *buf, uint64_t *out_unix_ms)
{
    const char *p;
    int yy, mo, dd, hh, mi, ss;
    int tz_q = 0;        /* local offset in quarter hours */
    char sign = 0;
    long days;
    int  y, m;
    long era;
    unsigned yoe, doy, doe;

    if (buf == NULL || out_unix_ms == NULL) return false;

    p = strstr(buf, "+CCLK:");
    if (p == NULL) return false;
    p = strchr(p, '"');
    if (p == NULL) return false;
    p++;

    if (sscanf(p, "%2d/%2d/%2d,%2d:%2d:%2d", &yy, &mo, &dd, &hh, &mi, &ss) != 6) {
        return false;
    }

    /* The zone is optional: some firmware omits it, and the reading is then UTC. */
    {
        const char *z = p;
        while (*z && *z != '"' && *z != '+' && *z != '-') z++;
        if (*z == '+' || *z == '-') {
            sign = *z;
            if (sscanf(z + 1, "%2d", &tz_q) != 1) return false;
            if (tz_q > 56) return false;   /* +/-14 h, the widest real offset */
            if (sign == '-') tz_q = -tz_q;
        }
    }

    /*
     * Refuse a clock the network has not set. A SIM7600 with no NITZ answers
     * with its build default in 1980, which parses cleanly and is wrong by
     * decades -- far more dangerous than a parse failure, because it would be
     * adopted as a bundle's time basis and look plausible.
     */
    if (yy > 99 || mo < 1 || mo > 12 || dd < 1 || dd > 31) return false;
    if (hh > 23 || mi > 59 || ss > 60) return false;  /* 60: leap second */

    /*
     * Two-digit years, split at 80. The module's unset default is "80/01/06" --
     * the GPS epoch, 1980 -- so mapping every yy to 2000+yy would turn that lie
     * into a plausible 2080 date and sail past the sanity check below. 80..99
     * therefore means 1980..1999, which is also the GSM convention, and the
     * mapping is good until 2080.
     */
    y = (yy >= 80) ? 1900 + yy : 2000 + yy;
    m = mo;
    if (m <= 2) { y -= 1; m += 12; }
    era  = (y >= 0 ? y : y - 399) / 400;
    yoe  = (unsigned)(y - era * 400);
    doy  = (unsigned)((153 * (m - 3) + 2) / 5 + dd - 1);
    doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + (long)doe - 719468;

    if (days < 10957) return false;  /* before 2000-01-01: not a network clock */

    {
        int64_t ms = (int64_t)days * 86400000LL +
                     (int64_t)hh * 3600000LL +
                     (int64_t)mi * 60000LL +
                     (int64_t)ss * 1000LL;
        /* The reading is local; the offset is quarter hours east of UTC. */
        ms -= (int64_t)tz_q * 15LL * 60LL * 1000LL;
        if (ms < 0) return false;
        *out_unix_ms = (uint64_t)ms;
    }
    return true;
}
