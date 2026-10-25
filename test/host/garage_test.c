/*
 * Which car the dongle is in (lib/cairn_garage): the owner's two, a 428i (N20) and an
 * M240i (B58), and the cases where the device must not guess.
 */

#include <stdio.h>
#include <string.h>

#include "cairn_garage.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [garage] %s\n", name); }         \
        else      { g_fail++; printf("  FAIL  [garage] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* Made-up VINs with the two cars' engine codes at positions 4-8. */
static const char *VIN_428 = "WBA3N3C50EXAMPLE0";
static const char *VIN_M240 = "WBA2G1C30HEXAMPL0";
static const char *VIN_OTHER_N20 = "WBA3N3C59FOTHER00";

static void slot(cairn_garage_t *g, int i, uint8_t tag, const char *engine)
{
    cairn_garage_slot_t *s = &g->slot[i];
    memset(s, 0, sizeof(*s));
    s->used = true;
    memset(s->vehicle_id, tag, 16);
    memset(s->assignment_id, (uint8_t)(tag + 0x80), 16);
    snprintf(s->engine_id, sizeof(s->engine_id), "%s", engine);
}

static void test_two_cars(void)
{
    cairn_garage_t g;
    memset(&g, 0, sizeof(g));
    slot(&g, 0, 0x28, "bmw-n20");
    slot(&g, 1, 0x24, "bmw-b58");

    uint8_t h428[8], hm240[8];
    cairn_garage_vin_hash(VIN_428, h428);
    cairn_garage_vin_hash(VIN_M240, hm240);

    cairn_garage_how_t how;
    int i = cairn_garage_pick(&g, hm240, "bmw-b58", &how);
    CHECK("the M240i's VIN picks the M240i by its engine", i == 1 && how == CAIRN_GARAGE_BY_ENGINE);
    g.slot[1].has_vin = true;
    memcpy(g.slot[1].vin_hash, hm240, 8);

    i = cairn_garage_pick(&g, hm240, "bmw-b58", &how);
    CHECK("and by its VIN once learned", i == 1 && how == CAIRN_GARAGE_BY_VIN);
    i = cairn_garage_pick(&g, hm240, NULL, &how);
    CHECK("even with no engine named", i == 1 && how == CAIRN_GARAGE_BY_VIN);

    i = cairn_garage_pick(&g, h428, "bmw-n20", &how);
    CHECK("the 428i's VIN picks the 428i", i == 0 && how == CAIRN_GARAGE_BY_ENGINE);

    uint8_t hother[8];
    cairn_garage_vin_hash(VIN_OTHER_N20, hother);
    g.slot[0].has_vin = true;
    memcpy(g.slot[0].vin_hash, h428, 8);
    i = cairn_garage_pick(&g, hother, "bmw-n20", &how);
    CHECK("a third N20 is not taken for the 428i once its VIN is known", i == -1 && how == CAIRN_GARAGE_NO_MATCH);

    i = cairn_garage_pick(&g, hother, NULL, &how);
    CHECK("an unknown VIN naming no engine picks nothing", i == -1);
}

static void test_never_guess(void)
{
    cairn_garage_t g;
    memset(&g, 0, sizeof(g));
    slot(&g, 0, 0x01, "bmw-n20");
    slot(&g, 2, 0x02, "bmw-n20");
    uint8_t h[8];
    cairn_garage_vin_hash(VIN_428, h);
    cairn_garage_how_t how;
    int i = cairn_garage_pick(&g, h, "bmw-n20", &how);
    CHECK("two cars with one engine and no VINs learned: ambiguous, no pick",
          i == -1 && how == CAIRN_GARAGE_AMBIGUOUS);

    cairn_garage_t empty;
    memset(&empty, 0, sizeof(empty));
    i = cairn_garage_pick(&empty, h, "bmw-n20", &how);
    CHECK("an empty garage picks nothing", i == -1 && how == CAIRN_GARAGE_NO_MATCH);
}

static void test_reprovision_keeps_vins(void)
{
    cairn_garage_t g, next;
    memset(&g, 0, sizeof(g));
    slot(&g, 0, 0x28, "bmw-n20");
    slot(&g, 1, 0x24, "bmw-b58");
    g.slot[0].has_vin = true;
    memset(g.slot[0].vin_hash, 0xAB, 8);
    g.slot[1].has_vin = true;
    memset(g.slot[1].vin_hash, 0xCD, 8);

    /* The same two assignments provisioned again, in the other order. */
    memset(&next, 0, sizeof(next));
    slot(&next, 0, 0x24, "bmw-b58");
    slot(&next, 1, 0x28, "bmw-n20");
    cairn_garage_replace(&g, &next);
    CHECK("reprovisioning the same cars keeps what each learned",
          g.slot[0].has_vin && g.slot[0].vin_hash[0] == 0xCD && g.slot[1].has_vin && g.slot[1].vin_hash[0] == 0xAB);

    /* A new assignment for a car starts unbound. */
    memset(&next, 0, sizeof(next));
    slot(&next, 0, 0x24, "bmw-b58");
    next.slot[0].assignment_id[0] = 0x11;
    cairn_garage_replace(&g, &next);
    CHECK("a new assignment does not inherit a VIN", !g.slot[0].has_vin && !g.slot[1].used);

    uint8_t a[16];
    memset(a, 0x11, 16);
    a[0] = 0x11;
    CHECK("find_assignment misses an assignment it does not hold", cairn_garage_find_assignment(&g, a) == -1);
    memcpy(a, g.slot[0].assignment_id, 16);
    CHECK("and finds one it does", cairn_garage_find_assignment(&g, a) == 0);
}

static void test_encoding(void)
{
    cairn_garage_t g, back;
    memset(&g, 0, sizeof(g));
    slot(&g, 0, 0x28, "bmw-n20");
    slot(&g, 3, 0x24, "bmw-b58");
    g.slot[3].has_vin = true;
    memset(g.slot[3].vin_hash, 0x5A, 8);

    uint8_t blob[CAIRN_GARAGE_BLOB_BYTES];
    cairn_garage_encode(&g, blob);
    CHECK("the garage survives NVS", cairn_garage_decode(blob, &back) && memcmp(&g, &back, sizeof(g)) == 0);

    blob[0] = 9;
    CHECK("an unknown blob version is refused, not misread", !cairn_garage_decode(blob, &back));
    cairn_garage_encode(&g, blob);
    blob[1 + 33 + CAIRN_GARAGE_ENGINE_MAX - 1] = 'x';
    CHECK("an unterminated engine id is refused", !cairn_garage_decode(blob, &back));
}

int main(void)
{
    printf("garage\n");
    test_two_cars();
    test_never_guess();
    test_reprovision_keeps_vins();
    test_encoding();
    printf("garage: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
