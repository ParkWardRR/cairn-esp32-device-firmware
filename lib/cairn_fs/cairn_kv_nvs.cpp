/* NVS backend, device only. */

#ifdef ARDUINO

#include "cairn_kv.h"

#include <Preferences.h>

static Preferences s_prefs;
static bool        s_open = false;

bool cairn_kv_begin(void)
{
    if (s_open) return true;
    s_open = s_prefs.begin("cairn", false);
    return s_open;
}

void cairn_kv_end(void)
{
    if (!s_open) return;
    s_prefs.end();
    s_open = false;
}

bool cairn_kv_get_blob(const char *key, void *out, size_t len)
{
    if (!s_open) return false;

    /* A stored length that differs means the key is not what the caller
     * expects; treating it as usable would hand back a partly initialized
     * signing seed. */
    if (s_prefs.getBytesLength(key) != len) return false;

    return s_prefs.getBytes(key, out, len) == len;
}

bool cairn_kv_set_blob(const char *key, const void *data, size_t len)
{
    if (!s_open) return false;
    return s_prefs.putBytes(key, data, len) == len;
}

bool cairn_kv_get_blob_var(const char *key, void *out, size_t cap, size_t *len)
{
    if (!s_open) return false;
    size_t n = s_prefs.getBytesLength(key);
    if (n == 0) return false;
    *len = n;
    if (n > cap) return true;
    return s_prefs.getBytes(key, out, n) == n;
}

bool cairn_kv_erase(const char *key)
{
    if (!s_open) return false;
    if (!s_prefs.isKey(key)) return true;
    return s_prefs.remove(key);
}

uint32_t cairn_kv_get_u32(const char *key, uint32_t fallback)
{
    if (!s_open) return fallback;
    return s_prefs.getUInt(key, fallback);
}

bool cairn_kv_set_u32(const char *key, uint32_t value)
{
    if (!s_open) return false;
    return s_prefs.putUInt(key, value) > 0;
}

#endif /* ARDUINO */
