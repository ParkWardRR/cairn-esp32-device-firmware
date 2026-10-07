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

    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
