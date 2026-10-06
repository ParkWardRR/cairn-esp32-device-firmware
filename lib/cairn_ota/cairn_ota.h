/*
 * Secure OTA. See docs/ota.md for the protocol and the ordering argument.
 *
 * A bad update is the most destructive thing that can happen to this system —
 * worse than a corrupt bundle, because a bricked device captures nothing and
 * cannot report that it is bricked. So this module is written around what has
 * to be true before a device is allowed to replace itself, not around
 * downloading. There is no network on this device, so the image arrives from
 * the enrolled phone app over BLE (contracts/ble/v1/offload.md); the gate below is
 * independent of how the bytes got here.
 */

#ifndef CAIRN_OTA_H
#define CAIRN_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAIRN_OTA_OK = 0,
    CAIRN_OTA_DISABLED,          /* no update key pinned at build time */
    CAIRN_OTA_UP_TO_DATE,
    CAIRN_OTA_BLOCKED,           /* a precondition is not met; try later */
    CAIRN_OTA_DESCRIPTOR_FAILED, /* parse or signature */
    CAIRN_OTA_REFUSED,           /* older image, or min_firmware_version */
    CAIRN_OTA_IMAGE_MISMATCH,    /* what reached flash is not what was signed */
    CAIRN_OTA_FLASH_FAILED,
} cairn_ota_result_t;

const char *cairn_ota_result_name(cairn_ota_result_t r);

/*
 * Why an update was not attempted. All of these are reasons to try later rather
 * than errors, and they are reported individually because "blocked" without a
 * reason is impossible to act on.
 */
typedef struct {
    bool unreceipted_bundles; /* data exists only on this card */
    bool not_parked;          /* a reboot mid-trip loses the open capture */
    bool supply_unhealthy;    /* a write that loses power leaves a junk slot */
    bool no_update_key;

    uint32_t pending_bundles;
    uint16_t battery_mv;
} cairn_ota_block_t;

/* True when every precondition holds. Fills `out` either way. */
bool cairn_ota_preconditions(bool parked, uint16_t battery_mv,
                             cairn_ota_block_t *out);

void cairn_ota_describe_block(const cairn_ota_block_t *b, char *out, size_t cap);

/*
 * Compare two firmware version strings of the form "<name>-v<major>.<minor>.<patch>".
 *
 * Returns <0, 0 or >0. An unparseable version on either side is reported via
 * `*ok` as false, and the caller must then refuse the update: guessing an
 * ordering is how a device installs something older than itself.
 */
int cairn_ota_version_compare(const char *a, const char *b, bool *ok);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_OTA_H */
