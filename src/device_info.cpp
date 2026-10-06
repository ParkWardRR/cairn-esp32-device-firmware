#include "device_info.h"

#include <Arduino.h>
#include <esp_flash_encrypt.h>
#include <esp_secure_boot.h>
#include <stdio.h>
#include <string.h>

#include "boot_timing.h"
#include "cairn_devinfo.h"
#include "cairn_engine.h"
#include "cairn_format.h"
#include "cairn_log.h"

static const char *TAG = "DEVINFO";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool     have_identity;
    uint8_t  device_id[16];
    uint8_t  fingerprint[4];
    uint8_t  enrol_state;
    uint32_t key_version;

    uint8_t  storage_state;
    uint32_t pending;
    uint32_t free_mib;

    uint32_t capabilities;
} s_state = { false, {0}, {0}, 0, 0, 0, 0, 0xFFFFFFFFu, 0 };

void device_info_set_identity(const uint8_t device_id[16], const uint8_t public_key[32],
                              uint32_t storage_key_version, bool assigned)
{
    uint8_t digest[32];
    cairn_sha256(public_key, 32, digest);       /* the fingerprint of enrolment/v1 */

    portENTER_CRITICAL(&s_lock);
    memcpy(s_state.device_id, device_id, 16);
    memcpy(s_state.fingerprint, digest, 4);
    s_state.enrol_state = assigned ? 2 : 0;
    s_state.key_version = storage_key_version;
    s_state.have_identity = true;
    portEXIT_CRITICAL(&s_lock);
}

void device_info_set_storage(uint8_t state, uint32_t free_mib)
{
    portENTER_CRITICAL(&s_lock);
    s_state.storage_state = state;
    s_state.free_mib = free_mib;
    portEXIT_CRITICAL(&s_lock);
}

void device_info_set_pending(uint32_t pending_bundles)
{
    portENTER_CRITICAL(&s_lock);
    s_state.pending = pending_bundles;
    portEXIT_CRITICAL(&s_lock);
}

void device_info_set_capabilities(uint32_t bits)
{
    portENTER_CRITICAL(&s_lock);
    s_state.capabilities = bits;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t device_info_capabilities(void) { return s_state.capabilities; }

/* ── what the build knows about itself ────────────────────────────────────── */

/* "Oct  6 2026" and "20:58:11" from the compiler, read as UTC. The builder's own time zone
 * is not known, so this can be off by up to a day; the field says when it was built, not
 * to the second. */
static uint32_t build_unix(void)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return 0;
    if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) return 0;

    const char *m = strstr(months, mon);
    if (!m) return 0;
    int month = (int)((m - months) / 3) + 1;

    /* days from civil, Howard Hinnant's algorithm */
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
}

static void fill_firmware(cairn_di_firmware_t *f)
{
    memset(f, 0, sizeof *f);
    unsigned a = 0, b = 0, c = 0;
    if (sscanf(CAIRN_FIRMWARE_VERSION, "cairn-v%u.%u.%u", &a, &b, &c) == 3) {
        f->major = (uint8_t)a; f->minor = (uint8_t)b; f->patch = (uint8_t)c;
    }
    uint8_t flags = 0;
#ifdef CAIRN_BUILD_DIRTY
    flags |= CAIRN_FW_DIRTY;
#endif
#ifdef CAIRN_RELEASE_BUILD
    flags |= CAIRN_FW_RELEASE;
#endif
    if (esp_secure_boot_enabled())     flags |= CAIRN_FW_SECURE_BOOT;
    if (esp_flash_encryption_enabled()) flags |= CAIRN_FW_FLASH_ENCRYPT;
    f->flags = flags;

#ifdef CAIRN_GIT_COMMIT_HEX
    /* 16 hex characters, passed in by the build (make firmware). Zeros mean "not recorded". */
    const char *h = CAIRN_GIT_COMMIT_HEX;
    for (int i = 0; i < 8 && h[2 * i] && h[2 * i + 1]; i++) {
        unsigned v;
        char pair[3] = { h[2 * i], h[2 * i + 1], 0 };
        if (sscanf(pair, "%2x", &v) == 1) f->commit[i] = (uint8_t)v;
    }
#endif
    f->build_unix = build_unix();
}

size_t device_info_build(uint8_t *out, size_t cap)
{
    static cairn_devinfo_t info;       /* ~2 KB: not on the BLE host task's stack */
    memset(&info, 0, sizeof info);

    portENTER_CRITICAL(&s_lock);
    info.capabilities = s_state.capabilities | CAIRN_CAP_DEVICE_INFO;
    info.has_identity = s_state.have_identity;
    if (s_state.have_identity) {
        memcpy(info.identity.device_id, s_state.device_id, 16);
        memcpy(info.identity.fingerprint, s_state.fingerprint, 4);
        info.identity.enrol_state = s_state.enrol_state;
        info.identity.storage_key_version = s_state.key_version;
    }
    info.has_storage = true;
    info.storage.state = s_state.storage_state;
    info.storage.pending_bundles = (s_state.pending > 0xFFFF) ? 0xFFFF : (uint16_t)s_state.pending;
    info.storage.free_mib = s_state.free_mib;
    portEXIT_CRITICAL(&s_lock);

    info.has_firmware = true;
    fill_firmware(&info.firmware);

    /* Transports: what exists, not what is promised. BLE is up if this is being read.
     * Wi-Fi: the ESP32 has the radio (hardware present) but the firmware has no Wi-Fi code
     * (issue 15). LTE: whether the unit carries a modem is unconfirmed (issue 16), so it is
     * not claimed. */
    info.n_transports = 3;
    info.transports[0].kind = 1; info.transports[0].state = 0x1F;
    info.transports[1].kind = 2; info.transports[1].state = 0x01;
    info.transports[2].kind = 3; info.transports[2].state = 0x00;

    size_t n = cairn_engine_installed_count();
    for (size_t i = 0; i < n && info.n_engines < CAIRN_DI_MAX_ENGINES; i++) {
        const cairn_engine_profile_t *p = cairn_engine_installed(i);
        size_t len = strlen(p->id);
        if (len < 1 || len > CAIRN_DI_ENGINE_ID_MAX) continue;
        cairn_di_engine_t *e = &info.engines[info.n_engines++];
        e->profile_version = p->version;
        memcpy(e->hash, p->sha256, 8);
        e->id_len = (uint8_t)len;
        memcpy(e->id, p->id, len);
    }
    if (n > info.n_engines) info.truncated = true;

    cairn_boottime_t bt;
    boot_timing_snapshot(&bt);
    info.has_boot = true;
    uint32_t ble = cairn_boottime_at(&bt, CAIRN_BOOT_BLE_ADVERTISING);
    uint32_t pre = bt.pre_app_us;          /* ROM and bootloader, before T0 */
    info.boot.to_ble_ms = (ble == CAIRN_BOOT_NOT_REACHED) ? CAIRN_DI_UNKNOWN_MS : (pre + ble) / 1000u;
    uint32_t ready = cairn_boottime_at(&bt, CAIRN_BOOT_CAPTURE_OPEN);
    info.boot.to_ready_ms = (ready == CAIRN_BOOT_NOT_REACHED) ? CAIRN_DI_UNKNOWN_MS : (pre + ready) / 1000u;
    uint32_t fix = cairn_boottime_at(&bt, CAIRN_BOOT_GNSS_FIRST_FIX);
    info.boot.to_first_fix_ms = (fix == CAIRN_BOOT_NOT_REACHED) ? CAIRN_DI_UNKNOWN_MS : (pre + fix) / 1000u;
    info.boot.reset_reason = boot_timing_reset_code();

    size_t len = cairn_devinfo_encode(&info, out, cap);
    if (len == 0) CAIRN_LOGE(TAG, "device info did not encode");
    return len;
}
