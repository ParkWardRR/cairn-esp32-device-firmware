/*
 * XChaCha20-Poly1305, the AEAD every v3 frame is sealed with (spec §3.6).
 *
 * Written here rather than taken from mbedTLS for two reasons. mbedTLS has
 * ChaCha20-Poly1305 but not the X variant, and the 24-byte nonce is the whole
 * reason this construction was chosen: it is what makes a *random* nonce per
 * frame safe, with no counter that a power cut could lose. And this file is
 * part of lib/cairn_format, which must compile natively against the committed
 * vectors with no IDF present — the host conformance run is the only check
 * that the device seals bytes the server can open.
 *
 * Construction, exactly as draft-irtf-cfrg-xchacha-03:
 *
 *   subkey = HChaCha20(key, nonce[0..16))
 *   AEAD_ChaCha20_Poly1305 (RFC 8439) under subkey, with the 12-byte nonce
 *   0x00000000 || nonce[16..24)
 *
 * Plain portable C11, 32-bit arithmetic throughout (Poly1305 in 26-bit limbs),
 * because the Xtensa LX6 has a 32x32 multiplier and nothing wider. Not tuned:
 * a 100-byte GNSS frame costs a few microseconds either way, and the scan is
 * bound by the card, not by this.
 */

#include <string.h>

#include "cairn_format.h"

/* ── little-endian helpers ────────────────────────────────────────────────── */

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void st64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/*
 * Wipe key material without the compiler deciding the store is dead. A plain
 * memset on a buffer about to go out of scope may legally be elided, and these
 * buffers hold one-time Poly1305 keys and derived subkeys.
 */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0) *v++ = 0;
}

/* ── ChaCha20 ─────────────────────────────────────────────────────────────── */

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QR(a, b, c, d)                                  \
    do {                                                \
        a += b; d ^= a; d = ROTL32(d, 16);              \
        c += d; b ^= c; b = ROTL32(b, 12);              \
        a += b; d ^= a; d = ROTL32(d, 8);               \
        c += d; b ^= c; b = ROTL32(b, 7);               \
    } while (0)

/* "expand 32-byte k" */
static const uint32_t SIGMA[4] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };

static void chacha_rounds(uint32_t x[16])
{
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
}

/* One 64-byte keystream block, RFC 8439 §2.3 (32-bit counter, 96-bit nonce). */
static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16], x[16];

    s[0] = SIGMA[0];
    s[1] = SIGMA[1];
    s[2] = SIGMA[2];
    s[3] = SIGMA[3];
    for (int i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
    s[12] = counter;
    s[13] = ld32(nonce + 0);
    s[14] = ld32(nonce + 4);
    s[15] = ld32(nonce + 8);

    memcpy(x, s, sizeof(x));
    chacha_rounds(x);
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);

    wipe(s, sizeof(s));
    wipe(x, sizeof(x));
}

/* XOR `len` bytes of keystream into `buf` in place, starting at `counter`. */
static void chacha20_xor(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12], uint8_t *buf, size_t len)
{
    uint8_t ks[64];

    while (len > 0) {
        chacha20_block(key, counter++, nonce, ks);
        size_t n = (len < 64) ? len : 64;
        for (size_t i = 0; i < n; i++) buf[i] ^= ks[i];
        buf += n;
        len -= n;
    }
    wipe(ks, sizeof(ks));
}

void cairn_hchacha20(const uint8_t key[32], const uint8_t in[16], uint8_t out[32])
{
    uint32_t x[16];

    x[0] = SIGMA[0];
    x[1] = SIGMA[1];
    x[2] = SIGMA[2];
    x[3] = SIGMA[3];
    for (int i = 0; i < 8; i++) x[4 + i] = ld32(key + 4 * i);
    for (int i = 0; i < 4; i++) x[12 + i] = ld32(in + 4 * i);

    /* No feed-forward addition: HChaCha20 outputs the permuted state's first
     * and last rows directly, which is what makes it a PRF on the nonce. */
    chacha_rounds(x);

    for (int i = 0; i < 4; i++) st32(out + 4 * i, x[i]);
    for (int i = 0; i < 4; i++) st32(out + 16 + 4 * i, x[12 + i]);

    wipe(x, sizeof(x));
}

/* ── Poly1305 (26-bit limbs, after poly1305-donna-32) ─────────────────────── */

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t  buf[16];
    size_t   buflen;
} poly1305_t;

static void poly_init(poly1305_t *p, const uint8_t key[32])
{
    /* r is clamped as the algorithm requires, split into 26-bit limbs. */
    p->r[0] = (ld32(key + 0)) & 0x3ffffff;
    p->r[1] = (ld32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (ld32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (ld32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (ld32(key + 12) >> 8) & 0x00fffff;

    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = ld32(key + 16 + 4 * i);

    p->buflen = 0;
}

/* Absorb whole 16-byte blocks. `hibit` is 2^128 for a full block and 0 for the
 * final padded partial block, whose terminating 1 is already in the bytes. */
static void poly_blocks(poly1305_t *p, const uint8_t *m, size_t len, uint32_t hibit)
{
    const uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3],
                   r4 = p->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];

    while (len >= 16) {
        h0 += (ld32(m + 0)) & 0x3ffffff;
        h1 += (ld32(m + 3) >> 2) & 0x3ffffff;
        h2 += (ld32(m + 6) >> 4) & 0x3ffffff;
        h3 += (ld32(m + 9) >> 6) & 0x3ffffff;
        h4 += (ld32(m + 12) >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
                      (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
                      (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                      (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                      (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                      (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;

        m   += 16;
        len -= 16;
    }

    p->h[0] = h0;
    p->h[1] = h1;
    p->h[2] = h2;
    p->h[3] = h3;
    p->h[4] = h4;
}

static void poly_update(poly1305_t *p, const uint8_t *m, size_t len)
{
    if (p->buflen > 0) {
        size_t want = 16 - p->buflen;
        if (want > len) want = len;
        memcpy(p->buf + p->buflen, m, want);
        p->buflen += want;
        m   += want;
        len -= want;
        if (p->buflen < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->buflen = 0;
    }

    size_t whole = len & ~(size_t)15;
    if (whole > 0) {
        poly_blocks(p, m, whole, 1u << 24);
        m   += whole;
        len -= whole;
    }

    if (len > 0) {
        memcpy(p->buf, m, len);
        p->buflen = len;
    }
}

/* Feed zeros up to the next 16-byte boundary of the bytes absorbed so far. The
 * AEAD pads AAD and ciphertext separately, so this is called between them. */
static void poly_pad16(poly1305_t *p)
{
    static const uint8_t zeros[16] = { 0 };
    if (p->buflen > 0) poly_update(p, zeros, 16 - p->buflen);
}

static void poly_final(poly1305_t *p, uint8_t tag[16])
{
    if (p->buflen > 0) {
        p->buf[p->buflen] = 1;
        for (size_t i = p->buflen + 1; i < 16; i++) p->buf[i] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }

    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    uint32_t c;

    /* Fully carry h. */
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    /* g = h + 5 - 2^130; select g if it did not borrow, in constant time. */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1u;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* h mod 2^128, then + s. */
    h0 = (h0) | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    uint64_t f;
    f = (uint64_t)h0 + p->pad[0];             h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->pad[3] + (f >> 32); h3 = (uint32_t)f;

    st32(tag + 0, h0);
    st32(tag + 4, h1);
    st32(tag + 8, h2);
    st32(tag + 12, h3);

    wipe(p, sizeof(*p));
}

/* ── the AEAD ─────────────────────────────────────────────────────────────── */

/*
 * Derive the ChaCha20 subkey and 12-byte nonce for an XChaCha20 nonce, and the
 * one-time Poly1305 key (block 0 of the keystream, RFC 8439 §2.6).
 */
static void xchacha_setup(const uint8_t key[32], const uint8_t nonce[24],
                          uint8_t subkey[32], uint8_t n12[12], uint8_t polykey[32])
{
    uint8_t block0[64];

    cairn_hchacha20(key, nonce, subkey);
    memset(n12, 0, 4);
    memcpy(n12 + 4, nonce + 16, 8);

    chacha20_block(subkey, 0, n12, block0);
    memcpy(polykey, block0, 32);
    wipe(block0, sizeof(block0));
}

/*
 * The tag over (aad_a || aad_b) and the ciphertext.
 *
 * The AAD is accepted in two pieces because a frame's AAD is the 24-byte frame
 * header followed by the segment header (§3.6), which live in different
 * buffers. Absorbing them back to back is identical to absorbing their
 * concatenation — Poly1305 is a stream over the bytes — and saves staging a
 * copy of a 128-byte header on every frame.
 */
static void aead_tag(const uint8_t polykey[32], const uint8_t *aad_a, size_t aad_a_len,
                     const uint8_t *aad_b, size_t aad_b_len, const uint8_t *ct,
                     size_t len, uint8_t tag[16])
{
    poly1305_t p;
    uint8_t    lens[16];

    poly_init(&p, polykey);
    if (aad_a_len > 0) poly_update(&p, aad_a, aad_a_len);
    if (aad_b_len > 0) poly_update(&p, aad_b, aad_b_len);
    poly_pad16(&p);
    if (len > 0) poly_update(&p, ct, len);
    poly_pad16(&p);

    st64(lens, (uint64_t)aad_a_len + (uint64_t)aad_b_len);
    st64(lens + 8, (uint64_t)len);
    poly_update(&p, lens, sizeof(lens));

    poly_final(&p, tag);
}

void cairn_xchacha20poly1305_seal2(const uint8_t key[32], const uint8_t nonce[24],
                                   const uint8_t *aad_a, size_t aad_a_len,
                                   const uint8_t *aad_b, size_t aad_b_len,
                                   const uint8_t *pt, size_t len, uint8_t *ct,
                                   uint8_t tag[16])
{
    uint8_t subkey[32], n12[12], polykey[32];

    xchacha_setup(key, nonce, subkey, n12, polykey);

    if (len > 0 && ct != pt) memmove(ct, pt, len);
    chacha20_xor(subkey, 1, n12, ct, len);

    aead_tag(polykey, aad_a, aad_a_len, aad_b, aad_b_len, ct, len, tag);

    wipe(subkey, sizeof(subkey));
    wipe(polykey, sizeof(polykey));
}

bool cairn_xchacha20poly1305_open2(const uint8_t key[32], const uint8_t nonce[24],
                                   const uint8_t *aad_a, size_t aad_a_len,
                                   const uint8_t *aad_b, size_t aad_b_len,
                                   const uint8_t *ct, size_t len,
                                   const uint8_t tag[16], uint8_t *pt)
{
    uint8_t subkey[32], n12[12], polykey[32], want[16];

    xchacha_setup(key, nonce, subkey, n12, polykey);
    aead_tag(polykey, aad_a, aad_a_len, aad_b, aad_b_len, ct, len, want);

    /*
     * Verify before decrypting, and compare in constant time. Decrypting first
     * would hand unauthenticated plaintext to a caller that might act on it
     * before checking the result; comparing with memcmp would leak how many
     * leading tag bytes an attacker had right.
     */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ tag[i]);

    bool ok = (diff == 0);
    if (ok) {
        if (len > 0 && pt != ct) memmove(pt, ct, len);
        chacha20_xor(subkey, 1, n12, pt, len);
    }

    wipe(subkey, sizeof(subkey));
    wipe(polykey, sizeof(polykey));
    wipe(want, sizeof(want));
    return ok;
}

void cairn_xchacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[24],
                                  const uint8_t *aad, size_t aad_len,
                                  const uint8_t *pt, size_t len, uint8_t *ct,
                                  uint8_t tag[16])
{
    cairn_xchacha20poly1305_seal2(key, nonce, aad, aad_len, NULL, 0, pt, len, ct, tag);
}

bool cairn_xchacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[24],
                                  const uint8_t *aad, size_t aad_len,
                                  const uint8_t *ct, size_t len,
                                  const uint8_t tag[16], uint8_t *pt)
{
    return cairn_xchacha20poly1305_open2(key, nonce, aad, aad_len, NULL, 0, ct, len,
                                         tag, pt);
}
