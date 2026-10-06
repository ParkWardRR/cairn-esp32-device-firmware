/*
 * Configuration receiver: apply a sealed configuration message to the dongle.
 *
 * Wi-Fi networks, LTE limits and the upload schedule are set in the web UI and the iOS
 * app and reach the dongle later and indirectly: through the phone at a BLE check-in, or
 * in any authenticated uplink session (issue #22; upstream contracts/config/v1, #27).
 * This module is what the dongle does with such a message. Portable C, no I/O of its
 * own: the key-value store, the clock and the credential store are passed in, so the
 * whole thing runs on the host.
 *
 * STATUS: PROVISIONAL. contracts/config/v1 is not released (it is an open design issue),
 * so the envelope below is built from the primitives the enrolment contract already uses
 * and nothing new: X25519 + HKDF-SHA256 + XChaCha20-Poly1305 for the seal, Ed25519 for the
 * signature, deterministic CBOR for the payload. It is isolated in cairn_config_open() and
 * cairn_config_build() so that when the contract is released only those move; the policy
 * below (order of checks, replay, allow-list, atomic apply, rollback, the credential gate,
 * the audit trail, the reported-state hash) is the part that is meant to survive.
 * Open design points the contract has to answer are listed in docs/config-receiver.md.
 *
 * What it enforces, in this order, cheapest first, and before anything is applied:
 *
 *   1. size and shape; the message names THIS device;
 *   2. the signer is authorised (the pinned server key or an enrolled client: a callback);
 *   3. the Ed25519 signature over the whole sealed envelope verifies;
 *   4. the counter is strictly greater than the last applied one (replay and rollback);
 *   5. the seal opens (a tampered ciphertext or header fails the tag);
 *   6. every field is on the allow-list, appears once, and is within its sanity limits;
 *   7. a credential-class field is refused unless storage is encrypted AND a credential
 *      store accepts it (issue #18: nothing secret is written to flash before then);
 *   8. the new configuration is written to the inactive slot and only then becomes
 *      current, so a power cut leaves the previous good configuration in force.
 *
 * Either the whole message applies or none of it does. A rejected message does not
 * consume the counter. Every outcome, applied or refused, is entered in a small audit
 * trail that the reported state carries, and attempts are rate limited so the signature
 * check cannot be used to burn the battery.
 *
 * It never decides policy it is not told: the LTE caps it stores are enforced by
 * lib/cairn_usage, which also holds the compiled-in hard ceilings. The limits checked
 * here are sanity limits so a message cannot even ask for something absurd.
 */

#ifndef CAIRN_CONFIG_H
#define CAIRN_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── what a configuration holds ───────────────────────────────────────────── */

/* Settings that are not secret. These apply today. */
typedef struct {
    uint8_t  lte_enabled;           /* 0/1 */
    uint8_t  lte_mode;              /* 0 = digests only (default), 1 = whole bundles up to the cap */
    uint8_t  lte_roaming;           /* 0/1, off by default */
    uint8_t  lte_paused;            /* 0/1: a quick pause */
    uint8_t  billing_day;           /* 1..28 */
    uint8_t  alert_pct;             /* 1..100: report when usage crosses this */
    uint32_t lte_monthly_cap_kb;
    uint32_t lte_daily_cap_kb;
    uint32_t lte_trip_cap_kb;
    uint16_t wifi_slot_max_s;       /* the upload schedule knobs (issue #17) */
    uint8_t  wifi_slots_per_session;
    uint8_t  home_scan_enabled;     /* 0/1: the no-phone scan for a home network */
} cairn_config_settings_t;

/* The safe starting point: LTE off, digests only, roaming off, schedule defaults. */
void cairn_config_settings_defaults(cairn_config_settings_t *s);

/* Payload keys (deterministic CBOR map, ascending). Anything else is refused. */
enum {
    CAIRN_CFG_KEY_LTE_ENABLED   = 1,
    CAIRN_CFG_KEY_LTE_MODE      = 2,
    CAIRN_CFG_KEY_MONTHLY_KB    = 3,
    CAIRN_CFG_KEY_DAILY_KB      = 4,
    CAIRN_CFG_KEY_TRIP_KB       = 5,
    CAIRN_CFG_KEY_BILLING_DAY   = 6,
    CAIRN_CFG_KEY_ROAMING       = 7,
    CAIRN_CFG_KEY_PAUSED        = 8,
    CAIRN_CFG_KEY_ALERT_PCT     = 9,
    CAIRN_CFG_KEY_SLOT_MAX_S    = 10,
    CAIRN_CFG_KEY_SLOTS         = 11,
    CAIRN_CFG_KEY_HOME_SCAN     = 12,

    /* Credential class: secret, or revealing where the owner lives. */
    CAIRN_CFG_KEY_APN           = 32,   /* text */
    CAIRN_CFG_KEY_SIM_PIN       = 33,   /* text */
    CAIRN_CFG_KEY_WIFI_NETWORKS = 34    /* array of [ssid text, psk text] */
};

#define CAIRN_CFG_MAX_NETWORKS   8
#define CAIRN_CFG_SSID_MAX       32
#define CAIRN_CFG_PSK_MAX        63

/* The sanity limits a message must satisfy to apply at all. */
#define CAIRN_CFG_CAP_KB_MAX         (64u * 1024u * 1024u)   /* 64 GiB: nobody means more */
#define CAIRN_CFG_SLOT_MAX_S_MIN     5u
#define CAIRN_CFG_SLOT_MAX_S_MAX     300u
#define CAIRN_CFG_SLOTS_MAX          4u

typedef enum {
    CAIRN_CRED_APN = 1,
    CAIRN_CRED_SIM_PIN,
    CAIRN_CRED_WIFI_NETWORKS
} cairn_cred_kind_t;

/* ── results ──────────────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_CFG_OK = 0,
    CAIRN_CFG_RATE_LIMITED,
    CAIRN_CFG_MALFORMED,            /* wrong size, magic, version or payload shape */
    CAIRN_CFG_WRONG_DEVICE,
    CAIRN_CFG_UNAUTHORIZED_SIGNER,
    CAIRN_CFG_BAD_SIGNATURE,
    CAIRN_CFG_REPLAYED_COUNTER,
    CAIRN_CFG_BAD_SEAL,             /* the tag did not verify */
    CAIRN_CFG_UNKNOWN_FIELD,
    CAIRN_CFG_DUPLICATE_FIELD,
    CAIRN_CFG_OUT_OF_RANGE,
    CAIRN_CFG_CREDENTIAL_REFUSED,   /* storage is not encrypted, or no credential store */
    CAIRN_CFG_STORE_FAILED
} cairn_cfg_result_t;

const char *cairn_cfg_result_name(cairn_cfg_result_t r);

/* ── the world it runs in ─────────────────────────────────────────────────── */

typedef struct {
    void *ctx;

    /* Is `pub` allowed to configure this device: the pinned server key or an enrolled
     * client. The module never decides who is trusted. */
    bool (*signer_authorized)(void *ctx, const uint8_t pub[32]);

    /* True only once flash and NVS encryption are on (issue #7). Until then every
     * credential-class field is refused, whatever the message says. */
    bool (*storage_encrypted)(void *ctx);

    /* Hand a credential to encrypted storage. Called only when storage_encrypted() is true.
     * NULL means there is no credential store yet: the field is refused. `data` is the
     * field's canonical CBOR value bytes and is wiped by the caller afterwards. */
    bool (*store_credential)(void *ctx, cairn_cred_kind_t kind, const uint8_t *data, size_t len);

    /* Optional: a line for the log. Never carries a value, only field names and results. */
    void (*log)(void *ctx, const char *msg);
} cairn_config_ops_t;

/* ── state ────────────────────────────────────────────────────────────────── */

#define CAIRN_CFG_AUDIT_N 8

typedef struct {
    uint64_t counter;               /* of the message */
    uint8_t  signer_id[4];          /* first 4 bytes of SHA-256(signer public key) */
    uint8_t  result;                /* cairn_cfg_result_t */
    uint32_t at_ms;
} cairn_cfg_audit_t;

typedef struct {
    uint8_t  device_id[16];
    uint8_t  enc_scalar[32];        /* the device's X25519 key for opening sealed configs */
    uint8_t  enc_public[32];

    cairn_config_settings_t settings;   /* in force */
    cairn_config_settings_t previous;   /* the previous good one, for rollback */
    bool     have_previous;
    uint64_t counter;               /* last applied; only ever rises */
    uint32_t generation;            /* of the stored slot */

    /* rate limit: a short window of attempts, good or bad */
    uint32_t window_start_ms;
    uint16_t window_count;

    cairn_cfg_audit_t audit[CAIRN_CFG_AUDIT_N];
    uint8_t  audit_next;
    uint32_t refused_credentials;   /* how many credential fields were refused, ever */
    uint32_t rejected_total;
} cairn_config_t;

/* Derive the device's configuration key pair from its signing seed with its own
 * domain-separation label, so the signing key is never used for key agreement. */
bool cairn_config_derive_keys(const uint8_t seed[32], uint8_t scalar[32], uint8_t pub[32]);

/* Load the stored configuration (or defaults) and set the device identity. */
void cairn_config_init(cairn_config_t *c, const uint8_t device_id[16], const uint8_t enc_scalar[32],
                       const uint8_t enc_public[32]);

/*
 * Process one message. `scratch` holds the decrypted payload and is wiped before return;
 * it must be at least CAIRN_CFG_SCRATCH_MIN bytes. Returns CAIRN_CFG_OK only when the
 * whole message was applied and persisted.
 */
#define CAIRN_CFG_SCRATCH_MIN 1100
cairn_cfg_result_t cairn_config_receive(cairn_config_t *c, const cairn_config_ops_t *ops,
                                        const uint8_t *msg, size_t len, uint32_t now_ms,
                                        uint8_t *scratch, size_t scratch_cap);

/* Restore the previous good configuration. The counter is kept: it never goes back. */
bool cairn_config_rollback(cairn_config_t *c);

/* ── reported state ───────────────────────────────────────────────────────── */

typedef struct {
    cairn_config_settings_t settings;
    uint64_t counter;
    uint8_t  state_hash[32];        /* SHA-256 over the canonical settings and counter */
    bool     credentials_accepted;  /* false until storage is encrypted and a store exists */
    uint32_t refused_credentials;
    uint32_t rejected_total;
    cairn_cfg_audit_t audit[CAIRN_CFG_AUDIT_N];   /* oldest first */
    uint8_t  audit_count;
} cairn_config_report_t;

void cairn_config_report(const cairn_config_t *c, const cairn_config_ops_t *ops,
                         cairn_config_report_t *out);

/* ── the sender's half, for tests and for a Go cross-check later ──────────── */

/* Envelope: "CCFG" | ver(1)=0 | device_id(16) | signer_pub(32) | counter(8 LE) |
 *           eph_pub(32) | nonce(24) | ct_len(2 LE) | ciphertext | tag(16) | signature(64) */
#define CAIRN_CFG_HEADER_LEN    119
#define CAIRN_CFG_MAX_PAYLOAD   1024
#define CAIRN_CFG_MIN_MSG       (CAIRN_CFG_HEADER_LEN + 16 + 64)
#define CAIRN_CFG_MAX_MSG       (CAIRN_CFG_HEADER_LEN + CAIRN_CFG_MAX_PAYLOAD + 16 + 64)

/* Returns the message length, or 0 on failure. Deterministic in its inputs, like
 * cairn_enroll_build; production passes hardware randomness. */
size_t cairn_config_build(uint8_t *out, size_t cap, const uint8_t device_id[16],
                          const uint8_t device_enc_pub[32], const uint8_t signer_seed[32],
                          const uint8_t signer_pub[32], uint64_t counter,
                          const uint8_t *payload, size_t payload_len,
                          const uint8_t eph_priv[32], const uint8_t nonce[24]);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_CONFIG_H */
