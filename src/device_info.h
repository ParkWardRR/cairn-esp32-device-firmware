#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <stddef.h>
#include <stdint.h>

/*
 * The live state behind the BLE DEVICE_INFO characteristic (contracts/ble/v1/device-info.md).
 * The encoding is lib/cairn_devinfo; this file only gathers what it reports.
 *
 * Identity and storage are cached by the lifecycle as they change and read here under a
 * lock, because the characteristic is read on the BLE host task, which must not touch the
 * card, NVS or a signing key.
 */

/* From lifecycle_begin(), once the identity is loaded. `assigned` is true when a vehicle
 * assignment is provisioned (enrol_state 2); with none the state is reported as 0, because
 * the dongle cannot tell "enrolled, no vehicle yet" from "not enrolled". */
void device_info_set_identity(const uint8_t device_id[16], const uint8_t public_key[32],
                              uint32_t storage_key_version, bool assigned);

/* Storage state: 0 no card, 1 ok, 2 read-only, 3 error. `free_mib` 0xFFFFFFFF = unknown. */
void device_info_set_storage(uint8_t state, uint32_t free_mib);

/* Sealed bundles awaiting a receipt; the lifecycle refreshes it when it changes. */
void device_info_set_pending(uint32_t pending_bundles);

/* The capability bits this build really has. Set once, when the service is registered. */
void device_info_set_capabilities(uint32_t bits);
uint32_t device_info_capabilities(void);

/* Build the DEVICE_INFO value. Returns its length (at most 512), 0 on failure. Not
 * reentrant: it uses one static work area, and the BLE host task is its only caller. */
size_t device_info_build(uint8_t *out, size_t cap);

#endif /* DEVICE_INFO_H */
