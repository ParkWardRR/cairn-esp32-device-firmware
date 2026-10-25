/*
 * The cars one dongle moves between, and which of them it is plugged into.
 *
 * A bundle names its vehicle and the assignment it was captured under, and until
 * 2026-10-25 a device held exactly one. Moving the dongle from the 428i to the M240i
 * recorded the M240i's drives under the 428i until someone reprovisioned it over USB.
 *
 * A garage is up to CAIRN_GARAGE_SLOTS assignments, one per car, provisioned together.
 * The device boots assuming the car it was last in. When the ECU gives up its VIN, the
 * VIN says which car this is:
 *
 *   1. a slot that has seen this VIN before (its hash is learned on first match), else
 *   2. the one slot, not yet bound to a VIN, whose engine the VIN names (BMW VDS
 *      positions 4-8, engines/<id>.yaml engine_codes) -- two such slots is ambiguous
 *      and picks nothing, so two cars with one engine need their VINs learned one at a
 *      time, else
 *   3. nothing: the device stays with the car it assumed. It never guesses.
 *
 * Unplugging a dongle powers it off, so a car swap is always a new boot; and the OBD
 * port is permanently powered, so a boot never spans two cars. The server files a boot
 * whose bundles name two cars under the last one (the bundles sealed before the VIN
 * arrived carry the assumed car).
 *
 * Portable C, host-tested (test/host/garage_test.c). Only a truncated SHA-256 of the VIN
 * is stored: the device keeps no VIN in flash.
 */
#ifndef CAIRN_GARAGE_H
#define CAIRN_GARAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAIRN_GARAGE_SLOTS      4
#define CAIRN_GARAGE_ENGINE_MAX 24   /* engine profile id, NUL included */
#define CAIRN_GARAGE_VIN_HASH   8

typedef struct {
    bool     used;
    uint8_t  vehicle_id[16];
    uint8_t  assignment_id[16];
    char     engine_id[CAIRN_GARAGE_ENGINE_MAX]; /* "" = not known */
    bool     has_vin;
    uint8_t  vin_hash[CAIRN_GARAGE_VIN_HASH];
} cairn_garage_slot_t;

typedef struct {
    cairn_garage_slot_t slot[CAIRN_GARAGE_SLOTS];
} cairn_garage_t;

typedef enum {
    CAIRN_GARAGE_NO_MATCH = 0,
    CAIRN_GARAGE_BY_VIN,
    CAIRN_GARAGE_BY_ENGINE,   /* the caller should learn the VIN into the slot */
    CAIRN_GARAGE_AMBIGUOUS,
} cairn_garage_how_t;

/* Truncated SHA-256 of the 17-character VIN. */
void cairn_garage_vin_hash(const char *vin, uint8_t out[CAIRN_GARAGE_VIN_HASH]);

/* The slot for this car, or -1. `engine_id` is what the VIN names (NULL if nothing). */
int cairn_garage_pick(const cairn_garage_t *g, const uint8_t vin_hash[CAIRN_GARAGE_VIN_HASH],
                      const char *engine_id, cairn_garage_how_t *how);

/* The slot holding this assignment, or -1. */
int cairn_garage_find_assignment(const cairn_garage_t *g, const uint8_t assignment_id[16]);

/*
 * Replace `g` with `next`, keeping the VIN a slot had learned when the same assignment
 * is provisioned again (reprovisioning one car must not forget the others' VINs).
 */
void cairn_garage_replace(cairn_garage_t *g, const cairn_garage_t *next);

/* Fixed little serialisation for NVS: version byte, then each slot. */
#define CAIRN_GARAGE_SLOT_BYTES (1 + 16 + 16 + CAIRN_GARAGE_ENGINE_MAX + 1 + CAIRN_GARAGE_VIN_HASH)
#define CAIRN_GARAGE_BLOB_BYTES (1 + CAIRN_GARAGE_SLOTS * CAIRN_GARAGE_SLOT_BYTES)
void cairn_garage_encode(const cairn_garage_t *g, uint8_t out[CAIRN_GARAGE_BLOB_BYTES]);
bool cairn_garage_decode(const uint8_t in[CAIRN_GARAGE_BLOB_BYTES], cairn_garage_t *g);

const char *cairn_garage_how_name(cairn_garage_how_t how);

#ifdef __cplusplus
}
#endif

#endif
