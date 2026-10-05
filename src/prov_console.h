/*
 * The USB console half of provisioning: reads lines from Serial, feeds the
 * portable protocol in lib/cairn_prov, and supplies the device-specific
 * operations (NVS, RNG, identity). See docs/device-provisioning.md.
 */

#ifndef CAIRN_PROV_CONSOLE_H
#define CAIRN_PROV_CONSOLE_H

#include <stdbool.h>

/* Call once after cairn_log_init(). */
void prov_console_begin(void);

/*
 * Call from loop() on every pass, mounted card or not — provisioning must work
 * on a device whose card is missing, since that is exactly when a technician is
 * most likely to be standing at it with a cable.
 *
 * `trip_active` is true whenever the controller is anywhere past Idle.
 */
void prov_console_poll(bool trip_active);

#endif /* CAIRN_PROV_CONSOLE_H */
