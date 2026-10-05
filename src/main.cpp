/*
 * Cairn v2 — entry point.
 *
 * Boot order is deliberate and is the part most worth reading:
 *
 *   1. Logging, before anything that can fail. Early lines are buffered in RAM
 *      and drained once the card mounts, so a card that will not mount is
 *      itself diagnosable.
 *   2. The card, then the directory tree.
 *   3. Interrupted seals, then interrupted prunes, then the capture. A bundle
 *      that was already sealed must not be reopened as a capture, and free
 *      space must reflect receipts already verified before anything new is
 *      written.
 *
 * CAIRN_SELFTEST builds a bench variant that exercises the format, the card and
 * the network and then stops, so the hardware can be confirmed before a drive
 * is trusted to it.
 */

#include <Arduino.h>
#include <SD.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>

#include "board_config.h"
#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_log.h"
#include "cairn_prune.h"
#include "cairn_store.h"
#include "cairn_sync.h"
#include "config.h"
#include "lifecycle.h"
#include "pidtest.h"
#include "sensors.h"

static const char *TAG = "BOOT";

/*
 * Must be at file scope: this defines the weak hook arduino-esp32 calls when it
 * creates loopTask, so it has to exist before setup() runs. See
 * CAIRN_LOOP_STACK_BYTES in config.h for why the default is too small.
 */
SET_LOOP_TASK_STACK_SIZE(CAIRN_LOOP_STACK_BYTES);

static Lifecycle g_lifecycle;
static bool      g_running = false;

/* Non-zero while a deferred SD mount retry is pending; see setup(). */
static uint32_t  g_mount_retry_at_ms = 0;

/*
 * Report how close the deepest path came to the canary. A seal that completes
 * with 300 bytes to spare is a crash waiting for one more member in the bundle,
 * and that is worth seeing in the log before it happens in a car.
 */
static void log_stack_headroom(const char *what)
{
    size_t free_bytes = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);

    if (free_bytes < CAIRN_STACK_WARN_BYTES) {
        CAIRN_LOGW(TAG, "stack headroom after %s: %u bytes free of %u — close to "
                        "the canary; raise CAIRN_LOOP_STACK_BYTES",
                   what, (unsigned)free_bytes, (unsigned)CAIRN_LOOP_STACK_BYTES);
    } else {
        CAIRN_LOGI(TAG, "stack headroom after %s: %u bytes free of %u", what,
                   (unsigned)free_bytes, (unsigned)CAIRN_LOOP_STACK_BYTES);
    }
}

/* ── boot banner ──────────────────────────────────────────────────────────── */

static void log_partition_state(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();

    if (running != nullptr) {
        CAIRN_LOGI(TAG, "running from %s at 0x%06x (%u KiB)", running->label,
                   (unsigned)running->address, (unsigned)(running->size / 1024));
    }

    /*
     * A boot partition that differs from the running one means the last update
     * was rolled back, which is worth knowing before blaming the data.
     */
    if (boot != nullptr && running != nullptr && boot != running) {
        CAIRN_LOGW(TAG, "boot partition is %s but %s is running; the last "
                        "update was rolled back", boot->label, running->label);
    }

    esp_ota_img_states_t state;
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        CAIRN_LOGW(TAG, "this image is pending verification; it will be marked "
                        "valid once storage is confirmed");
    }
}

/*
 * Mark a freshly flashed image valid only after the card checks out. Doing it
 * at the top of setup() would defeat the purpose of having a rollback slot.
 */
static void mark_image_valid_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == nullptr) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        CAIRN_LOGI(TAG, "image marked valid; rollback cancelled");
    } else {
        CAIRN_LOGE(TAG, "could not mark the image valid; a reboot will roll back");
    }
}

/* ── self-test ────────────────────────────────────────────────────────────── */

/* ── known-answer gate ────────────────────────────────────────────────────── */

/*
 * Cross-implementation agreement is the whole premise of the format: a bundle is
 * only worth keeping if the Go and Rust verifiers compute the same digests over
 * it. Both primitives are configurable per platform — CRC-32 routes through
 * ESP32 ROM under CAIRN_USE_ESP_ROM_CRC, SHA-256 through mbedTLS — so neither is
 * guaranteed by the host conformance run, which compiles the portable fallback.
 *
 * This ran only in the self-test build until the ROM wrapper turned out to be
 * wrong on real hardware. A device with a bad CRC wrapper is internally
 * consistent — it writes frames and scans them back happily — so nothing on the
 * device notices, and every trip it records is rejected later by the verifier.
 * Checking two vectors costs microseconds at boot and converts that into a
 * refusal to capture, which is the honest outcome.
 */
static bool format_known_answers_ok(void)
{
    const uint8_t probe[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    uint32_t      crc     = cairn_crc32(probe, sizeof(probe));

    if (crc != 0xCBF43926u) {
        CAIRN_LOGE(TAG, "CRC-32(\"123456789\") = %08x but the format requires "
                        "cbf43926. Every frame written here would be rejected "
                        "by the verifier, so capture is refused.",
                   (unsigned)crc);
        return false;
    }

    uint8_t digest[32];
    cairn_sha256(nullptr, 0, digest);

    static const uint8_t want_sha[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
        0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
        0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };

    if (memcmp(digest, want_sha, sizeof(want_sha)) != 0) {
        CAIRN_LOGE(TAG, "SHA-256(\"\") does not match the published digest. "
                        "Content roots computed here would not agree with the "
                        "verifier, so capture is refused.");
        return false;
    }

    CAIRN_LOGI(TAG, "format known-answer check passed (CRC-32, SHA-256)");
    return true;
}

#if CAIRN_SELFTEST

/*
 * Checks the things that silently make a drive worthless: that the format code
 * agrees with itself on this hardware, that the card accepts a framed write and
 * the scan reads it back, and that the server is reachable with the pinned key.
 */
static void run_selftest(void)
{
    CAIRN_LOGI(TAG, "=== self-test ===");

    /*
     * 1. CRC-32. On ESP32 this routes through ROM, whose convention is
     * inverted, so a wrong wrapper here would corrupt every frame on the card
     * in a way only the server would discover.
     */
    const uint8_t probe[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    uint32_t crc = cairn_crc32(probe, sizeof(probe));
    bool crc_ok = (crc == 0xCBF43926u);
    CAIRN_LOGI(TAG, "[%s] CRC-32(\"123456789\") = %08x, want cbf43926",
               crc_ok ? "PASS" : "FAIL", (unsigned)crc);

    /* 2. SHA-256 of the empty string. */
    uint8_t digest[32];
    cairn_sha256(nullptr, 0, digest);
    const uint8_t want_sha[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
        0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
        0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    bool sha_ok = memcmp(digest, want_sha, 32) == 0;
    CAIRN_LOGI(TAG, "[%s] SHA-256(\"\")", sha_ok ? "PASS" : "FAIL");

    /*
     * 3. Ed25519 on this hardware. Signing is what a seal depends on and the
     * slowest thing the firmware does, so it is timed as well as checked.
     */
    uint8_t seed[32], pub[32], sig[64];
    memset(seed, 0x42, sizeof(seed));
    cairn_ed25519_public_from_seed(seed, pub);

    const char *msg = "cairn self-test";
    uint32_t t0 = millis();
    cairn_ed25519_sign((const uint8_t *)msg, strlen(msg), seed, pub, sig);
    uint32_t sign_ms = millis() - t0;

    t0 = millis();
    bool sig_ok = cairn_ed25519_verify((const uint8_t *)msg, strlen(msg), sig, pub);
    uint32_t verify_ms = millis() - t0;

    /* A flipped bit must fail, or "verify" is not checking anything. */
    sig[0] ^= 0x01;
    bool neg_ok = !cairn_ed25519_verify((const uint8_t *)msg, strlen(msg), sig, pub);

    CAIRN_LOGI(TAG, "[%s] Ed25519 sign %u ms, verify %u ms, tamper rejected %d",
               (sig_ok && neg_ok) ? "PASS" : "FAIL", (unsigned)sign_ms,
               (unsigned)verify_ms, (int)neg_ok);

    /*
     * 4. A framed write to the card. Encode, store, then seal — the end-to-end
     * path, on real on-card bytes rather than a buffer.
     */
    uint8_t  device_id[16], key_seed[32], key_pub[32];
    uint32_t boot_count = 0;
    cairn_identity_load(device_id, key_seed, key_pub, &boot_count);

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    /* Static: the capture keeps a pointer to it. */
    static cairn_storage_identity_t storage;
    bool storage_ok = cairn_storage_identity_load(&storage);
    CAIRN_LOGI(TAG, "[%s] storage root loaded, key version %u, %s",
               storage_ok ? "PASS" : "FAIL", (unsigned)storage.storage_key_version,
               storage.assigned ? "assigned" : "UNASSIGNED (server will refuse bundles)");

    static cairn_capture_t cap;
    bool store_ok = storage_ok &&
                    cairn_capture_open_or_resume(&cap, device_id, boot_id, &storage);

    int appended = 0;
    if (store_ok) {
        for (int i = 0; i < 8; i++) {
            cairn_gnss_sample_t s;
            memset(&s, 0, sizeof(s));
            s.lat_e7     = 340000000 + i;  /* 34.0 degrees, as degrees x 1e7 */
            s.lon_e7     = -1185000000;
            s.fix_type   = 3;
            s.sats_used  = 9;
            s.hdop_e2    = 120;
            s.h_acc_cm   = CAIRN_U16_UNKNOWN;
            s.v_acc_cm   = CAIRN_U16_UNKNOWN;
            s.utc_acc_ms = CAIRN_U16_UNKNOWN;

            uint8_t payload[32];
            cairn_encode_gnss_sample(&s, payload);

            if (cairn_capture_append(&cap, CAIRN_CHAIN_CAPTURE,
                                     CAIRN_REC_GNSS_SAMPLE, 1, 0, millis(),
                                     payload, sizeof(payload))) {
                appended++;
            }
        }
    }
    CAIRN_LOGI(TAG, "[%s] wrote %d framed records to the card",
               (appended == 8) ? "PASS" : "FAIL", appended);

    bool seal_ok = false;
    if (appended > 0) {
        uint8_t sealed_id[16];
        t0 = millis();
        seal_ok = cairn_capture_seal(&cap, key_seed, key_pub,
                                     CAIRN_FIRMWARE_VERSION,
                                     CAIRN_POLICY_VERSION, 0, sealed_id);
        CAIRN_LOGI(TAG, "[%s] sealed in %u ms", seal_ok ? "PASS" : "FAIL",
                   (unsigned)(millis() - t0));
        log_stack_headroom("seal");
    }

    /*
     * 5. Network and the pinned key. Offline is a valid steady state for this
     * device, so this reports rather than fails.
     */
    if (cairn_sync_connect(CAIRN_SYNC_CONNECT_TIMEOUT_MS)) {
        CAIRN_LOGI(TAG, "[PASS] associated with \"%s\", rssi %d dBm",
                   CAIRN_WIFI_SSID, cairn_sync_rssi());

        cairn_sync_stats_t stats;
        cairn_sync_result_t r = cairn_sync_run(&stats);
        CAIRN_LOGI(TAG, "[INFO] sync: %s (%u offered, %u receipted, %u pruned)",
                   cairn_sync_result_name(r), (unsigned)stats.bundles_offered,
                   (unsigned)stats.bundles_receipted,
                   (unsigned)stats.bundles_pruned);
        cairn_sync_disconnect();
    } else {
        CAIRN_LOGW(TAG, "[INFO] no network; this device is offline-first, so "
                        "that is not a failure");
    }

    bool all = crc_ok && sha_ok && sig_ok && neg_ok && (appended == 8) && seal_ok;
    CAIRN_LOGI(TAG, "=== self-test %s ===", all ? "PASSED" : "FAILED");

    cairn_log_flush();
}

#endif /* CAIRN_SELFTEST */

/* ── setup ────────────────────────────────────────────────────────────────── */


/*
 * Attempt the mount, retrying a few times within one call.
 *
 * Lifted out of setup() so loop() can retry it. The card slot on this unit is
 * intermittent: observed mounting first-try on one boot and failing all five
 * attempts on the next, with differing token errors (0x20, 0x3, 0x31, 0x53),
 * on a card that verifies clean on a workstation. That is a marginal contact
 * rather than a settling delay or a bad filesystem, and it is not something a
 * one-second burst of retries reliably clears.
 */
static bool try_mount_sd(void)
{
    bool mounted = false;
    for (int attempt = 1; attempt <= CAIRN_SD_MOUNT_ATTEMPTS; attempt++) {
        if (SD.begin(CAIRN_PIN_SD_CS)) {
            mounted = true;
            if (attempt > 1) {
                CAIRN_LOGW(TAG, "SD mounted on attempt %d of %d; the card needed "
                                "a settling period",
                           attempt, CAIRN_SD_MOUNT_ATTEMPTS);
            }
            break;
        }

        if (attempt < CAIRN_SD_MOUNT_ATTEMPTS) {
            CAIRN_LOGW(TAG, "SD mount attempt %d of %d failed; retrying in %d ms",
                       attempt, CAIRN_SD_MOUNT_ATTEMPTS, CAIRN_SD_MOUNT_RETRY_MS);
            SD.end();
            delay(CAIRN_SD_MOUNT_RETRY_MS);
        }
    }

    return mounted;
}

/*
 * Everything that depends on the card, split out of setup() so it can run
 * later if the mount only succeeds on a retry from loop().
 *
 * Returns true once the capture loop is live.
 */
static bool bring_up_after_mount(void)
{

    CAIRN_LOGI(TAG, "SD mounted: %llu MiB total, %llu MiB used",
               SD.totalBytes() / (1024ULL * 1024ULL),
               SD.usedBytes() / (1024ULL * 1024ULL));

    /* The card is already mounted; this only binds the storage abstraction to
     * it, so the same cairn_store.c runs here and under the host fault tests. */
    cairn_fs_begin(nullptr);

    if (!cairn_store_init()) {
        CAIRN_LOGE(TAG, "cannot create the directory tree on the card");
        return false;
    }

    /*
     * Attach the SD log sink as soon as the tree exists, so everything from
     * here on is on the card. The boot count comes from NVS, which also names
     * the log file.
     */
    uint8_t  probe_device[16], probe_seed[32], probe_pub[32];
    uint32_t boot_count = 0;
    bool     have_identity =
        cairn_identity_load(probe_device, probe_seed, probe_pub, &boot_count);

    cairn_log_attach_sd(have_identity ? boot_count : 0);

    /*
     * After the SD sink is attached, so the enrolment command lands in the log
     * file too — not only on a console that may not be connected.
     */
    if (have_identity) {
        cairn_identity_print_enrolment(probe_device, probe_pub);
    }

    /*
     * TLS credentials live on the card, so they are loaded once it is mounted
     * and before anything tries to upload. Failure here is not fatal — it
     * downgrades the transport and says so.
     */
    cairn_sync_load_credentials();

    /*
     * Before anything is written: confirm this build's primitives agree with the
     * specification. A failure here is not recoverable by retrying, so the
     * device stays up and keeps logging rather than capturing data that cannot
     * be verified.
     *
     * The self-test build deliberately continues past a failure — it exists to
     * report every result, and stopping at the first one would hide the rest.
     */
    bool primitives_ok = format_known_answers_ok();

#if !CAIRN_SELFTEST
    if (!primitives_ok) {
        CAIRN_LOGE(TAG, "format self-check failed; not capturing. Reflash with a "
                        "build whose CRC-32 and SHA-256 match the specification.");
        cairn_log_flush();
        return false;
    }
#else
    (void)primitives_ok;
#endif

    /* The card is confirmed, so a pending image has earned its keep. */
    mark_image_valid_if_pending();

#if CAIRN_SELFTEST
    run_selftest();
    CAIRN_LOGI(TAG, "self-test build: halting rather than capturing");
    return false;
#endif

    if (!lifecycle_begin(&g_lifecycle)) {
        CAIRN_LOGE(TAG, "lifecycle failed to start");
        return false;
    }

    g_running = true;
    digitalWrite(CAIRN_PIN_LED, LOW);
    CAIRN_LOGI(TAG, "capture loop running");

    return true;
}

void setup()
{
    cairn_log_init(115200);
#if CAIRN_SELFTEST || CAIRN_PIDTEST
    delay(300);
#endif

    CAIRN_LOGI(TAG, "Cairn %s, policy v%d, built %s %s", CAIRN_FIRMWARE_VERSION,
               CAIRN_POLICY_VERSION, __DATE__, __TIME__);
    CAIRN_LOGI(TAG, "wake cause %d, free heap %u bytes",
               (int)esp_sleep_get_wakeup_cause(), (unsigned)ESP.getFreeHeap());
    log_partition_state();

    pinMode(CAIRN_PIN_LED, OUTPUT);
    digitalWrite(CAIRN_PIN_LED, HIGH);

    if (!try_mount_sd()) {
        /*
         * Not fatal any more, and that change matters more than it looks.
         *
         * This used to return and leave the device inert until someone
         * power-cycled it, so one marginal contact at ignition cost the whole
         * drive — recording nothing, which is the single outcome this firmware
         * exists to prevent. loop() now keeps retrying, so a slot that works
         * on the third attempt a minute later still captures the journey.
         */
        CAIRN_LOGE(TAG, "SD mount failed on CS=%d after %d attempts; will keep "
                        "retrying every %d ms. Check the card is seated.",
                   CAIRN_PIN_SD_CS, CAIRN_SD_MOUNT_ATTEMPTS,
                   CAIRN_SD_REMOUNT_RETRY_MS);
        g_mount_retry_at_ms = millis() + CAIRN_SD_REMOUNT_RETRY_MS;
        return;
    }

    bring_up_after_mount();
}

void loop()
{
    if (!g_running) {
        /*
         * Idle but alive. The log keeps flushing so the reason for not running
         * reaches the card, and the LED stays lit as a visible fault.
         */
        cairn_log_tick();

        /* Keep trying to mount. A marginal card contact that fails at boot
         * often succeeds moments later, and giving up permanently turns that
         * into a drive recorded nowhere. */
        if (g_mount_retry_at_ms != 0 &&
            (int32_t)(millis() - g_mount_retry_at_ms) >= 0) {
            if (try_mount_sd()) {
                CAIRN_LOGW(TAG, "SD mounted on a deferred retry; bringing the "
                                "capture path up now");
                g_mount_retry_at_ms = 0;
                bring_up_after_mount();
            } else {
                g_mount_retry_at_ms = millis() + CAIRN_SD_REMOUNT_RETRY_MS;
            }
        }

        delay(1000);
        return;
    }

    lifecycle_tick(&g_lifecycle);

#if CAIRN_PIDTEST
    pidtest_tick();
#endif

    /*
     * Sampling no longer happens here — the sensing task on core 0 owns it — so
     * this delay only sets how promptly facts are drained. 20 ms keeps the
     * queue shallow while leaving the radio and card tasks room to run.
     */
    delay(20);
}
