/*
 * A fingerprint of the BLE pairing configuration, so a bond is thrown away only when the thing
 * it was made under has changed.
 *
 * Why not clear bonds on every boot: a phone keeps its half of the bond. When the dongle forgets
 * its half at each ignition, the phone's next encrypted request is answered "key missing", and an
 * iPhone then reports peerRemovedPairingInformation and stays unusable until the person forgets
 * the dongle in Settings > Bluetooth, on every drive. When the pairing mode or passkey changes
 * (a reflash), the old bond really is wrong and must go; that is what this detects.
 *
 * Header-only and pure so the host tests exercise exactly what the device runs.
 */
#ifndef CAIRN_BLESEC_H
#define CAIRN_BLESEC_H

#include <stdint.h>

/* FNV-1a over a version tag, the auth-request flags, the IO capability and the passkey (LE).
 * Never returns 0, which NVS reads back as "nothing stored yet". */
static inline uint32_t cairn_ble_security_fingerprint(uint8_t authreq, uint8_t io_cap, uint32_t passkey)
{
    const uint8_t bytes[] = {
        'C', 'B', 'S', '1', authreq, io_cap,
        (uint8_t)(passkey & 0xFF), (uint8_t)((passkey >> 8) & 0xFF),
        (uint8_t)((passkey >> 16) & 0xFF), (uint8_t)((passkey >> 24) & 0xFF),
    };
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof bytes; i++) { h ^= bytes[i]; h *= 16777619u; }
    return h ? h : 1u;
}

/* True when bonds made under `stored` must be cleared to pair under `current`. */
static inline int cairn_ble_bonds_stale(uint32_t stored, uint32_t current)
{
    return stored != current;
}

#endif
