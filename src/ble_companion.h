#ifndef BLE_COMPANION_H
#define BLE_COMPANION_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if CAIRN_BLE_COMPANION

bool ble_companion_begin(void);
bool ble_companion_connected(void);

void ble_companion_radio_off(void);
void ble_companion_radio_on(void);
void ble_companion_clear_bonds(void);

void ble_companion_notify_quality(uint8_t fix_type, uint8_t sats_used,
                                  uint16_t hdop_e2, uint32_t fix_age_ms);
void ble_companion_notify_status(void);

#else

static inline bool ble_companion_begin(void) { return true; }
static inline bool ble_companion_connected(void) { return false; }
static inline void ble_companion_radio_off(void) {}
static inline void ble_companion_radio_on(void) {}
static inline void ble_companion_clear_bonds(void) {}
static inline void ble_companion_notify_quality(uint8_t a, uint8_t b,
                                                uint16_t c, uint32_t d) {
    (void)a; (void)b; (void)c; (void)d;
}
static inline void ble_companion_notify_status(void) {}

#endif

#ifdef __cplusplus
}
#endif

#endif /* BLE_COMPANION_H */
