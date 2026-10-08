/*
 * The LTE uplink: bring the modem up, open a TCP socket over AT, and upload
 * sealed bundles through the same TLS and HTTP code the Wi-Fi path uses.
 *
 * The modem is only a byte pipe here. Its own TLS stack is deliberately unused
 * (see tls_client.h for why: Funnel needs SNI, which this firmware revision may
 * not send, plus a 10 KB certificate cap, a TLS 1.2 ceiling and a default that
 * accepts expired server certificates). So this file is the AT sequencing and a
 * Client implementation over AT+CIPOPEN/CIPSEND/CIPRXGET; everything above it
 * is shared.
 *
 * What the bring-up has to get right, all of it learned from this unit:
 *
 *   - The module answers AT only about 45 seconds after power-on, printing RDY.
 *     A short probe concludes there is no modem.
 *   - AT+CMEE=2 first, or every failure is a bare "ERROR".
 *   - AT+CFUN=4 to edit a PDP context, never CFUN=0: minimum functionality
 *     powers the SIM interface down, so AT+CGDCONT is refused and the APN write
 *     silently does not happen.
 *   - Wait for +CPIN: READY after the RF cycle. An attach that races SIM init
 *     is refused, and looks exactly like a subscription problem.
 *   - Judge registration on CEREG/CGREG, never CREG: this SIM has no voice
 *     subscription and answers CREG 0,3 while packet registration is healthy.
 *   - State 5 (roaming) is the only healthy state reachable here, so transmit
 *     policy is a US PLMN allow-list read from AT+COPS=3,2, not a roaming flag.
 */

#ifndef CAIRN_LTE_LINK_H
#define CAIRN_LTE_LINK_H

#include <stdbool.h>
#include <stdint.h>

#if CAIRN_LTE_UPLINK

typedef enum {
    CAIRN_LTE_OK = 0,
    CAIRN_LTE_NO_MODEM,        /* nothing answered on the BEE UART */
    CAIRN_LTE_NO_SIM,
    CAIRN_LTE_SIM_LOCKED,
    CAIRN_LTE_NOT_REGISTERED,
    CAIRN_LTE_PLMN_REFUSED,    /* attached somewhere the allow-list forbids */
    CAIRN_LTE_NO_ADDRESS,      /* registered but no PDP address: APN or plan */
    CAIRN_LTE_NET_FAILED
} cairn_lte_status_t;

const char *cairn_lte_status_name(cairn_lte_status_t s);

/*
 * Power the module, attach, activate the context and open the network stack.
 * Blocking, and slow by nature: budget a minute or more from cold.
 */
cairn_lte_status_t lte_link_up(void);

/* Close the socket stack and power the module down. */
void lte_link_down(void);

/* The assigned PDP address, or "" when there is none. */
const char *lte_link_address(void);

/* The serving network as MCC+MNC, or "" when unknown. */
const char *lte_link_plmn(void);

int lte_link_rssi_dbm(void);

typedef struct {
    uint32_t considered;
    uint32_t delivered;
    uint32_t retained;
    uint32_t refused;
    uint32_t failed;
    uint32_t bytes_up;
    uint32_t bytes_down;
    uint32_t ms;
} lte_link_result_t;

/*
 * Upload up to `max_bundles` sealed bundles over the cellular link. Requires
 * lte_link_up() to have returned CAIRN_LTE_OK.
 *
 * Real sealed bundles, not digests: a digest earns no receipt, so it can never
 * prune the card, and a transport that never prunes only postpones the problem
 * it exists to solve. No compression either — the segments are AEAD ciphertext,
 * measured at 90% of original under gzip -9, so there is nothing to win.
 */
void lte_link_upload_pending(uint32_t max_bundles,
                             bool (*should_abort)(void *), void *abort_ctx,
                             lte_link_result_t *out);

#endif /* CAIRN_LTE_UPLINK */

#endif /* CAIRN_LTE_LINK_H */
