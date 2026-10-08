/* The device half of the v2 intake protocol. See cairn_intake.h. */

#include "cairn_intake.h"

#include <stdio.h>
#include <string.h>

#include "cairn_log.h"

static const char *TAG = "INTAKE";

const char *cairn_intake_outcome_name(cairn_intake_outcome_t o)
{
    switch (o) {
    case CAIRN_INTAKE_RECEIPT: return "receipt";
    case CAIRN_INTAKE_PARTIAL: return "partial";
    case CAIRN_INTAKE_FAILED:  return "failed";
    case CAIRN_INTAKE_ABORTED: return "aborted";
    case CAIRN_INTAKE_REFUSED: return "refused";
    default:                   return "?";
    }
}

void cairn_intake_hex(const uint8_t *in, size_t n, char *out)
{
    static const char D[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = D[in[i] >> 4];
        out[i * 2 + 1] = D[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

/*
 * Which refusals are final.
 *
 * The server's own error mapping is explicit that the distinction a device must
 * be able to make is 4xx versus 5xx, and it chose the codes so this function
 * can exist. The exceptions are the two 4xx codes that are not about the bytes:
 * 429 is backpressure and 409 means "you still have chunks to send", which is
 * the normal state in the middle of a transfer rather than an error at all.
 */
bool cairn_intake_status_permanent(int status)
{
    if (status == 429 || status == 409) return false;
    if (status == 507) return false;   /* quota: the owner can free space */
    return status >= 400 && status < 500;
}

/* ── the offer response reader ────────────────────────────────────────────── */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool           bad;
} scan_t;

static void skip_ws(scan_t *s)
{
    while (s->p < s->end &&
           (*s->p == ' ' || *s->p == '\t' || *s->p == '\r' || *s->p == '\n')) {
        s->p++;
    }
}

static bool take(scan_t *s, char c)
{
    skip_ws(s);
    if (s->p < s->end && (char)*s->p == c) { s->p++; return true; }
    return false;
}

static bool expect(scan_t *s, char c)
{
    if (!take(s, c)) { s->bad = true; return false; }
    return true;
}

/* A JSON string, into `out` (NUL-terminated). Escapes are refused rather than
 * decoded: every string this protocol sends is hex or a fixed word, so an
 * escape means the response is not the one we are parsing. */
static bool scan_string(scan_t *s, char *out, size_t cap)
{
    if (!expect(s, '"')) return false;
    size_t n = 0;
    while (s->p < s->end && *s->p != '"') {
        if (*s->p == '\\') { s->bad = true; return false; }
        if (n + 1 >= cap)  { s->bad = true; return false; }
        out[n++] = (char)*s->p++;
    }
    if (!expect(s, '"')) return false;
    out[n] = '\0';
    return true;
}

/* A non-negative integer. Fractions and exponents are refused: a chunk index
 * that arrived as 3.0 is not a response this server produces. */
static bool scan_uint(scan_t *s, uint64_t *out)
{
    skip_ws(s);
    if (s->p >= s->end || *s->p < '0' || *s->p > '9') { s->bad = true; return false; }

    uint64_t v = 0;
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') {
        if (v > (UINT64_MAX - 9) / 10) { s->bad = true; return false; }
        v = v * 10 + (uint64_t)(*s->p++ - '0');
    }
    if (s->p < s->end && (*s->p == '.' || *s->p == 'e' || *s->p == 'E')) {
        s->bad = true;
        return false;
    }
    *out = v;
    return true;
}

static bool scan_literal(scan_t *s, const char *lit)
{
    skip_ws(s);
    size_t n = strlen(lit);
    if ((size_t)(s->end - s->p) < n || memcmp(s->p, lit, n) != 0) {
        s->bad = true;
        return false;
    }
    s->p += n;
    return true;
}

/* Skip one value of any shape, so an unknown key does not stop the parse. A
 * minor version of the server that adds a field must not break the device. */
static bool skip_value(scan_t *s)
{
    skip_ws(s);
    if (s->p >= s->end) { s->bad = true; return false; }

    switch ((char)*s->p) {
    case '"': {
        char junk[256];
        return scan_string(s, junk, sizeof(junk));
    }
    case 't': return scan_literal(s, "true");
    case 'f': return scan_literal(s, "false");
    case 'n': return scan_literal(s, "null");
    case '[':
        s->p++;
        if (take(s, ']')) return true;
        do {
            if (!skip_value(s)) return false;
        } while (take(s, ','));
        return expect(s, ']');
    case '{':
        s->p++;
        if (take(s, '}')) return true;
        do {
            char key[128];
            if (!scan_string(s, key, sizeof(key))) return false;
            if (!expect(s, ':')) return false;
            if (!skip_value(s)) return false;
        } while (take(s, ','));
        return expect(s, '}');
    default: {
        /* A number, possibly negative: bytes_outstanding is int64 in the
         * response and nothing stops it being 0. */
        if (*s->p == '-') s->p++;
        uint64_t v;
        return scan_uint(s, &v);
    }
    }
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) != n * 2) return false;
    for (size_t i = 0; i < n * 2; i++) {
        char     c = hex[i];
        uint8_t  v;
        if      (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
        else return false;
        if (i % 2 == 0) out[i / 2] = (uint8_t)(v << 4);
        else            out[i / 2] |= v;
    }
    return true;
}

bool cairn_intake_parse_offer(const uint8_t *json, size_t len, size_t chunk_count,
                              cairn_intake_offer_t *out)
{
    memset(out, 0, sizeof(*out));
    if (json == NULL || len == 0) return false;

    scan_t s = { json, json + len, false };

    if (!expect(&s, '{')) return false;

    bool seen_id = false, seen_missing = false, seen_receipt = false;

    if (!take(&s, '}')) {
        do {
            char key[128];
            if (!scan_string(&s, key, sizeof(key))) return false;
            if (!expect(&s, ':')) return false;

            if (strcmp(key, "bundle_id") == 0) {
                if (seen_id) return false;          /* duplicate key */
                seen_id = true;
                char hex[80];
                if (!scan_string(&s, hex, sizeof(hex))) return false;
                if (!hex_to_bytes(hex, out->bundle_id, 16)) return false;
                out->have_bundle_id = true;

            } else if (strcmp(key, "missing_chunks") == 0) {
                if (seen_missing) return false;
                seen_missing = true;
                if (!expect(&s, '[')) return false;
                if (!take(&s, ']')) {
                    do {
                        uint64_t v;
                        if (!scan_uint(&s, &v)) return false;
                        /*
                         * An index the manifest does not describe cannot be
                         * served from the card. Refusing the whole response is
                         * right: a server and a device that disagree about how
                         * many chunks a signed manifest has do not have a
                         * disagreement worth working around.
                         */
                        if (v >= (uint64_t)chunk_count) return false;
                        if (out->missing_count >= CAIRN_MAX_CHUNKS) return false;
                        out->missing[out->missing_count++] = (uint32_t)v;
                    } while (take(&s, ','));
                    if (!expect(&s, ']')) return false;
                }

            } else if (strcmp(key, "receipt_available") == 0) {
                if (seen_receipt) return false;
                seen_receipt = true;
                skip_ws(&s);
                if (s.p < s.end && *s.p == 't') {
                    if (!scan_literal(&s, "true")) return false;
                    out->receipt_available = true;
                } else {
                    if (!scan_literal(&s, "false")) return false;
                }

            } else {
                if (!skip_value(&s)) return false;
            }
        } while (take(&s, ','));

        if (!expect(&s, '}')) return false;
    }

    skip_ws(&s);
    if (s.bad || s.p != s.end) return false;

    /* Without a bundle id there is nowhere to send a chunk. */
    return out->have_bundle_id;
}

/* ── the conversation ─────────────────────────────────────────────────────── */

static bool aborted(bool (*should_abort)(void *), void *ctx)
{
    return should_abort != NULL && should_abort(ctx);
}

/* Byte offset of a chunk in the member stream: the lengths before it.
 *
 * Derived rather than index * CHUNK_BYTES so the sealer's chunk size stays the
 * sealer's business. The descriptors are signed, so this sum is as trustworthy
 * as the manifest itself. */
static uint64_t chunk_offset(const cairn_chunk_t *chunks, size_t i)
{
    uint64_t off = 0;
    for (size_t k = 0; k < i; k++) off += (uint64_t)chunks[k].byte_length;
    return off;
}

/*
 * Read a chunk off the card and check it against its signed descriptor.
 *
 * Done before the chunk is sent, not after, because the cheap failure is a
 * local re-read and the expensive one is 256 KiB of the owner's cellular
 * allowance spent on bytes the server will reject anyway. The card slot on this
 * unit is already known to be intermittent, which makes this a live concern
 * rather than a theoretical one.
 */
static bool chunk_verifies(const cairn_intake_bundle_t *bundle,
                           const cairn_chunk_t *chunk, uint64_t offset,
                           uint8_t *scratch, size_t scratch_len,
                           bool (*should_abort)(void *), void *abort_ctx,
                           bool *was_aborted)
{
    cairn_sha256_t ctx;
    cairn_sha256_init(&ctx);

    uint64_t left = (uint64_t)chunk->byte_length;
    uint64_t at   = offset;

    while (left > 0) {
        if (aborted(should_abort, abort_ctx)) { *was_aborted = true; return false; }

        size_t want = (left > (uint64_t)scratch_len) ? scratch_len : (size_t)left;
        size_t got  = bundle->read_at(bundle->ctx, at, scratch, want);
        if (got != want) {
            CAIRN_LOGE(TAG, "chunk %u: card returned %u of %u bytes at offset %llu",
                       (unsigned)chunk->index, (unsigned)got, (unsigned)want,
                       (unsigned long long)at);
            return false;
        }

        cairn_sha256_update(&ctx, scratch, got);
        at   += got;
        left -= got;
    }

    uint8_t digest[32];
    cairn_sha256_final(&ctx, digest);

    if (memcmp(digest, chunk->sha256, 32) != 0) {
        CAIRN_LOGE(TAG, "chunk %u read off the card does not match its signed "
                        "digest; not sending it",
                   (unsigned)chunk->index);
        return false;
    }
    return true;
}

static bool send_chunk(const cairn_intake_http_t *http,
                       const cairn_intake_bundle_t *bundle,
                       const char *bundle_hex, const cairn_chunk_t *chunk,
                       uint64_t offset, uint8_t *scratch, size_t scratch_len,
                       bool (*should_abort)(void *), void *abort_ctx,
                       int *status, cairn_intake_stats_t *stats,
                       bool *was_aborted)
{
    char digest_hex[65];
    cairn_intake_hex(chunk->sha256, 32, digest_hex);

    char path[CAIRN_INTAKE_PATH_MAX];
    int  n = snprintf(path, sizeof(path), "/api/v2/bundles/%s/chunks/%s",
                      bundle_hex, digest_hex);
    if (n <= 0 || (size_t)n >= sizeof(path)) return false;

    if (!http->begin(http->ctx, "PUT", path, NULL, chunk->byte_length)) {
        return false;
    }

    uint64_t left = (uint64_t)chunk->byte_length;
    uint64_t at   = offset;

    while (left > 0) {
        if (aborted(should_abort, abort_ctx)) {
            *was_aborted = true;
            http->close(http->ctx);
            return false;
        }

        size_t want = (left > (uint64_t)scratch_len) ? scratch_len : (size_t)left;
        size_t got  = bundle->read_at(bundle->ctx, at, scratch, want);
        if (got != want) {
            CAIRN_LOGE(TAG, "chunk %u: short read mid-send", (unsigned)chunk->index);
            http->close(http->ctx);
            return false;
        }

        if (!http->write(http->ctx, scratch, got)) {
            http->close(http->ctx);
            return false;
        }

        stats->bytes_up += (uint32_t)got;
        at   += got;
        left -= got;
    }

    uint8_t resp[CAIRN_INTAKE_MAX_RESPONSE];
    size_t  resp_len = 0;
    bool    ok = http->finish(http->ctx, status, resp, sizeof(resp), &resp_len);
    stats->bytes_down += (uint32_t)resp_len;
    http->close(http->ctx);

    return ok;
}

cairn_intake_outcome_t cairn_intake_deliver(
    const cairn_intake_http_t *http,
    const cairn_intake_bundle_t *bundle,
    const cairn_chunk_t *chunks, size_t chunk_count,
    uint8_t *scratch, size_t scratch_len,
    bool (*should_abort)(void *), void *abort_ctx,
    uint8_t *receipt, size_t receipt_cap, size_t *receipt_len,
    cairn_intake_stats_t *stats)
{
    cairn_intake_stats_t local;
    if (stats == NULL) stats = &local;
    memset(stats, 0, sizeof(*stats));
    if (receipt_len != NULL) *receipt_len = 0;

    if (http == NULL || bundle == NULL || chunks == NULL || chunk_count == 0 ||
        scratch == NULL || scratch_len < CAIRN_INTAKE_MIN_SCRATCH) {
        return CAIRN_INTAKE_FAILED;
    }
    if (chunk_count > CAIRN_MAX_CHUNKS) return CAIRN_INTAKE_FAILED;

    if (aborted(should_abort, abort_ctx)) return CAIRN_INTAKE_ABORTED;

    /* ── offer ───────────────────────────────────────────────────────────── */

    const uint8_t *man = NULL, *sig = NULL;
    size_t         man_len = 0, sig_len = 0;

    if (!bundle->manifest(bundle->ctx, &man, &man_len) || man == NULL || man_len == 0) {
        CAIRN_LOGE(TAG, "cannot read manifest.cbor");
        return CAIRN_INTAKE_FAILED;
    }
    if (!bundle->signature(bundle->ctx, &sig, &sig_len) || sig == NULL || sig_len != 64) {
        CAIRN_LOGE(TAG, "manifest.sig is %u bytes, expected 64", (unsigned)sig_len);
        return CAIRN_INTAKE_FAILED;
    }

    static const char SIG_PREFIX[] = "X-Cairn-Signature: ";
    const size_t      PREFIX_LEN   = sizeof(SIG_PREFIX) - 1;

    char sig_header[sizeof(SIG_PREFIX) + 128];
    memcpy(sig_header, SIG_PREFIX, PREFIX_LEN);
    cairn_intake_hex(sig, 64, sig_header + PREFIX_LEN);

    if (!http->begin(http->ctx, "POST", "/api/v2/bundles/offer", sig_header, man_len)) {
        return CAIRN_INTAKE_FAILED;
    }
    if (!http->write(http->ctx, man, man_len)) {
        http->close(http->ctx);
        return CAIRN_INTAKE_FAILED;
    }
    stats->bytes_up += (uint32_t)man_len;

    uint8_t resp[CAIRN_INTAKE_MAX_RESPONSE];
    size_t  resp_len = 0;
    int     status   = 0;
    bool    ok       = http->finish(http->ctx, &status, resp, sizeof(resp), &resp_len);
    stats->bytes_down += (uint32_t)resp_len;
    stats->last_status = status;
    http->close(http->ctx);

    if (!ok) {
        CAIRN_LOGW(TAG, "offer: the link failed");
        return CAIRN_INTAKE_FAILED;
    }
    if (status != 200) {
        CAIRN_LOGW(TAG, "offer refused with HTTP %d%s", status,
                   cairn_intake_status_permanent(status) ? " (permanent)" : "");
        return cairn_intake_status_permanent(status) ? CAIRN_INTAKE_REFUSED
                                                     : CAIRN_INTAKE_FAILED;
    }

    cairn_intake_offer_t offer;
    if (!cairn_intake_parse_offer(resp, resp_len, chunk_count, &offer)) {
        CAIRN_LOGE(TAG, "offer response is not one this server should produce");
        return CAIRN_INTAKE_FAILED;
    }

    char bundle_hex[33];
    cairn_intake_hex(offer.bundle_id, 16, bundle_hex);

    stats->chunks_skipped = (uint16_t)(chunk_count - offer.missing_count);

    if (offer.receipt_available) {
        CAIRN_LOGI(TAG, "server already holds this bundle and its receipt");
    } else {
        CAIRN_LOGI(TAG, "offer accepted: %u of %u chunks still needed",
                   (unsigned)offer.missing_count, (unsigned)chunk_count);
    }

    /* ── chunks ──────────────────────────────────────────────────────────── */

    bool any_sent = false;

    for (size_t i = 0; i < offer.missing_count; i++) {
        if (aborted(should_abort, abort_ctx)) {
            return any_sent ? CAIRN_INTAKE_PARTIAL : CAIRN_INTAKE_ABORTED;
        }

        uint32_t idx = offer.missing[i];
        const cairn_chunk_t *c = &chunks[idx];
        uint64_t off = chunk_offset(chunks, idx);

        bool was_aborted = false;
        if (!chunk_verifies(bundle, c, off, scratch, scratch_len,
                            should_abort, abort_ctx, &was_aborted)) {
            if (was_aborted) {
                return any_sent ? CAIRN_INTAKE_PARTIAL : CAIRN_INTAKE_ABORTED;
            }
            /* A bad local read is not the server's fault and not permanent:
             * the card may read correctly next time. */
            return any_sent ? CAIRN_INTAKE_PARTIAL : CAIRN_INTAKE_FAILED;
        }

        was_aborted = false;
        if (!send_chunk(http, bundle, bundle_hex, c, off, scratch, scratch_len,
                        should_abort, abort_ctx, &status, stats, &was_aborted)) {
            if (was_aborted) {
                return any_sent ? CAIRN_INTAKE_PARTIAL : CAIRN_INTAKE_ABORTED;
            }
            CAIRN_LOGW(TAG, "chunk %u: the link failed", (unsigned)idx);
            return any_sent ? CAIRN_INTAKE_PARTIAL : CAIRN_INTAKE_FAILED;
        }

        stats->last_status = status;

        if (status != 200) {
            CAIRN_LOGW(TAG, "chunk %u refused with HTTP %d", (unsigned)idx, status);
            if (cairn_intake_status_permanent(status)) return CAIRN_INTAKE_REFUSED;
            return any_sent ? CAIRN_INTAKE_PARTIAL : CAIRN_INTAKE_FAILED;
        }

        any_sent = true;
        stats->chunks_sent++;
    }

    /* ── commit ──────────────────────────────────────────────────────────── */

    if (aborted(should_abort, abort_ctx)) {
        return any_sent ? CAIRN_INTAKE_PARTIAL : CAIRN_INTAKE_ABORTED;
    }

    char path[CAIRN_INTAKE_PATH_MAX];
    int  n = snprintf(path, sizeof(path), "/api/v2/bundles/%s/commit", bundle_hex);
    if (n <= 0 || (size_t)n >= sizeof(path)) return CAIRN_INTAKE_FAILED;

    if (!http->begin(http->ctx, "POST", path, NULL, 0)) {
        return any_sent ? CAIRN_INTAKE_PARTIAL : CAIRN_INTAKE_FAILED;
    }

    /*
     * Straight into the caller's buffer. The receipt's signature covers exactly
     * these bytes, so they are never copied through an intermediate decode.
     */
    size_t got_len = 0;
    ok = http->finish(http->ctx, &status, receipt, receipt_cap, &got_len);
    stats->bytes_down += (uint32_t)got_len;
    stats->last_status = status;
    http->close(http->ctx);

    if (!ok) {
        CAIRN_LOGW(TAG, "commit: the link failed");
        return CAIRN_INTAKE_PARTIAL;
    }

    if (status != 200) {
        CAIRN_LOGW(TAG, "commit refused with HTTP %d%s", status,
                   status == 409 ? " (chunks still missing)" : "");
        if (cairn_intake_status_permanent(status)) return CAIRN_INTAKE_REFUSED;
        return CAIRN_INTAKE_PARTIAL;
    }

    if (got_len == 0) {
        CAIRN_LOGE(TAG, "commit returned 200 with an empty body; no receipt");
        return CAIRN_INTAKE_PARTIAL;
    }

    if (receipt_len != NULL) *receipt_len = got_len;

    CAIRN_LOGI(TAG, "committed: %u chunks sent, %u already held, %u bytes up, "
                    "receipt %u bytes",
               (unsigned)stats->chunks_sent, (unsigned)stats->chunks_skipped,
               (unsigned)stats->bytes_up, (unsigned)got_len);

    return CAIRN_INTAKE_RECEIPT;
}
