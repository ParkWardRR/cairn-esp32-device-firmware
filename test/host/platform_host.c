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

/*
 * The key/nonce stream, injectable.
 *
 * By default a SplitMix64 stream of its own, seeded apart from cairn_random so
 * that reseeding one never replays the other. A test can reset it to a fixed
 * seed (to make two runs seal identical bytes) or force it to repeat a short
 * cycle — which is how the nonce row proves it would notice a reused nonce,
 * rather than passing because nothing could ever repeat.
 */
static uint64_t s_crypto_state = 0x9E3779B97F4A7C15ULL;
static size_t   s_crypto_cycle;  /* 0: no forced repeat */
static size_t   s_crypto_drawn;
static uint64_t s_crypto_seed = 0x9E3779B97F4A7C15ULL;

void cairn_host_rng_seed(uint64_t seed);
void cairn_host_rng_force_cycle(size_t bytes);

void cairn_host_rng_seed(uint64_t seed)
{
    s_crypto_seed  = seed;
    s_crypto_state = seed;
    s_crypto_drawn = 0;
    s_crypto_cycle = 0;
}

void cairn_host_rng_force_cycle(size_t bytes)
{
    s_crypto_state = s_crypto_seed;
    s_crypto_drawn = 0;
    s_crypto_cycle = bytes;
}

void cairn_rng_fill(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s_crypto_cycle > 0 && s_crypto_drawn % s_crypto_cycle == 0) {
            s_crypto_state = s_crypto_seed; /* replay from the top */
        }
        s_crypto_drawn++;

        uint64_t z = (s_crypto_state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        out[i] = (uint8_t)(z ^ (z >> 31));
    }
}

void cairn_hw_unique_id(uint8_t out[6])
{
    /* Fixed, so the derived device id is stable across simulated reboots — the
     * property being relied on is exactly that it does not change. */
    static const uint8_t fake_mac[6] = { 0x7c, 0x9e, 0xbd, 0xfa, 0x7f, 0xf8 };
    memcpy(out, fake_mac, 6);
}
