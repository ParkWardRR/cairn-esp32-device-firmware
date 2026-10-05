/* ESP32 implementation of the platform shim. Device only. */

#ifdef ARDUINO

#include "cairn_platform.h"

#include <Arduino.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>

uint32_t cairn_millis(void) { return millis(); }

uint64_t cairn_micros(void) { return (uint64_t)esp_timer_get_time(); }

void cairn_random(uint8_t *out, size_t len) { esp_fill_random(out, len); }

/* The hardware RNG, for keys and nonces. Never derived from anything. */
void cairn_rng_fill(uint8_t *out, size_t len) { esp_fill_random(out, len); }

void cairn_hw_unique_id(uint8_t out[6])
{
    /* The efuse MAC is burned at manufacture, so it survives an NVS wipe and a
     * firmware replacement. */
    esp_read_mac(out, ESP_MAC_WIFI_STA);
}

#endif /* ARDUINO */
