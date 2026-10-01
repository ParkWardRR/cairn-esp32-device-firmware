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

uint32_t cairn_kv_get_u32(const char *key, uint32_t fallback);
bool     cairn_kv_set_u32(const char *key, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_KV_H */
