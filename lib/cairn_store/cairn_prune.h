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
 * The verification half of the gate, with no side effects: decode the receipt,
 * check its signature against the pinned key, and check that it acknowledges
 * `uploaded_root`. Returns CAIRN_PRUNE_OK only when both hold.
 *
 * Exists so a caller can reject a bad receipt BEFORE writing anything to the
 * card. Storing first and verifying after would let a forged receipt overwrite a
 * genuine one that is already on the card awaiting its prune.
 */
cairn_prune_result_t cairn_receipt_check(const uint8_t *receipt, size_t receipt_len,
                                         const uint8_t pinned_key[32],
                                         const uint8_t uploaded_root[32]);

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
 * Delete sealed bundles whose manifest version is older than the one this
 * firmware writes. Returns false only if the bundle directory cannot be listed.
 *
 * The second and only other deletion path, and the one that does not require a
 * receipt — so it is here, beside the receipt gate, rather than somewhere a
 * reader would not think to audit.
 *
 * It exists because a pre-v3 bundle is unreachable, not merely inconvenient:
 * this firmware's decoder refuses it and the server would refuse it too, so no
 * transport can carry it and no receipt can ever be earned for it. Without this
 * it occupies the card forever. The owner invokes it deliberately over the USB
 * console, where physical access is already the trust boundary.
 *
 * The narrowing is what keeps it honest. A bundle is dropped only when the
 * version was positively read AND is below CAIRN_MANIFEST_VERSION. A manifest
 * whose shape cannot be parsed is left alone, because that may be a torn write
 * worth recovering — this must never become a way to delete data a receipt
 * could still redeem.
 */
bool cairn_prune_legacy_bundles(uint32_t *dropped, uint64_t *bytes_freed);

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
