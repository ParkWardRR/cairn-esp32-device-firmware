/*
 * Host tests for lib/cairn_prov: X25519, the sealed enrolment blob, the
 * provisioning protocol and the erasure of legacy credential slots.
 *
 * Three kinds of check, and the distinction is the point:
 *
 *   - known answers (RFC 7748; the Go reference's vectors) — the primitives and
 *     the blob are what other implementations expect;
 *   - protocol properties (window, trip, idle, atomic commit) — what the device
 *     will and will not accept;
 *   - confidentiality properties — a canary secret is sent through every path
 *     and must not appear in any log line or reply the device produces.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../lib/cairn_format/cairn_format.h"
#include "../../lib/cairn_fs/cairn_kv.h"
#include "../../lib/cairn_prov/cairn_prov.h"
#include "minijson.h"

void cairn_kv_host_set_path(const char *path);
int  cairn_kv_host_scrub_count(void);

static int s_pass, s_fail;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL  %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* ── helpers ──────────────────────────────────────────────────────────────── */

static bool unhex(const char *h, uint8_t *out, size_t n)
{
    if (strlen(h) != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = '\0';
    fclose(f);
    return b;
}

/* ── X25519 ───────────────────────────────────────────────────────────────── */

static bool row_x25519_rfc7748(void)
{
    uint8_t k[32], u[32], want[32], got[32];

    /* RFC 7748 §5.2, first vector. */
    CHECK(unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32), "hex");
    CHECK(unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32), "hex");
    CHECK(unhex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", want, 32), "hex");
    CHECK(cairn_x25519(got, k, u), "x25519 reported failure");
    CHECK(memcmp(got, want, 32) == 0, "RFC 7748 5.2 vector 1 differs");

    /* §5.2, second vector. */
    CHECK(unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", k, 32), "hex");
    CHECK(unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u, 32), "hex");
    CHECK(unhex("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", want, 32), "hex");
    CHECK(cairn_x25519(got, k, u), "x25519 reported failure");
    CHECK(memcmp(got, want, 32) == 0, "RFC 7748 5.2 vector 2 differs");

    /* §6.1: both sides derive one shared secret from each other's public key. */
    uint8_t a[32], b[32], apub[32], bpub[32], s1[32], s2[32], shared[32];
    CHECK(unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32), "hex");
    CHECK(unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32), "hex");
    CHECK(cairn_x25519_public(apub, a) && cairn_x25519_public(bpub, b), "base point");
    CHECK(unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want, 32), "hex");
    CHECK(memcmp(apub, want, 32) == 0, "Alice public key differs from RFC 7748 6.1");
    CHECK(unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", want, 32), "hex");
    CHECK(memcmp(bpub, want, 32) == 0, "Bob public key differs from RFC 7748 6.1");
    CHECK(cairn_x25519(s1, a, bpub) && cairn_x25519(s2, b, apub), "dh");
    CHECK(unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", shared, 32), "hex");
    CHECK(memcmp(s1, shared, 32) == 0 && memcmp(s2, shared, 32) == 0, "shared secret differs from RFC 7748 6.1");
    return true;
}

static bool row_x25519_low_order(void)
{
    uint8_t k[32], u[32] = { 0 }, out[32];
    memset(k, 0x42, 32);

    /* u = 0 and u = 1 are low-order points: the result is all zero, which would
     * be a "shared secret" known to everyone. It must be reported as failure. */
    CHECK(!cairn_x25519(out, k, u), "accepted u = 0");
    u[0] = 1;
    CHECK(!cairn_x25519(out, k, u), "accepted u = 1");
    return true;
}

/* ── base64 ───────────────────────────────────────────────────────────────── */

static bool row_base64(void)
{
    char enc[64];
    uint8_t dec[64];

    for (size_t n = 0; n <= 20; n++) {
        uint8_t in[20];
        for (size_t i = 0; i < n; i++) in[i] = (uint8_t)(i * 37 + 11);
        size_t el = cairn_b64_encode(in, n, enc, sizeof(enc));
        CHECK(el == cairn_b64_encoded_len(n), "length %zu", n);
        int dl = cairn_b64_decode(enc, el, dec, sizeof(dec));
        CHECK(dl == (int)n && memcmp(dec, in, n) == 0, "round trip %zu", n);
    }

    /* One byte string, one spelling. */
    CHECK(cairn_b64_decode("QQ==", 4, dec, sizeof(dec)) == 1, "QQ==");
    CHECK(cairn_b64_decode("QR==", 4, dec, sizeof(dec)) < 0, "non-zero padding bits accepted");
    CHECK(cairn_b64_decode("QUE=", 4, dec, sizeof(dec)) == 2, "QUE=");
    CHECK(cairn_b64_decode("QUF=", 4, dec, sizeof(dec)) < 0, "non-zero padding bits accepted (1 pad)");
    CHECK(cairn_b64_decode("QQ=", 3, dec, sizeof(dec)) < 0, "short");
    CHECK(cairn_b64_decode("QQ==QQ==", 8, dec, sizeof(dec)) < 0, "padding mid-stream");
    CHECK(cairn_b64_decode("QQ =", 4, dec, sizeof(dec)) < 0, "space");
    CHECK(cairn_b64_decode("Q-==", 4, dec, sizeof(dec)) < 0, "url alphabet");
    CHECK(cairn_b64_decode("QUJD", 4, dec, 2) < 0, "wrote past the buffer");
    return true;
}

/* ── the blob, against the Go vectors ─────────────────────────────────────── */

static bool row_blob_matches_go(void)
{
    /* The vectors belong to the Cairn contracts, not to this project: the Makefile hands
     * their location over (CAIRN_ENROLL_VECTORS). */
    const char *vpath = getenv("CAIRN_ENROLL_VECTORS");
    CHECK(vpath != NULL, "CAIRN_ENROLL_VECTORS is not set; run through the Makefile or point it at "
          "$CAIRN_CONTRACTS/enrolment/v1/vectors/vectors.json");
    char *text = slurp(vpath);
    CHECK(text != NULL, "cannot read %s", vpath);
    mj_doc_t *doc = (mj_doc_t *)malloc(sizeof(*doc));
    CHECK(mj_parse(doc, text), "vectors.json: %s", doc->error);

    const mj_node_t *vs = mj_get(doc, mj_root(doc), "vectors");
    size_t n = mj_len(doc, vs);
    CHECK(n >= 2, "expected at least two vectors, got %zu", n);

    for (size_t i = 0; i < n; i++) {
        const mj_node_t *v = mj_at(doc, vs, i);
        uint8_t seed[32], pub[32], id[16], root[32], spub[32], epriv[32], nonce[24], want[CAIRN_ENROLL_BLOB_SIZE];
        CHECK(unhex(mj_str_or(mj_get(doc, v, "device_seed"), ""), seed, 32), "seed");
        CHECK(unhex(mj_str_or(mj_get(doc, v, "device_public_key"), ""), pub, 32), "pub");
        CHECK(unhex(mj_str_or(mj_get(doc, v, "device_id"), ""), id, 16), "id");
        CHECK(unhex(mj_str_or(mj_get(doc, v, "storage_root"), ""), root, 32), "root");
        CHECK(unhex(mj_str_or(mj_get(doc, v, "server_public_key"), ""), spub, 32), "server pub");
        CHECK(unhex(mj_str_or(mj_get(doc, v, "ephemeral_private_key"), ""), epriv, 32), "eph");
        CHECK(unhex(mj_str_or(mj_get(doc, v, "nonce"), ""), nonce, 24), "nonce");
        CHECK(unhex(mj_str_or(mj_get(doc, v, "blob"), ""), want, sizeof(want)), "blob");
        uint32_t kver = (uint32_t)mj_num_or(mj_get(doc, v, "storage_key_version"), 0);

        /* The public key the device derives from its seed must be the vector's. */
        uint8_t derived[32];
        cairn_ed25519_public_from_seed(seed, derived);
        CHECK(memcmp(derived, pub, 32) == 0, "vector %zu: Ed25519 public key from seed differs", i);

        uint8_t got[CAIRN_ENROLL_BLOB_SIZE];
        CHECK(cairn_enroll_build(got, id, seed, pub, kver, root, spub, epriv, nonce), "vector %zu: build refused", i);
        CHECK(memcmp(got, want, sizeof(want)) == 0, "vector %zu: blob differs from the Go reference", i);

        char fp[9];
        cairn_enroll_fingerprint(pub, fp);
        CHECK(strcmp(fp, mj_str_or(mj_get(doc, v, "fingerprint"), "")) == 0, "vector %zu: fingerprint %s", i, fp);

        /* Refusals: never seal to a null key, never seal a null root, never a
         * key version of 0. */
        uint8_t zero[32] = { 0 };
        CHECK(!cairn_enroll_build(got, id, seed, pub, kver, root, zero, epriv, nonce), "sealed to an all-zero server key");
        CHECK(!cairn_enroll_build(got, id, seed, pub, kver, zero, spub, epriv, nonce), "sealed an all-zero root");
        CHECK(!cairn_enroll_build(got, id, seed, pub, 0, root, spub, epriv, nonce), "accepted key version 0");
    }
    free(doc);
    free(text);
    return true;
}

/* ── the protocol ─────────────────────────────────────────────────────────── */

/*
 * This firmware holds no network credential, so the console accepts none. The
 * canaries below are what an out-of-date host tool would send; the device must
 * refuse them, stage nothing, and never echo or log them.
 */
#define CANARY_PASS "SECRET-WIFI-PASSWORD-CANARY"
#define CANARY_KEY  "SECRET-KEY-CANARY-DO-NOT-LEAK"

static char s_out[16384];
static char s_log[16384];
static int  s_applies;
static bool s_apply_fails;
static cairn_prov_staged_t s_last;

static void m_identity(void *c, uint8_t id[16], char fp[9])
{
    (void)c;
    for (int i = 0; i < 16; i++) id[i] = (uint8_t)(0xA0 + i);
    memcpy(fp, "5fd41190", 9);
}
static bool m_enroll(void *c, char *out, size_t cap, const char **err)
{
    (void)c; (void)err;
    snprintf(out, cap, "Q0VOUg==");
    return true;
}
static const char *m_apply(void *c, const cairn_prov_staged_t *st)
{
    (void)c;
    s_applies++;
    if (s_apply_fails) return "storage write failed";
    s_last = *st;
    return NULL;
}
/* The two optional ops. Scripted so the protocol's handling of a build that
 * lacks them, and of a failure, can both be exercised. */
static uint32_t s_drop_count   = 2;
static uint64_t s_drop_bytes   = 4096;
static bool     s_drop_fails   = false;
static int      s_drops_called = 0;
static uint32_t s_bonds        = 1;
static bool     s_bond_fails   = false;
static int      s_bonds_called = 0;

static bool m_drop(void *c, uint32_t *dropped, uint64_t *bytes)
{
    (void)c;
    s_drops_called++;
    if (s_drop_fails) return false;
    *dropped = s_drop_count;
    *bytes   = s_drop_bytes;
    return true;
}

static bool m_bonds(void *c, uint32_t *cleared)
{
    (void)c;
    s_bonds_called++;
    if (s_bond_fails) return false;
    *cleared = s_bonds;
    return true;
}

static void m_log(void *c, const char *m) { (void)c; strncat(s_log, m, sizeof(s_log) - strlen(s_log) - 2); strcat(s_log, "\n"); }
static void m_reply(void *c, const char *l) { (void)c; strncat(s_out, l, sizeof(s_out) - strlen(s_out) - 2); strcat(s_out, "\n"); }

/* Designated, so adding an op does not silently shift these into the wrong
 * slots -- which is exactly what a positional initializer did here once. */
static const cairn_prov_ops_t OPS = {
    .ctx                 = NULL,
    .identity            = m_identity,
    .enroll_text         = m_enroll,
    .apply               = m_apply,
    .drop_legacy_bundles = m_drop,
    .clear_ble_bonds     = m_bonds,
    .log                 = m_log,
    .reply               = m_reply,
};

/* The same, without the optional ops, for a build that has neither. */
static const cairn_prov_ops_t OPS_MINIMAL = {
    .ctx                 = NULL,
    .identity            = m_identity,
    .enroll_text         = m_enroll,
    .apply               = m_apply,
    .drop_legacy_bundles = NULL,
    .clear_ble_bonds     = NULL,
    .log                 = m_log,
    .reply               = m_reply,
};

static void reset_io(void) { s_out[0] = s_log[0] = 0; s_applies = 0; s_apply_fails = false; memset(&s_last, 0, sizeof(s_last)); }

static void send(cairn_prov_t *p, const cairn_prov_env_t *env, const char *line) { cairn_prov_line(p, line, env); }

static char *b64(const char *s)
{
    size_t n = strlen(s);
    char *o = (char *)malloc(cairn_b64_encoded_len(n) + 1);
    cairn_b64_encode((const uint8_t *)s, n, o, cairn_b64_encoded_len(n) + 1);
    return o;
}

static void send_set(cairn_prov_t *p, const cairn_prov_env_t *env, const char *field, const char *value)
{
    char *e = b64(value);
    char *line = (char *)malloc(strlen(field) + strlen(e) + 8);
    snprintf(line, strlen(field) + strlen(e) + 8, "SET %s %s", field, e);
    send(p, env, line);
    free(line);
    free(e);
}

static const char *PEM_KEY  = "-----BEGIN EC PRIVATE KEY-----\n" CANARY_KEY "\n-----END EC PRIVATE KEY-----\n";
static const char *ASSIGN   = "SET assignment 606162636465666768696a6b6c6d6e6f 707172737475767778797a7b7c7d7e7f";

static bool row_window_rules(void)
{
    cairn_prov_t p;

    /* Unprovisioned: always open, whatever the uptime. */
    reset_io(); cairn_prov_init(&p, &OPS);
    cairn_prov_env_t late_fresh = { 900000, false, false };
    send(&p, &late_fresh, "CAIRN-PROV BEGIN");
    CHECK(strstr(s_out, "PROV-READY a0a1a2a3a4a5a6a7a8a9aaabacadaeaf 5fd41190"), "unprovisioned device refused: %s", s_out);
    cairn_prov_abort(&p);

    /* Provisioned: only within the window after boot. */
    reset_io(); cairn_prov_init(&p, &OPS);
    cairn_prov_env_t inside = { 59000, false, true }, outside = { 61000, false, true };
    send(&p, &outside, "CAIRN-PROV BEGIN");
    CHECK(strstr(s_out, "ERR refused") && !p.active, "a provisioned device accepted BEGIN after the window: %s", s_out);
    CHECK(strstr(s_log, "BEGIN refused"), "the refusal was not logged");
    reset_io();
    send(&p, &inside, "CAIRN-PROV BEGIN");
    CHECK(strstr(s_out, "PROV-READY") && p.active, "refused inside the window: %s", s_out);
    cairn_prov_abort(&p);

    /* Never during a trip, provisioned or not. */
    for (int prov = 0; prov < 2; prov++) {
        reset_io(); cairn_prov_init(&p, &OPS);
        cairn_prov_env_t trip = { 1000, true, prov != 0 };
        send(&p, &trip, "CAIRN-PROV BEGIN");
        CHECK(strstr(s_out, "ERR refused") && !p.active, "opened a session during a trip (provisioned=%d)", prov);
    }
    return true;
}

static bool row_trip_aborts_session(void)
{
    cairn_prov_t p;
    reset_io(); cairn_prov_init(&p, &OPS);
    cairn_prov_env_t env = { 1000, false, false };
    send(&p, &env, "CAIRN-PROV BEGIN");
    send(&p, &env, ASSIGN);
    CHECK(p.st.have_assignment, "assignment not staged");

    env.uptime_ms = 2000; env.trip_active = true;
    send(&p, &env, "SET counter_floor 5");
    CHECK(!p.active && !p.st.have_assignment, "a trip starting did not abort and scrub the session");
    CHECK(strstr(s_out, "ERR aborted"), "no abort reply: %s", s_out);

    /* tick() alone also drops it. */
    reset_io(); cairn_prov_init(&p, &OPS);
    env.trip_active = false;
    send(&p, &env, "CAIRN-PROV BEGIN");
    send(&p, &env, ASSIGN);
    env.trip_active = true;
    cairn_prov_tick(&p, &env);
    CHECK(!p.active && !p.st.have_assignment, "tick did not abort on a trip");
    return true;
}

static bool row_idle_timeout(void)
{
    cairn_prov_t p;
    reset_io(); cairn_prov_init(&p, &OPS);
    cairn_prov_env_t env = { 1000, false, false };
    send(&p, &env, "CAIRN-PROV BEGIN");
    send(&p, &env, ASSIGN);
    env.uptime_ms += CAIRN_PROV_IDLE_TIMEOUT_MS + 1;
    reset_io();
    send(&p, &env, "SET counter_floor 5");
    CHECK(strstr(s_out, "ERR session expired") && !p.active && !p.st.have_assignment, "idle session survived: %s", s_out);
    return true;
}

static bool row_validation(void)
{
    cairn_prov_t p;
    reset_io(); cairn_prov_init(&p, &OPS);
    cairn_prov_env_t env = { 1000, false, false };
    send(&p, &env, "CAIRN-PROV BEGIN");

    struct { const char *line; } bad[] = {
        { "SET assignment 00 00" },
        { "SET assignment 00000000000000000000000000000000 11111111111111111111111111111111" }, /* zero vehicle */
        { "SET assignment 11111111111111111111111111111111 00000000000000000000000000000000" }, /* zero assignment */
        { "SET counter_floor 18446744073709551616" },    /* 2^64 */
        { "SET counter_floor -1" },
        { "SET counter_floor 12x" },
        { "SET counter_floor " },                        /* missing argument */
        { "SET nosuchfield AAAA" },
        { "FROBNICATE" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset_io();
        send(&p, &env, bad[i].line);
        CHECK(strstr(s_out, "ERR"), "accepted: %s -> %s", bad[i].line, s_out);
    }
    CHECK(!p.st.have_assignment && !p.st.have_floor, "a rejected line left something staged");

    /* A line that is too long is refused whole. */
    char *big = (char *)malloc(CAIRN_PROV_LINE_MAX + 64);
    memset(big, 'A', CAIRN_PROV_LINE_MAX + 32);
    memcpy(big, "SET counter_floor ", 18);
    big[CAIRN_PROV_LINE_MAX + 32] = 0;
    reset_io();
    send(&p, &env, big);
    CHECK(strstr(s_out, "ERR"), "oversized line not refused: %.60s", s_out);
    free(big);

    /* Lines outside a session are ignored silently. */
    cairn_prov_abort(&p);
    reset_io();
    send(&p, &env, "SET counter_floor 5");
    send(&p, &env, "COMMIT");
    CHECK(s_out[0] == 0, "a device answered lines outside a session: %s", s_out);
    return true;
}

/*
 * The fields this firmware used to accept. A host tool from before the Wi-Fi
 * removal must be refused rather than obeyed: a private key written to NVS by a
 * stale script is exactly the material this change exists to keep off the chip.
 */
static bool row_legacy_credential_fields_are_refused(void)
{
    cairn_prov_t p;
    reset_io(); cairn_prov_init(&p, &OPS);
    cairn_prov_env_t env = { 1000, false, false };
    send(&p, &env, "CAIRN-PROV BEGIN");

    static const char *const fields[] = { "wifi_ssid", "wifi_pass", "client_cert", "client_key" };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        reset_io();
        send_set(&p, &env, fields[i], PEM_KEY);
        CHECK(strstr(s_out, "ERR unknown field"), "legacy field %s was not refused: %s", fields[i], s_out);
    }
    reset_io();
    send(&p, &env, "SET wifi_pass -");
    CHECK(strstr(s_out, "ERR"), "legacy open-network marker was accepted: %s", s_out);

    reset_io();
    send(&p, &env, "COMMIT");
    CHECK(strstr(s_out, "ERR nothing staged") && s_applies == 0, "COMMIT after refused credentials applied something: %s", s_out);
    return true;
}

static bool row_commit_rules(void)
{
    cairn_prov_t p;
    reset_io(); cairn_prov_init(&p, &OPS);
    cairn_prov_env_t env = { 1000, false, false };
    send(&p, &env, "CAIRN-PROV BEGIN");

    reset_io();
    send(&p, &env, "COMMIT");
    CHECK(strstr(s_out, "ERR nothing staged") && s_applies == 0, "empty COMMIT: %s", s_out);

    send(&p, &env, ASSIGN);
    send(&p, &env, "SET counter_floor 7");
    reset_io();
    send(&p, &env, "COMMIT");
    CHECK(strstr(s_out, "OK") && s_applies == 1, "valid COMMIT failed: %s", s_out);
    CHECK(s_last.have_floor && s_last.counter_floor == 7, "floor not delivered");
    CHECK(s_last.have_assignment && s_last.vehicle_id[0] == 0x60 && s_last.assignment_id[0] == 0x70, "assignment not delivered");
    CHECK(!p.active && !p.st.have_assignment && !p.st.have_floor, "committed session was not scrubbed and closed");

    /* A failed apply keeps the staged values so COMMIT can simply be re-sent. */
    reset_io(); cairn_prov_init(&p, &OPS);
    send(&p, &env, "CAIRN-PROV BEGIN");
    send(&p, &env, "SET counter_floor 9");
    reset_io(); s_apply_fails = true;
    send(&p, &env, "COMMIT");
    CHECK(strstr(s_out, "ERR storage write failed") && p.active && p.st.have_floor, "failed apply dropped the session: %s", s_out);
    s_apply_fails = false;
    reset_io();
    send(&p, &env, "COMMIT");
    CHECK(strstr(s_out, "OK") && s_applies == 1 && s_last.counter_floor == 9, "re-sent COMMIT did not converge: %s", s_out);
    return true;
}

static bool row_no_secret_leaks(void)
{
    cairn_prov_t p;
    reset_io(); cairn_prov_init(&p, &OPS);
    cairn_prov_env_t env = { 1000, false, false };
    send(&p, &env, "CAIRN-PROV BEGIN");
    send_set(&p, &env, "wifi_ssid", "HomeNet");
    send_set(&p, &env, "wifi_pass", CANARY_PASS);
    send_set(&p, &env, "client_key", PEM_KEY);
    send(&p, &env, "GET enroll_blob");
    send(&p, &env, ASSIGN);
    send(&p, &env, "COMMIT");
    send(&p, &env, "CAIRN-PROV END");

    /* Every string the device produced, replies and log together. Encoded forms
     * count too: base64 of a secret is the secret. */
    char *enc_pass = b64(CANARY_PASS), *enc_key = b64(PEM_KEY);
    CHECK(!strstr(s_out, CANARY_PASS) && !strstr(s_log, CANARY_PASS), "the Wi-Fi password was echoed or logged");
    CHECK(!strstr(s_out, CANARY_KEY) && !strstr(s_log, CANARY_KEY), "the private key was echoed or logged");
    CHECK(!strstr(s_out, enc_pass) && !strstr(s_log, enc_pass), "the Wi-Fi password was echoed or logged (base64)");
    CHECK(!strstr(s_out, enc_key) && !strstr(s_log, enc_key), "the private key was echoed or logged (base64)");
    CHECK(strstr(s_log, "committed"), "the log does not record the events at all: %s", s_log);
    CHECK(s_applies == 1 && s_last.have_assignment, "the legitimate part of the session did not apply");
    free(enc_pass); free(enc_key);
    return true;
}

/* ── legacy credential slots ──────────────────────────────────────────────── */

static bool kv_has(const char *key)
{
    uint8_t probe; size_t n = 0;
    return cairn_kv_get_blob_var(key, &probe, 0, &n) && n > 0;
}

static bool row_legacy_slots_are_erased(void)
{
    remove("/tmp/cairn-prov-kv.bin");
    cairn_kv_host_set_path("/tmp/cairn-prov-kv.bin");
    CHECK(cairn_kv_begin(), "kv");

    /*
     * A fresh store has no legacy entries, but the first boot still scrubs the
     * flash once: a unit whose entries an earlier boot already marked deleted looks
     * exactly like this, and its bytes are still readable.
     */
    int base = cairn_kv_host_scrub_count();
    CHECK(cairn_prov_erase_legacy_credentials(), "the first call did not scrub");
    CHECK(cairn_kv_host_scrub_count() == base + 1, "the first call scrubbed %d times, want 1", cairn_kv_host_scrub_count() - base);
    CHECK(!cairn_prov_erase_legacy_credentials(), "a second call on a clean store claimed to do something");
    CHECK(cairn_kv_host_scrub_count() == base + 1, "a clean store was scrubbed again: the marker is not honoured");

    /* What earlier firmware left behind: both slots, and the selector. */
    static const char *const names[] = { "pv0_ssid", "pv0_pass", "pv0_crt", "pv0_key",
                                         "pv1_ssid", "pv1_pass", "pv1_crt", "pv1_key" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK(cairn_kv_set_blob(names[i], PEM_KEY, strlen(PEM_KEY)), "seeding %s", names[i]);
    }
    CHECK(cairn_kv_set_u32("pv_slot", 1), "seeding the slot selector");
    /* Something that is not ours to touch. */
    CHECK(cairn_kv_set_u32("st_counter", 41), "seeding an unrelated key");

    CHECK(cairn_prov_erase_legacy_credentials(), "did not report erasing the legacy slots");
    CHECK(cairn_kv_host_scrub_count() == base + 2, "erasing legacy entries did not force a flash scrub (deleted bytes would stay readable)");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK(!kv_has(names[i]), "%s survived the erase: a private key is still in flash", names[i]);
    }
    CHECK(cairn_kv_get_u32("pv_slot", 0xFFFFFFFFu) == 0xFFFFFFFFu, "the slot selector survived");
    CHECK(cairn_kv_get_u32("st_counter", 0) == 41, "the erase touched an unrelated key");

    /* Idempotent: a second boot finds nothing, scrubs nothing and says so. */
    CHECK(!cairn_prov_erase_legacy_credentials(), "a second erase claimed to find something");
    CHECK(cairn_kv_host_scrub_count() == base + 2, "a second erase scrubbed again");

    cairn_kv_end();
    remove("/tmp/cairn-prov-kv.bin");
    return true;
}

typedef struct { const char *name; bool (*fn)(void); } row_t;

/*
 * DROP legacy-bundles and CLEAR bonds.
 *
 * Both delete something, so what matters is that they are reachable only
 * through a session that has already passed the window and trip rules, that
 * they report what they did, and that a build without them says so rather than
 * silently appearing to succeed.
 */
static bool row_drop_and_clear(void)
{
    cairn_prov_t p;
    cairn_prov_init(&p, &OPS);
    cairn_prov_env_t env = { 1000, false, false };

    /* Outside a session: no reply at all, and certainly no deletion. */
    reset_io();
    s_drops_called = 0;
    s_bonds_called = 0;
    send(&p, &env, "DROP legacy-bundles");
    send(&p, &env, "CLEAR bonds");
    CHECK(s_drops_called == 0 && s_bonds_called == 0,
          "DROP and CLEAR outside a session do nothing");
    CHECK(s_out[0] == '\0', "and say nothing");

    send(&p, &env, "CAIRN-PROV BEGIN");

    /* Inside a session: both work and report their counts. */
    reset_io();
    s_drop_count = 7;
    s_drop_bytes = 4917420;
    send(&p, &env, "DROP legacy-bundles");
    CHECK(s_drops_called == 1, "DROP calls through once");
    CHECK(strstr(s_out, "OK dropped 7 bundle(s)") != NULL,
          "DROP reports how many bundles went");
    CHECK(strstr(s_out, "4917420") != NULL, "and how many bytes were freed");

    reset_io();
    s_bonds = 1;
    send(&p, &env, "CLEAR bonds");
    CHECK(s_bonds_called == 1, "CLEAR bonds calls through once");
    CHECK(strstr(s_out, "OK cleared 1 bond(s)") != NULL,
          "CLEAR bonds reports the count");

    /* A trip starting kills the session, so neither can run mid-drive. */
    reset_io();
    s_drops_called = 0;
    env.trip_active = true;
    send(&p, &env, "DROP legacy-bundles");
    CHECK(s_drops_called == 0, "DROP is refused once a trip is active");
    CHECK(strstr(s_out, "ERR aborted") != NULL, "and says why");
    env.trip_active = false;

    /* A failure is reported as one, not as success. */
    cairn_prov_init(&p, &OPS);
    send(&p, &env, "CAIRN-PROV BEGIN");
    reset_io();
    s_drop_fails = true;
    send(&p, &env, "DROP legacy-bundles");
    CHECK(strstr(s_out, "ERR") != NULL, "a failed DROP reports ERR");
    s_drop_fails = false;

    reset_io();
    s_bond_fails = true;
    send(&p, &env, "CLEAR bonds");
    CHECK(strstr(s_out, "ERR") != NULL, "a failed CLEAR reports ERR");
    s_bond_fails = false;

    /* A build without the ops must refuse rather than appear to succeed. */
    cairn_prov_t q;
    cairn_prov_init(&q, &OPS_MINIMAL);
    send(&q, &env, "CAIRN-PROV BEGIN");
    reset_io();
    send(&q, &env, "DROP legacy-bundles");
    CHECK(strstr(s_out, "ERR") != NULL,
          "a build that cannot drop bundles says so");
    reset_io();
    send(&q, &env, "CLEAR bonds");
    CHECK(strstr(s_out, "ERR") != NULL, "a build with no BLE says so");

    /* Neither verb is a prefix match: the exact argument is required. */
    cairn_prov_init(&p, &OPS);
    send(&p, &env, "CAIRN-PROV BEGIN");
    reset_io();
    s_drops_called = 0;
    send(&p, &env, "DROP");
    send(&p, &env, "DROP bundles");
    send(&p, &env, "DROP legacy-bundles extra");
    send(&p, &env, "CLEAR");
    send(&p, &env, "CLEAR all");
    CHECK(s_drops_called == 0, "a near-miss argument drops nothing");

    return true;
}

int main(void)
{
    static const row_t ROWS[] = {
        { "x25519 matches RFC 7748 (5.2 and 6.1)", row_x25519_rfc7748 },
        { "x25519 reports low-order points as failure", row_x25519_low_order },
        { "base64 is strict: one byte string, one spelling", row_base64 },
        { "the enrolment blob is byte-identical to the Go reference", row_blob_matches_go },
        { "window: unprovisioned always, provisioned only after boot, never in a trip", row_window_rules },
        { "a trip starting aborts and scrubs the session", row_trip_aborts_session },
        { "an idle session expires and is scrubbed", row_idle_timeout },
        { "every malformed line is refused and stages nothing", row_validation },
        { "Wi-Fi and client-certificate fields are refused, not obeyed", row_legacy_credential_fields_are_refused },
        { "COMMIT is all-or-nothing and convergent on retry", row_commit_rules },
        { "no secret is ever echoed or logged (raw or base64)", row_no_secret_leaks },
        { "legacy Wi-Fi and key slots are erased, idempotently, touching nothing else", row_legacy_slots_are_erased },
        { "DROP legacy-bundles and CLEAR bonds obey the session and report", row_drop_and_clear },
    };
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        bool ok = ROWS[i].fn();
        printf("  %s  [prov] %s\n", ok ? "pass" : "FAIL", ROWS[i].name);
        ok ? s_pass++ : s_fail++;
    }
    printf("provisioning: %d/%d passed\n", s_pass, s_pass + s_fail);
    return s_fail ? 1 : 0;
}
