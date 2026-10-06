#include "cairn_config.h"

#include <string.h>

#include "cairn_format.h"
#include "cairn_kv.h"

/* ── small helpers ────────────────────────────────────────────────────────── */

/* Not dead-store eliminated: a volatile pointer is not. Used on anything decrypted. */
static void scrub(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void note(const cairn_config_ops_t *ops, const char *msg)
{
    if (ops && ops->log) ops->log(ops->ctx, msg);
}

const char *cairn_cfg_result_name(cairn_cfg_result_t r)
{
    switch (r) {
    case CAIRN_CFG_OK:                  return "ok";
    case CAIRN_CFG_RATE_LIMITED:        return "rate_limited";
    case CAIRN_CFG_MALFORMED:           return "malformed";
    case CAIRN_CFG_WRONG_DEVICE:        return "wrong_device";
    case CAIRN_CFG_UNAUTHORIZED_SIGNER: return "unauthorized_signer";
    case CAIRN_CFG_BAD_SIGNATURE:       return "bad_signature";
    case CAIRN_CFG_REPLAYED_COUNTER:    return "replayed_counter";
    case CAIRN_CFG_BAD_SEAL:            return "bad_seal";
    case CAIRN_CFG_UNKNOWN_FIELD:       return "unknown_field";
    case CAIRN_CFG_DUPLICATE_FIELD:     return "duplicate_field";
    case CAIRN_CFG_OUT_OF_RANGE:        return "out_of_range";
    case CAIRN_CFG_CREDENTIAL_REFUSED:  return "credential_refused";
    case CAIRN_CFG_STORE_FAILED:        return "store_failed";
    default:                            return "?";
    }
}

/* ── defaults ─────────────────────────────────────────────────────────────── */

void cairn_config_settings_defaults(cairn_config_settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->lte_enabled = 0;
    s->lte_mode = 0;               /* digests only */
    s->lte_roaming = 0;
    s->lte_paused = 0;
    s->billing_day = 1;
    s->alert_pct = 80;
    s->wifi_slot_max_s = 90;
    s->wifi_slots_per_session = 2;
    s->home_scan_enabled = 1;      /* the owner accepted the no-phone scan */
}

/* ── persistence: two slots, the newer valid one wins ─────────────────────── */

#define SLOT_MAGIC 0x43464731u   /* "CFG1" */

typedef struct {
    uint32_t magic;
    uint32_t generation;
    uint64_t counter;
    cairn_config_settings_t settings;
    cairn_config_settings_t previous;
    uint8_t  have_previous;
    uint8_t  pad[3];
    uint32_t crc;
} stored_t;

static const char *slot_key(uint32_t generation) { return (generation & 1u) ? "cfg_b" : "cfg_a"; }

static uint32_t stored_crc(const stored_t *s)
{
    return cairn_crc32((const uint8_t *)s, offsetof(stored_t, crc));
}

static bool load_slot(const char *key, stored_t *out)
{
    if (!cairn_kv_get_blob(key, out, sizeof *out)) return false;
    return out->magic == SLOT_MAGIC && out->crc == stored_crc(out);
}

static bool persist(const cairn_config_t *c, uint32_t generation, uint64_t counter,
                    const cairn_config_settings_t *settings,
                    const cairn_config_settings_t *previous, bool have_previous)
{
    stored_t s;
    memset(&s, 0, sizeof s);
    s.magic = SLOT_MAGIC;
    s.generation = generation;
    s.counter = counter;
    s.settings = *settings;
    if (previous) s.previous = *previous;
    s.have_previous = have_previous ? 1 : 0;
    s.crc = stored_crc(&s);
    (void)c;
    return cairn_kv_set_blob(slot_key(generation), &s, sizeof s);
}

void cairn_config_init(cairn_config_t *c, const uint8_t device_id[16], const uint8_t enc_scalar[32],
                       const uint8_t enc_public[32])
{
    memset(c, 0, sizeof *c);
    memcpy(c->device_id, device_id, 16);
    memcpy(c->enc_scalar, enc_scalar, 32);
    memcpy(c->enc_public, enc_public, 32);
    cairn_config_settings_defaults(&c->settings);
    c->previous = c->settings;

    stored_t a, b, *best = NULL;
    bool va = load_slot("cfg_a", &a), vb = load_slot("cfg_b", &b);
    if (va && vb) best = (a.generation >= b.generation) ? &a : &b;
    else if (va)  best = &a;
    else if (vb)  best = &b;

    if (best) {
        c->settings = best->settings;
        c->previous = best->previous;
        c->have_previous = best->have_previous != 0;
        c->counter = best->counter;
        c->generation = best->generation;
    }
}

/* ── keys and the seal ────────────────────────────────────────────────────── */

bool cairn_config_derive_keys(const uint8_t seed[32], uint8_t scalar[32], uint8_t pub[32])
{
    static const char info[] = "cairn/config-key/v1";
    if (!cairn_hkdf_sha256(NULL, 0, seed, 32, (const uint8_t *)info, sizeof info - 1, scalar, 32))
        return false;
    return cairn_x25519_public(pub, scalar);
}

static bool seal_key(uint8_t key[32], const uint8_t shared[32], const uint8_t eph_pub[32],
                     const uint8_t device_pub[32])
{
    static const char info[] = "cairn/config-seal/v1";
    uint8_t salt[64];
    memcpy(salt, eph_pub, 32);
    memcpy(salt + 32, device_pub, 32);
    return cairn_hkdf_sha256(salt, 64, shared, 32, (const uint8_t *)info, sizeof info - 1, key, 32);
}

size_t cairn_config_build(uint8_t *out, size_t cap, const uint8_t device_id[16],
                          const uint8_t device_enc_pub[32], const uint8_t signer_seed[32],
                          const uint8_t signer_pub[32], uint64_t counter,
                          const uint8_t *payload, size_t payload_len,
                          const uint8_t eph_priv[32], const uint8_t nonce[24])
{
    if (payload_len == 0 || payload_len > CAIRN_CFG_MAX_PAYLOAD) return 0;
    size_t total = CAIRN_CFG_HEADER_LEN + payload_len + 16 + 64;
    if (cap < total) return 0;

    uint8_t eph_pub[32], shared[32], key[32];
    size_t ret = 0;

    if (!cairn_x25519_public(eph_pub, eph_priv)) goto out;
    if (!cairn_x25519(shared, eph_priv, device_enc_pub)) goto out;
    if (!seal_key(key, shared, eph_pub, device_enc_pub)) goto out;

    memset(out, 0, total);
    memcpy(out, "CCFG", 4);
    out[4] = 0;
    memcpy(out + 5, device_id, 16);
    memcpy(out + 21, signer_pub, 32);
    put_le64(out + 53, counter);
    memcpy(out + 61, eph_pub, 32);
    memcpy(out + 93, nonce, 24);
    out[117] = (uint8_t)payload_len;
    out[118] = (uint8_t)(payload_len >> 8);

    /* The header is the AAD, so device, signer, counter and key cannot be edited
     * without failing the tag. */
    cairn_xchacha20poly1305_seal(key, nonce, out, CAIRN_CFG_HEADER_LEN, payload, payload_len,
                                 out + CAIRN_CFG_HEADER_LEN,
                                 out + CAIRN_CFG_HEADER_LEN + payload_len);
    cairn_ed25519_sign(out, total - 64, signer_seed, signer_pub, out + total - 64);
    ret = total;

out:
    scrub(shared, sizeof shared);
    scrub(key, sizeof key);
    return ret;
}

/* ── rate limit and audit ─────────────────────────────────────────────────── */

#define RATE_WINDOW_MS   60000u
#define RATE_MAX_IN_WIN  10u

static bool rate_ok(cairn_config_t *c, uint32_t now)
{
    if (c->window_count == 0 || (int32_t)(now - c->window_start_ms) >= (int32_t)RATE_WINDOW_MS) {
        c->window_start_ms = now;
        c->window_count = 0;
    }
    if (c->window_count >= RATE_MAX_IN_WIN) return false;
    c->window_count++;
    return true;
}

static void audit(cairn_config_t *c, uint64_t counter, const uint8_t signer_id[4],
                  cairn_cfg_result_t r, uint32_t now)
{
    cairn_cfg_audit_t *a = &c->audit[c->audit_next];
    a->counter = counter;
    if (signer_id) memcpy(a->signer_id, signer_id, 4); else memset(a->signer_id, 0, 4);
    a->result = (uint8_t)r;
    a->at_ms = now;
    c->audit_next = (uint8_t)((c->audit_next + 1) % CAIRN_CFG_AUDIT_N);
    if (r != CAIRN_CFG_OK) c->rejected_total++;
}

/* ── the payload ──────────────────────────────────────────────────────────── */

typedef struct {
    cairn_cred_kind_t kind;
    size_t off, len;            /* the field's CBOR bytes inside the decrypted payload */
} cred_span_t;

#define MAX_CREDS 3

typedef struct {
    cairn_config_settings_t next;
    cred_span_t creds[MAX_CREDS];
    size_t ncreds;
} parsed_t;

static bool read_u(cairn_cbor_dec_t *d, uint64_t max, uint64_t *v)
{
    if (cairn_cbor_uint_read(d, v) != CAIRN_OK) return false;
    return *v <= max;
}

static cairn_cfg_result_t parse_payload(const uint8_t *p, size_t len, const cairn_config_settings_t *cur,
                                        parsed_t *out)
{
    cairn_cbor_dec_t d;
    cairn_cbor_dec_init(&d, p, len);
    out->next = *cur;
    out->ncreds = 0;

    size_t n;
    if (cairn_cbor_map_header(&d, &n) != CAIRN_OK) return CAIRN_CFG_MALFORMED;
    if (n == 0 || n > 32) return CAIRN_CFG_MALFORMED;

    int64_t last = -1;
    for (size_t i = 0; i < n; i++) {
        uint64_t key, v;
        if (cairn_cbor_uint_read(&d, &key) != CAIRN_OK) return CAIRN_CFG_MALFORMED;
        if ((int64_t)key == last) return CAIRN_CFG_DUPLICATE_FIELD;
        if ((int64_t)key < last) return CAIRN_CFG_MALFORMED;      /* not canonical */
        last = (int64_t)key;

        switch (key) {
        case CAIRN_CFG_KEY_LTE_ENABLED:
            if (!read_u(&d, 1, &v)) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.lte_enabled = (uint8_t)v; break;
        case CAIRN_CFG_KEY_LTE_MODE:
            if (!read_u(&d, 1, &v)) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.lte_mode = (uint8_t)v; break;
        case CAIRN_CFG_KEY_MONTHLY_KB:
            if (!read_u(&d, CAIRN_CFG_CAP_KB_MAX, &v)) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.lte_monthly_cap_kb = (uint32_t)v; break;
        case CAIRN_CFG_KEY_DAILY_KB:
            if (!read_u(&d, CAIRN_CFG_CAP_KB_MAX, &v)) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.lte_daily_cap_kb = (uint32_t)v; break;
        case CAIRN_CFG_KEY_TRIP_KB:
            if (!read_u(&d, CAIRN_CFG_CAP_KB_MAX, &v)) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.lte_trip_cap_kb = (uint32_t)v; break;
        case CAIRN_CFG_KEY_BILLING_DAY:
            if (!read_u(&d, 28, &v) || v < 1) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.billing_day = (uint8_t)v; break;
        case CAIRN_CFG_KEY_ROAMING:
            if (!read_u(&d, 1, &v)) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.lte_roaming = (uint8_t)v; break;
        case CAIRN_CFG_KEY_PAUSED:
            if (!read_u(&d, 1, &v)) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.lte_paused = (uint8_t)v; break;
        case CAIRN_CFG_KEY_ALERT_PCT:
            if (!read_u(&d, 100, &v) || v < 1) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.alert_pct = (uint8_t)v; break;
        case CAIRN_CFG_KEY_SLOT_MAX_S:
            if (!read_u(&d, CAIRN_CFG_SLOT_MAX_S_MAX, &v) || v < CAIRN_CFG_SLOT_MAX_S_MIN)
                return CAIRN_CFG_OUT_OF_RANGE;
            out->next.wifi_slot_max_s = (uint16_t)v; break;
        case CAIRN_CFG_KEY_SLOTS:
            if (!read_u(&d, CAIRN_CFG_SLOTS_MAX, &v) || v < 1) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.wifi_slots_per_session = (uint8_t)v; break;
        case CAIRN_CFG_KEY_HOME_SCAN:
            if (!read_u(&d, 1, &v)) return CAIRN_CFG_OUT_OF_RANGE;
            out->next.home_scan_enabled = (uint8_t)v; break;

        case CAIRN_CFG_KEY_APN:
        case CAIRN_CFG_KEY_SIM_PIN: {
            char text[CAIRN_CFG_PSK_MAX + 1];
            size_t start = d.pos;
            if (cairn_cbor_text_read(&d, text, sizeof text) != CAIRN_OK) return CAIRN_CFG_MALFORMED;
            size_t tl = strlen(text);
            bool pin = (key == CAIRN_CFG_KEY_SIM_PIN);
            if (tl == 0 || tl > (pin ? 8u : 63u)) { scrub(text, sizeof text); return CAIRN_CFG_OUT_OF_RANGE; }
            scrub(text, sizeof text);
            if (out->ncreds >= MAX_CREDS) return CAIRN_CFG_MALFORMED;
            out->creds[out->ncreds].kind = pin ? CAIRN_CRED_SIM_PIN : CAIRN_CRED_APN;
            out->creds[out->ncreds].off = start;
            out->creds[out->ncreds].len = d.pos - start;
            out->ncreds++;
            break;
        }

        case CAIRN_CFG_KEY_WIFI_NETWORKS: {
            size_t start = d.pos, nn;
            if (cairn_cbor_array_header(&d, &nn) != CAIRN_OK) return CAIRN_CFG_MALFORMED;
            if (nn > CAIRN_CFG_MAX_NETWORKS) return CAIRN_CFG_OUT_OF_RANGE;
            for (size_t k = 0; k < nn; k++) {
                size_t pair;
                char ssid[CAIRN_CFG_SSID_MAX + 1], psk[CAIRN_CFG_PSK_MAX + 1];
                if (cairn_cbor_array_header(&d, &pair) != CAIRN_OK || pair != 2) return CAIRN_CFG_MALFORMED;
                cairn_err_t e1 = cairn_cbor_text_read(&d, ssid, sizeof ssid);
                cairn_err_t e2 = (e1 == CAIRN_OK) ? cairn_cbor_text_read(&d, psk, sizeof psk) : e1;
                size_t sl = strlen(ssid), pl = strlen(psk);
                scrub(psk, sizeof psk);
                scrub(ssid, sizeof ssid);
                if (e2 != CAIRN_OK) return CAIRN_CFG_OUT_OF_RANGE;
                /* No zero-length SSID; a passphrase is empty (open) or 8..63. */
                if (sl == 0) return CAIRN_CFG_OUT_OF_RANGE;
                if (pl != 0 && pl < 8) return CAIRN_CFG_OUT_OF_RANGE;
            }
            if (out->ncreds >= MAX_CREDS) return CAIRN_CFG_MALFORMED;
            out->creds[out->ncreds].kind = CAIRN_CRED_WIFI_NETWORKS;
            out->creds[out->ncreds].off = start;
            out->creds[out->ncreds].len = d.pos - start;
            out->ncreds++;
            break;
        }

        default:
            return CAIRN_CFG_UNKNOWN_FIELD;
        }
    }

    if (d.pos != len) return CAIRN_CFG_MALFORMED;      /* trailing bytes */

    /* Cross-field sanity: a per-trip cap above the monthly one asks for something the
     * monthly cap will never allow; refuse rather than guess which was meant. */
    if (out->next.lte_monthly_cap_kb && out->next.lte_daily_cap_kb &&
        out->next.lte_daily_cap_kb > out->next.lte_monthly_cap_kb)
        return CAIRN_CFG_OUT_OF_RANGE;
    if (out->next.lte_monthly_cap_kb && out->next.lte_trip_cap_kb &&
        out->next.lte_trip_cap_kb > out->next.lte_monthly_cap_kb)
        return CAIRN_CFG_OUT_OF_RANGE;
    return CAIRN_CFG_OK;
}

/* ── receive ──────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t counter;
    uint8_t  signer_id[4];
    bool     have_signer;
} info_t;

static cairn_cfg_result_t process(cairn_config_t *c, const cairn_config_ops_t *ops,
                                  const uint8_t *msg, size_t len, uint8_t *scratch,
                                  size_t scratch_cap, info_t *info)
{
    if (len < CAIRN_CFG_MIN_MSG || len > CAIRN_CFG_MAX_MSG) return CAIRN_CFG_MALFORMED;
    if (memcmp(msg, "CCFG", 4) != 0 || msg[4] != 0) return CAIRN_CFG_MALFORMED;
    if (scratch_cap < CAIRN_CFG_SCRATCH_MIN) return CAIRN_CFG_MALFORMED;

    size_t ct_len = (size_t)msg[117] | ((size_t)msg[118] << 8);
    if (ct_len == 0 || ct_len > CAIRN_CFG_MAX_PAYLOAD) return CAIRN_CFG_MALFORMED;
    if (len != CAIRN_CFG_HEADER_LEN + ct_len + 16 + 64) return CAIRN_CFG_MALFORMED;

    if (memcmp(msg + 5, c->device_id, 16) != 0) return CAIRN_CFG_WRONG_DEVICE;

    const uint8_t *signer = msg + 21;
    uint8_t digest[32];
    cairn_sha256(signer, 32, digest);
    memcpy(info->signer_id, digest, 4);
    info->have_signer = true;
    info->counter = get_le64(msg + 53);

    if (!ops->signer_authorized || !ops->signer_authorized(ops->ctx, signer))
        return CAIRN_CFG_UNAUTHORIZED_SIGNER;

    if (!cairn_ed25519_verify(msg, len - 64, msg + len - 64, signer)) return CAIRN_CFG_BAD_SIGNATURE;

    /* Strictly greater: an equal counter is a replay, and a lower one is a rollback. */
    if (info->counter <= c->counter) return CAIRN_CFG_REPLAYED_COUNTER;

    uint8_t shared[32], key[32];
    cairn_cfg_result_t r = CAIRN_CFG_BAD_SEAL;
    bool opened = false;

    if (cairn_x25519(shared, c->enc_scalar, msg + 61) &&
        seal_key(key, shared, msg + 61, c->enc_public) &&
        cairn_xchacha20poly1305_open(key, msg + 93, msg, CAIRN_CFG_HEADER_LEN,
                                     msg + CAIRN_CFG_HEADER_LEN, ct_len,
                                     msg + CAIRN_CFG_HEADER_LEN + ct_len, scratch)) {
        opened = true;
    }
    scrub(shared, sizeof shared);
    scrub(key, sizeof key);
    if (!opened) return r;

    parsed_t pr;
    r = parse_payload(scratch, ct_len, &c->settings, &pr);
    if (r != CAIRN_CFG_OK) goto done;

    /* The credential gate. Whole message or nothing: a message that carries a credential
     * the dongle may not hold is refused in full, settings included. */
    if (pr.ncreds > 0) {
        bool enc = ops->storage_encrypted && ops->storage_encrypted(ops->ctx);
        if (!enc || !ops->store_credential) {
            c->refused_credentials += (uint32_t)pr.ncreds;
            note(ops, "config: credential field refused, storage is not encrypted");
            r = CAIRN_CFG_CREDENTIAL_REFUSED;
            goto done;
        }
        for (size_t i = 0; i < pr.ncreds; i++) {
            if (!ops->store_credential(ops->ctx, pr.creds[i].kind, scratch + pr.creds[i].off,
                                       pr.creds[i].len)) {
                r = CAIRN_CFG_STORE_FAILED;
                goto done;
            }
        }
    }

    /* Apply: the inactive slot first, so a power cut leaves the previous good one in force. */
    {
        uint32_t gen = c->generation + 1;
        if (!persist(c, gen, info->counter, &pr.next, &c->settings, true)) {
            r = CAIRN_CFG_STORE_FAILED;
            goto done;
        }
        c->previous = c->settings;
        c->have_previous = true;
        c->settings = pr.next;
        c->counter = info->counter;
        c->generation = gen;
        r = CAIRN_CFG_OK;
    }

done:
    scrub(&pr, sizeof pr);
    scrub(scratch, ct_len);
    return r;
}

cairn_cfg_result_t cairn_config_receive(cairn_config_t *c, const cairn_config_ops_t *ops,
                                        const uint8_t *msg, size_t len, uint32_t now_ms,
                                        uint8_t *scratch, size_t scratch_cap)
{
    if (!rate_ok(c, now_ms)) {
        c->rejected_total++;
        return CAIRN_CFG_RATE_LIMITED;
    }

    info_t info;
    memset(&info, 0, sizeof info);
    cairn_cfg_result_t r = process(c, ops, msg, len, scratch, scratch_cap, &info);
    audit(c, info.counter, info.have_signer ? info.signer_id : NULL, r, now_ms);
    return r;
}

bool cairn_config_rollback(cairn_config_t *c)
{
    if (!c->have_previous) return false;
    uint32_t gen = c->generation + 1;
    /* The counter stays where it is: a rollback must not reopen the door to replays. */
    if (!persist(c, gen, c->counter, &c->previous, &c->settings, true)) return false;
    cairn_config_settings_t tmp = c->settings;
    c->settings = c->previous;
    c->previous = tmp;
    c->generation = gen;
    return true;
}

/* ── report ───────────────────────────────────────────────────────────────── */

static size_t encode_settings(const cairn_config_settings_t *s, uint8_t *buf, size_t cap)
{
    cairn_cbor_enc_t e;
    cairn_cbor_init(&e, buf, cap);
    cairn_cbor_map(&e, 12);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_LTE_ENABLED); cairn_cbor_uint(&e, s->lte_enabled);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_LTE_MODE);    cairn_cbor_uint(&e, s->lte_mode);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_MONTHLY_KB);  cairn_cbor_uint(&e, s->lte_monthly_cap_kb);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_DAILY_KB);    cairn_cbor_uint(&e, s->lte_daily_cap_kb);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_TRIP_KB);     cairn_cbor_uint(&e, s->lte_trip_cap_kb);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_BILLING_DAY); cairn_cbor_uint(&e, s->billing_day);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_ROAMING);     cairn_cbor_uint(&e, s->lte_roaming);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_PAUSED);      cairn_cbor_uint(&e, s->lte_paused);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_ALERT_PCT);   cairn_cbor_uint(&e, s->alert_pct);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_SLOT_MAX_S);  cairn_cbor_uint(&e, s->wifi_slot_max_s);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_SLOTS);       cairn_cbor_uint(&e, s->wifi_slots_per_session);
    cairn_cbor_key(&e, CAIRN_CFG_KEY_HOME_SCAN);   cairn_cbor_uint(&e, s->home_scan_enabled);
    return e.overflow ? 0 : e.len;
}

void cairn_config_report(const cairn_config_t *c, const cairn_config_ops_t *ops,
                         cairn_config_report_t *out)
{
    memset(out, 0, sizeof *out);
    out->settings = c->settings;
    out->counter = c->counter;

    static const char label[] = "cairn/config-state/v1";
    uint8_t buf[sizeof label - 1 + 160 + 8];
    memcpy(buf, label, sizeof label - 1);
    size_t n = encode_settings(&c->settings, buf + sizeof label - 1, 160);
    put_le64(buf + sizeof label - 1 + n, c->counter);
    cairn_sha256(buf, sizeof label - 1 + n + 8, out->state_hash);

    out->credentials_accepted = ops && ops->storage_encrypted && ops->storage_encrypted(ops->ctx) &&
                                ops->store_credential != NULL;
    out->refused_credentials = c->refused_credentials;
    out->rejected_total = c->rejected_total;

    /* Oldest first. Slots never written have result 0 and counter 0 and are skipped via
     * the next-slot walk below; track how many have been filled. */
    uint8_t count = 0;
    for (uint8_t i = 0; i < CAIRN_CFG_AUDIT_N; i++) {
        const cairn_cfg_audit_t *a = &c->audit[(c->audit_next + i) % CAIRN_CFG_AUDIT_N];
        if (a->at_ms == 0 && a->counter == 0 && a->result == 0) continue;
        out->audit[count++] = *a;
    }
    out->audit_count = count;
}
