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

/*
 * The other way a bond goes wrong, and the one that locked the dongle out of BLE entirely.
 *
 * Bonding is symmetric and the halves can get out of step without the pairing configuration
 * changing at all: the phone is restored, replaced or has Bluetooth reset, and arrives wanting
 * to pair afresh while this side still holds an LTK for its address. Pairing then fails — and
 * because the disconnect that follows is OUR OWN (`onAuthenticationComplete` disconnecting an
 * unencrypted link, which comes back as reason BLE_HS_ERR_HCI_BASE + 0x16, "terminated by local
 * host"), the peer-lost-keys recovery in `onDisconnect` never fires. With one bond slot and no
 * console command to clear it, every retry failed identically and reflashing with a changed
 * passkey was the only escape: exactly the dead end that recovery was added to remove.
 *
 * Dropping the bond on *any* failed pairing is not the answer either. A bond is what keeps a
 * stranger from reading trip metadata, and the shipping pairing is Just Works, so a blanket rule
 * would let anyone in range evict the owner's bond and pair in its place.
 *
 * So recovery is allowed only inside the window this device already treats as trusted: shortly
 * after boot, and never mid-trip. That is the rule the provisioning console follows, resting on
 * the same physical fact — whoever can power-cycle the dongle is in the car. The cost is that a
 * replaced phone needs one ignition cycle before it can pair, which the owner does anyway; the
 * gain is that an attacker must be present at boot, must find a bond already there to evict, and
 * still cannot disturb a recording.
 *
 * Pure, so the host tests exercise the decision the device actually makes.
 */
typedef struct {
    int      bonds_held;  /* bonds currently stored; 0 means there is nothing to recover from */
    uint32_t uptime_ms;   /* milliseconds since boot, at the moment pairing failed */
    int      trip_active; /* non-zero while capture is running */
} cairn_ble_pair_failure_t;

/* Why a failure was or was not acted on. Logged, so the reason survives in the SD log. */
typedef enum {
    CAIRN_BLE_DROP = 0,            /* drop the stale bond; the next attempt pairs cleanly */
    CAIRN_BLE_KEEP_NO_BOND,        /* nothing stored: a stranger failing to pair is not recovery */
    CAIRN_BLE_KEEP_TRIP_ACTIVE,    /* never disturb a recording */
    CAIRN_BLE_KEEP_WINDOW_CLOSED   /* too long after boot: recovery needs an ignition cycle */
} cairn_ble_pair_failure_action_t;

static inline cairn_ble_pair_failure_action_t
cairn_ble_pair_failure_action(const cairn_ble_pair_failure_t *f, uint32_t boot_window_ms)
{
    if (f == 0 || f->bonds_held <= 0)  return CAIRN_BLE_KEEP_NO_BOND;
    if (f->trip_active)                return CAIRN_BLE_KEEP_TRIP_ACTIVE;
    /* Strictly greater: a failure exactly at the boundary is still inside the window. */
    if (f->uptime_ms > boot_window_ms) return CAIRN_BLE_KEEP_WINDOW_CLOSED;
    return CAIRN_BLE_DROP;
}

/* What to tell the log. Never returns NULL. */
static inline const char *cairn_ble_pair_failure_reason(cairn_ble_pair_failure_action_t a)
{
    switch (a) {
    case CAIRN_BLE_DROP:               return "dropping the stale bond so the next connection pairs cleanly";
    case CAIRN_BLE_KEEP_TRIP_ACTIVE:   return "keeping the bond: a trip is recording";
    case CAIRN_BLE_KEEP_WINDOW_CLOSED: return "keeping the bond: past the post-boot window, so power-cycle the dongle and retry";
    case CAIRN_BLE_KEEP_NO_BOND:       break;
    }
    return "no bond is stored, so there is nothing to drop";
}

#endif
