/*
 * Configuration receiver (issue #22). The properties the issue names, plus the ones it
 * implies: a sealed config applies; a replayed counter, an unknown field and a tampered
 * message are refused; nothing secret is accepted before storage is encrypted; a power cut
 * leaves the previous good configuration in force; and attempts are rate limited.
 *
 * Every byte of a valid message is flipped, one at a time, and every prefix of it is
 * presented: each must be refused without changing the state, and (under ASan) without
 * reading out of bounds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_config.h"
#include "cairn_format.h"
#include "cairn_kv.h"

void cairn_kv_host_set_path(const char *path);

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [config] %s\n", name); }         \
        else      { g_fail++; printf("  FAIL  [config] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* ── fixtures ─────────────────────────────────────────────────────────────── */

static uint8_t g_dev_id[16];
static uint8_t g_dev_scalar[32], g_dev_pub[32];
static uint8_t g_srv_seed[32], g_srv_pub[32];       /* the authorised signer */
static uint8_t g_evil_seed[32], g_evil_pub[32];     /* a key that is not authorised */

static bool        g_encrypted;
static bool        g_have_store;
static int         g_stored_n;
static cairn_cred_kind_t g_stored_kind[4];
static uint8_t     g_stored_bytes[4][256];
static size_t      g_stored_len[4];
static bool        g_store_fails;

static bool ops_signer(void *ctx, const uint8_t pub[32]) { (void)ctx; return memcmp(pub, g_srv_pub, 32) == 0; }
static bool ops_encrypted(void *ctx) { (void)ctx; return g_encrypted; }
static bool ops_store(void *ctx, cairn_cred_kind_t k, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (g_store_fails) return false;
    if (g_stored_n < 4 && n <= sizeof g_stored_bytes[0]) {
        g_stored_kind[g_stored_n] = k;
        memcpy(g_stored_bytes[g_stored_n], d, n);
        g_stored_len[g_stored_n] = n;
        g_stored_n++;
    }
    return true;
}

static cairn_config_ops_t ops(void)
{
    cairn_config_ops_t o;
    memset(&o, 0, sizeof o);
    o.signer_authorized = ops_signer;
    o.storage_encrypted = ops_encrypted;
    o.store_credential = g_have_store ? ops_store : NULL;
    return o;
}

static const char *KV_PATH = "/tmp/cairn-config-test-kv.bin";

static void fresh(cairn_config_t *c)
{
    cairn_kv_end();
    remove(KV_PATH);
    cairn_kv_host_set_path(KV_PATH);
    cairn_kv_begin();
    g_encrypted = false;
    g_have_store = false;
    g_stored_n = 0;
    g_store_fails = false;
    cairn_config_init(c, g_dev_id, g_dev_scalar, g_dev_pub);
}

static void reboot(cairn_config_t *c)
{
    cairn_kv_end();
    cairn_kv_begin();
    cairn_config_init(c, g_dev_id, g_dev_scalar, g_dev_pub);
}

static uint8_t g_eph_n;

/* Build and send. `fields` is a ready CBOR payload. */
static size_t build(uint8_t *out, size_t cap, uint64_t counter, const uint8_t *payload, size_t plen,
                    const uint8_t seed[32], const uint8_t pub[32])
{
    uint8_t eph[32], nonce[24];
    memset(eph, 0x40 + ++g_eph_n, 32);
    memset(nonce, 0x80 + g_eph_n, 24);
    return cairn_config_build(out, cap, g_dev_id, g_dev_pub, seed, pub, counter, payload, plen, eph, nonce);
}

typedef struct { uint64_t key; uint64_t val; } kv_t;

static size_t payload_uints(uint8_t *buf, size_t cap, const kv_t *f, size_t n)
{
    cairn_cbor_enc_t e;
    cairn_cbor_init(&e, buf, cap);
    cairn_cbor_map(&e, n);
    for (size_t i = 0; i < n; i++) {
        cairn_cbor_key(&e, f[i].key);
        cairn_cbor_uint(&e, f[i].val);
    }
    return e.overflow ? 0 : e.len;
}

static uint8_t g_scratch[CAIRN_CFG_SCRATCH_MIN];
static uint32_t g_now = 100000;

static cairn_cfg_result_t send_uints(cairn_config_t *c, uint64_t counter, const kv_t *f, size_t n)
{
    uint8_t pl[256], msg[CAIRN_CFG_MAX_MSG];
    size_t pn = payload_uints(pl, sizeof pl, f, n);
    size_t mn = build(msg, sizeof msg, counter, pl, pn, g_srv_seed, g_srv_pub);
    cairn_config_ops_t o = ops();
    g_now += 7000;
    return cairn_config_receive(c, &o, msg, mn, g_now, g_scratch, sizeof g_scratch);
}

static bool same_settings(const cairn_config_settings_t *a, const cairn_config_settings_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/* ── tests ────────────────────────────────────────────────────────────────── */

static void test_applies(void)
{
    cairn_config_t c;
    fresh(&c);
    CHECK("it starts at the safe defaults: LTE off, digests only, roaming off",
          !c.settings.lte_enabled && c.settings.lte_mode == 0 && !c.settings.lte_roaming);

    const kv_t f[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 1}, {CAIRN_CFG_KEY_MONTHLY_KB, 500000},
                       {CAIRN_CFG_KEY_DAILY_KB, 50000}, {CAIRN_CFG_KEY_BILLING_DAY, 15},
                       {CAIRN_CFG_KEY_ALERT_PCT, 90} };
    cairn_cfg_result_t r = send_uints(&c, 1, f, 5);
    CHECK("a sealed, signed config applies", r == CAIRN_CFG_OK);
    CHECK("its fields are in force", c.settings.lte_enabled == 1 && c.settings.lte_monthly_cap_kb == 500000 &&
                                      c.settings.lte_daily_cap_kb == 50000 && c.settings.billing_day == 15 &&
                                      c.settings.alert_pct == 90);
    CHECK("fields it did not mention are unchanged", c.settings.lte_mode == 0 && c.settings.wifi_slots_per_session == 2);
    CHECK("the counter advanced", c.counter == 1);

    cairn_config_t after;
    reboot(&after);
    CHECK("it survives a reboot", same_settings(&after.settings, &c.settings) && after.counter == 1);
}

static void test_replay(void)
{
    cairn_config_t c;
    fresh(&c);
    uint8_t pl[64], msg[CAIRN_CFG_MAX_MSG];
    const kv_t f[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 1} };
    size_t pn = payload_uints(pl, sizeof pl, f, 1);
    size_t mn = build(msg, sizeof msg, 5, pl, pn, g_srv_seed, g_srv_pub);
    cairn_config_ops_t o = ops();

    CHECK("the first delivery applies", cairn_config_receive(&c, &o, msg, mn, 1000, g_scratch, sizeof g_scratch) == CAIRN_CFG_OK);
    const kv_t g[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 0} };
    send_uints(&c, 6, g, 1);
    CHECK("the setting was changed by a later message", c.settings.lte_enabled == 0);

    CHECK("the same message again is a replayed counter",
          cairn_config_receive(&c, &o, msg, mn, 20000, g_scratch, sizeof g_scratch) == CAIRN_CFG_REPLAYED_COUNTER);
    CHECK("and does not undo the later setting", c.settings.lte_enabled == 0 && c.counter == 6);

    CHECK("an equal counter is refused", send_uints(&c, 6, g, 1) == CAIRN_CFG_REPLAYED_COUNTER);
    CHECK("a lower counter is refused", send_uints(&c, 2, g, 1) == CAIRN_CFG_REPLAYED_COUNTER);

    cairn_config_t again;
    reboot(&again);
    cairn_config_ops_t o2 = ops();
    CHECK("a replay after a reboot is still refused",
          cairn_config_receive(&again, &o2, msg, mn, 40000, g_scratch, sizeof g_scratch) == CAIRN_CFG_REPLAYED_COUNTER);
}

static void test_fields(void)
{
    cairn_config_t c;
    fresh(&c);
    cairn_config_settings_t before = c.settings;

    /* An unknown field refuses the whole message, even next to a valid one. */
    const kv_t unk[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 1}, {99, 1} };
    CHECK("an unknown field is refused", send_uints(&c, 1, unk, 2) == CAIRN_CFG_UNKNOWN_FIELD);
    CHECK("and the valid field beside it is not applied", same_settings(&c.settings, &before) && c.counter == 0);

    {
        /* Hand-built: the encoder itself refuses to emit a repeated key. */
        uint8_t dup[] = { 0xA2, 0x01, 0x01, 0x01, 0x00 };
        uint8_t msg[CAIRN_CFG_MAX_MSG];
        size_t mn = build(msg, sizeof msg, 1, dup, sizeof dup, g_srv_seed, g_srv_pub);
        cairn_config_ops_t o = ops();
        g_now += 7000;
        CHECK("a duplicated field is refused",
              cairn_config_receive(&c, &o, msg, mn, g_now, g_scratch, sizeof g_scratch) == CAIRN_CFG_DUPLICATE_FIELD);
    }

    /* A key that the allow-list happens not to include but is close to one. */
    const kv_t near[] = { {13, 1} };
    CHECK("a key just past the allow-list is refused", send_uints(&c, 1, near, 1) == CAIRN_CFG_UNKNOWN_FIELD);

    struct { const char *name; kv_t f; } bad[] = {
        { "billing day 0",           {CAIRN_CFG_KEY_BILLING_DAY, 0} },
        { "billing day 29",          {CAIRN_CFG_KEY_BILLING_DAY, 29} },
        { "alert 0 percent",         {CAIRN_CFG_KEY_ALERT_PCT, 0} },
        { "alert 101 percent",       {CAIRN_CFG_KEY_ALERT_PCT, 101} },
        { "slot length 4 s",         {CAIRN_CFG_KEY_SLOT_MAX_S, 4} },
        { "slot length 301 s",       {CAIRN_CFG_KEY_SLOT_MAX_S, 301} },
        { "zero slots",              {CAIRN_CFG_KEY_SLOTS, 0} },
        { "five slots",              {CAIRN_CFG_KEY_SLOTS, 5} },
        { "an LTE mode of 2",        {CAIRN_CFG_KEY_LTE_MODE, 2} },
        { "a boolean of 2",          {CAIRN_CFG_KEY_ROAMING, 2} },
        { "an absurd monthly cap",   {CAIRN_CFG_KEY_MONTHLY_KB, (uint64_t)CAIRN_CFG_CAP_KB_MAX + 1} },
        { "a cap beyond 32 bits",    {CAIRN_CFG_KEY_DAILY_KB, 1ull << 33} },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s is out of range and not applied", bad[i].name);
        cairn_cfg_result_t r = send_uints(&c, 1, &bad[i].f, 1);
        CHECK(name, r == CAIRN_CFG_OUT_OF_RANGE && same_settings(&c.settings, &before));
    }

    const kv_t inv[] = { {CAIRN_CFG_KEY_MONTHLY_KB, 1000}, {CAIRN_CFG_KEY_DAILY_KB, 2000} };
    CHECK("a daily cap above the monthly one is refused", send_uints(&c, 1, inv, 2) == CAIRN_CFG_OUT_OF_RANGE);

    const kv_t ok[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 1} };
    CHECK("none of those refusals used up counter 1", send_uints(&c, 1, ok, 1) == CAIRN_CFG_OK);

    /* An empty map and trailing bytes. */
    uint8_t msg[CAIRN_CFG_MAX_MSG];
    uint8_t empty[] = { 0xA0 };
    size_t mn = build(msg, sizeof msg, 2, empty, sizeof empty, g_srv_seed, g_srv_pub);
    cairn_config_ops_t o = ops();
    g_now += 7000;
    CHECK("an empty configuration is refused",
          cairn_config_receive(&c, &o, msg, mn, g_now, g_scratch, sizeof g_scratch) == CAIRN_CFG_MALFORMED);
    uint8_t trailing[] = { 0xA1, 0x01, 0x01, 0x00 };
    mn = build(msg, sizeof msg, 2, trailing, sizeof trailing, g_srv_seed, g_srv_pub);
    g_now += 7000;
    CHECK("trailing bytes after the map are refused",
          cairn_config_receive(&c, &o, msg, mn, g_now, g_scratch, sizeof g_scratch) == CAIRN_CFG_MALFORMED);
    uint8_t unordered[] = { 0xA2, 0x02, 0x01, 0x01, 0x01 };
    mn = build(msg, sizeof msg, 2, unordered, sizeof unordered, g_srv_seed, g_srv_pub);
    g_now += 7000;
    CHECK("a non-canonical key order is refused",
          cairn_config_receive(&c, &o, msg, mn, g_now, g_scratch, sizeof g_scratch) == CAIRN_CFG_MALFORMED);
}

static void test_who(void)
{
    cairn_config_t c;
    fresh(&c);
    uint8_t pl[64], msg[CAIRN_CFG_MAX_MSG];
    const kv_t f[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 1} };
    size_t pn = payload_uints(pl, sizeof pl, f, 1);
    cairn_config_ops_t o = ops();

    size_t mn = build(msg, sizeof msg, 1, pl, pn, g_evil_seed, g_evil_pub);
    CHECK("a signer that is not authorised is refused",
          cairn_config_receive(&c, &o, msg, mn, 1000, g_scratch, sizeof g_scratch) == CAIRN_CFG_UNAUTHORIZED_SIGNER);

    /* Sealed for a different device. */
    uint8_t other_id[16];
    memset(other_id, 0x77, 16);
    uint8_t eph[32], nonce[24];
    memset(eph, 0x31, 32); memset(nonce, 0x32, 24);
    mn = cairn_config_build(msg, sizeof msg, other_id, g_dev_pub, g_srv_seed, g_srv_pub, 1, pl, pn, eph, nonce);
    CHECK("a message for another device is refused",
          cairn_config_receive(&c, &o, msg, mn, 9000, g_scratch, sizeof g_scratch) == CAIRN_CFG_WRONG_DEVICE);

    /* Sealed to a different device key but addressed to us: the seal does not open. */
    uint8_t s2[32], p2[32];
    uint8_t seed2[32];
    memset(seed2, 0x55, 32);
    cairn_config_derive_keys(seed2, s2, p2);
    mn = cairn_config_build(msg, sizeof msg, g_dev_id, p2, g_srv_seed, g_srv_pub, 1, pl, pn, eph, nonce);
    CHECK("a message sealed to another device's key does not open",
          cairn_config_receive(&c, &o, msg, mn, 20000, g_scratch, sizeof g_scratch) == CAIRN_CFG_BAD_SEAL);
    CHECK("none of that changed anything", c.counter == 0 && !c.settings.lte_enabled);
}

static void test_tamper_every_byte(void)
{
    cairn_config_t c;
    fresh(&c);
    uint8_t pl[64], msg[CAIRN_CFG_MAX_MSG], bad[CAIRN_CFG_MAX_MSG];
    const kv_t f[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 1}, {CAIRN_CFG_KEY_MONTHLY_KB, 123456} };
    size_t pn = payload_uints(pl, sizeof pl, f, 2);
    size_t mn = build(msg, sizeof msg, 1, pl, pn, g_srv_seed, g_srv_pub);
    cairn_config_ops_t o = ops();
    cairn_config_settings_t before = c.settings;

    int refused = 0, applied = 0;
    for (size_t i = 0; i < mn; i++) {
        memcpy(bad, msg, mn);
        bad[i] ^= 0x01;
        cairn_cfg_result_t r = cairn_config_receive(&c, &o, bad, mn, 1000000u + (uint32_t)i * 61000u,
                                                    g_scratch, sizeof g_scratch);
        if (r == CAIRN_CFG_OK) applied++; else refused++;
    }
    CHECK("flipping any single bit of a valid message is refused (every byte tried)",
          applied == 0 && refused == (int)mn);
    CHECK("and nothing changed", same_settings(&c.settings, &before) && c.counter == 0);

    /* Every prefix: a short read must never be accepted or read past its end. */
    int prefix_ok = 0;
    for (size_t n = 0; n < mn; n++) {
        cairn_cfg_result_t r = cairn_config_receive(&c, &o, msg, n, 90000000u + (uint32_t)n * 61000u,
                                                    g_scratch, sizeof g_scratch);
        if (r != CAIRN_CFG_OK) prefix_ok++;
    }
    CHECK("every truncation is refused", prefix_ok == (int)mn);

    /* The genuine message still applies afterwards. */
    CHECK("and the genuine message still applies",
          cairn_config_receive(&c, &o, msg, mn, 900000000u, g_scratch, sizeof g_scratch) == CAIRN_CFG_OK);

    /* An authorised signer cannot smuggle a changed ciphertext past the tag by re-signing. */
    cairn_config_t d;
    fresh(&d);
    memcpy(bad, msg, mn);
    bad[CAIRN_CFG_HEADER_LEN + 2] ^= 0x01;
    cairn_ed25519_sign(bad, mn - 64, g_srv_seed, g_srv_pub, bad + mn - 64);
    CHECK("a re-signed message with a changed ciphertext fails the seal",
          cairn_config_receive(&d, &o, bad, mn, 1000, g_scratch, sizeof g_scratch) == CAIRN_CFG_BAD_SEAL);
    memcpy(bad, msg, mn);
    bad[61] ^= 0x01;   /* the ephemeral key */
    cairn_ed25519_sign(bad, mn - 64, g_srv_seed, g_srv_pub, bad + mn - 64);
    CHECK("so does a re-signed message with a changed ephemeral key",
          cairn_config_receive(&d, &o, bad, mn, 100000, g_scratch, sizeof g_scratch) == CAIRN_CFG_BAD_SEAL);
    memcpy(bad, msg, mn);
    bad[54] ^= 0x01;   /* the counter (now 257) is in the AAD */
    cairn_ed25519_sign(bad, mn - 64, g_srv_seed, g_srv_pub, bad + mn - 64);
    CHECK("and a re-signed message with a changed counter (it is in the AAD)",
          cairn_config_receive(&d, &o, bad, mn, 200000, g_scratch, sizeof g_scratch) == CAIRN_CFG_BAD_SEAL);
}

static void test_credentials(void)
{
    cairn_config_t c;
    fresh(&c);
    uint8_t msg[CAIRN_CFG_MAX_MSG], pl[400];

    /* settings + a wifi network list */
    cairn_cbor_enc_t e;
    cairn_cbor_init(&e, pl, sizeof pl);
    cairn_cbor_map(&e, 2);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_LTE_ENABLED); cairn_cbor_uint(&e, 1);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_WIFI_NETWORKS);
    cairn_cbor_array(&e, 1);
    cairn_cbor_array(&e, 2);
    cairn_cbor_text(&e, "HomeNetwork-SSID");
    cairn_cbor_text(&e, "correct horse battery");
    size_t pn = e.len;
    size_t mn = build(msg, sizeof msg, 1, pl, pn, g_srv_seed, g_srv_pub);
    cairn_config_ops_t o = ops();

    CHECK("a credential is refused while storage is not encrypted",
          cairn_config_receive(&c, &o, msg, mn, 1000, g_scratch, sizeof g_scratch) == CAIRN_CFG_CREDENTIAL_REFUSED);
    CHECK("the whole message is refused: the non-secret setting beside it is not applied", !c.settings.lte_enabled);
    CHECK("nothing was handed to a credential store", g_stored_n == 0);
    CHECK("the refusal is counted", c.refused_credentials == 1);
    CHECK("and the counter was not used", c.counter == 0);

    cairn_config_report_t rep;
    cairn_config_report(&c, &o, &rep);
    CHECK("the reported state says credentials are not accepted", !rep.credentials_accepted && rep.refused_credentials == 1);

    g_encrypted = true;   /* encryption on, but no credential store yet */
    cairn_config_report(&c, &o, &rep);
    CHECK("encryption alone is not enough to claim credentials are accepted", !rep.credentials_accepted);
    CHECK("it is still refused without a store",
          cairn_config_receive(&c, &o, msg, mn, 20000, g_scratch, sizeof g_scratch) == CAIRN_CFG_CREDENTIAL_REFUSED);

    g_have_store = true;
    o = ops();
    cairn_config_report(&c, &o, &rep);
    CHECK("encryption and a store: credentials are accepted", rep.credentials_accepted);

    g_store_fails = true;
    CHECK("a store that fails fails the message and applies nothing",
          cairn_config_receive(&c, &o, msg, mn, 40000, g_scratch, sizeof g_scratch) == CAIRN_CFG_STORE_FAILED &&
          !c.settings.lte_enabled && c.counter == 0);
    g_store_fails = false;

    CHECK("with both, the message applies",
          cairn_config_receive(&c, &o, msg, mn, 60000, g_scratch, sizeof g_scratch) == CAIRN_CFG_OK &&
          c.settings.lte_enabled == 1);
    CHECK("the network list went to the credential store, once",
          g_stored_n == 1 && g_stored_kind[0] == CAIRN_CRED_WIFI_NETWORKS);

    /* The configuration's own slots must hold no secret and no network name. */
    uint8_t raw[512];
    bool leaked = false;
    const char *keys[] = { "cfg_a", "cfg_b" };
    for (int k = 0; k < 2; k++) {
        size_t n = 0;
        if (cairn_kv_get_blob_var(keys[k], raw, sizeof raw, &n) && n <= sizeof raw) {
            for (size_t i = 0; i + 8 < n; i++) {
                if (memcmp(raw + i, "HomeNetw", 8) == 0 || memcmp(raw + i, "correct ", 8) == 0) leaked = true;
            }
        }
    }
    CHECK("the persisted slots contain neither the SSID nor the passphrase", !leaked);

    /* APN and SIM PIN go the same way and are bounded. */
    cairn_cbor_init(&e, pl, sizeof pl);
    cairn_cbor_map(&e, 2);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_APN); cairn_cbor_text(&e, "iot.example");
    cairn_cbor_key(&e, CAIRN_CFG_KEY_SIM_PIN); cairn_cbor_text(&e, "1234");
    mn = build(msg, sizeof msg, 2, pl, e.len, g_srv_seed, g_srv_pub);
    g_stored_n = 0;
    CHECK("an APN and a SIM PIN are accepted when allowed",
          cairn_config_receive(&c, &o, msg, mn, 80000, g_scratch, sizeof g_scratch) == CAIRN_CFG_OK && g_stored_n == 2 &&
          g_stored_kind[0] == CAIRN_CRED_APN && g_stored_kind[1] == CAIRN_CRED_SIM_PIN);

    cairn_cbor_init(&e, pl, sizeof pl);
    cairn_cbor_map(&e, 1);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_SIM_PIN); cairn_cbor_text(&e, "123456789");
    mn = build(msg, sizeof msg, 3, pl, e.len, g_srv_seed, g_srv_pub);
    CHECK("a SIM PIN longer than 8 is refused",
          cairn_config_receive(&c, &o, msg, mn, 100000, g_scratch, sizeof g_scratch) == CAIRN_CFG_OUT_OF_RANGE);

    cairn_cbor_init(&e, pl, sizeof pl);
    cairn_cbor_map(&e, 1);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_WIFI_NETWORKS);
    cairn_cbor_array(&e, 1); cairn_cbor_array(&e, 2); cairn_cbor_text(&e, ""); cairn_cbor_text(&e, "longenoughpass");
    mn = build(msg, sizeof msg, 3, pl, e.len, g_srv_seed, g_srv_pub);
    CHECK("a zero-length SSID is refused",
          cairn_config_receive(&c, &o, msg, mn, 120000, g_scratch, sizeof g_scratch) == CAIRN_CFG_OUT_OF_RANGE);

    cairn_cbor_init(&e, pl, sizeof pl);
    cairn_cbor_map(&e, 1);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_WIFI_NETWORKS);
    cairn_cbor_array(&e, 1); cairn_cbor_array(&e, 2); cairn_cbor_text(&e, "net"); cairn_cbor_text(&e, "short");
    mn = build(msg, sizeof msg, 3, pl, e.len, g_srv_seed, g_srv_pub);
    CHECK("a passphrase between 1 and 7 characters is refused",
          cairn_config_receive(&c, &o, msg, mn, 140000, g_scratch, sizeof g_scratch) == CAIRN_CFG_OUT_OF_RANGE);
}

static void test_power_cut_and_rollback(void)
{
    cairn_config_t c;
    fresh(&c);
    const kv_t a[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 1}, {CAIRN_CFG_KEY_BILLING_DAY, 10} };
    const kv_t b[] = { {CAIRN_CFG_KEY_BILLING_DAY, 20} };
    CHECK("config 1", send_uints(&c, 1, a, 2) == CAIRN_CFG_OK);
    CHECK("config 2", send_uints(&c, 2, b, 1) == CAIRN_CFG_OK && c.settings.billing_day == 20);
    CHECK("the previous good one is kept", c.have_previous && c.previous.billing_day == 10);

    /* A power cut while writing the third: the slot it was writing holds garbage. */
    uint32_t next_gen = c.generation + 1;
    const char *key = (next_gen & 1u) ? "cfg_b" : "cfg_a";
    uint8_t junk[256];
    memset(junk, 0xEE, sizeof junk);
    cairn_kv_set_blob(key, junk, 200);                    /* a torn write: wrong length */
    cairn_config_t r1;
    reboot(&r1);
    CHECK("after a torn write the device comes up on the last good configuration",
          r1.settings.billing_day == 20 && r1.counter == 2);

    /* Same, with a full-length slot whose checksum is wrong. */
    uint8_t whole[256];
    size_t n = 0;
    cairn_kv_get_blob_var((next_gen & 1u) ? "cfg_a" : "cfg_b", whole, sizeof whole, &n);
    whole[n / 2] ^= 0xFF;
    cairn_kv_set_blob(key, whole, n);
    cairn_config_t r2;
    reboot(&r2);
    CHECK("a slot that fails its checksum is not used", r2.settings.billing_day == 20 && r2.counter == 2);

    /* A write that fails leaves the old one and the counter. */
    cairn_config_t d;
    fresh(&d);
    CHECK("config 1 again", send_uints(&d, 1, a, 2) == CAIRN_CFG_OK);
    char k[16];
    for (int i = 0; i < 80; i++) { snprintf(k, sizeof k, "junk%02d", i); if (!cairn_kv_set_blob(k, "x", 1)) break; }
    cairn_config_settings_t before = d.settings;
    CHECK("when the store is full the write fails and nothing is applied",
          send_uints(&d, 2, b, 1) == CAIRN_CFG_STORE_FAILED && same_settings(&d.settings, &before) && d.counter == 1);

    /* Rollback. */
    cairn_config_t e;
    fresh(&e);
    send_uints(&e, 1, a, 2);
    send_uints(&e, 2, b, 1);
    CHECK("rollback restores the previous configuration", cairn_config_rollback(&e) && e.settings.billing_day == 10);
    CHECK("but not the counter: it never goes back", e.counter == 2);
    CHECK("so the rolled-back message cannot be replayed", send_uints(&e, 2, b, 1) == CAIRN_CFG_REPLAYED_COUNTER);
    cairn_config_t f;
    reboot(&f);
    CHECK("a rollback survives a reboot", f.settings.billing_day == 10 && f.counter == 2);
    CHECK("a device with no history cannot roll back", ({ cairn_config_t z; fresh(&z); !cairn_config_rollback(&z); }));
}

static void test_rate_limit(void)
{
    cairn_config_t c;
    fresh(&c);
    uint8_t pl[64], msg[CAIRN_CFG_MAX_MSG], bad[CAIRN_CFG_MAX_MSG];
    const kv_t f[] = { {CAIRN_CFG_KEY_LTE_ENABLED, 1} };
    size_t pn = payload_uints(pl, sizeof pl, f, 1);
    size_t mn = build(msg, sizeof msg, 1, pl, pn, g_srv_seed, g_srv_pub);
    memcpy(bad, msg, mn);
    bad[mn - 1] ^= 1;
    cairn_config_ops_t o = ops();

    int limited = 0;
    for (int i = 0; i < 14; i++)
        if (cairn_config_receive(&c, &o, bad, mn, 5000 + (uint32_t)i * 100, g_scratch, sizeof g_scratch) == CAIRN_CFG_RATE_LIMITED)
            limited++;
    CHECK("a burst of bad messages is rate limited", limited == 4);
    CHECK("including a genuine one inside the window",
          cairn_config_receive(&c, &o, msg, mn, 7000, g_scratch, sizeof g_scratch) == CAIRN_CFG_RATE_LIMITED);
    CHECK("and the genuine one applies once the window passes",
          cairn_config_receive(&c, &o, msg, mn, 5000 + 61000, g_scratch, sizeof g_scratch) == CAIRN_CFG_OK);
}

static void test_report_and_audit(void)
{
    cairn_config_t c;
    fresh(&c);
    cairn_config_ops_t o = ops();
    cairn_config_report_t r0, r1, r2;
    cairn_config_report(&c, &o, &r0);

    const kv_t a[] = { {CAIRN_CFG_KEY_BILLING_DAY, 12} };
    send_uints(&c, 1, a, 1);
    cairn_config_report(&c, &o, &r1);
    CHECK("the state hash changes when the configuration does", memcmp(r0.state_hash, r1.state_hash, 32) != 0);

    cairn_config_t d;
    fresh(&d);
    send_uints(&d, 1, a, 1);
    cairn_config_report(&d, &o, &r2);
    CHECK("two devices with the same configuration and counter report the same hash",
          memcmp(r1.state_hash, r2.state_hash, 32) == 0);

    const kv_t same[] = { {CAIRN_CFG_KEY_BILLING_DAY, 12} };
    send_uints(&d, 2, same, 1);
    cairn_config_report(&d, &o, &r2);
    CHECK("the counter is part of the hash", memcmp(r1.state_hash, r2.state_hash, 32) != 0);

    cairn_config_t e;
    fresh(&e);
    const kv_t unk[] = { {99, 1} };
    send_uints(&e, 1, unk, 1);
    send_uints(&e, 1, a, 1);
    send_uints(&e, 1, a, 1);        /* replay */
    cairn_config_report(&e, &o, &r0);
    CHECK("the audit trail records refusals and applications, oldest first",
          r0.audit_count == 3 && r0.audit[0].result == CAIRN_CFG_UNKNOWN_FIELD &&
          r0.audit[1].result == CAIRN_CFG_OK && r0.audit[2].result == CAIRN_CFG_REPLAYED_COUNTER);
    CHECK("each entry names the signer by a short id, not the key",
          r0.audit[1].signer_id[0] || r0.audit[1].signer_id[1] || r0.audit[1].signer_id[2] || r0.audit[1].signer_id[3]);
    CHECK("rejections are counted", r0.rejected_total == 2);

    for (uint64_t i = 2; i < 14; i++) send_uints(&e, i, a, 1);
    cairn_config_report(&e, &o, &r0);
    CHECK("the trail is a ring of the last eight", r0.audit_count == CAIRN_CFG_AUDIT_N &&
          r0.audit[CAIRN_CFG_AUDIT_N - 1].counter == 13);
}

static void test_scratch_wiped(void)
{
    cairn_config_t c;
    fresh(&c);
    memset(g_scratch, 0xAA, sizeof g_scratch);
    const kv_t a[] = { {CAIRN_CFG_KEY_MONTHLY_KB, 77777}, {CAIRN_CFG_KEY_BILLING_DAY, 12} };
    CHECK("applies", send_uints(&c, 1, a, 2) == CAIRN_CFG_OK);
    bool dirty = false;
    for (size_t i = 0; i < 16; i++) if (g_scratch[i] != 0 && g_scratch[i] != 0xAA) dirty = true;
    CHECK("the decrypted payload does not stay in the caller's scratch", !dirty);
}

static void test_keys(void)
{
    uint8_t seed[32], s1[32], p1[32], s2[32], p2[32], edpub[32];
    memset(seed, 0x09, 32);
    CHECK("key derivation works", cairn_config_derive_keys(seed, s1, p1) && cairn_config_derive_keys(seed, s2, p2));
    CHECK("it is deterministic", memcmp(s1, s2, 32) == 0 && memcmp(p1, p2, 32) == 0);
    cairn_ed25519_public_from_seed(seed, edpub);
    CHECK("the key-agreement key is not the signing key", memcmp(p1, edpub, 32) != 0 && memcmp(s1, seed, 32) != 0);
    cairn_config_t c;
    fresh(&c);
    CHECK("a message too short for the scratch is refused, not overrun",
          ({ uint8_t tiny[10]; uint8_t pl[16], msg[CAIRN_CFG_MAX_MSG];
             const kv_t f[] = { {CAIRN_CFG_KEY_BILLING_DAY, 3} };
             size_t pn = payload_uints(pl, sizeof pl, f, 1);
             size_t mn = build(msg, sizeof msg, 1, pl, pn, g_srv_seed, g_srv_pub);
             cairn_config_ops_t o = ops();
             cairn_config_receive(&c, &o, msg, mn, 5, tiny, sizeof tiny) == CAIRN_CFG_MALFORMED; }));
}

int main(void)
{
    memset(g_dev_id, 0xD1, 16);
    uint8_t dev_seed[32];
    memset(dev_seed, 0x11, 32);
    cairn_config_derive_keys(dev_seed, g_dev_scalar, g_dev_pub);
    memset(g_srv_seed, 0x22, 32);
    cairn_ed25519_public_from_seed(g_srv_seed, g_srv_pub);
    memset(g_evil_seed, 0x33, 32);
    cairn_ed25519_public_from_seed(g_evil_seed, g_evil_pub);

    test_applies();
    test_replay();
    test_fields();
    test_who();
    test_tamper_every_byte();
    test_credentials();
    test_power_cut_and_rollback();
    test_rate_limit();
    test_report_and_audit();
    test_scratch_wiped();
    test_keys();

    cairn_kv_end();
    remove(KV_PATH);
    printf("config receiver: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
