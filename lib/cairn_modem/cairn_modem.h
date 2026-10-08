/*
 * Reading a SIMCom modem's replies, as portable C.
 *
 * The AT protocol is line-oriented text with unsolicited messages interleaved
 * at any moment, which makes it exactly the kind of parsing that looks trivial
 * and then fails on the hardware for a reason no reading of the code reveals.
 * So the parsing lives here, with no UART and no Arduino, and is exercised on
 * the host against the real replies this modem actually produced — captured
 * from env:cairn-netprobe on the car's unit.
 *
 * The sequencing (power, waiting for registration, opening a socket) stays in
 * src/lte_link.cpp, because it is inherently about blocking on a UART. What is
 * here is every decision that can be made from bytes alone.
 *
 * Three traps this module exists to encode, all of them observed:
 *
 *   1. `AT+CREG?` reports the *circuit-switched* domain. This SIM has no voice
 *      subscription, so it answers `0,3` — "registration denied" — while packet
 *      registration is perfectly healthy. A health check that reads CREG
 *      concludes there is no network on a working link, and at least one
 *      open-source project shipped exactly that bug. Use CEREG or CGREG.
 *   2. Registration state 5 means "registered, roaming", and on a
 *      permanently-roaming IoT SIM it is the *only* reachable healthy state —
 *      state 1 (home) cannot happen, because the home network is in Hong Kong.
 *      Both 1 and 5 are usable.
 *   3. The operator name in `AT+COPS?` is a string stored on the SIM, not
 *      something the network broadcasts: this card reports "T-Mobile EIOTCLUB"
 *      whatever it is attached to. Only the numeric form identifies the serving
 *      network, so policy must read MCC/MNC and never the name.
 */

#ifndef CAIRN_MODEM_H
#define CAIRN_MODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── how a command ended ──────────────────────────────────────────────────── */

typedef enum {
    CAIRN_AT_PENDING = 0,  /* no terminator yet; keep reading */
    CAIRN_AT_OK,
    CAIRN_AT_ERROR,        /* bare ERROR, or +CME/+CMS ERROR */
    CAIRN_AT_PROMPT        /* "> ", the invitation to send payload bytes */
} cairn_at_status_t;

const char *cairn_at_status_name(cairn_at_status_t s);

/*
 * Classify a reply buffer. Looks only for a terminator, so it is safe to call
 * repeatedly as bytes arrive.
 *
 * `cme` receives the +CME ERROR number when there is one, or -1. That number is
 * the difference between a diagnosis and another flash cycle: AT+CMEE=2 turns a
 * bare "ERROR" into "+CME ERROR: SIM not inserted".
 */
cairn_at_status_t cairn_at_classify(const char *buf, size_t len, int *cme);

/* ── registration ─────────────────────────────────────────────────────────── */

/*
 * The <stat> field of a "+CEREG:"/"+CGREG:"/"+CREG:" reply, or -1 if absent.
 *
 * Parsed from the field, never by searching the buffer for ",5": a cell id, a
 * tracking-area code or the echo of another command can all contain that, and a
 * registration check that unrelated digits can satisfy is worse than none.
 */
int cairn_modem_reg_state(const char *buf, const char *tag);

/* 1 (home) or 5 (roaming) only. See trap 2 above. */
bool cairn_modem_reg_usable(int stat);

/* A human reason for a state that is not usable, for the log. */
const char *cairn_modem_reg_reason(int stat);

/* ── SIM ──────────────────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_SIM_UNKNOWN = 0,
    CAIRN_SIM_READY,
    CAIRN_SIM_PIN_REQUIRED,
    CAIRN_SIM_PUK_REQUIRED,
    CAIRN_SIM_ABSENT
} cairn_sim_state_t;

cairn_sim_state_t cairn_modem_sim_state(const char *buf);
const char *cairn_modem_sim_state_name(cairn_sim_state_t s);

/* ── serving network ──────────────────────────────────────────────────────── */

/*
 * The numeric operator from `AT+COPS?` in format 2, as a 5- or 6-digit string
 * (MCC then MNC). Returns false when the reply carries a name instead, which is
 * the default format and is not usable for policy.
 */
bool cairn_modem_cops_numeric(const char *buf, char *out, size_t cap);

/*
 * Is this PLMN one the device is permitted to transmit on?
 *
 * The owner's policy is a US allow-list: MCC 310, 311 and 312. This replaces a
 * roaming on/off flag, which could not work here — the SIM is permanently
 * roaming in the US, so refusing to transmit while roaming refuses everything,
 * forever. An MCC allow-list keeps the real intent (do not transmit on a
 * network the owner has not agreed to pay for) without blocking the normal case.
 */
bool cairn_modem_plmn_allowed(const char *plmn);

/* ── signal ───────────────────────────────────────────────────────────────── */

/*
 * `AT+CSQ` <rssi> as dBm, per the manual's linear mapping -113 + 2*rssi.
 * Returns 0 when unknown (rssi 99). <ber> 99 is normal on LTE and not a fault:
 * the GSM-era field is simply not populated.
 */
int cairn_modem_csq_dbm(const char *buf);

/* ── PDP context ──────────────────────────────────────────────────────────── */

/*
 * The address from `AT+CGPADDR`. An address here is the gate for everything
 * else: without one the problem is the APN, the plan or the subscription, and no
 * amount of socket work will help. "0.0.0.0" means the context is not
 * activated and is reported as absent.
 */
bool cairn_modem_cgpaddr(const char *buf, char *out, size_t cap);

/* ── sockets ──────────────────────────────────────────────────────────────── */

/*
 * `+CIPOPEN: <link>,<err>` — 0 is success. Returns false if the reply is not
 * present yet; the result can arrive well after the OK, so the caller must
 * keep reading rather than treating OK as "connected".
 */
bool cairn_modem_cipopen_result(const char *buf, int link, int *err);

/*
 * `+CIPSEND: <link>,<reqlen>,<cnflen>`. The two lengths differing means the
 * modem accepted fewer bytes than were offered, which must be treated as a
 * short write rather than a success.
 */
bool cairn_modem_cipsend_result(const char *buf, int link, int *requested,
                                int *confirmed);

/*
 * `+CIPRXGET: 2,<link>,<read>,<remaining>` followed by `<read>` raw bytes.
 * Returns the header's fields and the offset at which the payload begins, so
 * the caller can copy from the same buffer without rescanning.
 */
bool cairn_modem_ciprxget_header(const char *buf, size_t len, int *link,
                                 int *have, int *remaining, size_t *data_offset);

/* True when the buffer holds a `+IPCLOSE: <link>,` or `CLOSED` notice for this
 * link: the far end went away and the socket must not be reused. */
bool cairn_modem_socket_closed(const char *buf, int link);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_MODEM_H */
