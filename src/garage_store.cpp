#include "garage_store.h"

#include <string.h>

#include "cairn_kv.h"
#include "cairn_log.h"

static const char *TAG = "GARAGE";
static const char *KV_GARAGE = "garage";

bool garage_load(cairn_garage_t *g)
{
    uint8_t blob[CAIRN_GARAGE_BLOB_BYTES];
    memset(g, 0, sizeof(*g));
    if (!cairn_kv_begin()) return false;
    if (!cairn_kv_get_blob(KV_GARAGE, blob, sizeof(blob))) return false;
    if (!cairn_garage_decode(blob, g)) {
        CAIRN_LOGE(TAG, "the stored garage does not decode; ignoring it (reprovision)");
        memset(g, 0, sizeof(*g));
        return false;
    }
    for (int i = 0; i < CAIRN_GARAGE_SLOTS; i++) {
        if (g->slot[i].used) return true;
    }
    return false;
}

bool garage_save(const cairn_garage_t *g)
{
    uint8_t blob[CAIRN_GARAGE_BLOB_BYTES];
    cairn_garage_encode(g, blob);
    return cairn_kv_begin() && cairn_kv_set_blob(KV_GARAGE, blob, sizeof(blob));
}
