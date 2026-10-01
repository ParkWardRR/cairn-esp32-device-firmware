/*
 * Deterministic CBOR, restricted to the subset the manifest and receipt need.
 *
 * RFC 8949 §4.2.1 core deterministic encoding: definite lengths only, unsigned
 * integer map keys in ascending order, smallest-width integers, null for absent
 * optionals, no tags, no floats, no indefinite lengths.
 *
 * A specified encoding is what makes the signed byte sequence reproducible
 * across three implementations and across firmware versions. Both halves here
 * are strict: the decoder rejects any input it would not itself have produced,
 * because accepting a non-canonical encoding would mean verifying bytes other
 * than those received.
 */

#include <string.h>

#include "cairn_format.h"

#define MAJOR_UINT   0
#define MAJOR_BYTES  2
#define MAJOR_TEXT   3
#define MAJOR_ARRAY  4
#define MAJOR_MAP    5
#define MAJOR_SIMPLE 7

#define SIMPLE_NULL 22 /* 0xf6 */

/* ── encoder ──────────────────────────────────────────────────────────────── */

void cairn_cbor_init(cairn_cbor_enc_t *e, uint8_t *buf, size_t cap)
{
    e->buf      = buf;
    e->cap      = cap;
    e->len      = 0;
    e->overflow = false;
    e->last_key = -1;
}

static void put(cairn_cbor_enc_t *e, uint8_t b)
{
    if (e->len >= e->cap) {
        e->overflow = true;
        return;
    }
    e->buf[e->len++] = b;
}

static void put_bytes(cairn_cbor_enc_t *e, const uint8_t *b, size_t n)
{
    if (e->len + n > e->cap) {
        e->overflow = true;
        return;
    }
    memcpy(e->buf + e->len, b, n);
    e->len += n;
}

/* CBOR heads are big-endian, and must use the smallest width that fits. */
static void head(cairn_cbor_enc_t *e, uint8_t major, uint64_t arg)
{
    uint8_t m = (uint8_t)(major << 5);

    if (arg < 24) {
        put(e, (uint8_t)(m | arg));
    } else if (arg <= 0xFF) {
        put(e, (uint8_t)(m | 24));
        put(e, (uint8_t)arg);
    } else if (arg <= 0xFFFF) {
        put(e, (uint8_t)(m | 25));
        put(e, (uint8_t)(arg >> 8));
        put(e, (uint8_t)arg);
    } else if (arg <= 0xFFFFFFFFu) {
        put(e, (uint8_t)(m | 26));
        for (int i = 3; i >= 0; i--) put(e, (uint8_t)(arg >> (8 * i)));
    } else {
        put(e, (uint8_t)(m | 27));
        for (int i = 7; i >= 0; i--) put(e, (uint8_t)(arg >> (8 * i)));
    }
}

void cairn_cbor_uint(cairn_cbor_enc_t *e, uint64_t v) { head(e, MAJOR_UINT, v); }

void cairn_cbor_bytes(cairn_cbor_enc_t *e, const uint8_t *b, size_t len)
{
    head(e, MAJOR_BYTES, len);
    put_bytes(e, b, len);
}

void cairn_cbor_text(cairn_cbor_enc_t *e, const char *s)
{
    size_t n = strlen(s);
    head(e, MAJOR_TEXT, n);
    put_bytes(e, (const uint8_t *)s, n);
}

void cairn_cbor_array(cairn_cbor_enc_t *e, size_t n) { head(e, MAJOR_ARRAY, n); }

void cairn_cbor_map(cairn_cbor_enc_t *e, size_t n)
{
    head(e, MAJOR_MAP, n);
    e->last_key = -1;
}

void cairn_cbor_null(cairn_cbor_enc_t *e)
{
    put(e, (uint8_t)((MAJOR_SIMPLE << 5) | SIMPLE_NULL));
}

/*
 * Map keys must ascend. Encoding is hand-written in key order, so a violation
 * is a programming error — and one that would silently produce a non-canonical
 * signature, so it is flagged rather than tolerated.
 */
void cairn_cbor_key(cairn_cbor_enc_t *e, uint64_t k)
{
    if ((int64_t)k <= e->last_key) {
        e->overflow = true; /* reuse the failure channel: the result is unusable */
        return;
    }
    e->last_key = (int64_t)k;
    cairn_cbor_uint(e, k);
}

/* ── decoder ──────────────────────────────────────────────────────────────── */

void cairn_cbor_dec_init(cairn_cbor_dec_t *d, const uint8_t *buf, size_t len)
{
    d->buf = buf;
    d->len = len;
    d->pos = 0;
}

static cairn_err_t read_head(cairn_cbor_dec_t *d, uint8_t *major, uint64_t *arg)
{
    if (d->pos >= d->len) return CAIRN_ERR_TRUNCATED;

    uint8_t ib = d->buf[d->pos++];
    *major = (uint8_t)(ib >> 5);
    uint8_t ai = (uint8_t)(ib & 0x1f);

    if (ai < 24) {
        *arg = ai;
        return CAIRN_OK;
    }

    switch (ai) {
    case 24: {
        if (d->pos + 1 > d->len) return CAIRN_ERR_TRUNCATED;
        uint64_t v = d->buf[d->pos++];
        if (v < 24) return CAIRN_ERR_NON_CANONICAL;
        *arg = v;
        return CAIRN_OK;
    }
    case 25: {
        if (d->pos + 2 > d->len) return CAIRN_ERR_TRUNCATED;
        uint64_t v = ((uint64_t)d->buf[d->pos] << 8) | d->buf[d->pos + 1];
        d->pos += 2;
        if (v <= 0xFF) return CAIRN_ERR_NON_CANONICAL;
        *arg = v;
        return CAIRN_OK;
    }
    case 26: {
        if (d->pos + 4 > d->len) return CAIRN_ERR_TRUNCATED;
        uint64_t v = 0;
        for (int i = 0; i < 4; i++) v = (v << 8) | d->buf[d->pos + i];
        d->pos += 4;
        if (v <= 0xFFFF) return CAIRN_ERR_NON_CANONICAL;
        *arg = v;
        return CAIRN_OK;
    }
    case 27: {
        if (d->pos + 8 > d->len) return CAIRN_ERR_TRUNCATED;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | d->buf[d->pos + i];
        d->pos += 8;
        if (v <= 0xFFFFFFFFu) return CAIRN_ERR_NON_CANONICAL;
        *arg = v;
        return CAIRN_OK;
    }
    case 31:
        return CAIRN_ERR_UNSUPPORTED_CBOR; /* indefinite length */
    default:
        return CAIRN_ERR_UNSUPPORTED_CBOR;
    }
}

static cairn_err_t expect(cairn_cbor_dec_t *d, uint8_t want, uint64_t *arg)
{
    uint8_t major;
    cairn_err_t err = read_head(d, &major, arg);
    if (err != CAIRN_OK) return err;
    if (major != want) return CAIRN_ERR_MALFORMED;
    return CAIRN_OK;
}

cairn_err_t cairn_cbor_uint_read(cairn_cbor_dec_t *d, uint64_t *out)
{
    return expect(d, MAJOR_UINT, out);
}

cairn_err_t cairn_cbor_map_header(cairn_cbor_dec_t *d, size_t *n)
{
    uint64_t v;
    cairn_err_t err = expect(d, MAJOR_MAP, &v);
    if (err != CAIRN_OK) return err;
    *n = (size_t)v;
    return CAIRN_OK;
}

cairn_err_t cairn_cbor_array_header(cairn_cbor_dec_t *d, size_t *n)
{
    uint64_t v;
    cairn_err_t err = expect(d, MAJOR_ARRAY, &v);
    if (err != CAIRN_OK) return err;
    *n = (size_t)v;
    return CAIRN_OK;
}

cairn_err_t cairn_cbor_bytes_read(cairn_cbor_dec_t *d, const uint8_t **p, size_t *len)
{
    uint64_t n;
    cairn_err_t err = expect(d, MAJOR_BYTES, &n);
    if (err != CAIRN_OK) return err;
    if (d->pos + n > d->len) return CAIRN_ERR_TRUNCATED;

    *p = d->buf + d->pos;
    *len = (size_t)n;
    d->pos += (size_t)n;
    return CAIRN_OK;
}

cairn_err_t cairn_cbor_bytes_n(cairn_cbor_dec_t *d, uint8_t *out, size_t n)
{
    const uint8_t *p;
    size_t got;

    cairn_err_t err = cairn_cbor_bytes_read(d, &p, &got);
    if (err != CAIRN_OK) return err;
    if (got != n) return CAIRN_ERR_MALFORMED;

    memcpy(out, p, n);
    return CAIRN_OK;
}

cairn_err_t cairn_cbor_text_read(cairn_cbor_dec_t *d, char *out, size_t cap)
{
    uint64_t n;
    cairn_err_t err = expect(d, MAJOR_TEXT, &n);
    if (err != CAIRN_OK) return err;
    if (d->pos + n > d->len) return CAIRN_ERR_TRUNCATED;
    if (n + 1 > cap) return CAIRN_ERR_BUFFER_TOO_SMALL;

    memcpy(out, d->buf + d->pos, (size_t)n);
    out[n] = '\0';
    d->pos += (size_t)n;
    return CAIRN_OK;
}

bool cairn_cbor_is_null(cairn_cbor_dec_t *d)
{
    if (d->pos < d->len &&
        d->buf[d->pos] == (uint8_t)((MAJOR_SIMPLE << 5) | SIMPLE_NULL)) {
        d->pos++;
        return true;
    }
    return false;
}
