/*
 * Host backend: a flat record file beside the simulated card.
 *
 * Kept separate from the card tree so a test can simulate NVS surviving a card
 * swap, or a wiped NVS against an intact card — the two failure modes that
 * determine whether a device keeps its identity.
 *
 * Values are heap-allocated and variable length so PEM credentials fit; the
 * file is rewritten whole on every change (write-through), which is what makes a
 * simulated crash unable to lose a committed value.
 */

#ifndef ARDUINO

#include "cairn_kv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KV_MAX_ENTRIES 64
#define KV_MAX_KEY     32
#define KV_MAX_VALUE   (64u * 1024u)

typedef struct {
    char     key[KV_MAX_KEY];
    uint8_t *value;
    size_t   len;
    bool     used;
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

static void clear_all(void)
{
    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        free(s_entries[i].value);
    }
    memset(s_entries, 0, sizeof(s_entries));
}

static void load(void)
{
    clear_all();

    FILE *f = fopen(s_path, "rb");
    if (f == NULL) return;

    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        uint8_t klen;
        uint32_t vlen;
        if (fread(&klen, 1, 1, f) != 1 || klen == 0 || klen >= KV_MAX_KEY) break;
        if (fread(s_entries[i].key, 1, klen, f) != klen) break;
        s_entries[i].key[klen] = '\0';
        if (fread(&vlen, sizeof(vlen), 1, f) != 1 || vlen > KV_MAX_VALUE) break;
        s_entries[i].value = (uint8_t *)malloc(vlen ? vlen : 1);
        if (s_entries[i].value == NULL) break;
        if (vlen && fread(s_entries[i].value, 1, vlen, f) != vlen) break;
        s_entries[i].len = vlen;
        s_entries[i].used = true;
    }
    fclose(f);
}

static void store(void)
{
    if (s_path[0] == '\0') return;

    FILE *f = fopen(s_path, "wb");
    if (f == NULL) return;

    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (!s_entries[i].used) continue;
        uint8_t klen = (uint8_t)strlen(s_entries[i].key);
        uint32_t vlen = (uint32_t)s_entries[i].len;
        fwrite(&klen, 1, 1, f);
        fwrite(s_entries[i].key, 1, klen, f);
        fwrite(&vlen, sizeof(vlen), 1, f);
        if (vlen) fwrite(s_entries[i].value, 1, vlen, f);
    }
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

bool cairn_kv_get_blob_var(const char *key, void *out, size_t cap, size_t *len)
{
    if (!s_open) return false;

    kv_entry_t *e = find(key);
    if (e == NULL) return false;

    *len = e->len;
    if (e->len <= cap) memcpy(out, e->value, e->len);
    return true;
}

bool cairn_kv_set_blob(const char *key, const void *data, size_t len)
{
    if (!s_open || len > KV_MAX_VALUE || strlen(key) >= KV_MAX_KEY) return false;

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

    uint8_t *copy = (uint8_t *)malloc(len ? len : 1);
    if (copy == NULL) return false;
    if (len) memcpy(copy, data, len);

    free(e->value);
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->value = copy;
    e->len = len;
    e->used = true;

    store(); /* write through, so a simulated crash cannot lose identity */
    return true;
}

bool cairn_kv_erase(const char *key)
{
    if (!s_open) return false;

    kv_entry_t *e = find(key);
    if (e == NULL) return true;

    if (e->value != NULL) memset(e->value, 0, e->len); /* do not leave a secret behind */
    free(e->value);
    memset(e, 0, sizeof(*e));
    store();
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
