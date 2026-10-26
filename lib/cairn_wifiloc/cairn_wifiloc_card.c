/* The sealed card copy of the anchor table. See cairn_wifiloc.h. */

#include "cairn_wifiloc.h"

#include <string.h>

#include "cairn_format.h"

static const char SALT[] = "cairn-wifi-anchors/v1";
static const char INFO[] = "card copy";

void cairn_wifiloc_card_key(const uint8_t root[32], uint8_t key[32])
{
    (void)cairn_hkdf_sha256((const uint8_t *)SALT, sizeof(SALT) - 1, root, 32,
                            (const uint8_t *)INFO, sizeof(INFO) - 1, key, 32);
}

size_t cairn_wifiloc_card_seal(const uint8_t key[32], const uint8_t nonce[24], const uint8_t *pt,
                               size_t len, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < len + CAIRN_WIFILOC_CARD_OVERHEAD) return 0;
    memcpy(out, CAIRN_WIFILOC_CARD_MAGIC, 4);
    memcpy(out + 4, nonce, 24);
    cairn_xchacha20poly1305_seal(key, nonce, out, 4, pt, len, out + CAIRN_WIFILOC_CARD_OVERHEAD, out + 28);
    return len + CAIRN_WIFILOC_CARD_OVERHEAD;
}

bool cairn_wifiloc_card_open(const uint8_t key[32], const uint8_t *in, size_t len, uint8_t *pt,
                             size_t cap, size_t *pt_len)
{
    *pt_len = 0;
    if (in == NULL || len < CAIRN_WIFILOC_CARD_OVERHEAD || memcmp(in, CAIRN_WIFILOC_CARD_MAGIC, 4) != 0)
        return false;
    size_t n = len - CAIRN_WIFILOC_CARD_OVERHEAD;
    if (cap < n) return false;
    if (!cairn_xchacha20poly1305_open(key, in + 4, in, 4, in + CAIRN_WIFILOC_CARD_OVERHEAD, n, in + 28, pt))
        return false;
    *pt_len = n;
    return true;
}
