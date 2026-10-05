#include "cairn_ota.h"

#include <stdio.h>
#include <string.h>

#include "cairn_format.h"
#include "cairn_store.h"
#include "config.h"

const char *cairn_ota_result_name(cairn_ota_result_t r)
{
    switch (r) {
    case CAIRN_OTA_OK:                 return "OK";
    case CAIRN_OTA_DISABLED:           return "DISABLED";
    case CAIRN_OTA_UP_TO_DATE:         return "UP_TO_DATE";
    case CAIRN_OTA_BLOCKED:            return "BLOCKED";
    case CAIRN_OTA_DESCRIPTOR_FAILED:  return "DESCRIPTOR_FAILED";
    case CAIRN_OTA_REFUSED:            return "REFUSED";
    case CAIRN_OTA_IMAGE_MISMATCH:     return "IMAGE_MISMATCH";
    case CAIRN_OTA_FLASH_FAILED:       return "FLASH_FAILED";
    default:                           return "UNKNOWN";
    }
}

/* ── version comparison ───────────────────────────────────────────────────── */

/* Parse the trailing "v<major>.<minor>.<patch>" of a version string. */
static bool parse_version(const char *s, unsigned out[3])
{
    const char *v = strrchr(s, 'v');
    if (v == NULL) return false;
    v++;

    out[0] = out[1] = out[2] = 0;
    int consumed = 0;
    if (sscanf(v, "%u.%u.%u%n", &out[0], &out[1], &out[2], &consumed) != 3) {
        return false;
    }

    /* Trailing junk means this is not a version this build understands, and
     * guessing would be worse than refusing. A pre-release suffix is the common
     * case and is deliberately not accepted. */
    return v[consumed] == '\0';
}

int cairn_ota_version_compare(const char *a, const char *b, bool *ok)
{
    unsigned va[3], vb[3];

    if (!parse_version(a, va) || !parse_version(b, vb)) {
        if (ok != NULL) *ok = false;
        return 0;
    }
    if (ok != NULL) *ok = true;

    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return (va[i] < vb[i]) ? -1 : 1;
    }
    return 0;
}

/* ── preconditions ────────────────────────────────────────────────────────── */

/*
 * "Externally powered" needs interpreting, because taken literally it
 * contradicts "parked": a parked car's engine is off, so there is no alternator
 * and the rail sits near 12.4 V rather than 13.5 V. Requiring a charging supply
 * would mean updates only while driving, which is the one time an update must
 * not happen.
 *
 * So the condition is a *healthy* supply rather than a charging one: comfortably
 * above the low-battery threshold, so a multi-second flash write cannot be what
 * flattens it. The A/B layout already makes a lost write survivable; this is
 * about not taking the risk needlessly.
 */
#define OTA_MIN_SUPPLY_MV 12200

bool cairn_ota_preconditions(bool parked, uint16_t battery_mv,
                             cairn_ota_block_t *out)
{
    memset(out, 0, sizeof(*out));
    out->battery_mv = battery_mv;

#if !CAIRN_OTA_AVAILABLE
    out->no_update_key = true;
#endif

    uint32_t pending = 0;
    uint64_t bytes = 0;
    if (cairn_store_pending_stats(&pending, &bytes)) {
        out->pending_bundles = pending;

        /*
         * A bundle with no receipt exists only here. If the new image fails to
         * boot and rollback also fails, that data is gone — trading something
         * irreplaceable for something that can wait.
         */
        out->unreceipted_bundles = pending > 0;
    }

    out->not_parked = !parked;

    /* An unknown voltage is treated as unhealthy. Updating on the strength of a
     * reading the device could not take is exactly the wrong direction. */
    out->supply_unhealthy =
        (battery_mv == CAIRN_U16_UNKNOWN) || (battery_mv < OTA_MIN_SUPPLY_MV);

    return !out->no_update_key && !out->unreceipted_bundles &&
           !out->not_parked && !out->supply_unhealthy;
}

void cairn_ota_describe_block(const cairn_ota_block_t *b, char *out, size_t cap)
{
    size_t used = 0;
    out[0] = '\0';

    const char *reasons[4] = { NULL, NULL, NULL, NULL };
    char        pending_buf[64];
    char        supply_buf[64];
    int         n = 0;

    if (b->no_update_key) {
        reasons[n++] = "no update key is pinned (OTA is disabled)";
    }
    if (b->unreceipted_bundles) {
        snprintf(pending_buf, sizeof(pending_buf),
                 "%u bundle(s) await a receipt", (unsigned)b->pending_bundles);
        reasons[n++] = pending_buf;
    }
    if (b->not_parked) reasons[n++] = "a trip is in progress";
    if (b->supply_unhealthy) {
        if (b->battery_mv == CAIRN_U16_UNKNOWN) {
            snprintf(supply_buf, sizeof(supply_buf), "supply voltage is unknown");
        } else {
            snprintf(supply_buf, sizeof(supply_buf), "supply is %u mV, below %d mV",
                     (unsigned)b->battery_mv, OTA_MIN_SUPPLY_MV);
        }
        reasons[n++] = supply_buf;
    }

    if (n == 0) {
        snprintf(out, cap, "all preconditions met");
        return;
    }

    for (int i = 0; i < n; i++) {
        int w = snprintf(out + used, cap - used, "%s%s", (i > 0) ? "; " : "",
                         reasons[i]);
        if (w < 0 || (size_t)w >= cap - used) return;
        used += (size_t)w;
    }
}

