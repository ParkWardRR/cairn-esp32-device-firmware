#ifndef BLE_OFFLOAD_H
#define BLE_OFFLOAD_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * The NimBLE shim for BLE bundle offload (docs/ble-offload.md). The protocol is
 * lib/cairn_offload; this file only moves bytes between it and the radio.
 */

#if CAIRN_BLE_COMPANION

#ifdef __cplusplus
class NimBLEService;
extern "C++" {

/* Create the two characteristics on the companion service and start the offload
 * task. Call once, before the server starts. Returns false if the task could not
 * be created, in which case the capability bit must not be advertised. */
bool ble_offload_register(NimBLEService *svc);

/* Whether a drive is in progress (offload is refused while it is). Until a source
 * is set the answer is "yes", so a transfer is never served on a guess. */
void ble_offload_set_trip_source(bool (*trip_active)(void));

/* Connection events, from the server callbacks. They only enqueue: nothing here
 * does card or crypto work on NimBLE's small host-task stack. */
void ble_offload_on_connect(uint16_t conn_handle);
void ble_offload_on_disconnect(void);
void ble_offload_on_mtu(uint16_t att_mtu);

/* True while a phone is working the offload (an operation in progress, or recent
 * activity inside a bounded window). The lifecycle uses it to hold standby so the
 * radio is not switched off under a transfer. Bounded on purpose: a phone that
 * stays connected without ever finishing must not keep the dongle awake forever. */
bool ble_offload_active(void);

}
#endif

#else

static inline bool ble_offload_active(void) { return false; }
static inline void ble_offload_set_trip_source(bool (*trip_active)(void)) { (void)trip_active; }

#endif

#endif /* BLE_OFFLOAD_H */
