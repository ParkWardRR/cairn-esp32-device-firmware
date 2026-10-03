/* Manifest and receipt encoding, decoding and signing. */

#include <string.h>

#include "cairn_format.h"

/* Manifest CBOR keys. Integer keys keep the encoding compact and unambiguous. */
#define K_MANIFEST_VERSION 1
#define K_BUNDLE_ID        2
#define K_DEVICE_ID        3
#define K_DEVICE_KEY_ID    4
#define K_BOOT_ID          5
#define K_FIRMWARE_VERSION 6
#define K_SCHEMA_VERSION   7
#define K_CAPTURE_STARTED  8
#define K_CAPTURE_ENDED    9
#define K_UTC_BASIS_MS     10
#define K_UTC_BASIS_ACC_MS 11
#define K_FIRST_SEQ        12
#define K_LAST_SEQ         13
#define K_RECORD_COUNTS    14
#define K_MEMBERS          15
#define K_CHUNK_DESCS      16
#define K_CONTENT_ROOT     17
#define K_PREVIOUS_ROOT    18
#define K_POLICY_VERSION   19
#define K_RECOVERY_STATE   20
#define K_DISCARDED_TAIL   21
#define K_SIGNATURE_ALGO   22
#define K_TRIP_SEQ         23

#define MANIFEST_FIELD_COUNT_BASE 22
#define MANIFEST_FIELD_COUNT_MAX  23

/* Receipt keys. 1..10 are covered by the signature; 11 is the signature. */
#define RK_VERSION      1
#define RK_RECEIPT_ID   2
#define RK_DEVICE_ID    3
#define RK_BUNDLE_ID    4
#define RK_CONTENT_ROOT 5
#define RK_INGEST_UTC   6
#define RK_SERVER_KEY   7
#define RK_SCHEMA_VER   8
#define RK_OBJECT_IDS   9
#define RK_SIG_ALGO     10
#define RK_SIGNATURE    11

#define RECEIPT_SIGNED_FIELD_COUNT 10
#define RECEIPT_FIELD_COUNT        11

/* ── manifest encode ──────────────────────────────────────────────────────── */

cairn_err_t cairn_manifest_encode(const cairn_manifest_t *m,
                                  uint8_t *out, size_t out_cap, size_t *written)
{
    if (strcmp(m->signature_algorithm, CAIRN_SIGALG_ED25519) != 0) {
        return CAIRN_ERR_MALFORMED;
    }
    if (m->member_count > CAIRN_MAX_MEMBERS) return CAIRN_ERR_TOO_MANY_MEMBERS;

    cairn_cbor_enc_t e;
    cairn_cbor_init(&e, out, out_cap);

    cairn_cbor_map(&e, m->has_trip_seq ? MANIFEST_FIELD_COUNT_MAX
                                      : MANIFEST_FIELD_COUNT_BASE);

    cairn_cbor_key(&e, K_MANIFEST_VERSION);
    cairn_cbor_uint(&e, m->manifest_version);
    cairn_cbor_key(&e, K_BUNDLE_ID);
    cairn_cbor_bytes(&e, m->bundle_id, 16);
    cairn_cbor_key(&e, K_DEVICE_ID);
    cairn_cbor_bytes(&e, m->device_id, 16);
    cairn_cbor_key(&e, K_DEVICE_KEY_ID);
    cairn_cbor_bytes(&e, m->device_key_id, 8);
    cairn_cbor_key(&e, K_BOOT_ID);
    cairn_cbor_bytes(&e, m->boot_id, 16);
    cairn_cbor_key(&e, K_FIRMWARE_VERSION);
    cairn_cbor_text(&e, m->firmware_version);
    cairn_cbor_key(&e, K_SCHEMA_VERSION);
    cairn_cbor_uint(&e, m->schema_version);
    cairn_cbor_key(&e, K_CAPTURE_STARTED);
    cairn_cbor_uint(&e, m->capture_started_monotonic_us);
    cairn_cbor_key(&e, K_CAPTURE_ENDED);
    cairn_cbor_uint(&e, m->capture_ended_monotonic_us);
    cairn_cbor_key(&e, K_UTC_BASIS_MS);
    cairn_cbor_uint(&e, m->utc_basis_ms);
    cairn_cbor_key(&e, K_UTC_BASIS_ACC_MS);
    cairn_cbor_uint(&e, m->utc_basis_acc_ms);
    cairn_cbor_key(&e, K_FIRST_SEQ);
    cairn_cbor_uint(&e, m->first_seq);
    cairn_cbor_key(&e, K_LAST_SEQ);
    cairn_cbor_uint(&e, m->last_seq);

    /* Record counts: a nested map, keys ascending by record type. */
    size_t counted = 0;
    for (int t = 0; t < 256; t++) {
        if (m->record_counts[t] != 0) counted++;
    }
    cairn_cbor_key(&e, K_RECORD_COUNTS);
    cairn_cbor_map(&e, counted);
    for (int t = 0; t < 256; t++) {
        if (m->record_counts[t] == 0) continue;
        cairn_cbor_key(&e, (uint64_t)t);
        cairn_cbor_uint(&e, m->record_counts[t]);
    }

    /* Members: sorted by raw name bytes, as the content root requires. */
    cairn_member_t sorted[CAIRN_MAX_MEMBERS];
    for (size_t i = 0; i < m->member_count; i++) sorted[i] = m->members[i];
    cairn_sort_members(sorted, m->member_count);

    cairn_cbor_key(&e, K_MEMBERS);
    cairn_cbor_array(&e, m->member_count);
    for (size_t i = 0; i < m->member_count; i++) {
        cairn_cbor_array(&e, 3);
        cairn_cbor_text(&e, sorted[i].name);
        cairn_cbor_uint(&e, sorted[i].length);
        cairn_cbor_bytes(&e, sorted[i].sha256, 32);
    }

    cairn_cbor_key(&e, K_CHUNK_DESCS);
    cairn_cbor_array(&e, m->chunk_count);
    for (size_t i = 0; i < m->chunk_count; i++) {
        cairn_cbor_array(&e, 3);
        cairn_cbor_uint(&e, m->chunks[i].index);
        cairn_cbor_uint(&e, m->chunks[i].byte_length);
        cairn_cbor_bytes(&e, m->chunks[i].sha256, 32);
    }

    cairn_cbor_key(&e, K_CONTENT_ROOT);
    cairn_cbor_bytes(&e, m->content_root, 32);

    cairn_cbor_key(&e, K_PREVIOUS_ROOT);
    if (m->has_previous_root) {
        cairn_cbor_bytes(&e, m->previous_bundle_root, 32);
    } else {
        cairn_cbor_null(&e);
    }

    cairn_cbor_key(&e, K_POLICY_VERSION);
    cairn_cbor_uint(&e, m->policy_version);
    cairn_cbor_key(&e, K_RECOVERY_STATE);
    cairn_cbor_uint(&e, m->recovery_state);
    cairn_cbor_key(&e, K_DISCARDED_TAIL);
    cairn_cbor_uint(&e, m->discarded_tail_bytes);
    cairn_cbor_key(&e, K_SIGNATURE_ALGO);
    cairn_cbor_text(&e, m->signature_algorithm);

    if (m->has_trip_seq) {
        cairn_cbor_key(&e, K_TRIP_SEQ);
        cairn_cbor_uint(&e, m->trip_seq);
    }

    if (e.overflow) return CAIRN_ERR_BUFFER_TOO_SMALL;
    if (written) *written = e.len;
    return CAIRN_OK;
}

/* ── manifest decode ──────────────────────────────────────────────────────── */

static cairn_err_t manifest_decode_raw(const uint8_t *buf, size_t len,
                                       cairn_manifest_t *m)
{
    cairn_cbor_dec_t d;
    cairn_cbor_dec_init(&d, buf, len);

    memset(m, 0, sizeof(*m));

    size_t n;
    cairn_err_t err = cairn_cbor_map_header(&d, &n);
    if (err != CAIRN_OK) return err;
    if (n != MANIFEST_FIELD_COUNT_BASE && n != MANIFEST_FIELD_COUNT_MAX)
        return CAIRN_ERR_MALFORMED;

    for (size_t i = 0; i < n; i++) {
        uint64_t key;
        err = cairn_cbor_uint_read(&d, &key);
        if (err != CAIRN_OK) return err;

        uint64_t v;

        switch (key) {
        case K_MANIFEST_VERSION:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            if (v != CAIRN_MANIFEST_VERSION) return CAIRN_ERR_MALFORMED;
            m->manifest_version = (uint8_t)v;
            break;
        case K_BUNDLE_ID:
            if ((err = cairn_cbor_bytes_n(&d, m->bundle_id, 16)) != CAIRN_OK) return err;
            break;
        case K_DEVICE_ID:
            if ((err = cairn_cbor_bytes_n(&d, m->device_id, 16)) != CAIRN_OK) return err;
            break;
        case K_DEVICE_KEY_ID:
            if ((err = cairn_cbor_bytes_n(&d, m->device_key_id, 8)) != CAIRN_OK) return err;
            break;
        case K_BOOT_ID:
            if ((err = cairn_cbor_bytes_n(&d, m->boot_id, 16)) != CAIRN_OK) return err;
            break;
        case K_FIRMWARE_VERSION:
            err = cairn_cbor_text_read(&d, m->firmware_version, sizeof(m->firmware_version));
            if (err != CAIRN_OK) return err;
            break;
        case K_SCHEMA_VERSION:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            m->schema_version = (uint8_t)v;
            break;
        case K_CAPTURE_STARTED:
            if ((err = cairn_cbor_uint_read(&d, &m->capture_started_monotonic_us)) != CAIRN_OK)
                return err;
            break;
        case K_CAPTURE_ENDED:
            if ((err = cairn_cbor_uint_read(&d, &m->capture_ended_monotonic_us)) != CAIRN_OK)
                return err;
            break;
        case K_UTC_BASIS_MS:
            if ((err = cairn_cbor_uint_read(&d, &m->utc_basis_ms)) != CAIRN_OK) return err;
            break;
        case K_UTC_BASIS_ACC_MS:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            m->utc_basis_acc_ms = (uint32_t)v;
            break;
        case K_FIRST_SEQ:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            m->first_seq = (uint32_t)v;
            break;
        case K_LAST_SEQ:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            m->last_seq = (uint32_t)v;
            break;
        case K_RECORD_COUNTS: {
            size_t cnt;
            if ((err = cairn_cbor_map_header(&d, &cnt)) != CAIRN_OK) return err;
            int64_t prev = -1;
            for (size_t j = 0; j < cnt; j++) {
                uint64_t rt, c;
                if ((err = cairn_cbor_uint_read(&d, &rt)) != CAIRN_OK) return err;
                if (rt > 255 || (int64_t)rt <= prev) return CAIRN_ERR_NON_CANONICAL;
                prev = (int64_t)rt;
                if ((err = cairn_cbor_uint_read(&d, &c)) != CAIRN_OK) return err;
                m->record_counts[rt] = (uint32_t)c;
            }
            break;
        }
        case K_MEMBERS: {
            size_t cnt;
            if ((err = cairn_cbor_array_header(&d, &cnt)) != CAIRN_OK) return err;
            if (cnt > CAIRN_MAX_MEMBERS) return CAIRN_ERR_TOO_MANY_MEMBERS;
            for (size_t j = 0; j < cnt; j++) {
                size_t fields;
                if ((err = cairn_cbor_array_header(&d, &fields)) != CAIRN_OK) return err;
                if (fields != 3) return CAIRN_ERR_MALFORMED;

                err = cairn_cbor_text_read(&d, m->members[j].name, CAIRN_MAX_MEMBER_NAME);
                if (err != CAIRN_OK) return err;
                if ((err = cairn_cbor_uint_read(&d, &m->members[j].length)) != CAIRN_OK)
                    return err;
                if ((err = cairn_cbor_bytes_n(&d, m->members[j].sha256, 32)) != CAIRN_OK)
                    return err;

                if (j > 0 && strcmp(m->members[j].name, m->members[j - 1].name) <= 0) {
                    return CAIRN_ERR_NON_CANONICAL;
                }
            }
            m->member_count = cnt;
            break;
        }
        case K_CHUNK_DESCS: {
            size_t cnt;
            if ((err = cairn_cbor_array_header(&d, &cnt)) != CAIRN_OK) return err;
            if (cnt > CAIRN_MAX_CHUNKS) return CAIRN_ERR_MALFORMED;
            for (size_t j = 0; j < cnt; j++) {
                size_t fields;
                if ((err = cairn_cbor_array_header(&d, &fields)) != CAIRN_OK) return err;
                if (fields != 3) return CAIRN_ERR_MALFORMED;

                uint64_t idx, blen;
                if ((err = cairn_cbor_uint_read(&d, &idx)) != CAIRN_OK) return err;
                if ((err = cairn_cbor_uint_read(&d, &blen)) != CAIRN_OK) return err;
                if ((err = cairn_cbor_bytes_n(&d, m->chunks[j].sha256, 32)) != CAIRN_OK)
                    return err;

                if (idx != j) return CAIRN_ERR_MALFORMED;
                m->chunks[j].index       = (uint32_t)idx;
                m->chunks[j].byte_length = (uint32_t)blen;
            }
            m->chunk_count = cnt;
            break;
        }
        case K_CONTENT_ROOT:
            if ((err = cairn_cbor_bytes_n(&d, m->content_root, 32)) != CAIRN_OK) return err;
            break;
        case K_PREVIOUS_ROOT:
            if (cairn_cbor_is_null(&d)) {
                m->has_previous_root = false;
            } else {
                err = cairn_cbor_bytes_n(&d, m->previous_bundle_root, 32);
                if (err != CAIRN_OK) return err;
                m->has_previous_root = true;
            }
            break;
        case K_POLICY_VERSION:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            m->policy_version = (uint8_t)v;
            break;
        case K_RECOVERY_STATE:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            if (v > CAIRN_RECOVERY_SALVAGED) return CAIRN_ERR_MALFORMED;
            m->recovery_state = (uint8_t)v;
            break;
        case K_DISCARDED_TAIL:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            m->discarded_tail_bytes = (uint32_t)v;
            break;
        case K_SIGNATURE_ALGO:
            err = cairn_cbor_text_read(&d, m->signature_algorithm,
                                       sizeof(m->signature_algorithm));
            if (err != CAIRN_OK) return err;
            if (strcmp(m->signature_algorithm, CAIRN_SIGALG_ED25519) != 0) {
                return CAIRN_ERR_MALFORMED;
            }
            break;
        case K_TRIP_SEQ:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            m->trip_seq = (uint32_t)v;
            m->has_trip_seq = true;
            break;
        default:
            return CAIRN_ERR_MALFORMED;
        }
    }

    if (d.pos != d.len) return CAIRN_ERR_MALFORMED;
    return CAIRN_OK;
}

cairn_err_t cairn_manifest_decode(const uint8_t *buf, size_t len,
                                  cairn_manifest_t *out,
                                  uint8_t *scratch, size_t scratch_cap)
{
    cairn_err_t err = manifest_decode_raw(buf, len, out);
    if (err != CAIRN_OK) return err;

    /*
     * Canonicality by round-trip: re-encode and require byte equality. A
     * manifest whose bytes we would not ourselves have produced cannot be
     * safely re-serialized, and any discrepancy would silently break signature
     * verification.
     */
    size_t written = 0;
    err = cairn_manifest_encode(out, scratch, scratch_cap, &written);
    if (err != CAIRN_OK) return err;

    if (written != len || memcmp(scratch, buf, len) != 0) {
        return CAIRN_ERR_NON_CANONICAL;
    }

    return CAIRN_OK;
}

cairn_err_t cairn_manifest_verify_content_root(const cairn_manifest_t *m)
{
    uint8_t computed[32];

    cairn_err_t err = cairn_content_root(m->members, m->member_count, computed);
    if (err != CAIRN_OK) return err;

    if (memcmp(computed, m->content_root, 32) != 0) {
        return CAIRN_ERR_CONTENT_ROOT_MISMATCH;
    }
    return CAIRN_OK;
}

/* ── manifest signing ─────────────────────────────────────────────────────── */

cairn_err_t cairn_manifest_sign(const cairn_manifest_t *m,
                                const uint8_t seed[32], const uint8_t pub[32],
                                uint8_t *encoded, size_t encoded_cap, size_t *written,
                                uint8_t sig[64])
{
    size_t n = 0;
    cairn_err_t err = cairn_manifest_encode(m, encoded, encoded_cap, &n);
    if (err != CAIRN_OK) return err;

    cairn_ed25519_sign(encoded, n, seed, pub, sig);
    if (written) *written = n;
    return CAIRN_OK;
}

cairn_err_t cairn_manifest_verify(const uint8_t *encoded, size_t len,
                                  const uint8_t sig[64], const uint8_t pub[32])
{
    /*
     * Verified against the bytes as received, before parsing, so verification
     * never depends on this implementation's ability to re-encode.
     */
    if (!cairn_ed25519_verify(encoded, len, sig, pub)) return CAIRN_ERR_BAD_SIGNATURE;
    return CAIRN_OK;
}

/* ── receipt ──────────────────────────────────────────────────────────────── */

static cairn_err_t receipt_encode_fields(const cairn_receipt_t *r, bool with_sig,
                                         uint8_t *out, size_t out_cap, size_t *written)
{
    if (strcmp(r->signature_algorithm, CAIRN_SIGALG_ED25519) != 0) {
        return CAIRN_ERR_MALFORMED;
    }

    cairn_cbor_enc_t e;
    cairn_cbor_init(&e, out, out_cap);

    cairn_cbor_map(&e, with_sig ? RECEIPT_FIELD_COUNT : RECEIPT_SIGNED_FIELD_COUNT);

    cairn_cbor_key(&e, RK_VERSION);
    cairn_cbor_uint(&e, r->receipt_version);
    cairn_cbor_key(&e, RK_RECEIPT_ID);
    cairn_cbor_bytes(&e, r->receipt_id, 16);
    cairn_cbor_key(&e, RK_DEVICE_ID);
    cairn_cbor_bytes(&e, r->device_id, 16);
    cairn_cbor_key(&e, RK_BUNDLE_ID);
    cairn_cbor_bytes(&e, r->bundle_id, 16);
    cairn_cbor_key(&e, RK_CONTENT_ROOT);
    cairn_cbor_bytes(&e, r->content_root, 32);
    cairn_cbor_key(&e, RK_INGEST_UTC);
    cairn_cbor_uint(&e, r->server_ingest_utc_ms);
    cairn_cbor_key(&e, RK_SERVER_KEY);
    cairn_cbor_bytes(&e, r->server_key_id, 8);
    cairn_cbor_key(&e, RK_SCHEMA_VER);
    cairn_cbor_uint(&e, r->ingest_schema_version);

    cairn_cbor_key(&e, RK_OBJECT_IDS);
    cairn_cbor_array(&e, r->object_id_count);
    for (size_t i = 0; i < r->object_id_count; i++) {
        cairn_cbor_text(&e, r->object_ids[i]);
    }

    cairn_cbor_key(&e, RK_SIG_ALGO);
    cairn_cbor_text(&e, r->signature_algorithm);

    if (with_sig) {
        cairn_cbor_key(&e, RK_SIGNATURE);
        cairn_cbor_bytes(&e, r->signature, 64);
    }

    if (e.overflow) return CAIRN_ERR_BUFFER_TOO_SMALL;
    if (written) *written = e.len;
    return CAIRN_OK;
}

cairn_err_t cairn_receipt_signing_bytes(const cairn_receipt_t *r,
                                        uint8_t *out, size_t out_cap, size_t *written)
{
    return receipt_encode_fields(r, false, out, out_cap, written);
}

cairn_err_t cairn_receipt_encode(const cairn_receipt_t *r,
                                 uint8_t *out, size_t out_cap, size_t *written)
{
    return receipt_encode_fields(r, true, out, out_cap, written);
}

static cairn_err_t receipt_decode_raw(const uint8_t *buf, size_t len, cairn_receipt_t *r)
{
    cairn_cbor_dec_t d;
    cairn_cbor_dec_init(&d, buf, len);

    memset(r, 0, sizeof(*r));

    size_t n;
    cairn_err_t err = cairn_cbor_map_header(&d, &n);
    if (err != CAIRN_OK) return err;
    if (n != RECEIPT_FIELD_COUNT) return CAIRN_ERR_MALFORMED;

    for (size_t i = 0; i < n; i++) {
        uint64_t key;
        err = cairn_cbor_uint_read(&d, &key);
        if (err != CAIRN_OK) return err;

        uint64_t v;

        switch (key) {
        case RK_VERSION:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            if (v != CAIRN_RECEIPT_VERSION) return CAIRN_ERR_MALFORMED;
            r->receipt_version = (uint8_t)v;
            break;
        case RK_RECEIPT_ID:
            if ((err = cairn_cbor_bytes_n(&d, r->receipt_id, 16)) != CAIRN_OK) return err;
            break;
        case RK_DEVICE_ID:
            if ((err = cairn_cbor_bytes_n(&d, r->device_id, 16)) != CAIRN_OK) return err;
            break;
        case RK_BUNDLE_ID:
            if ((err = cairn_cbor_bytes_n(&d, r->bundle_id, 16)) != CAIRN_OK) return err;
            break;
        case RK_CONTENT_ROOT:
            if ((err = cairn_cbor_bytes_n(&d, r->content_root, 32)) != CAIRN_OK) return err;
            break;
        case RK_INGEST_UTC:
            if ((err = cairn_cbor_uint_read(&d, &r->server_ingest_utc_ms)) != CAIRN_OK)
                return err;
            break;
        case RK_SERVER_KEY:
            if ((err = cairn_cbor_bytes_n(&d, r->server_key_id, 8)) != CAIRN_OK) return err;
            break;
        case RK_SCHEMA_VER:
            if ((err = cairn_cbor_uint_read(&d, &v)) != CAIRN_OK) return err;
            r->ingest_schema_version = (uint8_t)v;
            break;
        case RK_OBJECT_IDS: {
            size_t cnt;
            if ((err = cairn_cbor_array_header(&d, &cnt)) != CAIRN_OK) return err;
            if (cnt > CAIRN_MAX_OBJECT_IDS) return CAIRN_ERR_MALFORMED;
            for (size_t j = 0; j < cnt; j++) {
                err = cairn_cbor_text_read(&d, r->object_ids[j], CAIRN_MAX_OBJECT_ID_LEN);
                if (err != CAIRN_OK) return err;
            }
            r->object_id_count = cnt;
            break;
        }
        case RK_SIG_ALGO:
            err = cairn_cbor_text_read(&d, r->signature_algorithm,
                                       sizeof(r->signature_algorithm));
            if (err != CAIRN_OK) return err;
            if (strcmp(r->signature_algorithm, CAIRN_SIGALG_ED25519) != 0) {
                return CAIRN_ERR_MALFORMED;
            }
            break;
        case RK_SIGNATURE:
            if ((err = cairn_cbor_bytes_n(&d, r->signature, 64)) != CAIRN_OK) return err;
            break;
        default:
            return CAIRN_ERR_MALFORMED;
        }
    }

    if (d.pos != d.len) return CAIRN_ERR_MALFORMED;
    return CAIRN_OK;
}

cairn_err_t cairn_receipt_decode(const uint8_t *buf, size_t len,
                                 cairn_receipt_t *out,
                                 uint8_t *scratch, size_t scratch_cap)
{
    cairn_err_t err = receipt_decode_raw(buf, len, out);
    if (err != CAIRN_OK) return err;

    /*
     * The round-trip check matters more here than for the manifest: a receipt
     * carries its signature inline, so verification must re-encode the signed
     * fields. Accepting a non-canonical receipt would mean verifying bytes
     * other than those received.
     */
    size_t written = 0;
    err = cairn_receipt_encode(out, scratch, scratch_cap, &written);
    if (err != CAIRN_OK) return err;

    if (written != len || memcmp(scratch, buf, len) != 0) {
        return CAIRN_ERR_NON_CANONICAL;
    }

    return CAIRN_OK;
}

cairn_err_t cairn_receipt_verify(const cairn_receipt_t *r, const uint8_t pub[32])
{
    uint8_t signing[512];
    size_t  n = 0;

    cairn_err_t err = cairn_receipt_signing_bytes(r, signing, sizeof(signing), &n);
    if (err != CAIRN_OK) return err;

    if (!cairn_ed25519_verify(signing, n, r->signature, pub)) {
        return CAIRN_ERR_BAD_SIGNATURE;
    }
    return CAIRN_OK;
}

cairn_err_t cairn_receipt_verify_acknowledges(const cairn_receipt_t *r,
                                              const uint8_t pub[32],
                                              const uint8_t uploaded_root[32])
{
    cairn_err_t err = cairn_receipt_verify(r, pub);
    if (err != CAIRN_OK) return err;

    /*
     * Both conditions are required. A validly signed receipt for a different
     * bundle is not an acknowledgement of this one, and treating it as one
     * would let a misconfigured or hostile server induce deletion of data it
     * never received.
     */
    if (memcmp(r->content_root, uploaded_root, 32) != 0) {
        return CAIRN_ERR_RECEIPT_ROOT_MISMATCH;
    }
    return CAIRN_OK;
}
