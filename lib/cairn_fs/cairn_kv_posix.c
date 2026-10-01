/*
 * Host backend: a flat record file beside the simulated card.
 *
 * Kept separate from the card tree so a test can simulate NVS surviving a card
 * swap, or a wiped NVS against an intact card — the two failure modes that
 * determine whether a device keeps its identity.
 */

#ifndef ARDUINO

#include "cairn_kv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KV_MAX_ENTRIES 32
#define KV_MAX_KEY     32
#define KV_MAX_VALUE   64

typedef struct {
    char    key[KV_MAX_KEY];
    uint8_t value[KV_MAX_VALUE];
    size_t  len;
    bool    used;
} kv_entry_t;

static kv_entry_t s_entries[KV_MAX_ENTRIES];
static bool       s_open = false;

static char s_path[1024];

/* Set by the test harness so identity can be reset independently of the card. */
void cairn_kv_host_set_path(const char *path);

void cairn_kv_host_set_path(const char *path)
{
    snprintf(s_path, sizeof(s_path), "%s", path);
}

static void load(void)
{
    memset(s_entries, 0, sizeof(s_entries));

    FILE *f = fopen(s_path, "rb");
    if (f == NULL) return;

    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        kv_entry_t e;
        if (fread(&e, sizeof(e), 1, f) != 1) break;
        s_entries[i] = e;
    }
    fclose(f);
}

static void store(void)
{
    if (s_path[0] == '\0') return;

    FILE *f = fopen(s_path, "wb");
    if (f == NULL) return;

    fwrite(s_entries, sizeof(s_entries[0]), KV_MAX_ENTRIES, f);
    fclose(f);
}

bool cairn_kv_begin(void)
{
    if (s_path[0] == '\0') snprintf(s_path, sizeof(s_path), "cairn-kv.bin");
    load();
    s_open = true;
    return true;
}

void cairn_kv_end(void)
{
    if (!s_open) return;
    store();
    s_open = false;
}

static kv_entry_t *find(const char *key)
{
    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

bool cairn_kv_get_blob(const char *key, void *out, size_t len)
{
    if (!s_open) return false;

    kv_entry_t *e = find(key);
    if (e == NULL || e->len != len) return false;

    memcpy(out, e->value, len);
    return true;
}

bool cairn_kv_set_blob(const char *key, const void *data, size_t len)
{
    if (!s_open || len > KV_MAX_VALUE) return false;

    kv_entry_t *e = find(key);
    if (e == NULL) {
        for (int i = 0; i < KV_MAX_ENTRIES; i++) {
            if (!s_entries[i].used) {
                e = &s_entries[i];
                break;
            }
        }
    }
    if (e == NULL) return false;

    snprintf(e->key, sizeof(e->key), "%s", key);
    memcpy(e->value, data, len);
    e->len = len;
    e->used = true;

    store(); /* write through, so a simulated crash cannot lose identity */
    return true;
}

uint32_t cairn_kv_get_u32(const char *key, uint32_t fallback)
{
    uint32_t v = 0;
    if (!cairn_kv_get_blob(key, &v, sizeof(v))) return fallback;
    return v;
}

bool cairn_kv_set_u32(const char *key, uint32_t value)
{
    return cairn_kv_set_blob(key, &value, sizeof(value));
}

#endif /* !ARDUINO */
