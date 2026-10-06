#include "cairn_digest_ack.h"

#include <string.h>

#include "cairn_format.h"

const char *cairn_digest_ack_result_name(cairn_digest_ack_result_t r)
{
    switch (r) {
    case CAIRN_DIGEST_ACK_OK:         return "OK";
    case CAIRN_DIGEST_ACK_MALFORMED:  return "MALFORMED";
    case CAIRN_DIGEST_ACK_NO_KEY:     return "NO_KEY";
    case CAIRN_DIGEST_ACK_UNVERIFIED: return "UNVERIFIED";
    case CAIRN_DIGEST_ACK_WRONG_TRIP: return "WRONG_TRIP";
    default:                          return "UNKNOWN";
    }
}

static void put_u64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

size_t cairn_digest_ack_signing_bytes(const cairn_digest_ack_t *a, uint8_t *out, size_t cap)
{
    /* MUTATION-SITE ack-context begin */
    const size_t label_len = sizeof(CAIRN_DIGEST_ACK_SIGN_LABEL) - 1;
    size_t need = label_len + 32 + 8 + 8;
    if (cap < need) return 0;

    memcpy(out, CAIRN_DIGEST_ACK_SIGN_LABEL, label_len);
    memcpy(out + label_len, a->trip_root, 32);
    put_u64le(out + label_len + 32, a->server_ingest_utc_ms);
    memcpy(out + label_len + 40, a->server_key_id, 8);
    return need;
    /* MUTATION-SITE ack-context end */
}

size_t cairn_digest_ack_encode(const cairn_digest_ack_t *a, uint8_t *out, size_t cap)
{
    if (cap < CAIRN_DIGEST_ACK_LEN) return 0;
    out[0] = 'C';
    out[1] = 'D';
    out[2] = 'A';
    out[3] = 0xD0;
    memcpy(out + 4, a->trip_root, 32);
    put_u64le(out + 36, a->server_ingest_utc_ms);
    memcpy(out + 44, a->server_key_id, 8);
    memcpy(out + 52, a->signature, 64);
    return CAIRN_DIGEST_ACK_LEN;
}

bool cairn_digest_ack_decode(const uint8_t *buf, size_t len, cairn_digest_ack_t *out)
{
    if (buf == NULL || out == NULL || len != CAIRN_DIGEST_ACK_LEN) return false;
    /* MUTATION-SITE ack-type begin */
    if (buf[0] != 'C' || buf[1] != 'D' || buf[2] != 'A' || buf[3] != 0xD0) return false;
    /* MUTATION-SITE ack-type end */

    memcpy(out->trip_root, buf + 4, 32);
    out->server_ingest_utc_ms = get_u64le(buf + 36);
    memcpy(out->server_key_id, buf + 44, 8);
    memcpy(out->signature, buf + 52, 64);
    return true;
}

static bool key_is_usable(const uint8_t key[32])
{
    if (key == NULL) return false;
    for (int i = 0; i < 32; i++) {
        if (key[i] != 0) return true;
    }
    return false;
}

cairn_digest_ack_result_t cairn_digest_ack_check(const uint8_t *buf, size_t len,
                                                 const uint8_t pinned_key[32],
                                                 const uint8_t trip_root[32])
{
    if (!key_is_usable(pinned_key)) return CAIRN_DIGEST_ACK_NO_KEY;

    cairn_digest_ack_t a;
    if (!cairn_digest_ack_decode(buf, len, &a)) return CAIRN_DIGEST_ACK_MALFORMED;

    uint8_t msg[CAIRN_DIGEST_ACK_SIGNING_MAX];
    size_t  n = cairn_digest_ack_signing_bytes(&a, msg, sizeof(msg));
    if (n == 0) return CAIRN_DIGEST_ACK_MALFORMED;

    if (!cairn_ed25519_verify(msg, n, a.signature, pinned_key)) return CAIRN_DIGEST_ACK_UNVERIFIED;
    if (trip_root == NULL || memcmp(a.trip_root, trip_root, 32) != 0) {
        return CAIRN_DIGEST_ACK_WRONG_TRIP;
    }
    return CAIRN_DIGEST_ACK_OK;
}
