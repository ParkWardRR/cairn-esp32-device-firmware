/*
 * Device enrolment and serial provisioning (docs/device-provisioning.md).
 *
 * Portable C with no Arduino dependency, so the whole protocol — its window
 * rules, its refusal to echo a secret, its all-or-nothing commit — is exercised
 * by host tests against exactly the code the device runs.
 *
 * Three jobs:
 *
 *   1. Seal the storage root to the server's enrolment key and sign the result
 *      (cairn_enroll_build). The blob is byte-identical to the Go reference in
 *      server/internal/enroll; fixtures/enroll-v1 pins that.
 *   2. Speak a small line protocol over the USB console so an operator's
 *      workstation can fetch that blob and install the credentials the device
 *      needs (cairn_prov_line).
 *   3. Keep those credentials in NVS in two slots flipped by one write, so a
 *      power cut mid-provisioning leaves the old set intact (cairn_prov_creds_*).
 *
 * What this does NOT do is protect the console from someone with the USB cable:
 * physical access is the trust boundary here until flash encryption and secure
 * boot (ROADMAP Phase 24) exist. The rules below narrow what such a person can
 * do — never mid-trip, only in a short window after boot once provisioned, never
 * logging or echoing a secret — they do not eliminate it, and the doc says so.
 */

#ifndef CAIRN_PROV_H
#define CAIRN_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── base64 (RFC 4648, standard alphabet, padding required) ───────────────── */

size_t cairn_b64_encoded_len(size_t n);
/* Writes len*4/3 chars plus a NUL; returns the length written. */
size_t cairn_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
/*
 * Strict: any character outside the alphabet, misplaced or missing padding,
 * whitespace, or non-zero padding bits is refused, so one byte string has one
 * text form. Returns the decoded length, or -1.
 */
int cairn_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

/* ── the sealed enrolment blob ────────────────────────────────────────────── */

#define CAIRN_ENROLL_BLOB_SIZE 225

/*
 * Build a version-1 enrolment blob. Deterministic in its inputs: production
 * passes hardware-random `eph_priv` and `nonce`; the host tests pass the values
 * from fixtures/enroll-v1 and compare bytes.
 *
 * Refuses (returns false) when `server_pub` is all zero — the unconfigured
 * placeholder — or when the key agreement yields a low-order result. Sealing a
 * root to a key nobody holds would "succeed" and lose it.
 */
bool cairn_enroll_build(uint8_t blob[CAIRN_ENROLL_BLOB_SIZE],
                        const uint8_t device_id[16], const uint8_t seed[32],
                        const uint8_t pub[32], uint32_t key_version,
                        const uint8_t root[32], const uint8_t server_pub[32],
                        const uint8_t eph_priv[32], const uint8_t nonce[24]);

/* First four bytes of SHA-256(public key) as 8 lowercase hex chars + NUL. */
void cairn_enroll_fingerprint(const uint8_t pub[32], char out[9]);

/* ── staged provisioning values ───────────────────────────────────────────── */

#define CAIRN_PROV_SSID_MAX  32
#define CAIRN_PROV_PASS_MAX  63
#define CAIRN_PROV_CERT_MAX  2048
#define CAIRN_PROV_KEY_MAX   1536
#define CAIRN_PROV_LINE_MAX  4096

typedef struct {
    bool     have_ssid, have_pass;
    uint8_t  ssid[CAIRN_PROV_SSID_MAX + 1];
    size_t   ssid_len;
    char     pass[CAIRN_PROV_PASS_MAX + 1];
    size_t   pass_len;

    char    *cert;                /* NUL-terminated PEM, heap, or NULL */
    size_t   cert_len;
    char    *key;
    size_t   key_len;

    bool     have_assignment;
    uint8_t  vehicle_id[16];
    uint8_t  assignment_id[16];

    bool     have_floor;
    uint64_t counter_floor;
} cairn_prov_staged_t;

/* ── the console protocol ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t uptime_ms;      /* since boot */
    bool     trip_active;
    bool     provisioned;    /* credentials already installed */
} cairn_prov_env_t;

/* How long after boot a provisioned device accepts provisioning. */
#define CAIRN_PROV_WINDOW_MS       60000u
/* A session with no traffic this long is abandoned and its values scrubbed. */
#define CAIRN_PROV_IDLE_TIMEOUT_MS 30000u

typedef struct {
    void *ctx;

    /* Identity for PROV-READY: device_id hex and the fingerprint. */
    void (*identity)(void *ctx, uint8_t device_id[16], char fingerprint[9]);

    /* The console text for ENROLL-BLOB (base64), or false if it cannot be built
     * (no server key pinned, RNG failure, ...). `err` explains, without secrets. */
    bool (*enroll_text)(void *ctx, char *out, size_t cap, const char **err);

    /*
     * Install the staged values. Returns NULL on success or a short, secret-free
     * reason. The credential set is replaced atomically; the assignment and the
     * counter floor are idempotent (the floor only ever rises), so a COMMIT that
     * reports failure is safe to simply send again.
     */
    const char *(*apply)(void *ctx, const cairn_prov_staged_t *staged);

    /* A line for the log. Never contains a value, only field names and events. */
    void (*log)(void *ctx, const char *msg);

    /* A line for the host. */
    void (*reply)(void *ctx, const char *line);
} cairn_prov_ops_t;

typedef struct {
    const cairn_prov_ops_t *ops;
    bool                    active;
    uint32_t                last_ms;
    cairn_prov_staged_t     st;
} cairn_prov_t;

void cairn_prov_init(cairn_prov_t *p, const cairn_prov_ops_t *ops);

/* Handle one complete line (no CR/LF). */
void cairn_prov_line(cairn_prov_t *p, const char *line, const cairn_prov_env_t *env);

/* Call periodically; drops an idle session and a session that outlived a trip start. */
void cairn_prov_tick(cairn_prov_t *p, const cairn_prov_env_t *env);

/* Scrub and release everything staged. */
void cairn_prov_abort(cairn_prov_t *p);

/* ── credential slots in NVS ──────────────────────────────────────────────── */

typedef struct {
    uint8_t ssid[CAIRN_PROV_SSID_MAX + 1];
    size_t  ssid_len;
    char    pass[CAIRN_PROV_PASS_MAX + 1];
    size_t  pass_len;
    char   *cert;   /* heap, NUL-terminated, or NULL */
    char   *key;
    bool    have_wifi;
    bool    have_tls;
} cairn_prov_creds_t;

/* Load the live slot. Returns false when nothing is provisioned. */
bool cairn_prov_creds_load(cairn_prov_creds_t *out);
void cairn_prov_creds_free(cairn_prov_creds_t *c);

/*
 * Replace the credential set with `staged` merged over what is live: a value not
 * staged is carried over. Written to the inactive slot, read back, and only then
 * made live by a single u32 write; the old slot is erased afterwards. A power
 * cut at any point leaves either the complete old set or the complete new one.
 */
bool cairn_prov_creds_apply(const cairn_prov_staged_t *staged);

bool cairn_prov_has_credentials(void);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_PROV_H */
