#include "prov_console.h"

#include <Arduino.h>
#include <string.h>

#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_kv.h"
#include "cairn_platform.h"
#include "cairn_log.h"
#include "cairn_prov.h"
#include "cairn_prune.h"
#include "ble_companion.h"
#include "cairn_store.h"
#include "config.h"

static const char *TAG = "PROV";

static cairn_prov_t s_prov;
static char         s_line[CAIRN_PROV_LINE_MAX];
static size_t       s_len;
static bool         s_overflow;

static void scrub(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* Whether an assignment is installed. Read once and after a commit, not on every
 * 50 ms poll: loading it means reading the storage root out of NVS. */
static bool         s_provisioned;

static bool load_provisioned(void)
{
    cairn_storage_identity_t id;
    bool ok = cairn_storage_identity_load(&id) && id.assigned;
    scrub(&id, sizeof(id));
    return ok;
}

static bool unhex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        char *end;
        v = (unsigned)strtoul(b, &end, 16);
        if (*end != '\0') return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* ── operations ───────────────────────────────────────────────────────────── */

static void op_identity(void *, uint8_t device_id[16], char fingerprint[9])
{
    uint8_t seed[32], pub[32];
    uint32_t boot_count = 0;
    memset(device_id, 0, 16);
    memcpy(fingerprint, "00000000", 9);
    if (cairn_identity_load(device_id, seed, pub, &boot_count)) {
        cairn_enroll_fingerprint(pub, fingerprint);
    }
    scrub(seed, sizeof(seed));
}

static bool op_enroll_text(void *, char *out, size_t cap, const char **err)
{
    uint8_t server_pub[32];
    if (!unhex32(CAIRN_SERVER_ENROLL_PUBKEY_HEX, server_pub)) {
        *err = "the pinned server enrolment key in this firmware is malformed";
        return false;
    }

    uint8_t id[16], seed[32], pub[32];
    uint32_t boot_count = 0;
    if (!cairn_identity_load(id, seed, pub, &boot_count)) {
        *err = "device identity unavailable";
        return false;
    }

    /* Generates K_root on first call if it does not exist yet. The root is
     * copied nowhere but into the sealed blob, and scrubbed below. */
    cairn_storage_identity_t sid;
    if (!cairn_storage_identity_load(&sid)) {
        scrub(seed, sizeof(seed));
        *err = "storage identity unavailable";
        return false;
    }

    /* Hardware RNG for both the ephemeral key and the nonce. A repeated
     * ephemeral key would not break the seal (the shared secret differs per
     * server key only), but there is no reason to risk it. */
    uint8_t eph[32], nonce[24];
    cairn_rng_fill(eph, sizeof(eph));
    cairn_rng_fill(nonce, sizeof(nonce));

    uint8_t blob[CAIRN_ENROLL_BLOB_SIZE];
    bool ok = cairn_enroll_build(blob, id, seed, pub, sid.storage_key_version,
                                 sid.root_key, server_pub, eph, nonce);

    scrub(seed, sizeof(seed));
    scrub(&sid, sizeof(sid));
    scrub(eph, sizeof(eph));

    if (!ok) {
        *err = "no server enrolment key is pinned in this firmware "
               "(CAIRN_SERVER_ENROLL_PUBKEY_HEX is the all-zero placeholder)";
        return false;
    }
    if (cairn_b64_encode(blob, sizeof(blob), out, cap) == 0) {
        *err = "internal: console buffer too small";
        return false;
    }
    return true;
}

static const char *op_apply(void *, const cairn_prov_staged_t *st)
{
    /*
     * Both parts are idempotent, so a failure part-way leaves a state COMMIT can
     * simply be re-sent against. The counter floor only ever raises; an
     * assignment change is logged as the state change it is.
     */
    if (st->have_assignment) {
        if (!cairn_storage_set_assignment(st->vehicle_id, st->assignment_id)) {
            return "could not store the vehicle assignment";
        }
        CAIRN_LOGW(TAG, "vehicle assignment changed by provisioning (takes effect "
                        "for the next bundle opened)");
        s_provisioned = true;
    }
    if (st->have_floor) {
        if (!cairn_storage_raise_counter(st->counter_floor)) {
            return "could not raise the device counter";
        }
        CAIRN_LOGI(TAG, "device counter raised to at least %llu",
                   (unsigned long long)st->counter_floor);
    }
    return nullptr;
}

static bool op_drop_legacy_bundles(void *, uint32_t *dropped, uint64_t *bytes)
{
    return cairn_prune_legacy_bundles(dropped, bytes);
}

static bool op_clear_ble_bonds(void *, uint32_t *cleared)
{
#if CAIRN_BLE_COMPANION
    *cleared = (uint32_t)ble_companion_bond_count();
    ble_companion_clear_bonds();
    return true;
#else
    (void)cleared;
    return false;
#endif
}

static void op_log(void *, const char *msg)
{
    CAIRN_LOGI(TAG, "%s", msg);
}

/*
 * Replies carry a fixed prefix. The console also carries the device's own log,
 * and a bare "OK" or "ERR" would be ambiguous against it; the host tool keys on
 * the prefix and ignores everything else.
 */
static void op_reply(void *, const char *line)
{
    Serial.print("@prov ");
    Serial.println(line);
}

static const cairn_prov_ops_t OPS = { nullptr, op_identity, op_enroll_text,
                                      op_apply, op_drop_legacy_bundles,
                                      op_clear_ble_bonds, op_log, op_reply };

/* ── console ──────────────────────────────────────────────────────────────── */

void prov_console_begin(void)
{
    cairn_prov_init(&s_prov, &OPS);
    s_len = 0;
    s_overflow = false;

    /* Firmware that still had Wi-Fi left a password and a client private key in
     * NVS. This one has no use for them, so they go. */
    if (cairn_prov_erase_legacy_credentials()) {
        CAIRN_LOGW(TAG, "removed any Wi-Fi and client-certificate material earlier firmware "
                        "left in NVS and overwrote the flash that held it; this device "
                        "holds no network credentials");
    }

    /* Deleting an NVS entry only marks it; the bytes stay readable in flash until the page
     * is collected, which the churn above cannot guarantee for a page that also holds live
     * entries (the Wi-Fi stack's saved password survived it on the car's dongle). Zero
     * them in place. Cheap when there is nothing to do, so it runs every boot. */
    int zeroed = cairn_kv_zero_erased();
    if (zeroed > 0) {
        CAIRN_LOGI(TAG, "zeroed %d deleted NVS entr%s whose bytes were still readable in flash",
                   zeroed, zeroed == 1 ? "y" : "ies");
    } else if (zeroed < 0) {
        CAIRN_LOGW(TAG, "could not check the NVS partition for deleted entries still readable in flash");
    }

    s_provisioned = load_provisioned();
    if (s_provisioned) {
        CAIRN_LOGI(TAG, "an assignment is installed; the console accepts "
                        "provisioning for %u s after boot, never during a trip",
                   (unsigned)(CAIRN_PROV_WINDOW_MS / 1000));
    } else {
        CAIRN_LOGW(TAG, "NOT ASSIGNED: bundles will be refused by the server. Run "
                        "cairn-provision over USB. Capture is unaffected.");
    }
}

void prov_console_poll(bool trip_active)
{
    cairn_prov_env_t env;
    env.uptime_ms = millis();
    env.trip_active = trip_active;
    env.provisioned = s_provisioned;

    cairn_prov_tick(&s_prov, &env);

    /* Bounded work per call: a chatty host must not starve capture. */
    for (int budget = 0; budget < 512 && Serial.available() > 0; budget++) {
        int c = Serial.read();
        if (c < 0) break;

        if (c == '\n') {
            if (s_overflow) {
                Serial.println("@prov ERR line too long");
            } else if (s_len > 0) {
                s_line[s_len] = '\0';
                cairn_prov_line(&s_prov, s_line, &env);
            }
            scrub(s_line, sizeof(s_line));
            s_len = 0;
            s_overflow = false;
        } else if (c == '\r') {
            /* ignored */
        } else if (s_len < sizeof(s_line) - 1) {
            s_line[s_len++] = (char)c;
        } else {
            s_overflow = true;
        }
    }
}
