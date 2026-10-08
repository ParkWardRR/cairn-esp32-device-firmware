/* See cairn_prov.h. */

#include "cairn_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cairn_format/cairn_format.h"

/* ── scrubbing ────────────────────────────────────────────────────────────── */

/* A plain memset of a buffer that is about to be freed or go out of scope is
 * dead-store eliminated; a volatile pointer is not. Secrets are scrubbed. */
static void scrub(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* ── base64 ───────────────────────────────────────────────────────────────── */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t cairn_b64_encoded_len(size_t n) { return ((n + 2) / 3) * 4; }

size_t cairn_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t need = cairn_b64_encoded_len(n);
    if (cap < need + 1) return 0;

    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int cairn_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap)
{
    if (n % 4 != 0) return -1;
    size_t o = 0;

    for (size_t i = 0; i < n; i += 4) {
        int a = b64val(in[i]), b = b64val(in[i + 1]);
        if (a < 0 || b < 0) return -1;

        bool last = (i + 4 == n);
        int pad = 0;
        int c, d;
        if (in[i + 2] == '=') {
            /* "xx==": padding is only legal in the final quad, and only as ==. */
            if (!last || in[i + 3] != '=') return -1;
            pad = 2;
            c = d = 0;
            if (b & 15) return -1; /* the unused low bits must be zero */
        } else {
            c = b64val(in[i + 2]);
            if (c < 0) return -1;
            if (in[i + 3] == '=') {
                if (!last) return -1;
                pad = 1;
                d = 0;
                if (c & 3) return -1;
            } else {
                d = b64val(in[i + 3]);
                if (d < 0) return -1;
            }
        }

        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        size_t take = 3 - (size_t)pad;
        if (o + take > cap) return -1;
        out[o++] = (uint8_t)(v >> 16);
        if (take > 1) out[o++] = (uint8_t)(v >> 8);
        if (take > 2) out[o++] = (uint8_t)v;
    }
    return (int)o;
}

/* ── the sealed enrolment blob ────────────────────────────────────────────── */

#define OFF_PUB   21
#define OFF_KVER  53
#define OFF_EPH   57
#define OFF_NONCE 89
#define OFF_CT    113
#define OFF_TAG   145
#define OFF_SIG   161

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= p[i];
    return acc == 0;
}

void cairn_enroll_fingerprint(const uint8_t pub[32], char out[9])
{
    uint8_t h[32];
    cairn_sha256(pub, 32, h);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        out[2 * i] = hex[h[i] >> 4];
        out[2 * i + 1] = hex[h[i] & 15];
    }
    out[8] = '\0';
}

bool cairn_enroll_build(uint8_t blob[CAIRN_ENROLL_BLOB_SIZE],
                        const uint8_t device_id[16], const uint8_t seed[32],
                        const uint8_t pub[32], uint32_t key_version,
                        const uint8_t root[32], const uint8_t server_pub[32],
                        const uint8_t eph_priv[32], const uint8_t nonce[24])
{
    if (key_version == 0 || all_zero(server_pub, 32) || all_zero(root, 32)) return false;

    uint8_t eph_pub[32], shared[32], key[32], salt[64];
    bool ok = false;

    if (!cairn_x25519_public(eph_pub, eph_priv)) goto out;
    if (!cairn_x25519(shared, eph_priv, server_pub)) goto out;

    memcpy(salt, eph_pub, 32);
    memcpy(salt + 32, server_pub, 32);
    static const char info[] = "cairn/enroll-seal/v1";
    if (!cairn_hkdf_sha256(salt, 64, shared, 32, (const uint8_t *)info, sizeof(info) - 1, key, 32))
        goto out;

    memset(blob, 0, CAIRN_ENROLL_BLOB_SIZE);
    memcpy(blob, "CENR", 4);
    blob[4] = 1;
    memcpy(blob + 5, device_id, 16);
    memcpy(blob + OFF_PUB, pub, 32);
    for (int i = 0; i < 4; i++) blob[OFF_KVER + i] = (uint8_t)(key_version >> (8 * i));
    memcpy(blob + OFF_EPH, eph_pub, 32);
    memcpy(blob + OFF_NONCE, nonce, 24);

    /* The AAD is every header byte before the ciphertext, so identity, key and
     * key version cannot be edited without failing the tag. */
    cairn_xchacha20poly1305_seal(key, nonce, blob, OFF_CT, root, 32, blob + OFF_CT, blob + OFF_TAG);

    cairn_ed25519_sign(blob, OFF_SIG, seed, pub, blob + OFF_SIG);
    ok = true;

out:
    scrub(shared, sizeof(shared));
    scrub(key, sizeof(key));
    scrub(eph_pub, sizeof(eph_pub));
    return ok;
}

/* ── the line protocol ────────────────────────────────────────────────────── */

static void say(const cairn_prov_t *p, const char *line)
{
    if (p->ops->reply) p->ops->reply(p->ops->ctx, line);
}

static void note(const cairn_prov_t *p, const char *msg)
{
    if (p->ops->log) p->ops->log(p->ops->ctx, msg);
}

static void staged_free(cairn_prov_staged_t *st)
{
    scrub(st, sizeof(*st));
    memset(st, 0, sizeof(*st));
}

void cairn_prov_init(cairn_prov_t *p, const cairn_prov_ops_t *ops)
{
    memset(p, 0, sizeof(*p));
    p->ops = ops;
}

void cairn_prov_abort(cairn_prov_t *p)
{
    staged_free(&p->st);
    p->active = false;
}

/*
 * May a session be opened or continue right now?
 *
 *  - never during a trip: the controller owns the bus and the radio, and a
 *    credential change mid-drive is exactly the confusion to avoid;
 *  - an unprovisioned device always may — there is nothing yet to protect;
 *  - a provisioned device only in a short window after boot, so an unattended
 *    one cannot be re-provisioned by someone who plugs in an hour later.
 */
static const char *refusal(const cairn_prov_env_t *env)
{
    if (env->trip_active) return "a trip is active";
    if (env->provisioned && env->uptime_ms > CAIRN_PROV_WINDOW_MS)
        return "the provisioning window after boot has closed (reset the device)";
    return NULL;
}

static bool is_hex32(const char *s, uint8_t out[16])
{
    if (strlen(s) != 32) return false;
    for (int i = 0; i < 16; i++) {
        int hi, lo;
        char a = s[2 * i], b = s[2 * i + 1];
        hi = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
        lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void set_field(cairn_prov_t *p, char *field, char *a1, char *a2)
{
    cairn_prov_staged_t *st = &p->st;
    char msg[96];

    if (strcmp(field, "assignment") == 0 && a1 && a2) {
        uint8_t veh[16], asg[16];
        if (!is_hex32(a1, veh) || !is_hex32(a2, asg) || all_zero(veh, 16) || all_zero(asg, 16)) {
            say(p, "ERR assignment needs two non-zero 32-hex ids: <vehicle> <assignment>");
            return;
        }
        memcpy(st->vehicle_id, veh, 16); memcpy(st->assignment_id, asg, 16);
        st->have_assignment = true;
    } else if (strcmp(field, "counter_floor") == 0 && a1 && !a2) {
        size_t n = strlen(a1);
        if (n == 0 || n > 20) { say(p, "ERR counter_floor must be a decimal u64"); return; }
        uint64_t v = 0;
        for (size_t i = 0; i < n; i++) {
            if (a1[i] < '0' || a1[i] > '9') { say(p, "ERR counter_floor must be a decimal u64"); return; }
            uint64_t d = (uint64_t)(a1[i] - '0');
            if (v > (UINT64_MAX - d) / 10) { say(p, "ERR counter_floor overflows u64"); return; }
            v = v * 10 + d;
        }
        st->counter_floor = v; st->have_floor = true;
    } else {
        say(p, "ERR unknown field or wrong argument count");
        return;
    }

    /* Field names only, never values. */
    snprintf(msg, sizeof(msg), "provisioning: staged %s", field);
    note(p, msg);
    say(p, "OK");
}

void cairn_prov_line(cairn_prov_t *p, const char *line, const cairn_prov_env_t *env)
{
    if (strlen(line) >= CAIRN_PROV_LINE_MAX) { say(p, "ERR line too long"); return; }

    /* Only ASCII printable lines are protocol. Anything else is ignored, not
     * echoed: the console also carries boot noise and the log. */
    for (const char *c = line; *c; c++) {
        if ((unsigned char)*c < 0x20 || (unsigned char)*c >= 0x7f) return;
    }

    /* Tokenize in place on a copy so secrets are not left in the caller's buffer
     * twice. */
    char buf[CAIRN_PROV_LINE_MAX];
    snprintf(buf, sizeof(buf), "%s", line);

    char *tok[5] = { 0 };
    int nt = 0;
    for (char *s = strtok(buf, " "); s && nt < 5; s = strtok(NULL, " ")) tok[nt++] = s;
    if (nt == 0) { scrub(buf, sizeof(buf)); return; }

    if (strcmp(tok[0], "CAIRN-PROV") == 0 && nt == 2 && strcmp(tok[1], "BEGIN") == 0) {
        const char *why = refusal(env);
        if (why != NULL) {
            char m[160];
            snprintf(m, sizeof(m), "provisioning: BEGIN refused (%s)", why);
            note(p, m);
            char r[200];
            snprintf(r, sizeof(r), "ERR refused: %s", why);
            say(p, r);
            scrub(buf, sizeof(buf));
            return;
        }
        cairn_prov_abort(p);
        p->active = true;
        p->last_ms = env->uptime_ms;
        uint8_t id[16]; char fp[9];
        p->ops->identity(p->ops->ctx, id, fp);
        char r[96] = "PROV-READY ";
        static const char hex[] = "0123456789abcdef";
        size_t o = strlen(r);
        for (int i = 0; i < 16; i++) { r[o++] = hex[id[i] >> 4]; r[o++] = hex[id[i] & 15]; }
        r[o++] = ' ';
        memcpy(r + o, fp, 9);
        say(p, r);
        note(p, "provisioning: session opened");
        scrub(buf, sizeof(buf));
        return;
    }

    if (!p->active) {
        /* Not in a session: say nothing. A device that answered every stray
         * line would be noisy on a console that also carries its log. */
        scrub(buf, sizeof(buf));
        return;
    }

    /* Every line inside a session re-checks the rules: a trip may have started,
     * and an idle session expires. */
    if (env->trip_active) {
        cairn_prov_abort(p);
        note(p, "provisioning: session aborted, a trip started");
        say(p, "ERR aborted: a trip is active");
        scrub(buf, sizeof(buf));
        return;
    }
    if (env->uptime_ms - p->last_ms > CAIRN_PROV_IDLE_TIMEOUT_MS) {
        cairn_prov_abort(p);
        note(p, "provisioning: session expired");
        say(p, "ERR session expired; send CAIRN-PROV BEGIN again");
        scrub(buf, sizeof(buf));
        return;
    }
    p->last_ms = env->uptime_ms;

    if (strcmp(tok[0], "CAIRN-PROV") == 0 && nt == 2 && strcmp(tok[1], "END") == 0) {
        cairn_prov_abort(p);
        note(p, "provisioning: session closed");
        say(p, "OK");
    } else if (strcmp(tok[0], "GET") == 0 && nt == 2 && strcmp(tok[1], "enroll_blob") == 0) {
        char text[CAIRN_PROV_LINE_MAX / 2];
        const char *err = NULL;
        if (p->ops->enroll_text(p->ops->ctx, text, sizeof(text), &err)) {
            char r[CAIRN_PROV_LINE_MAX / 2 + 16];
            snprintf(r, sizeof(r), "ENROLL-BLOB %s", text);
            say(p, r);
            note(p, "provisioning: enrolment blob issued");
        } else {
            char r[160];
            snprintf(r, sizeof(r), "ERR %s", err ? err : "cannot build the enrolment blob");
            say(p, r);
        }
    } else if (strcmp(tok[0], "DROP") == 0 && nt == 2 &&
               strcmp(tok[1], "legacy-bundles") == 0) {
        /*
         * Spelled out rather than abbreviated, and it takes no wildcard, so it
         * cannot be mistaken for or grown into a general delete. The
         * implementation refuses any bundle it can still read as current, so
         * the worst a typo does is free space nothing could use.
         */
        if (p->ops->drop_legacy_bundles == NULL) {
            say(p, "ERR this build cannot drop bundles");
        } else {
            uint32_t dropped = 0;
            uint64_t bytes   = 0;
            if (p->ops->drop_legacy_bundles(p->ops->ctx, &dropped, &bytes)) {
                char r[128];
                snprintf(r, sizeof(r), "OK dropped %u bundle(s), %llu bytes freed",
                         (unsigned)dropped, (unsigned long long)bytes);
                say(p, r);
                note(p, "provisioning: legacy bundles dropped");
            } else {
                say(p, "ERR could not scan the bundle directory");
            }
        }
    } else if (strcmp(tok[0], "CLEAR") == 0 && nt == 2 &&
               strcmp(tok[1], "bonds") == 0) {
        if (p->ops->clear_ble_bonds == NULL) {
            say(p, "ERR this build has no BLE");
        } else {
            uint32_t cleared = 0;
            if (p->ops->clear_ble_bonds(p->ops->ctx, &cleared)) {
                char r[96];
                snprintf(r, sizeof(r), "OK cleared %u bond(s); the next phone to "
                                       "connect may pair", (unsigned)cleared);
                say(p, r);
                note(p, "provisioning: BLE bonds cleared");
            } else {
                say(p, "ERR could not clear bonds");
            }
        }
    } else if (strcmp(tok[0], "SET") == 0 && nt >= 3) {
        set_field(p, tok[1], tok[2], nt >= 4 ? tok[3] : NULL);
    } else if (strcmp(tok[0], "COMMIT") == 0 && nt == 1) {
        cairn_prov_staged_t *st = &p->st;
        bool any = st->have_assignment || st->have_floor;
        if (!any) {
            say(p, "ERR nothing staged");
        } else {
            const char *why = p->ops->apply(p->ops->ctx, st);
            if (why == NULL) {
                note(p, "provisioning: committed");
                say(p, "OK");
                cairn_prov_abort(p);
            } else {
                char r[200];
                snprintf(r, sizeof(r), "ERR %s", why);
                note(p, "provisioning: commit failed");
                say(p, r);
                /* Staged values are kept so COMMIT can simply be re-sent. */
            }
        }
    } else {
        say(p, "ERR unknown command");
    }
    scrub(buf, sizeof(buf));
}

void cairn_prov_tick(cairn_prov_t *p, const cairn_prov_env_t *env)
{
    if (!p->active) return;
    if (env->trip_active) {
        cairn_prov_abort(p);
        note(p, "provisioning: session aborted, a trip started");
    } else if (env->uptime_ms - p->last_ms > CAIRN_PROV_IDLE_TIMEOUT_MS) {
        cairn_prov_abort(p);
        note(p, "provisioning: session expired");
    }
}
