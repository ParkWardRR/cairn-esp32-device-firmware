/*
 * Ed25519, field and group arithmetic adapted from TweetNaCl (Daniel J.
 * Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange, Peter Schwabe,
 * Sjaak Smetsers — public domain).
 *
 * Vendored because mbedTLS does not provide Ed25519 signing, and the
 * specification requires it: the manifest is signed by a device key, and a
 * receipt must be verified against a pinned server key before a single byte may
 * be pruned.
 *
 * Correctness here is not optional and not self-evident, so it is checked two
 * ways against the committed conformance vectors:
 *
 *   - verification, against a signature produced by the Go implementation; and
 *   - signing, byte-for-byte against that same signature. Ed25519 is
 *     deterministic (RFC 8032), so signing identical bytes with an identical
 *     seed must reproduce it exactly. A subtly wrong field implementation
 *     cannot survive that comparison.
 *
 * SHA-512 is streaming rather than one-shot. That is not a micro-optimisation:
 * signing hashes a 32-byte prefix followed by the message, and a manifest with
 * a full chunk list runs well past 2 KB. A fixed staging buffer would either
 * cap the manifest size or need a heap allocation in the sealing path, where
 * failing to allocate means failing to seal a captured trip.
 */

#include <string.h>

#include "cairn_format.h"

/* ── SHA-512 ──────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t state[8];
    uint64_t bitlen;
    uint8_t  buf[128];
    size_t   buflen;
} sha512_t;

#define ROTR64(x, c) (((x) >> (c)) | ((x) << (64 - (c))))
#define Ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define Sigma0(x) (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define Sigma1(x) (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
#define sigma0(x) (ROTR64(x, 1) ^ ROTR64(x, 8) ^ ((x) >> 7))
#define sigma1(x) (ROTR64(x, 19) ^ ROTR64(x, 61) ^ ((x) >> 6))

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void sha512_block(sha512_t *ctx, const uint8_t *p)
{
    uint64_t w[80];

    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int b = 0; b < 8; b++) v = (v << 8) | p[i * 8 + b];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        w[i] = w[i - 16] + sigma0(w[i - 15]) + w[i - 7] + sigma1(w[i - 2]);
    }

    uint64_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2];
    uint64_t d = ctx->state[3], e = ctx->state[4], f = ctx->state[5];
    uint64_t g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + Sigma1(e) + Ch(e, f, g) + K512[i] + w[i];
        uint64_t t2 = Sigma0(a) + Maj(a, b, c);

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha512_init(sha512_t *ctx)
{
    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

static void sha512_update(sha512_t *ctx, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t take = 128 - ctx->buflen;
        if (take > len) take = len;

        memcpy(ctx->buf + ctx->buflen, data, take);
        ctx->buflen += take;
        data += take;
        len -= take;

        if (ctx->buflen == 128) {
            sha512_block(ctx, ctx->buf);
            ctx->bitlen += 1024;
            ctx->buflen = 0;
        }
    }
}

static void sha512_final(sha512_t *ctx, uint8_t out[64])
{
    uint64_t total = ctx->bitlen + (uint64_t)ctx->buflen * 8;
    size_t i = ctx->buflen;

    ctx->buf[i++] = 0x80;
    if (i > 112) {
        while (i < 128) ctx->buf[i++] = 0x00;
        sha512_block(ctx, ctx->buf);
        i = 0;
    }
    while (i < 112) ctx->buf[i++] = 0x00;

    /* 128-bit big-endian length; the high half is always zero at our sizes. */
    memset(ctx->buf + 112, 0, 8);
    for (int b = 0; b < 8; b++) ctx->buf[120 + b] = (uint8_t)(total >> (56 - 8 * b));
    sha512_block(ctx, ctx->buf);

    for (int w = 0; w < 8; w++) {
        for (int b = 0; b < 8; b++) {
            out[w * 8 + b] = (uint8_t)(ctx->state[w] >> (56 - 8 * b));
        }
    }
}

static void sha512(const uint8_t *data, size_t len, uint8_t out[64])
{
    sha512_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}

/* ── field arithmetic over 2^255 - 19 ─────────────────────────────────────── */

typedef int64_t gf[16];

static const gf gf0;
static const gf gf1 = { 1 };
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};
static const gf X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
static const gf Y = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};
static const gf SQRTM1 = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};

static void set25519(gf r, const gf a)
{
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        /* c is routinely negative here, and shifting a negative value left is
         * undefined. Multiplying is exactly equivalent and defined, which
         * matters because the Xtensa compiler need not behave like clang. */
        o[i] -= c * 65536;
    }
}

/* Constant-time conditional swap: a scalar bit must not steer a branch. */
static void sel25519(gf p, gf q, int64_t b)
{
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    gf m, t;

    set25519(t, n);
    car25519(t);
    car25519(t);
    car25519(t);

    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }

    for (int i = 0; i < 16; i++) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static int neq25519(const gf a, const gf b)
{
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return memcmp(c, d, 32) != 0;
}

static uint8_t par25519(const gf a)
{
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void fsub(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fmul(gf o, const gf a, const gf b)
{
    int64_t t[31];

    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];

    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void fsq(gf o, const gf a) { fmul(o, a, a); }

static void finv(gf o, const gf i)
{
    gf c;
    set25519(c, i);
    for (int a = 253; a >= 0; a--) {
        fsq(c, c);
        if (a != 2 && a != 4) fmul(c, c, i);
    }
    set25519(o, c);
}

static void pow2523(gf o, const gf i)
{
    gf c;
    set25519(c, i);
    for (int a = 250; a >= 0; a--) {
        fsq(c, c);
        if (a != 1) fmul(c, c, i);
    }
    set25519(o, c);
}

/* ── group operations on the twisted Edwards curve ────────────────────────── */

static void ge_add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;

    fsub(a, p[1], p[0]);
    fsub(t, q[1], q[0]);
    fmul(a, a, t);
    fadd(b, p[0], p[1]);
    fadd(t, q[0], q[1]);
    fmul(b, b, t);
    fmul(c, p[3], q[3]);
    fmul(c, c, D2);
    fmul(d, p[2], q[2]);
    fadd(d, d, d);
    fsub(e, b, a);
    fsub(f, d, c);
    fadd(g, d, c);
    fadd(h, b, a);

    fmul(p[0], e, f);
    fmul(p[1], h, g);
    fmul(p[2], g, f);
    fmul(p[3], e, h);
}

static void ge_cswap(gf p[4], gf q[4], uint8_t b)
{
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void ge_pack(uint8_t *r, gf p[4])
{
    gf tx, ty, zi;

    finv(zi, p[2]);
    fmul(tx, p[0], zi);
    fmul(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(par25519(tx) << 7);
}

static void ge_scalarmult(gf p[4], gf q[4], const uint8_t *s)
{
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);

    for (int i = 255; i >= 0; --i) {
        uint8_t b = (uint8_t)((s[i / 8] >> (i & 7)) & 1);
        ge_cswap(p, q, b);
        ge_add(q, p);
        ge_add(p, p);
        ge_cswap(p, q, b);
    }
}

static void ge_base(gf q[4])
{
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    fmul(q[3], X, Y);
}

static void ge_scalarbase(gf p[4], const uint8_t *s)
{
    gf q[4];
    ge_base(q);
    ge_scalarmult(p, q, s);
}

/* ── scalar arithmetic mod L ──────────────────────────────────────────────── */

static const int64_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
    0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0x10
};

static void mod_l(uint8_t r[32], int64_t x[64])
{
    for (int i = 63; i >= 32; --i) {
        int64_t carry = 0;
        int j;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256; /* carry may be negative; see car25519 */
        }
        x[j] += carry;
        x[i] = 0;
    }

    int64_t carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * L[j];
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

/* Reduce a 64-byte hash to a 32-byte scalar mod L. */
static void sc_reduce(uint8_t out[32], const uint8_t in[64])
{
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)in[i];
    mod_l(out, x);
}

static int ge_unpackneg(gf r[4], const uint8_t p[32])
{
    gf t, chk, num, den, den2, den4, den6;

    set25519(r[2], gf1);
    unpack25519(r[1], p);
    fsq(num, r[1]);
    fmul(den, num, D);
    fsub(num, num, r[2]);
    fadd(den, r[2], den);

    fsq(den2, den);
    fsq(den4, den2);
    fmul(den6, den4, den2);
    fmul(t, den6, num);
    fmul(t, t, den);

    pow2523(t, t);
    fmul(t, t, num);
    fmul(t, t, den);
    fmul(t, t, den);
    fmul(r[0], t, den);

    fsq(chk, r[0]);
    fmul(chk, chk, den);
    if (neq25519(chk, num)) fmul(r[0], r[0], SQRTM1);

    fsq(chk, r[0]);
    fmul(chk, chk, den);
    if (neq25519(chk, num)) return -1; /* not a point on the curve */

    if (par25519(r[0]) == (p[31] >> 7)) fsub(r[0], gf0, r[0]);

    fmul(r[3], r[0], r[1]);
    return 0;
}

/* ── public API ───────────────────────────────────────────────────────────── */

/*
 * Expand a 32-byte seed into the clamped scalar (low half) and the per-message
 * nonce prefix (high half). RFC 8032 §5.1.5.
 */
static void expand_seed(const uint8_t seed[32], uint8_t az[64])
{
    sha512(seed, 32, az);
    az[0] &= 248;
    az[31] &= 127;
    az[31] |= 64;
}

void cairn_ed25519_public_from_seed(const uint8_t seed[32], uint8_t pub[32])
{
    uint8_t az[64];
    gf p[4];

    expand_seed(seed, az);
    ge_scalarbase(p, az);
    ge_pack(pub, p);

    memset(az, 0, sizeof(az));
}

void cairn_ed25519_sign(const uint8_t *msg, size_t len,
                        const uint8_t seed[32], const uint8_t pub[32],
                        uint8_t sig[64])
{
    uint8_t az[64], nonce[64], hram[64], r[32], h[32];
    int64_t x[64];
    sha512_t ctx;
    gf p[4];

    expand_seed(seed, az);

    /* r = H(prefix || M) mod L */
    sha512_init(&ctx);
    sha512_update(&ctx, az + 32, 32);
    sha512_update(&ctx, msg, len);
    sha512_final(&ctx, nonce);
    sc_reduce(r, nonce);

    /* R = [r]B */
    ge_scalarbase(p, r);
    ge_pack(sig, p);

    /* k = H(R || A || M) mod L */
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pub, 32);
    sha512_update(&ctx, msg, len);
    sha512_final(&ctx, hram);
    sc_reduce(h, hram);

    /* S = (r + k*a) mod L */
    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = (int64_t)r[i];
    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 32; j++) x[i + j] += (int64_t)h[i] * (int64_t)az[j];
    }
    mod_l(sig + 32, x);

    memset(az, 0, sizeof(az));
    memset(nonce, 0, sizeof(nonce));
}

bool cairn_ed25519_verify(const uint8_t *msg, size_t len,
                          const uint8_t sig[64], const uint8_t pub[32])
{
    uint8_t hram[64], h[32], check[32];
    sha512_t ctx;
    gf p[4], q[4], base[4], sb[4];

    /* Decode -A. A malformed public key is a verification failure, not a crash. */
    if (ge_unpackneg(q, pub) != 0) return false;

    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pub, 32);
    sha512_update(&ctx, msg, len);
    sha512_final(&ctx, hram);
    sc_reduce(h, hram);

    /* Check R == [S]B - [k]A, computed as [k](-A) + [S]B and compared packed. */
    ge_scalarmult(p, q, h);

    ge_base(base);
    ge_scalarmult(sb, base, sig + 32);

    ge_add(p, sb);
    ge_pack(check, p);

    return memcmp(sig, check, 32) == 0;
}
