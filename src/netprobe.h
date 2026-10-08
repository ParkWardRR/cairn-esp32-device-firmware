/*
 * Bench network probe (env:cairn-netprobe). Answers two hardware questions this
 * repository has only ever recorded as unconfirmed:
 *
 *   1. Which Wi-Fi networks this unit can actually see, with signal and channel.
 *   2. Whether a cellular modem is fitted in the BEE socket at all, and if so
 *      which module, whether the SIM is readable, and whether it registers.
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
