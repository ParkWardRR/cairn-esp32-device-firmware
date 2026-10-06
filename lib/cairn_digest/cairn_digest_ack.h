/*
 * The server's acknowledgement of a DIGEST. It is not a receipt and it cannot
 * authorise deletion.
 *
 * The dangerous mistake in this feature is one line long: a firmware path that
 * treats "the server got the digest" as "the server has the bundle" and prunes.
 * A digest is a lossy summary; the bundle is the record. So the rule from
 * contracts/digest/v1 (issue #26, invariant 1) is made structural here:
 *
 *   - a different MESSAGE TYPE: a distinct magic ("CDA" 0xD0) that
 *     cairn_receipt_decode cannot parse, so the prune gate refuses it as malformed;
 *   - a different SIGNING CONTEXT: the signature covers a domain label that no
 *     receipt signs, so even a genuine server signature on an ack, copied into a
 *     receipt-shaped message, does not verify as a receipt
 *     (cairn_receipt_verify fails: RECEIPT_UNVERIFIED);
 *   - a different VERIFIER with a result type that carries no prune authority:
 *     CAIRN_DIGEST_ACK_OK means only "the server holds the provisional digest for
 *     this trip", and nothing in this library can delete anything.
 *
 * Nothing in lib/cairn_digest includes cairn_prune.h, and cairn_prune.c does not
 * know this message exists. A host test presents an ack to
 * cairn_prune_if_receipted and requires refusal (test/host/digest_test.c).
 *
 * PROVISIONAL: layout and label are drafts until contracts/digest/v1 is released.
 */

#ifndef CAIRN_DIGEST_ACK_H
#define CAIRN_DIGEST_ACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "CDA" 0xD0 then the fields; fixed length. */
#define CAIRN_DIGEST_ACK_LEN (4 + 32 + 8 + 8 + 64)

/* Domain separation for the signature. A receipt signs canonical CBOR and no label;
 * this signs the label first. Never share a label between message types. */
#define CAIRN_DIGEST_ACK_SIGN_LABEL "cairn/digest-ack/DRAFT-0"

typedef struct {
    uint8_t  trip_root[32];          /* the digest's trip: content root of its bundle */
    uint64_t server_ingest_utc_ms;
    uint8_t  server_key_id[8];
    uint8_t  signature[64];
} cairn_digest_ack_t;

typedef enum {
    CAIRN_DIGEST_ACK_OK = 0,         /* digest delivered. NOT permission to prune. */
    CAIRN_DIGEST_ACK_MALFORMED,
    CAIRN_DIGEST_ACK_NO_KEY,
    CAIRN_DIGEST_ACK_UNVERIFIED,
    CAIRN_DIGEST_ACK_WRONG_TRIP,
} cairn_digest_ack_result_t;

const char *cairn_digest_ack_result_name(cairn_digest_ack_result_t r);

/* What the server signs: label || trip_root || ingest_ms (LE) || key_id. Returns
 * the length (always label + 48). `out` must hold CAIRN_DIGEST_ACK_SIGNING_MAX. */
#define CAIRN_DIGEST_ACK_SIGNING_MAX 128
size_t cairn_digest_ack_signing_bytes(const cairn_digest_ack_t *a, uint8_t *out, size_t cap);

size_t cairn_digest_ack_encode(const cairn_digest_ack_t *a, uint8_t *out, size_t cap);

bool cairn_digest_ack_decode(const uint8_t *buf, size_t len, cairn_digest_ack_t *out);

/*
 * Decode and verify against the pinned server key and the trip this device sent a
 * digest for. Same strictness as the receipt path: a missing or all-zero key
 * verifies nothing.
 */
cairn_digest_ack_result_t cairn_digest_ack_check(const uint8_t *buf, size_t len,
                                                 const uint8_t pinned_key[32],
                                                 const uint8_t trip_root[32]);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_DIGEST_ACK_H */
