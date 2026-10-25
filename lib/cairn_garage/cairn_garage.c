#include "cairn_garage.h"

#include <string.h>

#include "cairn_format.h"

#define GARAGE_BLOB_VERSION 1

void cairn_garage_vin_hash(const char *vin, uint8_t out[CAIRN_GARAGE_VIN_HASH])
{
    uint8_t d[32];
    cairn_sha256_t h;
    cairn_sha256_init(&h);
    cairn_sha256_update(&h, (const uint8_t *)vin, vin ? strlen(vin) : 0);
    cairn_sha256_final(&h, d);
    memcpy(out, d, CAIRN_GARAGE_VIN_HASH);
}

int cairn_garage_pick(const cairn_garage_t *g, const uint8_t vin_hash[CAIRN_GARAGE_VIN_HASH],
                      const char *engine_id, cairn_garage_how_t *how)
{
    int i, found = -1, n = 0;
    *how = CAIRN_GARAGE_NO_MATCH;

    for (i = 0; i < CAIRN_GARAGE_SLOTS; i++) {
        const cairn_garage_slot_t *s = &g->slot[i];
        if (s->used && s->has_vin && memcmp(s->vin_hash, vin_hash, CAIRN_GARAGE_VIN_HASH) == 0) {
            *how = CAIRN_GARAGE_BY_VIN;
            return i;
        }
    }
    if (engine_id == NULL || engine_id[0] == '\0') return -1;

    /* A slot already bound to another VIN is that other car, whatever its engine. */
    for (i = 0; i < CAIRN_GARAGE_SLOTS; i++) {
        const cairn_garage_slot_t *s = &g->slot[i];
        if (s->used && !s->has_vin && strcmp(s->engine_id, engine_id) == 0) {
            found = i;
            n++;
        }
    }
    if (n == 1) {
        *how = CAIRN_GARAGE_BY_ENGINE;
        return found;
    }
    if (n > 1) *how = CAIRN_GARAGE_AMBIGUOUS;
    return -1;
}

int cairn_garage_find_assignment(const cairn_garage_t *g, const uint8_t assignment_id[16])
{
    int i;
    for (i = 0; i < CAIRN_GARAGE_SLOTS; i++) {
        if (g->slot[i].used && memcmp(g->slot[i].assignment_id, assignment_id, 16) == 0) return i;
    }
    return -1;
}

void cairn_garage_replace(cairn_garage_t *g, const cairn_garage_t *next)
{
    cairn_garage_t out = *next;
    int i, j;
    for (i = 0; i < CAIRN_GARAGE_SLOTS; i++) {
        cairn_garage_slot_t *s = &out.slot[i];
        if (!s->used || s->has_vin) continue;
        for (j = 0; j < CAIRN_GARAGE_SLOTS; j++) {
            const cairn_garage_slot_t *o = &g->slot[j];
            if (o->used && o->has_vin && memcmp(o->assignment_id, s->assignment_id, 16) == 0 &&
                memcmp(o->vehicle_id, s->vehicle_id, 16) == 0) {
                s->has_vin = true;
                memcpy(s->vin_hash, o->vin_hash, CAIRN_GARAGE_VIN_HASH);
            }
        }
    }
    *g = out;
}

void cairn_garage_encode(const cairn_garage_t *g, uint8_t out[CAIRN_GARAGE_BLOB_BYTES])
{
    int i;
    uint8_t *p = out;
    memset(out, 0, CAIRN_GARAGE_BLOB_BYTES);
    *p++ = GARAGE_BLOB_VERSION;
    for (i = 0; i < CAIRN_GARAGE_SLOTS; i++) {
        const cairn_garage_slot_t *s = &g->slot[i];
        *p++ = s->used ? 1 : 0;
        memcpy(p, s->vehicle_id, 16); p += 16;
        memcpy(p, s->assignment_id, 16); p += 16;
        memcpy(p, s->engine_id, CAIRN_GARAGE_ENGINE_MAX);
        p[CAIRN_GARAGE_ENGINE_MAX - 1] = '\0'; /* the id is always terminated */
        p += CAIRN_GARAGE_ENGINE_MAX;
        *p++ = s->has_vin ? 1 : 0;
        memcpy(p, s->vin_hash, CAIRN_GARAGE_VIN_HASH); p += CAIRN_GARAGE_VIN_HASH;
    }
}

bool cairn_garage_decode(const uint8_t in[CAIRN_GARAGE_BLOB_BYTES], cairn_garage_t *g)
{
    int i;
    const uint8_t *p = in;
    memset(g, 0, sizeof(*g));
    if (*p++ != GARAGE_BLOB_VERSION) return false;
    for (i = 0; i < CAIRN_GARAGE_SLOTS; i++) {
        cairn_garage_slot_t *s = &g->slot[i];
        if (*p > 1) return false;
        s->used = *p++ == 1;
        memcpy(s->vehicle_id, p, 16); p += 16;
        memcpy(s->assignment_id, p, 16); p += 16;
        memcpy(s->engine_id, p, CAIRN_GARAGE_ENGINE_MAX); p += CAIRN_GARAGE_ENGINE_MAX;
        if (s->engine_id[CAIRN_GARAGE_ENGINE_MAX - 1] != '\0') return false;
        if (*p > 1) return false;
        s->has_vin = *p++ == 1;
        memcpy(s->vin_hash, p, CAIRN_GARAGE_VIN_HASH); p += CAIRN_GARAGE_VIN_HASH;
    }
    return true;
}

const char *cairn_garage_how_name(cairn_garage_how_t how)
{
    switch (how) {
    case CAIRN_GARAGE_BY_VIN:    return "by its VIN";
    case CAIRN_GARAGE_BY_ENGINE: return "by the engine its VIN names";
    case CAIRN_GARAGE_AMBIGUOUS: return "ambiguous: more than one car has that engine";
    default:                     return "no car in the garage matches";
    }
}
