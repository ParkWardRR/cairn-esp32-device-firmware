/*
 * The handful of platform facilities the storage layer needs, so cairn_store
 * can be plain portable C and run under host tests.
 *
 * Deliberately tiny. Everything here is something the store genuinely cannot
 * do for itself: read a monotonic clock, get randomness it can trust, and
 * learn a hardware-unique value for the device id.
 */

#ifndef CAIRN_PLATFORM_H
#define CAIRN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Monotonic milliseconds and microseconds since boot. Never wall-clock: the
 * format's ordering truth is (boot_id, seq) and its durations are monotonic,
 * because UTC can be absent at boot and can jump afterwards.
 */
uint32_t cairn_millis(void);
uint64_t cairn_micros(void);

/*
 * Cryptographically usable randomness. On ESP32 this is esp_fill_random; under
 * test it is a seeded generator so a failing run is reproducible from its seed.
 */
void cairn_random(uint8_t *out, size_t len);

/* A hardware-unique value, hashed into the device id so that wiping NVS does
 * not change which vehicle the data came from. */
void cairn_hw_unique_id(uint8_t out[6]);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_PLATFORM_H */
