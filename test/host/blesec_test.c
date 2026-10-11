/*
 * The BLE pairing fingerprint: stable for one configuration, different for any change that
 * makes an old bond wrong, and never the "nothing stored" value.
 */

#include <stdio.h>

#include "cairn_blesec.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [blesec] %s\n", name); }         \
        else      { g_fail++; printf("  FAIL  [blesec] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

int main(void)
{
    const uint8_t bond_sc = 0x01 | 0x08, mitm_bond_sc = 0x01 | 0x04 | 0x08;
    const uint8_t none = 3, display = 0;

    const uint32_t base = cairn_ble_security_fingerprint(bond_sc, none, 123456);

    CHECK("the same configuration fingerprints the same, so a normal reboot keeps its bond",
          cairn_ble_security_fingerprint(bond_sc, none, 123456) == base
          && !cairn_ble_bonds_stale(base, cairn_ble_security_fingerprint(bond_sc, none, 123456)));
    CHECK("a new passkey invalidates the old bond",
          cairn_ble_bonds_stale(base, cairn_ble_security_fingerprint(bond_sc, none, 654321)));
    CHECK("switching to MITM protection invalidates the old bond",
          cairn_ble_bonds_stale(base, cairn_ble_security_fingerprint(mitm_bond_sc, none, 123456)));
    CHECK("switching the IO capability invalidates the old bond",
          cairn_ble_bonds_stale(base, cairn_ble_security_fingerprint(bond_sc, display, 123456)));
    CHECK("a device that stored nothing (first boot) clears nothing it does not have, then records",
          cairn_ble_bonds_stale(0u, base));

    /* Never 0, whatever the inputs: 0 is what an empty NVS reads back as. */
    int zero = 0;
    for (unsigned a = 0; a < 256; a++)
        for (unsigned io = 0; io < 6; io++)
            if (cairn_ble_security_fingerprint((uint8_t)a, (uint8_t)io, a * 4099u + io) == 0) zero = 1;
    CHECK("no input fingerprints to zero", !zero);

    /*
     * Recovery from a failed pairing (#44). The dongle disconnects an unencrypted link itself,
     * so the reason that comes back is "terminated by local host" and the peer-lost-keys path in
     * onDisconnect never sees it. These are the guards that decide whether to self-heal.
     */
    const uint32_t win = 60000u;
    cairn_ble_pair_failure_t f;

    f.bonds_held = 1; f.uptime_ms = 5000; f.trip_active = 0;
    CHECK("a stale bond is dropped when pairing fails soon after boot, outside a trip",
          cairn_ble_pair_failure_action(&f, win) == CAIRN_BLE_DROP);

    f.bonds_held = 1; f.uptime_ms = win; f.trip_active = 0;
    CHECK("a failure exactly at the window boundary is still inside it",
          cairn_ble_pair_failure_action(&f, win) == CAIRN_BLE_DROP);

    f.bonds_held = 1; f.uptime_ms = win + 1; f.trip_active = 0;
    CHECK("past the window the bond is kept, so a stranger cannot evict it at will",
          cairn_ble_pair_failure_action(&f, win) == CAIRN_BLE_KEEP_WINDOW_CLOSED);

    f.bonds_held = 1; f.uptime_ms = 5000; f.trip_active = 1;
    CHECK("a recording is never disturbed, even inside the window",
          cairn_ble_pair_failure_action(&f, win) == CAIRN_BLE_KEEP_TRIP_ACTIVE);

    f.bonds_held = 0; f.uptime_ms = 5000; f.trip_active = 0;
    CHECK("with no bond stored there is nothing to drop: a stranger failing to pair is not recovery",
          cairn_ble_pair_failure_action(&f, win) == CAIRN_BLE_KEEP_NO_BOND);

    f.bonds_held = -1; f.uptime_ms = 0; f.trip_active = 0;
    CHECK("a negative bond count (the count could not be read) drops nothing",
          cairn_ble_pair_failure_action(&f, win) == CAIRN_BLE_KEEP_NO_BOND);

    CHECK("a null failure record drops nothing",
          cairn_ble_pair_failure_action(0, win) == CAIRN_BLE_KEEP_NO_BOND);

    /* The trip guard outranks the window guard: both true must still report the trip, because
     * that is the reason a person needs to see in the log. */
    f.bonds_held = 1; f.uptime_ms = win + 10000; f.trip_active = 1;
    CHECK("when a trip is running and the window is shut, the trip is the reason given",
          cairn_ble_pair_failure_action(&f, win) == CAIRN_BLE_KEEP_TRIP_ACTIVE);

    int reasons_ok = 1;
    for (int a = CAIRN_BLE_DROP; a <= CAIRN_BLE_KEEP_WINDOW_CLOSED; a++) {
        const char *r = cairn_ble_pair_failure_reason((cairn_ble_pair_failure_action_t)a);
        if (r == 0 || r[0] == 0) reasons_ok = 0;
    }
    CHECK("every action has a reason to log", reasons_ok);

    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
