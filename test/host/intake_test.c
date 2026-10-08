/*
 * The v2 intake client against a simulated server and a simulated card.
 *
 * The properties under test are the ones that decide whether a second attempt
 * is cheap or wasteful, and whether a metered link can be trusted with this
 * code at all:
 *
 *   - a bundle ends in exactly one commit and one receipt, and the receipt
 *     bytes arrive byte-for-byte as the server sent them;
 *   - resume sends only what the offer says is missing, and a chunk's offset in
 *     the member stream is derived from the descriptors rather than a compiled
 *     chunk size (so unequal chunk lengths are handled, not just the uniform
 *     case the sealer happens to produce today);
 *   - a permanent refusal is distinguished from a transient one, because a
 *     device that cannot tell them apart either gives up on an outage or
 *     retries a quarantine forever over cellular;
 *   - an abort stops a transfer partway, and chunks already accepted are not
 *     resent on the next attempt;
 *   - bytes read off the card are checked against the signed descriptor BEFORE
 *     being sent, so a flaky card costs a local re-read and not the owner's
 *     data allowance;
 *   - the offer reader refuses any response it would not itself have produced,
 *     while still tolerating a field a later server adds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_intake.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [intake] %s\n", name); }          \
        else      { g_fail++; printf("  FAIL  [intake] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* ── the simulated card ───────────────────────────────────────────────────── */

#define STREAM_BYTES 5000
#define NCHUNKS      4

/* Deliberately unequal, and deliberately not a round number: a client that
 * assumed index * CHUNK_BYTES would read the wrong bytes for every chunk after
 * the first and still pass a uniform-length test. */
static const uint32_t CHUNK_LENS[NCHUNKS] = { 1500, 900, 2000, 600 };

typedef struct {
    uint8_t  stream[STREAM_BYTES];
    uint8_t  manifest[64];
    uint8_t  sig[64];
    bool     corrupt_chunk2;   /* simulate a bad card read */
    int      reads;
} card_t;

static card_t g_card;

static bool card_manifest(void *ctx, const uint8_t **bytes, size_t *len)
{
    card_t *c = ctx;
    *bytes = c->manifest;
    *len   = sizeof(c->manifest);
    return true;
}

static bool card_signature(void *ctx, const uint8_t **sig, size_t *len)
{
    card_t *c = ctx;
    *sig = c->sig;
    *len = sizeof(c->sig);
    return true;
}

static size_t card_read_at(void *ctx, uint64_t offset, uint8_t *out, size_t len)
{
    card_t *c = ctx;
    c->reads++;
    if (offset + len > STREAM_BYTES) return 0;
    memcpy(out, c->stream + offset, len);

    /* Flip a byte inside chunk 2 (offset 2400..4399) on demand. */
    if (c->corrupt_chunk2) {
        for (size_t i = 0; i < len; i++) {
            uint64_t abs = offset + i;
            if (abs == 2500) out[i] ^= 0xff;
        }
    }
    return len;
}

static cairn_chunk_t g_chunks[NCHUNKS];

static void build_card(void)
{
    memset(&g_card, 0, sizeof(g_card));
    for (size_t i = 0; i < STREAM_BYTES; i++) {
        g_card.stream[i] = (uint8_t)((i * 31u + 7u) & 0xff);
    }
    memset(g_card.manifest, 0xA5, sizeof(g_card.manifest));
    memset(g_card.sig, 0x5A, sizeof(g_card.sig));

    uint64_t off = 0;
    for (size_t i = 0; i < NCHUNKS; i++) {
        g_chunks[i].index       = (uint32_t)i;
        g_chunks[i].byte_length = CHUNK_LENS[i];
        cairn_sha256(g_card.stream + off, CHUNK_LENS[i], g_chunks[i].sha256);
        off += CHUNK_LENS[i];
    }
}

/* ── the simulated server ─────────────────────────────────────────────────── */

typedef struct {
    /* Scripted behaviour. */
    int      offer_status;
    int      chunk_status;
    int      commit_status;
    bool     receipt_available;
    bool     have[NCHUNKS];        /* chunks already held, for resume */
    bool     link_fails_on_chunk;  /* the link dies rather than answering */
    int      fail_at_chunk_no;     /* 1-based; 0 = never */
    bool     bad_offer_json;

    /* Observed. */
    int      offers, commits;
    int      chunks_received;
    bool     received[NCHUNKS];
    uint8_t  received_bytes[NCHUNKS][2048];
    uint32_t received_len[NCHUNKS];

    /* In-flight request state. */
    char     method[8];
    char     path[CAIRN_INTAKE_PATH_MAX];
    char     header[256];
    /* The offer's header, kept separately: later requests send none, and a
     * check after the run would read the empty one they left behind. */
    char     offer_header[256];
    uint8_t  body[8192];
    size_t   body_len;
    bool     open;
} server_t;

static server_t g_srv;

static const uint8_t RECEIPT_BYTES[] = {
    0xa3, 0x01, 0x18, 0x2a, 0x02, 0x58, 0x20, 0xde, 0xad, 0xbe, 0xef
};

static bool srv_begin(void *ctx, const char *method, const char *path,
                      const char *extra_header, size_t content_length)
{
    server_t *s = ctx;
    s->open     = true;
    s->body_len = 0;
    snprintf(s->method, sizeof(s->method), "%s", method);
    snprintf(s->path, sizeof(s->path), "%s", path);
    snprintf(s->header, sizeof(s->header), "%s", extra_header ? extra_header : "");
    (void)content_length;
    return true;
}

static bool srv_write(void *ctx, const uint8_t *data, size_t len)
{
    server_t *s = ctx;
    if (s->body_len + len > sizeof(s->body)) return false;
    memcpy(s->body + s->body_len, data, len);
    s->body_len += len;
    return true;
}

/* Which chunk index a PUT path names, by matching the digest. -1 if none. */
static int chunk_index_of_path(const char *path)
{
    for (int i = 0; i < NCHUNKS; i++) {
        char hex[65];
        cairn_intake_hex(g_chunks[i].sha256, 32, hex);
        if (strstr(path, hex) != NULL) return i;
    }
    return -1;
}

static bool srv_finish(void *ctx, int *status, uint8_t *resp, size_t resp_cap,
                       size_t *resp_len)
{
    server_t *s = ctx;
    *resp_len   = 0;

    if (strcmp(s->method, "POST") == 0 && strstr(s->path, "/offer") != NULL) {
        s->offers++;
        snprintf(s->offer_header, sizeof(s->offer_header), "%s", s->header);
        *status = s->offer_status;
        if (s->offer_status != 200) return true;

        if (s->bad_offer_json) {
            const char *bad = "{\"bundle_id\":\"00\"}";
            size_t n = strlen(bad);
            if (n > resp_cap) return false;
            memcpy(resp, bad, n);
            *resp_len = n;
            return true;
        }

        char buf[512];
        int  n = snprintf(buf, sizeof(buf),
                          "{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\","
                          "\"total_chunks\":%d,\"bytes_expected\":%d,"
                          "\"bytes_outstanding\":0,\"missing_chunks\":[",
                          NCHUNKS, STREAM_BYTES);
        bool first = true;
        for (int i = 0; i < NCHUNKS; i++) {
            if (s->have[i]) continue;
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%d",
                          first ? "" : ",", i);
            first = false;
        }
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "],\"receipt_available\":%s}",
                      s->receipt_available ? "true" : "false");

        if ((size_t)n > resp_cap) return false;
        memcpy(resp, buf, (size_t)n);
        *resp_len = (size_t)n;
        return true;
    }

    if (strcmp(s->method, "PUT") == 0 && strstr(s->path, "/chunks/") != NULL) {
        s->chunks_received++;

        if (s->fail_at_chunk_no != 0 && s->chunks_received == s->fail_at_chunk_no) {
            if (s->link_fails_on_chunk) return false;   /* the link died */
        }

        int idx = chunk_index_of_path(s->path);
        if (idx >= 0 && s->body_len <= sizeof(s->received_bytes[0])) {
            s->received[idx] = true;
            memcpy(s->received_bytes[idx], s->body, s->body_len);
            s->received_len[idx] = (uint32_t)s->body_len;
            s->have[idx] = true;
        }

        *status = s->chunk_status;
        const char *ok = "{\"accepted\":true,\"missing_chunks\":[]}";
        size_t n = strlen(ok);
        if (n <= resp_cap) { memcpy(resp, ok, n); *resp_len = n; }
        return true;
    }

    if (strcmp(s->method, "POST") == 0 && strstr(s->path, "/commit") != NULL) {
        s->commits++;
        *status = s->commit_status;
        if (s->commit_status != 200) return true;
        if (sizeof(RECEIPT_BYTES) > resp_cap) return false;
        memcpy(resp, RECEIPT_BYTES, sizeof(RECEIPT_BYTES));
        *resp_len = sizeof(RECEIPT_BYTES);
        return true;
    }

    *status = 404;
    return true;
}

static void srv_close(void *ctx)
{
    server_t *s = ctx;
    s->open = false;
}

static void reset_server(void)
{
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.offer_status  = 200;
    g_srv.chunk_status  = 200;
    g_srv.commit_status = 200;
}

/* ── the abort hook ───────────────────────────────────────────────────────── */

static int  g_abort_after_reads = -1;   /* -1: never */
static bool abort_hook(void *ctx)
{
    (void)ctx;
    if (g_abort_after_reads < 0) return false;
    return g_card.reads >= g_abort_after_reads;
}

/* ── the harness ──────────────────────────────────────────────────────────── */

static cairn_intake_outcome_t run(uint8_t *receipt, size_t *receipt_len,
                                  cairn_intake_stats_t *st, bool with_abort)
{
    static uint8_t scratch[1024];

    cairn_intake_http_t http = {
        .ctx = &g_srv, .begin = srv_begin, .write = srv_write,
        .finish = srv_finish, .close = srv_close
    };
    cairn_intake_bundle_t bundle = {
        .ctx = &g_card, .manifest = card_manifest,
        .signature = card_signature, .read_at = card_read_at
    };

    g_card.reads = 0;

    return cairn_intake_deliver(&http, &bundle, g_chunks, NCHUNKS,
                                scratch, sizeof(scratch),
                                with_abort ? abort_hook : NULL, NULL,
                                receipt, CAIRN_INTAKE_MAX_RECEIPT, receipt_len, st);
}

/* ── the offer reader ─────────────────────────────────────────────────────── */

static bool parse_ok(const char *json, cairn_intake_offer_t *out)
{
    return cairn_intake_parse_offer((const uint8_t *)json, strlen(json), NCHUNKS, out);
}

static void test_offer_reader(void)
{
    cairn_intake_offer_t o;

    CHECK("offer: a well-formed response parses",
          parse_ok("{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\","
                   "\"missing_chunks\":[0,2],\"receipt_available\":false}", &o) &&
          o.missing_count == 2 && o.missing[0] == 0 && o.missing[1] == 2 &&
          !o.receipt_available && o.bundle_id[15] == 0x0f);

    CHECK("offer: an empty missing set is valid and means nothing to send",
          parse_ok("{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\","
                   "\"missing_chunks\":[],\"receipt_available\":true}", &o) &&
          o.missing_count == 0 && o.receipt_available);

    CHECK("offer: a field a later server adds is skipped, not fatal",
          parse_ok("{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\","
                   "\"something_new\":{\"a\":[1,2,{\"b\":null}]},"
                   "\"bytes_outstanding\":-1,"
                   "\"missing_chunks\":[1]}", &o) &&
          o.missing_count == 1 && o.missing[0] == 1);

    CHECK("offer: no bundle_id is refused — there is nowhere to send a chunk",
          !parse_ok("{\"missing_chunks\":[0]}", &o));

    CHECK("offer: a duplicate key is refused",
          !parse_ok("{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\","
                    "\"bundle_id\":\"0f0e0d0c0b0a09080706050403020100\"}", &o));

    CHECK("offer: a bundle_id of the wrong length is refused",
          !parse_ok("{\"bundle_id\":\"0001020304\"}", &o));

    CHECK("offer: a non-hex bundle_id is refused",
          !parse_ok("{\"bundle_id\":\"g00102030405060708090a0b0c0d0e0f\"}", &o));

    CHECK("offer: a chunk index the manifest does not describe is refused",
          !parse_ok("{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\","
                    "\"missing_chunks\":[0," "99" "]}", &o));

    CHECK("offer: a fractional chunk index is refused",
          !parse_ok("{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\","
                    "\"missing_chunks\":[1.0]}", &o));

    CHECK("offer: trailing bytes after the object are refused",
          !parse_ok("{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\"} junk", &o));

    CHECK("offer: an escape in a string is refused rather than decoded",
          !parse_ok("{\"bundle_id\":\"00\\u0031\"}", &o));

    CHECK("offer: an empty body is refused",
          !cairn_intake_parse_offer((const uint8_t *)"", 0, NCHUNKS, &o));

    CHECK("offer: a truncated object is refused",
          !parse_ok("{\"bundle_id\":\"000102030405060708090a0b0c0d0e0f\"", &o));
}

static void test_status_mapping(void)
{
    CHECK("status: 403 not enrolled is permanent",
          cairn_intake_status_permanent(403));
    CHECK("status: 422 quarantined is permanent",
          cairn_intake_status_permanent(422));
    CHECK("status: 401 bad signature is permanent",
          cairn_intake_status_permanent(401));
    CHECK("status: 429 backpressure is NOT permanent",
          !cairn_intake_status_permanent(429));
    CHECK("status: 409 chunks-still-missing is NOT permanent",
          !cairn_intake_status_permanent(409));
    CHECK("status: 507 over quota is NOT permanent — the owner can free space",
          !cairn_intake_status_permanent(507));
    CHECK("status: 500 is NOT permanent",
          !cairn_intake_status_permanent(500));
    CHECK("status: 503 is NOT permanent",
          !cairn_intake_status_permanent(503));
}

/* ── the conversation ─────────────────────────────────────────────────────── */

static void test_happy_path(void)
{
    build_card();
    reset_server();
    g_abort_after_reads = -1;

    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_stats_t st;

    cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

    CHECK("happy: outcome is a receipt", r == CAIRN_INTAKE_RECEIPT);
    CHECK("happy: exactly one offer and one commit",
          g_srv.offers == 1 && g_srv.commits == 1);
    CHECK("happy: every chunk was sent once", g_srv.chunks_received == NCHUNKS);
    CHECK("happy: the receipt arrives byte-for-byte",
          rlen == sizeof(RECEIPT_BYTES) &&
          memcmp(receipt, RECEIPT_BYTES, rlen) == 0);
    CHECK("happy: stats count the chunks sent", st.chunks_sent == NCHUNKS);
    CHECK("happy: bytes_up covers the manifest and every chunk",
          st.bytes_up == STREAM_BYTES + sizeof(g_card.manifest));

    /* The signature header must be the hex of the 64 signature bytes. */
    char want[200];
    char hex[129];
    cairn_intake_hex(g_card.sig, 64, hex);
    snprintf(want, sizeof(want), "X-Cairn-Signature: %s", hex);
    CHECK("happy: the manifest signature travels as a hex header",
          strcmp(g_srv.offer_header, want) == 0);

    /* Each chunk must carry the right bytes at the right offset: the property
     * a compiled-in chunk size would break. */
    bool bytes_ok = true;
    uint64_t off = 0;
    for (int i = 0; i < NCHUNKS; i++) {
        if (g_srv.received_len[i] != CHUNK_LENS[i] ||
            memcmp(g_srv.received_bytes[i], g_card.stream + off, CHUNK_LENS[i]) != 0) {
            bytes_ok = false;
        }
        off += CHUNK_LENS[i];
    }
    CHECK("happy: unequal chunks are read from the right offsets", bytes_ok);
}

static void test_resume(void)
{
    build_card();
    reset_server();
    g_abort_after_reads = -1;

    /* The server already holds 0 and 2: a previous slot got that far. */
    g_srv.have[0] = true;
    g_srv.have[2] = true;

    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_stats_t st;

    cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

    CHECK("resume: outcome is a receipt", r == CAIRN_INTAKE_RECEIPT);
    CHECK("resume: only the two missing chunks were sent",
          g_srv.chunks_received == 2 && st.chunks_sent == 2);
    CHECK("resume: the two already held are reported as skipped",
          st.chunks_skipped == 2);
    CHECK("resume: chunk 1 and 3 are the ones that arrived",
          g_srv.received[1] && g_srv.received[3]);
    CHECK("resume: only the missing bytes crossed the link",
          st.bytes_up == CHUNK_LENS[1] + CHUNK_LENS[3] + sizeof(g_card.manifest));
}

static void test_receipt_already_available(void)
{
    build_card();
    reset_server();
    g_abort_after_reads = -1;

    for (int i = 0; i < NCHUNKS; i++) g_srv.have[i] = true;
    g_srv.receipt_available = true;

    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_stats_t st;

    cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

    CHECK("already committed: a receipt comes back with no chunks sent",
          r == CAIRN_INTAKE_RECEIPT && g_srv.chunks_received == 0 &&
          rlen == sizeof(RECEIPT_BYTES));
    CHECK("already committed: commit is still called to fetch the bytes",
          g_srv.commits == 1);
}

static void test_permanent_refusals(void)
{
    const int codes[] = { 403, 401, 422, 400 };
    for (size_t k = 0; k < sizeof(codes) / sizeof(codes[0]); k++) {
        build_card();
        reset_server();
        g_abort_after_reads = -1;
        g_srv.offer_status = codes[k];

        cairn_intake_stats_t st;
        uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
        size_t  rlen = 0;
        cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

        char name[96];
        snprintf(name, sizeof(name),
                 "refusal: HTTP %d on the offer is permanent, nothing is sent", codes[k]);
        CHECK(name, r == CAIRN_INTAKE_REFUSED && g_srv.chunks_received == 0);
    }
}

static void test_transient_failures(void)
{
    const int codes[] = { 500, 503, 429, 507 };
    for (size_t k = 0; k < sizeof(codes) / sizeof(codes[0]); k++) {
        build_card();
        reset_server();
        g_abort_after_reads = -1;
        g_srv.offer_status = codes[k];

        cairn_intake_stats_t st;
        uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
        size_t  rlen = 0;
        cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

        char name[96];
        snprintf(name, sizeof(name),
                 "transient: HTTP %d on the offer is retryable, not refused", codes[k]);
        CHECK(name, r == CAIRN_INTAKE_FAILED);
    }
}

static void test_commit_conflict(void)
{
    build_card();
    reset_server();
    g_abort_after_reads = -1;
    g_srv.commit_status = 409;

    cairn_intake_stats_t st;
    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

    CHECK("commit 409: partial, so the chunks already accepted are kept",
          r == CAIRN_INTAKE_PARTIAL);
    CHECK("commit 409: no receipt is reported", rlen == 0);
}

static void test_link_dies_mid_transfer(void)
{
    build_card();
    reset_server();
    g_abort_after_reads = -1;
    g_srv.link_fails_on_chunk = true;
    g_srv.fail_at_chunk_no    = 3;

    cairn_intake_stats_t st;
    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

    CHECK("link death: partial, because two chunks did land",
          r == CAIRN_INTAKE_PARTIAL);
    CHECK("link death: no commit was attempted", g_srv.commits == 0);
    CHECK("link death: no receipt", rlen == 0);

    /* The second attempt must send only what is left. */
    int first_round = g_srv.chunks_received;
    g_srv.link_fails_on_chunk = false;
    g_srv.fail_at_chunk_no    = 0;

    r = run(receipt, &rlen, &st, false);
    CHECK("link death: the retry commits", r == CAIRN_INTAKE_RECEIPT);
    CHECK("link death: the retry resends only the chunks that did not land",
          g_srv.chunks_received - first_round < NCHUNKS);
}

static void test_abort(void)
{
    build_card();
    reset_server();

    /* Abort once the card has been read a few times: partway through. */
    g_abort_after_reads = 3;

    cairn_intake_stats_t st;
    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_outcome_t r = run(receipt, &rlen, &st, true);

    CHECK("abort: the transfer stops without a receipt",
          (r == CAIRN_INTAKE_ABORTED || r == CAIRN_INTAKE_PARTIAL) && rlen == 0);
    CHECK("abort: not every chunk was sent", g_srv.chunks_received < NCHUNKS);
    CHECK("abort: no commit was attempted", g_srv.commits == 0);
}

static void test_abort_before_anything(void)
{
    build_card();
    reset_server();
    g_abort_after_reads = 0;   /* already true on the first poll */

    cairn_intake_stats_t st;
    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_outcome_t r = run(receipt, &rlen, &st, true);

    CHECK("abort: an abort already pending means no offer is even made",
          r == CAIRN_INTAKE_ABORTED && g_srv.offers == 0);
}

static void test_bad_card_read(void)
{
    build_card();
    reset_server();
    g_abort_after_reads   = -1;
    g_card.corrupt_chunk2 = true;

    cairn_intake_stats_t st;
    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

    CHECK("bad card read: the transfer stops without a receipt",
          r != CAIRN_INTAKE_RECEIPT && rlen == 0);
    CHECK("bad card read: the corrupt chunk is never sent", !g_srv.received[2]);
    CHECK("bad card read: it is not treated as a permanent refusal",
          r != CAIRN_INTAKE_REFUSED);
    CHECK("bad card read: the good chunks before it did go",
          g_srv.received[0] && g_srv.received[1]);
}

static void test_bad_offer_response(void)
{
    build_card();
    reset_server();
    g_abort_after_reads = -1;
    g_srv.bad_offer_json = true;

    cairn_intake_stats_t st;
    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    cairn_intake_outcome_t r = run(receipt, &rlen, &st, false);

    CHECK("bad offer body: refused as a failure, nothing sent",
          r == CAIRN_INTAKE_FAILED && g_srv.chunks_received == 0);
}

static void test_argument_guards(void)
{
    build_card();
    reset_server();

    cairn_intake_http_t http = {
        .ctx = &g_srv, .begin = srv_begin, .write = srv_write,
        .finish = srv_finish, .close = srv_close
    };
    cairn_intake_bundle_t bundle = {
        .ctx = &g_card, .manifest = card_manifest,
        .signature = card_signature, .read_at = card_read_at
    };

    uint8_t receipt[CAIRN_INTAKE_MAX_RECEIPT];
    size_t  rlen = 0;
    uint8_t tiny[8];

    CHECK("guard: a scratch buffer below the floor is refused",
          cairn_intake_deliver(&http, &bundle, g_chunks, NCHUNKS, tiny, sizeof(tiny),
                               NULL, NULL, receipt, sizeof(receipt), &rlen, NULL)
          == CAIRN_INTAKE_FAILED);

    static uint8_t scratch[1024];
    CHECK("guard: a bundle with no chunks is refused",
          cairn_intake_deliver(&http, &bundle, g_chunks, 0, scratch, sizeof(scratch),
                               NULL, NULL, receipt, sizeof(receipt), &rlen, NULL)
          == CAIRN_INTAKE_FAILED);

    CHECK("guard: no request was made in either case", g_srv.offers == 0);
}

int main(void)
{
    printf("intake: the v2 offer/chunks/commit client\n");

    test_offer_reader();
    test_status_mapping();
    test_happy_path();
    test_resume();
    test_receipt_already_available();
    test_permanent_refusals();
    test_transient_failures();
    test_commit_conflict();
    test_link_dies_mid_transfer();
    test_abort();
    test_abort_before_anything();
    test_bad_card_read();
    test_bad_offer_response();
    test_argument_guards();

    printf("intake: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
