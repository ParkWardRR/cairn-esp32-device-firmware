/* CRC-32, SHA-256 and the domain-separated Merkle tree. */

#include <string.h>

#include "cairn_format.h"

#ifdef CAIRN_USE_ESP_ROM_CRC
#include "esp_rom_crc.h"
#endif

/* ── CRC-32 ───────────────────────────────────────────────────────────────── */

#ifdef CAIRN_USE_ESP_ROM_CRC

/*
 * Yields values identical to Go's crc32.ChecksumIEEE and Rust's crc32fast,
 * which is the whole point of the specification choosing CRC-32 over CRC32C.
 *
 * The call looks too simple, so here is the derivation. esp_rom_crc.h documents
 * the ROM routines as carrying a `~` at both ends, and gives the pattern for a
 * reflected algorithm with a non-zero xorout — CRC-16/X25 — as
 *
 *     crc = (~crc16_le((uint16_t)~0xffff, buf, len)) ^ 0xffff;
 *
 * CRC-32/ISO-HDLC has the same shape: init 0xffffffff, refin and refout true,
 * xorout 0xffffffff. Substituting gives
 *
 *     (~esp_rom_crc32_le(~0xffffffff, buf, len)) ^ 0xffffffff
 *
 * and since ~0xffffffff is 0 and (~x) ^ 0xffffffff is x, the whole expression
 * collapses to a plain call with an initial value of 0.
 *
 * This was previously written as ~esp_rom_crc32_le(~0u, ...), which applies the
 * inversions but drops the final xorout, returning the raw shift-register value
 * instead. It is internally consistent — frames written that way scan back
 * cleanly on the same device — so only a cross-implementation check catches it.
 * The boot self-test did, on the first run against real hardware:
 * CRC-32("123456789") came back 2dfd2d88 where the specification requires
 * cbf43926.
 */
uint32_t cairn_crc32(const uint8_t *data, size_t len)
{
    return esp_rom_crc32_le(0, data, (uint32_t)len);
}

#else

/*
 * Portable bitwise CRC-32. Used on the host test build and anywhere the ROM
 * routine is unavailable. Table-free: the firmware path uses ROM, so the few
 * extra cycles here only ever cost test time.
 */
uint32_t cairn_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88321u & mask);
        }
    }

    return ~crc;
}

#endif

/* ── SHA-256 ──────────────────────────────────────────────────────────────── */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(cairn_sha256_t *ctx, const uint8_t *p)
{
    uint32_t w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void cairn_sha256_init(cairn_sha256_t *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void cairn_sha256_update(cairn_sha256_t *ctx, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ctx->buf[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

void cairn_sha256_final(cairn_sha256_t *ctx, uint8_t out[32])
{
    size_t i = ctx->buflen;

    if (ctx->buflen < 56) {
        ctx->buf[i++] = 0x80;
        while (i < 56) ctx->buf[i++] = 0x00;
    } else {
        ctx->buf[i++] = 0x80;
        while (i < 64) ctx->buf[i++] = 0x00;
        sha256_block(ctx, ctx->buf);
        memset(ctx->buf, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->buflen * 8;
    for (int b = 0; b < 8; b++) {
        ctx->buf[56 + b] = (uint8_t)(ctx->bitlen >> (56 - 8 * b));
    }
    sha256_block(ctx, ctx->buf);

    for (int w = 0; w < 8; w++) {
        out[w * 4 + 0] = (uint8_t)(ctx->state[w] >> 24);
        out[w * 4 + 1] = (uint8_t)(ctx->state[w] >> 16);
        out[w * 4 + 2] = (uint8_t)(ctx->state[w] >> 8);
        out[w * 4 + 3] = (uint8_t)(ctx->state[w]);
    }
}

void cairn_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    cairn_sha256_t ctx;
    cairn_sha256_init(&ctx);
    cairn_sha256_update(&ctx, data, len);
    cairn_sha256_final(&ctx, out);
}

/* ── Merkle tree ──────────────────────────────────────────────────────────── */

#define DOMAIN_LEAF     0x00
#define DOMAIN_INTERNAL 0x01
#define DOMAIN_EMPTY    0x02

void cairn_leaf_hash(const uint8_t *data, size_t len, uint8_t out[32])
{
    const uint8_t tag = DOMAIN_LEAF;
    cairn_sha256_t ctx;

    cairn_sha256_init(&ctx);
    cairn_sha256_update(&ctx, &tag, 1);
    cairn_sha256_update(&ctx, data, len);
    cairn_sha256_final(&ctx, out);
}

static void internal_hash(const uint8_t l[32], const uint8_t r[32], uint8_t out[32])
{
    const uint8_t tag = DOMAIN_INTERNAL;
    cairn_sha256_t ctx;

    cairn_sha256_init(&ctx);
    cairn_sha256_update(&ctx, &tag, 1);
    cairn_sha256_update(&ctx, l, 32);
    cairn_sha256_update(&ctx, r, 32);
    cairn_sha256_final(&ctx, out);
}

void cairn_merkle_root(uint8_t (*leaves)[32], size_t count, uint8_t out[32])
{
    if (count == 0) {
        const uint8_t tag = DOMAIN_EMPTY;
        cairn_sha256(&tag, 1, out);
        return;
    }

    /*
     * Collapse in place. An odd final node is promoted unchanged rather than
     * duplicated: duplicating it would make the leaf lists [a,b,c] and
     * [a,b,c,c] produce the same root, which is a second-preimage weakness and
     * would let a bundle's member list be altered without changing its identity.
     */
    while (count > 1) {
        size_t next = 0;
        size_t i = 0;

        while (i + 1 < count) {
            internal_hash(leaves[i], leaves[i + 1], leaves[next]);
            i += 2;
            next++;
        }
        if (count % 2 == 1) {
            memmove(leaves[next], leaves[count - 1], 32);
            next++;
        }
        count = next;
    }

    memcpy(out, leaves[0], 32);
}

/* ── members ──────────────────────────────────────────────────────────────── */

void cairn_sort_members(cairn_member_t *members, size_t count)
{
    /* Insertion sort: member counts are tiny and bounded by CAIRN_MAX_MEMBERS. */
    for (size_t i = 1; i < count; i++) {
        cairn_member_t key = members[i];
        size_t j = i;
        while (j > 0 && strcmp(members[j - 1].name, key.name) > 0) {
            members[j] = members[j - 1];
            j--;
        }
        members[j] = key;
    }
}

cairn_err_t cairn_content_root(const cairn_member_t *members, size_t count,
                               uint8_t out[32])
{
    if (count > CAIRN_MAX_MEMBERS) return CAIRN_ERR_TOO_MANY_MEMBERS;

    cairn_member_t sorted[CAIRN_MAX_MEMBERS];
    for (size_t i = 0; i < count; i++) {
        if (members[i].name[0] == '\0') return CAIRN_ERR_MALFORMED;
        sorted[i] = members[i];
    }
    cairn_sort_members(sorted, count);

    /* Duplicate names would make the root ambiguous. */
    for (size_t i = 1; i < count; i++) {
        if (strcmp(sorted[i - 1].name, sorted[i].name) == 0) return CAIRN_ERR_MALFORMED;
    }

    uint8_t leaves[CAIRN_MAX_MEMBERS][32];
    for (size_t i = 0; i < count; i++) {
        /* leaf_input = u16le(len(name)) || name || sha256(contents) */
        uint8_t input[2 + CAIRN_MAX_MEMBER_NAME + 32];
        size_t  nlen = strlen(sorted[i].name);
        size_t  n = 0;

        input[n++] = (uint8_t)(nlen & 0xFF);
        input[n++] = (uint8_t)((nlen >> 8) & 0xFF);
        memcpy(input + n, sorted[i].name, nlen);
        n += nlen;
        memcpy(input + n, sorted[i].sha256, 32);
        n += 32;

        cairn_leaf_hash(input, n, leaves[i]);
    }

    cairn_merkle_root(leaves, count, out);
    return CAIRN_OK;
}

void cairn_device_key_id(const uint8_t pub[32], uint8_t out[8])
{
    uint8_t digest[32];
    cairn_sha256(pub, 32, digest);
    memcpy(out, digest, 8);
}
