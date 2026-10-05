/*
 * HMAC-SHA256, HKDF-SHA256 (RFC 5869) and the v3 segment key derivation.
 *
 * Built on cf_hash.c's SHA-256 so the format library stays one self-contained
 * body of portable C that the host conformance run compiles unchanged. The KDF
 * is checked twice over: against the RFC 5869 vectors in the conformance
 * runner, and against every committed segment_key_hex — the second is the one
 * that proves the device derives the key the server will.
 */

#include <string.h>

#include "cairn_format.h"

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0) *v++ = 0;
}

/* ── HMAC-SHA256 ──────────────────────────────────────────────────────────── */

typedef struct {
    cairn_sha256_t inner;
    cairn_sha256_t outer;
} hmac_t;

static void hmac_init(hmac_t *h, const uint8_t *key, size_t key_len)
{
    uint8_t k[64], pad[64];

    memset(k, 0, sizeof(k));
    if (key_len > sizeof(k)) {
        cairn_sha256(key, key_len, k); /* RFC 2104: long keys are hashed first */
    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }

    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    cairn_sha256_init(&h->inner);
    cairn_sha256_update(&h->inner, pad, sizeof(pad));

    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    cairn_sha256_init(&h->outer);
    cairn_sha256_update(&h->outer, pad, sizeof(pad));

    wipe(k, sizeof(k));
    wipe(pad, sizeof(pad));
}

static void hmac_final(hmac_t *h, uint8_t out[32])
{
    uint8_t inner[32];

    cairn_sha256_final(&h->inner, inner);
    cairn_sha256_update(&h->outer, inner, sizeof(inner));
    cairn_sha256_final(&h->outer, out);

    wipe(inner, sizeof(inner));
    wipe(h, sizeof(*h));
}

void cairn_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                       size_t len, uint8_t out[32])
{
    hmac_t h;
    hmac_init(&h, key, key_len);
    if (len > 0) cairn_sha256_update(&h.inner, msg, len);
    hmac_final(&h, out);
}

/* ── HKDF-SHA256 ──────────────────────────────────────────────────────────── */

bool cairn_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                       size_t ikm_len, const uint8_t *info, size_t info_len,
                       uint8_t *out, size_t out_len)
{
    /* RFC 5869 §2.3: at most 255 blocks of output. */
    if (out_len > 255u * 32u) return false;

    /*
     * Extract. An absent salt is HashLen zero bytes, which as an HMAC key is the
     * same as an empty key — both pad to 64 zero bytes — so the zero-length case
     * needs no special branch.
     */
    uint8_t prk[32];
    cairn_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);

    /* Expand: T(i) = HMAC(PRK, T(i-1) || info || i). */
    uint8_t t[32];
    size_t  t_len = 0;
    size_t  done  = 0;

    for (uint8_t counter = 1; done < out_len; counter++) {
        hmac_t h;
        hmac_init(&h, prk, sizeof(prk));
        if (t_len > 0) cairn_sha256_update(&h.inner, t, t_len);
        if (info_len > 0) cairn_sha256_update(&h.inner, info, info_len);
        cairn_sha256_update(&h.inner, &counter, 1);
        hmac_final(&h, t);
        t_len = sizeof(t);

        size_t take = out_len - done;
        if (take > sizeof(t)) take = sizeof(t);
        memcpy(out + done, t, take);
        done += take;
    }

    wipe(prk, sizeof(prk));
    wipe(t, sizeof(t));
    return true;
}

/* ── the segment key ──────────────────────────────────────────────────────── */

/*
 * K_seg = HKDF-SHA256(ikm  = K_root,
 *                     salt = vehicle_id,
 *                     info = "cairn/segment/v3" || device_id || assignment_id
 *                            || boot_id || segment_index u32le,
 *                     L    = 32)
 *
 * The vehicle id is the salt, not part of info, because it is the identifier
 * that must never be shared between keys: one vehicle's derived keys say
 * nothing about another's even under the same device root. storage_key_version
 * selects which root this is and device_counter is bound through the AAD;
 * neither is an input here, so advancing the counter never changes a key.
 */
void cairn_derive_segment_key(const uint8_t root[CAIRN_ROOT_KEY_SIZE],
                              const cairn_segment_header_t *h,
                              uint8_t out[CAIRN_SEGMENT_KEY_SIZE])
{
    static const char LABEL[] = CAIRN_HKDF_SEGMENT_LABEL;
    const size_t      label_len = sizeof(LABEL) - 1;

    uint8_t info[sizeof(LABEL) - 1 + 16 * 3 + 4];
    size_t  n = 0;

    memcpy(info + n, LABEL, label_len);
    n += label_len;
    memcpy(info + n, h->device_id, 16);
    n += 16;
    memcpy(info + n, h->assignment_id, 16);
    n += 16;
    memcpy(info + n, h->boot_id, 16);
    n += 16;
    info[n++] = (uint8_t)(h->segment_index);
    info[n++] = (uint8_t)(h->segment_index >> 8);
    info[n++] = (uint8_t)(h->segment_index >> 16);
    info[n++] = (uint8_t)(h->segment_index >> 24);

    (void)cairn_hkdf_sha256(h->vehicle_id, 16, root, CAIRN_ROOT_KEY_SIZE, info, n,
                            out, CAIRN_SEGMENT_KEY_SIZE);
}

cairn_err_t cairn_root_key_provider(void *user, const cairn_segment_header_t *h,
                                    uint8_t key_out[CAIRN_SEGMENT_KEY_SIZE])
{
    const cairn_root_key_t *root = (const cairn_root_key_t *)user;

    if (root == NULL) return CAIRN_ERR_NO_KEY;

    /*
     * Refuse a different version up front. Deriving anyway would produce a key
     * that fails every tag, which reads exactly like tampering; a version
     * mismatch means a key is missing, and an operator must be told which.
     */
    if (h->storage_key_version != root->version) return CAIRN_ERR_KEY_VERSION_MISMATCH;

    cairn_derive_segment_key(root->root, h, key_out);
    return CAIRN_OK;
}
