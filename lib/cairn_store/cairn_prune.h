/*
 * Receipt-gated pruning.
 *
 * Kept as portable C, apart from any transport, for one reason: this is the
 * invariant with the worst failure mode in the whole system. Everything else
 * that goes wrong costs a trip's worth of detail; a wrongly authorized prune
 * deletes data permanently and reports success. So the gate lives here, away
 * from the HTTP code, where a host test can drive it with a forged receipt, a
 * receipt for a different bundle, and a receipt signed by the wrong key.
 *
 * The rule, stated once: a bundle may be deleted only when a receipt has been
 * verified against the *pinned* server key and that receipt acknowledges the
 * content root actually uploaded. Both conditions, every time.
 */

#ifndef CAIRN_PRUNE_H
#define CAIRN_PRUNE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAIRN_PRUNE_OK = 0,
    CAIRN_PRUNE_RECEIPT_MALFORMED,
    CAIRN_PRUNE_RECEIPT_UNVERIFIED, /* signature failed against the pinned key */
    CAIRN_PRUNE_WRONG_BUNDLE,       /* genuine signature, different content root */
    CAIRN_PRUNE_NO_PINNED_KEY,
    CAIRN_PRUNE_STORE_FAILED,
} cairn_prune_result_t;

const char *cairn_prune_result_name(cairn_prune_result_t r);

/*
 * Persist a receipt for a bundle. Done before anything is deleted: the receipt
 * is the durable evidence that the data is safe elsewhere, and the bundle bytes
 * are not.
 */
bool cairn_receipt_store(const char *id_text, const uint8_t *receipt,
                         size_t receipt_len);

bool cairn_receipt_exists(const char *id_text);

/*
 * Verify, then prune. The only sanctioned path to deleting bundle data.
 *
 * `pinned_key` may be NULL or all-zero, which returns CAIRN_PRUNE_NO_PINNED_KEY
 * and deletes nothing: an unconfigured device fills its card rather than
 * guessing. Returns CAIRN_PRUNE_WRONG_BUNDLE when the signature is genuine but
 * acknowledges different content — accepting that would let a misconfigured or
 * hostile server induce deletion of data it never received.
 */
cairn_prune_result_t cairn_prune_if_receipted(const char *id_text,
                                              const uint8_t *receipt,
                                              size_t receipt_len,
                                              const uint8_t pinned_key[32],
                                              const uint8_t uploaded_root[32]);

/*
 * Finish a prune interrupted by power loss. An intent record is written before
 * the first delete, so an interrupted prune leaves an intent with no
 * completion — recognizable here, rather than a half-deleted directory that
 * still looks like a sealed bundle awaiting upload.
 *
 * Idempotent; run at boot before any capture begins.
 */
int cairn_prune_resume_interrupted(void);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_PRUNE_H */
