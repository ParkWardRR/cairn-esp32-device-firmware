/*
 * The device half of the v2 intake protocol: offer, chunks, commit.
 *
 * This is the shared core of both network transports. Wi-Fi and LTE differ in
 * how a byte reaches the server, not in what is said, so the conversation lives
 * here once — as portable C with the HTTP primitive and the bundle's bytes both
 * injected — and the two shims differ only in their implementation of those.
 * That is the same split as lib/cairn_offload plus src/ble_offload.cpp, and for
 * the same reason: the protocol, its refusals and its resume logic are then
 * exercised on the host against exactly the code the device runs.
 *
 * The conversation (server internal/httpapi):
 *
 *   POST /api/v2/bundles/offer
 *       body: manifest.cbor verbatim
 *       header X-Cairn-Signature: hex of the 64-byte Ed25519 manifest signature
 *       -> 200 {bundle_id, missing_chunks[], total_chunks, bytes_expected,
 *               bytes_outstanding, receipt_available}
 *
 *   PUT  /api/v2/bundles/{bundle_id}/chunks/{chunk_sha256}
 *       body: that chunk's bytes
 *       -> 200 {accepted, missing_chunks[]}
 *
 *   POST /api/v2/bundles/{bundle_id}/commit
 *       -> 200, body is the receipt as raw CBOR
 *
 * Three properties are worth stating because they are what make a second
 * attempt cheap rather than wasteful:
 *
 *   - Chunks are addressed by digest, never by offset, so either side may
 *     restart without invalidating progress. `missing_chunks` from the offer is
 *     the resume point: a slot that died after eleven of twenty chunks sends
 *     nine on the next attempt, not twenty.
 *   - A chunk's offset in the bundle stream is derived by summing the preceding
 *     descriptors' lengths, not from a compiled-in chunk size. The sealer's
 *     chunk size is its own business; coupling to it here would be a silent
 *     break the day it changed.
 *   - Every chunk is hashed off the card and checked against its signed
 *     descriptor BEFORE it is sent. The server would catch a bad chunk anyway,
 *     but on a metered link finding out after spending 256 KiB of the owner's
 *     data is the expensive way to learn that a card read was flaky.
 *
 * What this module does not do: decide whether a path may run (lib/cairn_uplink),
 * count bytes against a cap (lib/cairn_usage), or delete anything. It returns
 * the receipt bytes; whether they authorize a prune is cairn_prune_if_receipted's
 * judgement, against the key pinned in firmware.
 */

#ifndef CAIRN_INTAKE_H
#define CAIRN_INTAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cairn_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Longest path this module builds, with room to spare.
 *
 * The chunk route is the long one: "/api/v2/bundles/" (16) + the bundle id as
 * 32 hex characters + "/chunks/" (8) + the chunk digest as 64 hex characters,
 * so 120 plus the terminator.
 */
#define CAIRN_INTAKE_PATH_MAX 160

/* A receipt is small; the offload path bounds it the same way. */
#define CAIRN_INTAKE_MAX_RECEIPT 1024u

/* Enough for the offer response with a full missing_chunks array of 64. */
#define CAIRN_INTAKE_MAX_RESPONSE 1024u

/* The smallest scratch buffer the caller may supply for streaming chunk bytes
 * off the card. Larger is faster; this is the floor, not a recommendation. */
#define CAIRN_INTAKE_MIN_SCRATCH 512u

/* ── injected HTTP ────────────────────────────────────────────────────────── */

/*
 * One request at a time, streamed.
 *
 * Streamed rather than buffered because a chunk is 256 KiB and the device has
 * about 289 KB of internal heap: a buffer-the-body interface would work on the
 * bench and fail on the second concurrent allocation in a car.
 *
 * `begin` is told the exact content length, so the shim sends a plain
 * Content-Length request and never has to chunk the transfer-encoding.
 * `finish` returns the HTTP status and up to `resp_cap` bytes of the response
 * body; a longer body is a protocol error, not something to grow a buffer for.
 *
 * Any of these returning false means the link failed. That is distinct from a
 * 4xx or 5xx, which is a *successful* exchange carrying a refusal, and the two
 * lead to different decisions.
 */
typedef struct {
    void *ctx;

    bool (*begin)(void *ctx, const char *method, const char *path,
                  const char *extra_header, size_t content_length);
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    bool (*finish)(void *ctx, int *status,
                   uint8_t *resp, size_t resp_cap, size_t *resp_len);
    void (*close)(void *ctx);
} cairn_intake_http_t;

/* ── injected bundle bytes ────────────────────────────────────────────────── */

/*
 * The sealed bundle, as this module needs to see it: the signed manifest, its
 * signature, and random access into the concatenated member stream.
 *
 * `read_at` offsets are into that stream in canonical member order, which is
 * the same partition the sealer chunked (spec §6.1). A short read is a failure,
 * not an end of stream: every offset this module asks for is covered by a
 * signed descriptor, so a card that returns fewer bytes is a card to stop
 * trusting mid-transfer.
 */
typedef struct {
    void *ctx;

    bool   (*manifest)(void *ctx, const uint8_t **bytes, size_t *len);
    bool   (*signature)(void *ctx, const uint8_t **sig, size_t *len);
    size_t (*read_at)(void *ctx, uint64_t offset, uint8_t *out, size_t len);
} cairn_intake_bundle_t;

/* ── outcome ──────────────────────────────────────────────────────────────── */

typedef enum {
    /* Committed, and the receipt bytes are in the caller's buffer. */
    CAIRN_INTAKE_RECEIPT = 0,
    /* Chunks were accepted but the bundle is not committed. Try again; the next
     * offer will ask only for what is still missing. */
    CAIRN_INTAKE_PARTIAL,
    /* The link failed and nothing durable was achieved on this attempt. */
    CAIRN_INTAKE_FAILED,
    /* should_abort fired: a trip started, the battery fell, the slot ended. */
    CAIRN_INTAKE_ABORTED,
    /*
     * The server refused in a way that the same bytes will always be refused:
     * not enrolled, revoked, key mismatch, quarantined, bad signature,
     * non-canonical manifest, content root mismatch.
     *
     * Separate from FAILED on purpose. A device that cannot tell "never" from
     * "not now" either gives up on a recoverable outage or spends a metered
     * link retrying a permanent rejection forever.
     */
    CAIRN_INTAKE_REFUSED
} cairn_intake_outcome_t;

const char *cairn_intake_outcome_name(cairn_intake_outcome_t o);

typedef struct {
    uint32_t bytes_up;         /* body bytes written, for the usage accounting */
    uint32_t bytes_down;
    uint16_t chunks_sent;
    uint16_t chunks_skipped;   /* already held by the server: the resume saving */
    int      last_status;      /* the HTTP status that ended the attempt, or 0 */
    bool     already_committed;
} cairn_intake_stats_t;

/* ── the one entry point ──────────────────────────────────────────────────── */

/*
 * Move one sealed bundle to the server and bring back its receipt.
 *
 * `should_abort` is polled before every request and between scratch-sized
 * writes, so an ordered abort stops a 256 KiB chunk partway rather than after
 * it. Returning CAIRN_INTAKE_ABORTED after some chunks were accepted is normal
 * and not a loss: the server keeps them.
 *
 * `receipt` receives the raw CBOR receipt on CAIRN_INTAKE_RECEIPT and is
 * untouched otherwise. The bytes are passed through exactly as received,
 * because the signature covers exactly those bytes and a decode-and-re-encode
 * step here is precisely where a canonical-encoding bug would hide.
 */
cairn_intake_outcome_t cairn_intake_deliver(
    const cairn_intake_http_t *http,
    const cairn_intake_bundle_t *bundle,
    const cairn_chunk_t *chunks, size_t chunk_count,
    uint8_t *scratch, size_t scratch_len,
    bool (*should_abort)(void *), void *abort_ctx,
    uint8_t *receipt, size_t receipt_cap, size_t *receipt_len,
    cairn_intake_stats_t *stats);

/* ── exposed for the host tests ───────────────────────────────────────────── */

/*
 * The offer response, as much of it as the device acts on.
 *
 * A strict, purpose-built reader rather than a JSON library: the firmware's own
 * wire format is deterministic CBOR and pulling in a parser for three fields
 * would be the larger cost. It refuses anything it would not itself have
 * produced — a duplicate key, a missing bundle_id, a chunk index past the
 * manifest's count, a number that is not an integer — on the same principle as
 * cairn_manifest_decode.
 */
typedef struct {
    uint8_t  bundle_id[16];
    bool     have_bundle_id;
    uint32_t missing[CAIRN_MAX_CHUNKS];
    size_t   missing_count;
    bool     receipt_available;
} cairn_intake_offer_t;

bool cairn_intake_parse_offer(const uint8_t *json, size_t len, size_t chunk_count,
                              cairn_intake_offer_t *out);

/* True when this status will never accept the same bytes. */
bool cairn_intake_status_permanent(int status);

/* Lowercase hex, no NUL beyond the string. out needs 2*n+1 bytes. */
void cairn_intake_hex(const uint8_t *in, size_t n, char *out);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_INTAKE_H */
