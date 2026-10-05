/* NVS backend, device only. */

#ifdef ARDUINO

#include "cairn_kv.h"

#include <Preferences.h>
#include <string.h>
#include <nvs.h>

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

bool cairn_kv_erase_platform_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) != ESP_OK) return false;

    size_t len = 0;
    bool had = (nvs_get_str(h, "sta.pswd", nullptr, &len) == ESP_OK) ||
               (nvs_get_str(h, "sta.ssid", nullptr, &len) == ESP_OK) ||
               (nvs_get_blob(h, "sta.pswd", nullptr, &len) == ESP_OK) ||
               (nvs_get_blob(h, "sta.ssid", nullptr, &len) == ESP_OK);

    /* The whole namespace: the stack only ever stores station and AP settings
     * here, and none of it is used by firmware that has no Wi-Fi. */
    bool ok = (nvs_erase_all(h) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok && had;
}

bool cairn_kv_scrub_freed(void)
{
    if (!s_open) return false;

    /*
     * 64 writes of 1.5 KB is roughly five times the 20 KB partition, so every
     * page is filled, garbage-collected and erased more than once. The old copy
     * of a blob is deleted as each new one lands, which is what makes the pages
     * collectable. The content is a counter pattern, never anything secret.
     */
    static uint8_t buf[1500];
    bool ok = true;
    for (int i = 0; i < 64 && ok; i++) {
        memset(buf, (uint8_t)(i * 7 + 1), sizeof(buf));
        ok = (s_prefs.putBytes("scrubtmp", buf, sizeof(buf)) == sizeof(buf));
    }
    if (s_prefs.isKey("scrubtmp")) s_prefs.remove("scrubtmp");
    return ok;
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
