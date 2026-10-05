/* Legacy credential slots in NVS. See cairn_prov.h. */

#include <stdbool.h>
#include <stddef.h>

#include "../cairn_fs/cairn_kv.h"
#include "cairn_prov.h"

#define KV_SLOT "pv_slot"
/* v2: v1 scrubbed the flash but left the Wi-Fi stack's own copy of the password. */
#define KV_SCRUBBED "scrub_v2"

static const char *const FIELDS[] = { "ssid", "pass", "crt", "key" };

static bool erase_slot(int slot)
{
    bool any = false;
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        /* pv0_ssid, pv1_crt, ... — the names the old two-slot store used. */
        char k[16];
        size_t n = 0;
        k[n++] = 'p'; k[n++] = 'v'; k[n++] = (char)('0' + slot); k[n++] = '_';
        for (const char *f = FIELDS[i]; *f; f++) k[n++] = *f;
        k[n] = '\0';

        size_t len = 0;
        uint8_t probe;
        if (cairn_kv_get_blob_var(k, &probe, 0, &len) && len > 0) any = true;
        cairn_kv_erase(k);
    }
    return any;
}

bool cairn_prov_erase_legacy_credentials(void)
{
    if (!cairn_kv_begin()) return false;

    bool any = erase_slot(0);
    any = erase_slot(1) || any;

    /* The slot selector goes last: with it gone nothing can point at a slot. */
    if (cairn_kv_get_u32(KV_SLOT, 0xFFFFFFFFu) != 0xFFFFFFFFu) any = true;
    cairn_kv_erase(KV_SLOT);

    /* The Wi-Fi stack keeps its own copy of the SSID and password. */
    if (cairn_kv_erase_platform_wifi()) any = true;

    /*
     * Make the deleted bytes unreadable. Once: on a unit where the legacy entries
     * were erased by an earlier boot there is nothing left to detect, but the
     * bytes are still in flash, so the marker, not the presence of entries,
     * decides. The marker is only set after a scrub that completed.
     */
    if (any || cairn_kv_get_u32(KV_SCRUBBED, 0) == 0) {
        any = true;
        if (cairn_kv_scrub_freed()) (void)cairn_kv_set_u32(KV_SCRUBBED, 1);
    }
    return any;
}
