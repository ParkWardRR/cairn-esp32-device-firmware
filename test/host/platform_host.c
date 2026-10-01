/*
 * Host implementation of the platform shim.
 *
 * The clock is driven by the test rather than read from the wall clock, and the
 * generator is seeded, so a failing fault-injection run is reproducible from
 * its seed — the same rule the Rust fault matrix follows.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cairn_platform.h"

static uint64_t s_micros;
static uint64_t s_rng_state = 0x243F6A8885A308D3ULL;

uint32_t cairn_millis(void) { return (uint32_t)(s_micros / 1000); }
uint64_t cairn_micros(void) { return s_micros; }

void cairn_host_advance(uint32_t ms);
void cairn_host_seed(uint64_t seed);

void cairn_host_advance(uint32_t ms) { s_micros += (uint64_t)ms * 1000ULL; }

void cairn_host_seed(uint64_t seed)
{
    /* Never zero: the xorshift below would be stuck there. */
    s_rng_state = (seed != 0) ? seed : 0x243F6A8885A308D3ULL;
}

/* xorshift64*, adequate for generating distinct ids in a test and fully
 * determined by the seed. */
void cairn_random(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_rng_state ^= s_rng_state >> 12;
        s_rng_state ^= s_rng_state << 25;
        s_rng_state ^= s_rng_state >> 27;
        out[i] = (uint8_t)((s_rng_state * 0x2545F4914F6CDD1DULL) >> 56);
    }
}

void cairn_hw_unique_id(uint8_t out[6])
{
    /* Fixed, so the derived device id is stable across simulated reboots — the
     * property being relied on is exactly that it does not change. */
    static const uint8_t fake_mac[6] = { 0x7c, 0x9e, 0xbd, 0xfa, 0x7f, 0xf8 };
    memcpy(out, fake_mac, 6);
}
