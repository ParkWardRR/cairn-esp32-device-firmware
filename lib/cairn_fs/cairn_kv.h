/*
 * Small persistent key-value store for device identity: NVS on the device, a
 * file on the host.
 *
 * Separated from cairn_fs because the device backings are genuinely different —
 * identity lives in internal flash so it survives a card swap, while bundles
 * live on the card. Keeping them distinct also means a host test can reset
 * identity without touching the simulated card, which is how "a new signing key
 * forces re-enrolment" becomes testable.
 */

#ifndef CAIRN_KV_H
#define CAIRN_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool cairn_kv_begin(void);
void cairn_kv_end(void);

/* Returns false when the key is absent or its stored length differs — a
 * truncated key is not a usable key. */
bool cairn_kv_get_blob(const char *key, void *out, size_t len);
bool cairn_kv_set_blob(const char *key, const void *data, size_t len);

/*
 * Variable-length read, for values whose size is not fixed (PEM credentials).
 * Copies at most cap bytes and reports the stored length in *len, which may
 * exceed cap — the caller then knows the value did not fit and must not treat a
 * truncated PEM as a credential. Returns false when the key is absent.
 */
bool cairn_kv_get_blob_var(const char *key, void *out, size_t cap, size_t *len);

/* Remove a key. Absent is success: the postcondition is "not there". */
bool cairn_kv_erase(const char *key);

/*
 * Overwrite the flash that held deleted values.
 *
 * Erasing a key only marks its entry deleted: NVS physically wipes a page when
 * it garbage-collects it, so a deleted private key can sit readable in the chip
 * for as long as nothing forces a collection. This churns scratch data through
 * the store until every page has been recycled, which erases the old bytes.
 * Live values survive (NVS copies them forward); it costs about a second and some
 * flash wear, so call it once, not on every boot. Returns false if it could not
 * complete, in which case deleted values may still be recoverable.
 *
 * **Best effort, not a guarantee.** NVS collects the page with the most deleted
 * entries, so a page where a few deleted values sit among live ones can survive
 * the churn. Measured on the car's dongle: a PEM private key and certificate (large
 * blobs, so their pages were mostly deleted) were overwritten; a Wi-Fi password in
 * the Wi-Fi stack's own namespace was not. Only flash encryption (ROADMAP Phase 24)
 * makes residue in the chip harmless.
 */
bool cairn_kv_scrub_freed(void);

/*
 * Erase the Wi-Fi credentials the ESP32 Wi-Fi stack persists on its own.
 *
 * Firmware that called WiFi.begin() left the SSID and password in the stack's NVS
 * namespace (nvs.net80211), outside this module's namespace and so invisible to
 * every other call here. The current firmware has no Wi-Fi to use them, and a
 * password sitting in flash is a liability. Returns true if any were present.
 * A no-op on the host.
 */
bool cairn_kv_erase_platform_wifi(void);

uint32_t cairn_kv_get_u32(const char *key, uint32_t fallback);
bool     cairn_kv_set_u32(const char *key, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_KV_H */
