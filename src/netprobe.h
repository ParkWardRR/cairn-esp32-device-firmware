/*
 * Bench network probe (env:cairn-netprobe). Answers the hardware question this
 * repository once only recorded as unconfirmed: whether a cellular modem is
 * fitted in the BEE socket at all, and if so which module, whether the SIM is
 * readable, and whether it registers. (It also used to scan Wi-Fi and print
 * each SSID with its exact length; the Wi-Fi stack is deleted, so that half
 * went with it.)
 *
 * docs/flashing-and-testing.md records GPIO 27 (BEE_PWR) LOW and "modem not
 * initializing", and README says whether this unit carries one is unconfirmed.
 * Neither is evidence of absence: the firmware never powered the socket. This
 * build does, reads the module's own replies, and prints them verbatim.
 *
 * It runs before the card is mounted and halts instead of capturing, so it
 * cannot disturb a bundle. It writes nothing to NVS and burns no eFuse.
 */

#ifndef CAIRN_NETPROBE_H
#define CAIRN_NETPROBE_H

void netprobe_run(void);

#endif /* CAIRN_NETPROBE_H */
