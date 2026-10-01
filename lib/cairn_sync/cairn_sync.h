/*
 * Upload, receipt verification, and receipt-gated pruning.
 *
 * This module owns the invariant that matters most on a device with a finite
 * card: no byte is deleted without a locally verified signed receipt. Three
 * things follow, and none of them are negotiable here:
 *
 *   - A receipt is verified on the device against a *pinned* server key, not
 *     against whatever key the response happens to carry. A server that cannot
 *     produce a signature over the content root it received cannot induce a
 *     deletion.
 *   - A chunk acknowledgement is not a receipt. Accepted bytes are not durable
 *     bytes, and only a receipt says the data is safe.
 *   - Pruning is transactional: an intent record is written before the first
 *     delete and a completion record after the last, so an interrupted prune is
 *     recognizable at the next boot rather than leaving a half-deleted bundle
 *     that looks sealed.
 *
 * Transport integrity is separate from data identity. The content root is what
 * is signed and verified; TLS or its absence changes who can read the upload,
 * not whether a deletion is authorized.
 */

#ifndef CAIRN_SYNC_H
#define CAIRN_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAIRN_SYNC_OK = 0,
    CAIRN_SYNC_NO_NETWORK,
    CAIRN_SYNC_NOTHING_TO_DO,
    CAIRN_SYNC_OFFER_FAILED,
    CAIRN_SYNC_TRANSFER_FAILED,
    CAIRN_SYNC_COMMIT_FAILED,
    CAIRN_SYNC_RECEIPT_INVALID,
    CAIRN_SYNC_LOCAL_ERROR,
} cairn_sync_result_t;

const char *cairn_sync_result_name(cairn_sync_result_t r);

typedef struct {
    uint32_t bundles_offered;
    uint32_t bundles_receipted;
    uint32_t bundles_pruned;
    uint32_t chunks_sent;
    uint32_t chunks_skipped;    /* the server already held these bytes */
    uint32_t receipts_rejected;
    uint64_t bytes_sent;
} cairn_sync_stats_t;

/*
 * Load the device's TLS credentials from the card. Call once after the card
 * mounts and before the first sync.
 *
 * Returns true when mTLS is usable: a CA pinned at build time *and* a client
 * certificate and key present on the card. Returns false — and logs exactly
 * what is missing — when uploads will fall back to plain HTTP. That fallback is
 * deliberate rather than fatal: the receipt signature, never the transport, is
 * what authorizes deleting data.
 */
bool cairn_sync_load_credentials(void);
bool cairn_sync_tls_active(void);

/*
 * The server base URL on whichever transport is active, and a request started
 * on it. Exposed so the OTA path reaches the server exactly as sync does —
 * including mTLS when it is configured, rather than quietly downgrading for
 * firmware of all things.
 */
void cairn_sync_base_url(char *out, size_t cap);

/* Bring up the network. Returns false if no usable link appeared in time. */
bool cairn_sync_connect(uint32_t timeout_ms);
void cairn_sync_disconnect(void);
bool cairn_sync_is_connected(void);
int  cairn_sync_rssi(void);

/*
 * Upload every sealed bundle awaiting a receipt, verify each receipt, and prune
 * what the receipts authorize. Safe to interrupt at any point: progress is
 * derived from what is on the card, never from in-memory state.
 */
cairn_sync_result_t cairn_sync_run(cairn_sync_stats_t *stats);

/*
 * Finish or roll forward a prune interrupted by power loss. Idempotent; run at
 * boot before any capture begins.
 */
int cairn_sync_resume_interrupted_prunes(void);

#ifdef __cplusplus
}

/* C++ only: HTTPClient is a C++ type, and OTA is the only other caller. */
class HTTPClient;
bool cairn_sync_begin_request(HTTPClient &http, const char *url);
#endif

#endif /* CAIRN_SYNC_H */
