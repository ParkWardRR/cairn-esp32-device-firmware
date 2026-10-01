/*
 * The OTA install path: fetch, verify, stage, swap.
 *
 * Split from cairn_ota.c because this half is Arduino and ESP-IDF while the
 * preconditions and version ordering are portable C — and those are the parts
 * that decide whether a device is *allowed* to replace itself, so they belong
 * where host tests can reach them. See docs/ota.md for the ordering argument.
 */

#include "cairn_ota.h"

#include <stdio.h>
#include <string.h>

#include "cairn_format.h"
#include "cairn_log.h"
#include "cairn_store.h"
#include "config.h"

static const char *TAG = "OTA";

#if CAIRN_OTA_AVAILABLE

#include <Arduino.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "cairn_sync.h"

static bool unhex_key(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return false;

    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/*
 * Hash what is actually in the partition.
 *
 * Hashing the bytes as they arrive proves the download was intact; it does not
 * prove the write succeeded. Flash can fail to program, and a partially written
 * slot that hashed correctly in RAM is exactly the failure that produces a boot
 * loop. The hash has to come from the slot.
 */
static bool hash_partition(const esp_partition_t *part, uint32_t length,
                           uint8_t out[32])
{
    cairn_sha256_t ctx;
    cairn_sha256_init(&ctx);

    static uint8_t block[1024];
    uint32_t done = 0;

    while (done < length) {
        size_t want = length - done;
        if (want > sizeof(block)) want = sizeof(block);

        if (esp_partition_read(part, done, block, want) != ESP_OK) {
            CAIRN_LOGE(TAG, "reading back the staged image failed at %u",
                       (unsigned)done);
            return false;
        }
        cairn_sha256_update(&ctx, block, want);
        done += (uint32_t)want;
    }

    cairn_sha256_final(&ctx, out);
    return true;
}

cairn_ota_result_t cairn_ota_check_and_install(bool parked, uint16_t battery_mv,
                                               bool *reboot_required)
{
    if (reboot_required != NULL) *reboot_required = false;

    cairn_ota_block_t block;
    if (!cairn_ota_preconditions(parked, battery_mv, &block)) {
        char why[256];
        cairn_ota_describe_block(&block, why, sizeof(why));
        CAIRN_LOGI(TAG, "no update attempted: %s", why);
        return CAIRN_OTA_BLOCKED;
    }

    if (!cairn_sync_is_connected()) return CAIRN_OTA_NO_NETWORK;

    uint8_t update_key[32];
    if (!unhex_key(CAIRN_UPDATE_KEY_HEX, update_key)) {
        CAIRN_LOGE(TAG, "CAIRN_UPDATE_KEY_HEX is not 64 hex characters");
        return CAIRN_OTA_DISABLED;
    }

    /* ── descriptor ───────────────────────────────────────────────────────── */

    char base[96], url[192];
    cairn_sync_base_url(base, sizeof(base));
    snprintf(url, sizeof(url), "%s/api/v2/firmware/latest", base);

    HTTPClient http;
    if (!cairn_sync_begin_request(http, url)) return CAIRN_OTA_NO_NETWORK;

    int code = http.GET();
    if (code != 200) {
        http.end();
        CAIRN_LOGW(TAG, "no update descriptor available: HTTP %d", code);
        return (code == 404) ? CAIRN_OTA_UP_TO_DATE : CAIRN_OTA_DESCRIPTOR_FAILED;
    }

    String sig_hex = http.header("X-Cairn-Signature");

    static uint8_t encoded[512];
    int got = 0;
    {
        WiFiClient *s = http.getStreamPtr();
        if (s != nullptr) got = s->readBytes(encoded, sizeof(encoded));
    }
    http.end();

    if (got <= 0 || sig_hex.length() != 128) {
        CAIRN_LOGE(TAG, "descriptor is %d bytes with a %u-character signature",
                   got, (unsigned)sig_hex.length());
        return CAIRN_OTA_DESCRIPTOR_FAILED;
    }

    uint8_t sig[64];
    for (int i = 0; i < 64; i++) {
        unsigned v = 0;
        if (sscanf(sig_hex.c_str() + i * 2, "%2x", &v) != 1) {
            return CAIRN_OTA_DESCRIPTOR_FAILED;
        }
        sig[i] = (uint8_t)v;
    }

    /*
     * Verified before a single byte of image is fetched. Otherwise a hostile
     * server could make the device write megabytes into its spare slot on
     * demand.
     */
    cairn_update_descriptor_t desc;
    static uint8_t scratch[512];
    cairn_err_t err = cairn_update_verify(encoded, (size_t)got, sig, update_key,
                                          &desc, scratch, sizeof(scratch));
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "update descriptor rejected: %s", cairn_strerror(err));
        return CAIRN_OTA_DESCRIPTOR_FAILED;
    }

    /* ── version gates ────────────────────────────────────────────────────── */

    bool ok = false;
    int cmp = cairn_ota_version_compare(desc.firmware_version,
                                        CAIRN_FIRMWARE_VERSION, &ok);
    if (!ok) {
        CAIRN_LOGE(TAG, "cannot order \"%s\" against \"%s\"; refusing rather "
                        "than guessing", desc.firmware_version,
                   CAIRN_FIRMWARE_VERSION);
        return CAIRN_OTA_REFUSED;
    }
    if (cmp <= 0) {
        CAIRN_LOGI(TAG, "already at %s; offered %s", CAIRN_FIRMWARE_VERSION,
                   desc.firmware_version);
        return CAIRN_OTA_UP_TO_DATE;
    }

    if (desc.min_firmware_version[0] != '\0') {
        bool min_ok = false;
        int  m = cairn_ota_version_compare(CAIRN_FIRMWARE_VERSION,
                                           desc.min_firmware_version, &min_ok);
        if (!min_ok || m < 0) {
            CAIRN_LOGE(TAG, "%s requires at least %s; this build is %s",
                       desc.firmware_version, desc.min_firmware_version,
                       CAIRN_FIRMWARE_VERSION);
            return CAIRN_OTA_REFUSED;
        }
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == nullptr) {
        CAIRN_LOGE(TAG, "no OTA slot available");
        return CAIRN_OTA_FLASH_FAILED;
    }
    if (desc.image_length > target->size) {
        CAIRN_LOGE(TAG, "image is %u bytes, slot %s holds %u",
                   (unsigned)desc.image_length, target->label,
                   (unsigned)target->size);
        return CAIRN_OTA_REFUSED;
    }

    CAIRN_LOGW(TAG, "installing %s over %s: %u bytes into %s",
               desc.firmware_version, CAIRN_FIRMWARE_VERSION,
               (unsigned)desc.image_length, target->label);

    /* ── download into the inactive slot ──────────────────────────────────── */

    char digest_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(digest_hex + i * 2, 3, "%02x", desc.image_sha256[i]);
    }
    snprintf(url, sizeof(url), "%s/api/v2/firmware/%s/image", base, digest_hex);

    HTTPClient img;
    if (!cairn_sync_begin_request(img, url)) return CAIRN_OTA_NO_NETWORK;

    if (img.GET() != 200) {
        img.end();
        CAIRN_LOGE(TAG, "image fetch failed");
        return CAIRN_OTA_DOWNLOAD_FAILED;
    }

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(target, desc.image_length, &handle) != ESP_OK) {
        img.end();
        CAIRN_LOGE(TAG, "esp_ota_begin failed");
        return CAIRN_OTA_FLASH_FAILED;
    }

    WiFiClient *stream = img.getStreamPtr();
    static uint8_t buf[2048];
    uint32_t written = 0;
    bool     transfer_ok = true;

    while (written < desc.image_length && stream != nullptr) {
        int n = stream->readBytes(buf, sizeof(buf));
        if (n <= 0) break;

        if (esp_ota_write(handle, buf, (size_t)n) != ESP_OK) {
            CAIRN_LOGE(TAG, "esp_ota_write failed at %u", (unsigned)written);
            transfer_ok = false;
            break;
        }
        written += (uint32_t)n;
    }
    img.end();

    if (!transfer_ok || written != desc.image_length) {
        /*
         * Abort rather than finalize. The old slot is untouched either way, so
         * the device keeps running what it has and the next attempt overwrites
         * this one.
         */
        esp_ota_abort(handle);
        CAIRN_LOGE(TAG, "wrote %u of %u bytes; the running image is unaffected",
                   (unsigned)written, (unsigned)desc.image_length);
        return CAIRN_OTA_DOWNLOAD_FAILED;
    }

    if (esp_ota_end(handle) != ESP_OK) {
        CAIRN_LOGE(TAG, "esp_ota_end rejected the image");
        return CAIRN_OTA_FLASH_FAILED;
    }

    /* ── verify what is in flash, not what arrived ────────────────────────── */

    uint8_t actual[32];
    if (!hash_partition(target, desc.image_length, actual)) {
        return CAIRN_OTA_FLASH_FAILED;
    }
    if (memcmp(actual, desc.image_sha256, 32) != 0) {
        CAIRN_LOGE(TAG, "the staged image does not match the signed digest; "
                        "the boot partition is left alone");
        return CAIRN_OTA_IMAGE_MISMATCH;
    }

    CAIRN_LOGI(TAG, "staged image matches the signed digest");

    /* ── only now ─────────────────────────────────────────────────────────── */

    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        CAIRN_LOGE(TAG, "esp_ota_set_boot_partition failed");
        return CAIRN_OTA_FLASH_FAILED;
    }

    CAIRN_LOGW(TAG, "%s staged in %s and will run at the next reboot; it marks "
                    "itself valid only after storage checks out, otherwise the "
                    "bootloader rolls back",
               desc.firmware_version, target->label);

    if (reboot_required != NULL) *reboot_required = true;
    return CAIRN_OTA_OK;
}

#else /* !CAIRN_OTA_AVAILABLE */

cairn_ota_result_t cairn_ota_check_and_install(bool parked, uint16_t battery_mv,
                                               bool *reboot_required)
{
    (void)parked;
    (void)battery_mv;
    if (reboot_required != NULL) *reboot_required = false;
    return CAIRN_OTA_DISABLED;
}

#endif /* CAIRN_OTA_AVAILABLE */
