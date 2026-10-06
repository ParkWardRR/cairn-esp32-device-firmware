/*
 * Check-in (contracts/ble/v1/checkin.md): signed instructions and the home trigger, against
 * the contract's vectors (the instruction_* and home_trigger blocks of
 * vectors/device-info/vectors.json) and against rows the vectors cannot hold: the rate limit,
 * the trip refusal, a failing store, the effects being called exactly when they should be, and
 * every single-bit change of a valid frame.
 *
 *   checkin_test <path to vectors.json>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_checkin.h"
#include "cairn_format.h"
#include "minijson.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [checkin] %s\n", name); }        \
        else      { g_fail++; printf("  FAIL  [checkin] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

static int hex_decode(const char *hex, uint8_t *out, size_t max)
{
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned b;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1) return -1;
        out[i] = (uint8_t)b;
    }
    return (int)(n / 2);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ── the world the instructions land in ───────────────────────────────────── */

typedef struct {
    bool     trip;
    bool     persist_fails;
    cairn_ci_status_t config_status;
    uint32_t persisted;
    int      persist_calls, upload_now, stop_trying, clear_stop, config_calls;
    uint16_t stop_hours;
    uint8_t  config_body[CAIRN_CI_MAX_CONFIG];
    size_t   config_len;
} world_t;

static bool w_trip(void *c) { return ((world_t *)c)->trip; }
static void w_upload(void *c) { ((world_t *)c)->upload_now++; }
static void w_stop(void *c, uint16_t h) { world_t *w = c; w->stop_trying++; w->stop_hours = h; }
static void w_clear(void *c) { ((world_t *)c)->clear_stop++; }
static cairn_ci_status_t w_config(void *c, const uint8_t *b, size_t n)
{
    world_t *w = c;
    w->config_calls++;
    w->config_len = n;
    if (n <= sizeof w->config_body) memcpy(w->config_body, b, n);
    return w->config_status;
}
static bool w_persist(void *c, uint32_t counter)
{
    world_t *w = c;
    w->persist_calls++;
    if (w->persist_fails) return false;
    w->persisted = counter;
    return true;
}

static cairn_checkin_ops_t ops_for(world_t *w, bool with_config)
{
    cairn_checkin_ops_t o;
    memset(&o, 0, sizeof o);
    o.ctx = w;
    o.trip_active = w_trip;
    o.upload_now = w_upload;
    o.stop_trying = w_stop;
    o.clear_stop = w_clear;
    o.config = with_config ? w_config : NULL;
    o.persist_floor = w_persist;
    return o;
}

static uint8_t g_key[32], g_seed[32], g_dev[16];

/* Build a signed frame the way the server does. */
static size_t make_frame(uint8_t *out, uint8_t type, uint32_t counter, const uint8_t *body, size_t body_len,
                         const uint8_t device_id[16])
{
    out[0] = 1; out[1] = type;
    out[2] = (uint8_t)body_len; out[3] = (uint8_t)(body_len >> 8);
    out[4] = (uint8_t)counter; out[5] = (uint8_t)(counter >> 8);
    out[6] = (uint8_t)(counter >> 16); out[7] = (uint8_t)(counter >> 24);
    if (body_len) memcpy(out + 8, body, body_len);

    uint8_t msg[15 + 16 + CAIRN_CI_MAX_FRAME];
    memcpy(msg, "CAIRN-INSTR-V1", 14);
    msg[14] = 0;
    memcpy(msg + 15, device_id, 16);
    memcpy(msg + 31, out, 8 + body_len);
    cairn_ed25519_sign(msg, 31 + 8 + body_len, g_seed, g_key, out + 8 + body_len);
    return 8 + body_len + 64;
}

static void fresh(cairn_checkin_t *c, uint32_t floor) { cairn_checkin_init(c, g_key, g_dev, floor); }

/* ── the contract's vectors ───────────────────────────────────────────────── */

static void test_instruction_vectors(const mj_doc_t *d, const mj_node_t *list)
{
    uint32_t now = 100000;
    for (size_t i = 0; i < mj_len(d, list); i++) {
        const mj_node_t *v = mj_at(d, list, i);
        const char *name = mj_str_or(mj_get(d, v, "name"), "?");
        uint8_t frame[1024], want[16], got[CAIRN_CI_RESULT_LEN];
        int n = hex_decode(mj_str_or(mj_get(d, v, "frame"), ""), frame, sizeof frame);
        int wn = hex_decode(mj_str_or(mj_get(d, v, "want_result"), ""), want, sizeof want);
        uint32_t floor_before = (uint32_t)mj_num_or(mj_get(d, v, "counter_floor_before"), 0);
        int want_status = (int)mj_num_or(mj_get(d, v, "want_status"), -1);
        uint32_t floor_after = (uint32_t)mj_num_or(mj_get(d, v, "want_counter_floor_after"), 0);

        world_t w;
        memset(&w, 0, sizeof w);
        w.config_status = CAIRN_CI_APPLIED;
        cairn_checkin_ops_t o = ops_for(&w, true);
        cairn_checkin_t c;
        fresh(&c, floor_before);
        now += 5000;                                   /* each vector is an independent situation */

        cairn_ci_status_t st = cairn_checkin_instruction(&c, &o, frame, (size_t)n, now, got);
        char label[220];
        snprintf(label, sizeof label, "%s: status %d, answer bytes and floor", name, want_status);
        CHECK(label, n >= 0 && wn == CAIRN_CI_RESULT_LEN && (int)st == want_status &&
                     memcmp(got, want, CAIRN_CI_RESULT_LEN) == 0 && c.counter_floor == floor_after);

        /* An instruction acts only when applied, and a refused one never reaches an effect. */
        int effects = w.upload_now + w.stop_trying + w.clear_stop + w.config_calls;
        snprintf(label, sizeof label, "%s: effects only if applied", name);
        CHECK(label, (st == CAIRN_CI_APPLIED) ? effects == 1 : effects == 0);
    }
}

static void test_home_vectors(const mj_doc_t *d, const mj_node_t *list)
{
    for (size_t i = 0; i < mj_len(d, list); i++) {
        const mj_node_t *v = mj_at(d, list, i);
        const char *name = mj_str_or(mj_get(d, v, "name"), "?");
        bool must_reject = strstr(mj_str_or(mj_get(d, v, "note"), ""), "must be rejected") != NULL;
        uint8_t f[16];
        int n = hex_decode(mj_str_or(mj_get(d, v, "hex"), ""), f, sizeof f);
        cairn_checkin_t c;
        fresh(&c, 0);
        cairn_ci_home_result_t r = cairn_checkin_home_trigger(&c, f, (size_t)n, 1000, false);
        char label[200];
        snprintf(label, sizeof label, "home trigger %s: %s", must_reject ? "refused" : "accepted", name);
        CHECK(label, must_reject ? r == CAIRN_CI_HOME_REFUSED : r == CAIRN_CI_HOME_ACCEPTED);
    }
}

/* ── synthetic rows ───────────────────────────────────────────────────────── */

static void test_effects_and_floor(void)
{
    world_t w;
    memset(&w, 0, sizeof w);
    w.config_status = CAIRN_CI_APPLIED;
    cairn_checkin_ops_t o = ops_for(&w, true);
    cairn_checkin_t c;
    fresh(&c, 0);
    uint8_t f[CAIRN_CI_MAX_FRAME], res[8];
    uint32_t now = 10000;

    size_t n = make_frame(f, CAIRN_CI_STOP_TRYING, 1, (const uint8_t[]){ 48, 0 }, 2, g_dev);
    CHECK("stop-trying 48 h applies", cairn_checkin_instruction(&c, &o, f, n, now, res) == CAIRN_CI_APPLIED);
    CHECK("with the hours passed on", w.stop_trying == 1 && w.stop_hours == 48);
    CHECK("the floor was made durable", w.persisted == 1 && c.counter_floor == 1);

    now += 3000;
    n = make_frame(f, CAIRN_CI_CLEAR_STOP, 2, NULL, 0, g_dev);
    CHECK("clear-stop applies", cairn_checkin_instruction(&c, &o, f, n, now, res) == CAIRN_CI_APPLIED && w.clear_stop == 1);

    now += 3000;
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 3, NULL, 0, g_dev);
    CHECK("upload-now applies", cairn_checkin_instruction(&c, &o, f, n, now, res) == CAIRN_CI_APPLIED && w.upload_now == 1);

    /* The signed body is handed to the configuration verifier unread. */
    uint8_t body[CAIRN_CI_MAX_CONFIG];
    for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 7 + 3);
    now += 3000;
    n = make_frame(f, CAIRN_CI_CONFIG, 4, body, sizeof body, g_dev);
    CHECK("a 440-byte config applies", cairn_checkin_instruction(&c, &o, f, n, now, res) == CAIRN_CI_APPLIED);
    CHECK("its body reached the verifier byte for byte", w.config_calls == 1 && w.config_len == sizeof body &&
                                                         memcmp(w.config_body, body, sizeof body) == 0);

    now += 3000;
    n = make_frame(f, CAIRN_CI_CONFIG, 5, body, sizeof body, g_dev);
    f[2] = (uint8_t)(sizeof body + 1); f[3] = (uint8_t)((sizeof body + 1) >> 8);
    CHECK("a 441-byte config claim is a bad length (signature is not even reached as valid)",
          cairn_checkin_instruction(&c, &o, f, n, now, res) == CAIRN_CI_BAD_LENGTH);

    uint8_t big[CAIRN_CI_MAX_CONFIG + 1];
    memset(big, 0x5A, sizeof big);
    n = make_frame(f, CAIRN_CI_CONFIG, 5, big, sizeof big, g_dev);
    now += 3000;
    CHECK("a correctly signed 441-byte config body is refused", cairn_checkin_instruction(&c, &o, f, n, now, res) == CAIRN_CI_BAD_LENGTH &&
                                                                 c.counter_floor == 4);
    n = make_frame(f, CAIRN_CI_CONFIG, 5, NULL, 0, g_dev);
    now += 3000;
    CHECK("a correctly signed empty config body is refused", cairn_checkin_instruction(&c, &o, f, n, now, res) == CAIRN_CI_BAD_LENGTH);
}

static void test_config_outcomes(void)
{
    uint8_t f[CAIRN_CI_MAX_FRAME], res[8], body[40];
    memset(body, 0x33, sizeof body);
    world_t w;
    cairn_checkin_t c;

    memset(&w, 0, sizeof w);
    w.config_status = CAIRN_CI_ENCRYPTION_REQUIRED;
    cairn_checkin_ops_t o = ops_for(&w, true);
    fresh(&c, 0);
    size_t n = make_frame(f, CAIRN_CI_CONFIG, 1, body, sizeof body, g_dev);
    CHECK("a config the verifier refuses for want of encryption answers ENCRYPTION_REQUIRED",
          cairn_checkin_instruction(&c, &o, f, n, 5000, res) == CAIRN_CI_ENCRYPTION_REQUIRED && res[0] == 7);
    CHECK("and does not move the floor", c.counter_floor == 0 && w.persist_calls == 0);

    cairn_checkin_ops_t none = ops_for(&w, false);
    fresh(&c, 0);
    CHECK("with no configuration verifier a config is an unknown type",
          cairn_checkin_instruction(&c, &none, f, n, 5000, res) == CAIRN_CI_UNKNOWN_TYPE);
}

static void test_trip_and_store(void)
{
    uint8_t f[CAIRN_CI_MAX_FRAME], res[8];
    world_t w;
    memset(&w, 0, sizeof w);
    cairn_checkin_ops_t o = ops_for(&w, true);
    cairn_checkin_t c;
    fresh(&c, 0);
    size_t n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 1, NULL, 0, g_dev);

    w.trip = true;
    CHECK("an instruction during a trip is refused", cairn_checkin_instruction(&c, &o, f, n, 5000, res) == CAIRN_CI_TRIP_ACTIVE);
    CHECK("no effect, floor unchanged", w.upload_now == 0 && c.counter_floor == 0 && res[4] == 0);
    w.trip = false;
    CHECK("the same frame applies after the drive", cairn_checkin_instruction(&c, &o, f, n, 9000, res) == CAIRN_CI_APPLIED);

    /* A store that fails: nothing took effect and the frame is not left replayable. */
    world_t v;
    memset(&v, 0, sizeof v);
    v.persist_fails = true;
    cairn_checkin_ops_t ov = ops_for(&v, true);
    cairn_checkin_t d;
    fresh(&d, 0);
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 1, NULL, 0, g_dev);
    CHECK("a floor that cannot be made durable is not applied", cairn_checkin_instruction(&d, &ov, f, n, 5000, res) == CAIRN_CI_STORE_FAILED);
    CHECK("the effect did not happen and the floor did not move", v.upload_now == 0 && d.counter_floor == 0);
    v.persist_fails = false;
    CHECK("it applies once the store works", cairn_checkin_instruction(&d, &ov, f, n, 9000, res) == CAIRN_CI_APPLIED && v.upload_now == 1);
}

static void test_ordering_of_checks(void)
{
    uint8_t f[CAIRN_CI_MAX_FRAME], res[8];
    world_t w;
    memset(&w, 0, sizeof w);
    cairn_checkin_ops_t o = ops_for(&w, true);
    cairn_checkin_t c;
    fresh(&c, 5);

    /* Bad signature AND unknown type AND replayed counter: only the signature is reported. */
    size_t n = make_frame(f, 0x7E, 1, NULL, 0, g_dev);
    f[n - 1] ^= 1;
    CHECK("the signature is judged before the type and the counter",
          cairn_checkin_instruction(&c, &o, f, n, 5000, res) == CAIRN_CI_BAD_SIGNATURE);

    /* Correctly signed, unknown type, and a replayed counter: the type is reported. */
    n = make_frame(f, 0x7E, 1, NULL, 0, g_dev);
    CHECK("then the type before the counter", cairn_checkin_instruction(&c, &o, f, n, 9000, res) == CAIRN_CI_UNKNOWN_TYPE);

    /* Version 2 is a bad length by the contract's own wording. */
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 9, NULL, 0, g_dev);
    f[0] = 2;
    CHECK("version 2 is refused as a bad length", cairn_checkin_instruction(&c, &o, f, n, 13000, res) == CAIRN_CI_BAD_LENGTH);

    /* A frame for another device. */
    uint8_t other[16];
    memset(other, 0x99, 16);
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 9, NULL, 0, other);
    CHECK("a frame signed for another device is a bad signature", cairn_checkin_instruction(&c, &o, f, n, 17000, res) == CAIRN_CI_BAD_SIGNATURE);

    /* A signature by another key. */
    uint8_t seed2[32], pub2[32];
    memset(seed2, 0x77, 32);
    cairn_ed25519_public_from_seed(seed2, pub2);
    cairn_checkin_t c2;
    cairn_checkin_init(&c2, pub2, g_dev, 5);
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 9, NULL, 0, g_dev);
    CHECK("a frame signed by a key that is not the pinned one is refused",
          cairn_checkin_instruction(&c2, &o, f, n, 21000, res) == CAIRN_CI_BAD_SIGNATURE);

    /* A short frame must never be read past its end (ASan watches this). */
    int sloppy = 0;
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 9, NULL, 0, g_dev);
    for (size_t cut = 0; cut < n; cut++)
        if (cairn_checkin_instruction(&c, &o, f, cut, 30000, res) == CAIRN_CI_APPLIED) sloppy++;
    CHECK("no proper prefix of a valid frame applies", sloppy == 0 && w.upload_now == 0);
}

/* The 512-byte limit is also what keeps the signed-bytes buffer in bounds: a frame over it
 * must be refused before anything is copied. Built to be self-consistent (the header's
 * body_len matches the length) so no other check refuses it first. Under ASan, removing the
 * guard turns these rows into a buffer overflow. */
static void test_oversize(void)
{
    static const size_t sizes[] = { 513, 520, 600, 1000, 4000 };
    uint8_t *f = malloc(4096);
    uint8_t res[8];
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t len = sizes[i], body = len - 72;
        memset(f, 0xA5, len);
        f[0] = 1; f[1] = CAIRN_CI_UPLOAD_NOW;
        f[2] = (uint8_t)body; f[3] = (uint8_t)(body >> 8);
        world_t w;
        memset(&w, 0, sizeof w);
        cairn_checkin_ops_t o = ops_for(&w, true);
        cairn_checkin_t c;
        fresh(&c, 0);
        char label[96];
        snprintf(label, sizeof label, "a self-consistent %zu-byte frame is refused as a bad length", len);
        CHECK(label, cairn_checkin_instruction(&c, &o, f, len, 5000, res) == CAIRN_CI_BAD_LENGTH);
    }
    free(f);
}

static void test_rate_limit(void)
{
    uint8_t f[CAIRN_CI_MAX_FRAME], res[8];
    world_t w;
    memset(&w, 0, sizeof w);
    cairn_checkin_ops_t o = ops_for(&w, true);
    cairn_checkin_t c;
    fresh(&c, 0);

    size_t n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 1, NULL, 0, g_dev);
    CHECK("the first applies", cairn_checkin_instruction(&c, &o, f, n, 100000, res) == CAIRN_CI_APPLIED);
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 2, NULL, 0, g_dev);
    CHECK("a second within 2 s is rate limited", cairn_checkin_instruction(&c, &o, f, n, 101500, res) == CAIRN_CI_RATE_LIMITED);
    CHECK("and did not move the floor or act", c.counter_floor == 1 && w.upload_now == 1);
    CHECK("the same frame passes once 2 s have gone by the first",
          cairn_checkin_instruction(&c, &o, f, n, 102000, res) == CAIRN_CI_APPLIED);

    /* A refused frame does not push the window out. */
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 3, NULL, 0, g_dev);
    cairn_checkin_instruction(&c, &o, f, n, 102500, res);                    /* rate limited */
    cairn_checkin_instruction(&c, &o, f, n, 103000, res);                    /* rate limited */
    CHECK("a flood of refused frames does not extend the lockout",
          cairn_checkin_instruction(&c, &o, f, n, 104000, res) == CAIRN_CI_APPLIED);

    /* Thirty an hour. */
    cairn_checkin_t d;
    fresh(&d, 0);
    uint32_t t = 1000000;
    int ok = 0;
    for (uint32_t k = 1; k <= 30; k++) {
        n = make_frame(f, CAIRN_CI_UPLOAD_NOW, k, NULL, 0, g_dev);
        if (cairn_checkin_instruction(&d, &o, f, n, t, res) == CAIRN_CI_APPLIED) ok++;
        t += 2100;
    }
    CHECK("thirty instructions in an hour apply", ok == 30);
    n = make_frame(f, CAIRN_CI_UPLOAD_NOW, 31, NULL, 0, g_dev);
    CHECK("the thirty-first is rate limited", cairn_checkin_instruction(&d, &o, f, n, t, res) == CAIRN_CI_RATE_LIMITED);
    CHECK("it applies in the next hour", cairn_checkin_instruction(&d, &o, f, n, 1000000 + 3600u * 1000u + 1, res) == CAIRN_CI_APPLIED);
}

static void test_every_bit(void)
{
    uint8_t f[CAIRN_CI_MAX_FRAME], bad[CAIRN_CI_MAX_FRAME], res[8];
    uint8_t body[2] = { 24, 0 };
    size_t n = make_frame(f, CAIRN_CI_STOP_TRYING, 7, body, 2, g_dev);

    int applied = 0, tried = 0;
    for (size_t i = 0; i < n; i++)
        for (int bit = 0; bit < 8; bit++) {
            memcpy(bad, f, n);
            bad[i] ^= (uint8_t)(1 << bit);
            world_t w;
            memset(&w, 0, sizeof w);
            cairn_checkin_ops_t o = ops_for(&w, true);
            cairn_checkin_t c;
            fresh(&c, 0);
            tried++;
            if (cairn_checkin_instruction(&c, &o, bad, n, 5000, res) == CAIRN_CI_APPLIED) applied++;
        }
    CHECK("no single-bit change of a signed frame applies", applied == 0 && tried == (int)n * 8);
}

static void test_home_rules(void)
{
    cairn_checkin_t c;
    fresh(&c, 0);
    uint8_t home600[6]  = { 1, 0, 0x58, 0x02, 1, 0 };     /* valid 600 s, seq 1 */
    uint8_t withdraw[6] = { 0, 0, 0, 0, 2, 0 };           /* seq 2 */

    CHECK("home for 600 s is accepted", cairn_checkin_home_trigger(&c, home600, 6, 100000, false) == CAIRN_CI_HOME_ACCEPTED);
    CHECK("and asserted", cairn_checkin_home_asserted(&c, 100001));
    CHECK("still asserted just before it expires", cairn_checkin_home_asserted(&c, 100000 + 599999));
    CHECK("it expires on its own after valid_seconds", !cairn_checkin_home_asserted(&c, 100000 + 600000));
    CHECK("and stays expired", !cairn_checkin_home_asserted(&c, 100000 + 1));

    cairn_checkin_t d;
    fresh(&d, 0);
    cairn_checkin_home_trigger(&d, home600, 6, 100000, false);
    CHECK("a repeat of the same seq is ignored", cairn_checkin_home_trigger(&d, home600, 6, 120000, false) == CAIRN_CI_HOME_IGNORED);
    CHECK("a second within 10 s is ignored", cairn_checkin_home_trigger(&d, withdraw, 6, 105000, false) == CAIRN_CI_HOME_IGNORED);
    CHECK("it did not withdraw anything", cairn_checkin_home_asserted(&d, 106000));
    CHECK("after 10 s the withdrawal is accepted", cairn_checkin_home_trigger(&d, withdraw, 6, 111000, false) == CAIRN_CI_HOME_ACCEPTED);
    CHECK("and removes the permission at once", !cairn_checkin_home_asserted(&d, 111001));

    uint8_t older[6] = { 1, 0, 0x58, 0x02, 1, 0 };
    CHECK("an older seq is ignored", cairn_checkin_home_trigger(&d, older, 6, 200000, false) == CAIRN_CI_HOME_IGNORED);

    cairn_checkin_t e;
    fresh(&e, 0);
    uint8_t top[6]  = { 1, 0, 0x58, 0x02, 0xFF, 0xFF };   /* seq 65535 */
    uint8_t wrap[6] = { 1, 0, 0x58, 0x02, 0x00, 0x00 };   /* seq 0 */
    cairn_checkin_home_trigger(&e, top, 6, 100000, false);
    CHECK("the sequence number wraps: 0 follows 65535", cairn_checkin_home_trigger(&e, wrap, 6, 120000, false) == CAIRN_CI_HOME_ACCEPTED);

    cairn_checkin_t g;
    fresh(&g, 0);
    CHECK("it is ignored during a trip", cairn_checkin_home_trigger(&g, home600, 6, 100000, true) == CAIRN_CI_HOME_IGNORED &&
                                          !cairn_checkin_home_asserted(&g, 100001));
    CHECK("a 5-byte frame is refused", cairn_checkin_home_trigger(&g, home600, 5, 100000, false) == CAIRN_CI_HOME_REFUSED);
    CHECK("a 7-byte frame is refused", cairn_checkin_home_trigger(&g, home600, 7, 100000, false) == CAIRN_CI_HOME_REFUSED);
    uint8_t max[6] = { 1, 0, 0x84, 0x03, 9, 0 };          /* 900 s */
    CHECK("900 s is the most allowed", cairn_checkin_home_trigger(&g, max, 6, 100000, false) == CAIRN_CI_HOME_ACCEPTED);

    cairn_checkin_t h;
    fresh(&h, 0);
    uint8_t zero[6] = { 1, 0, 0, 0, 1, 0 };               /* home with 0 seconds */
    cairn_checkin_home_trigger(&h, zero, 6, 100000, false);
    CHECK("home with zero seconds grants nothing", !cairn_checkin_home_asserted(&h, 100000));

    /* The trigger carries no state across a re-init: it is not held over a reboot. */
    cairn_checkin_t k;
    fresh(&k, 0);
    CHECK("a fresh start has no permission", !cairn_checkin_home_asserted(&k, 1));
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: checkin_test <vectors.json>\n"); return 2; }

    char *text = slurp(argv[1]);
    if (!text) { printf("  FAIL  [checkin] cannot read %s\n", argv[1]); return 1; }
    mj_doc_t *doc = malloc(sizeof *doc);
    if (!doc || !mj_parse(doc, text)) {
        printf("  FAIL  [checkin] cannot parse %s\n", argv[1]);
        return 1;
    }

    const mj_node_t *root = mj_root(doc);
    int a = hex_decode(mj_str_or(mj_get(doc, root, "instruction_public_key"), ""), g_key, 32);
    int b = hex_decode(mj_str_or(mj_get(doc, root, "instruction_seed"), ""), g_seed, 32);
    int c = hex_decode(mj_str_or(mj_get(doc, root, "device_id"), ""), g_dev, 16);
    CHECK("the vector file names the key, seed and device", a == 32 && b == 32 && c == 16);

    uint8_t derived[32];
    cairn_ed25519_public_from_seed(g_seed, derived);
    CHECK("the published seed derives the published key", memcmp(derived, g_key, 32) == 0);

    const mj_node_t *ins = mj_get(doc, root, "instruction");
    const mj_node_t *home = mj_get(doc, root, "home_trigger");
    CHECK("the vector file has instruction and home_trigger blocks", ins && home && mj_len(doc, ins) >= 14 && mj_len(doc, home) >= 4);
    if (ins && home) {
        test_instruction_vectors(doc, ins);
        test_home_vectors(doc, home);
    }

    test_effects_and_floor();
    test_config_outcomes();
    test_trip_and_store();
    test_ordering_of_checks();
    test_oversize();
    test_rate_limit();
    test_every_bit();
    test_home_rules();

    free(doc);
    free(text);
    printf("check-in: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
