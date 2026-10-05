/* Credential slots in NVS. See cairn_prov.h. */

#include <stdlib.h>
#include <string.h>

#include "../cairn_fs/cairn_kv.h"
#include "cairn_prov.h"

#define KV_SLOT "pv_slot"

static const char *const FIELDS[] = { "ssid", "pass", "crt", "key" };

static void keyname(char out[16], int slot, const char *field)
{
    /* pv0_ssid, pv1_crt, ... — within NVS's 15-character key limit. */
    out[0] = 'p'; out[1] = 'v'; out[2] = (char)('0' + slot); out[3] = '_';
    size_t i = 0;
    while (field[i] && i < 11) { out[4 + i] = field[i]; i++; }
    out[4 + i] = '\0';
}

static void scrub(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static int live_slot(void)
{
    uint32_t s = cairn_kv_get_u32(KV_SLOT, 0xFFFFFFFFu);
    return (s == 0 || s == 1) ? (int)s : -1;
}

static void erase_slot(int slot)
{
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        char k[16];
        keyname(k, slot, FIELDS[i]);
        cairn_kv_erase(k);
    }
}

/* Read one field into a heap buffer; the password is stored behind a marker so
 * an empty (open-network) one is a non-empty blob. */
static bool read_field(int slot, const char *field, uint8_t **out, size_t *len)
{
    char k[16];
    keyname(k, slot, field);
    size_t n = 0;
    uint8_t probe;
    if (!cairn_kv_get_blob_var(k, &probe, 0, &n) || n == 0) return false;
    uint8_t *buf = (uint8_t *)malloc(n + 1);
    if (buf == NULL) return false;
    size_t got = 0;
    if (!cairn_kv_get_blob_var(k, buf, n, &got) || got != n) { free(buf); return false; }
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return true;
}

bool cairn_prov_creds_load(cairn_prov_creds_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cairn_kv_begin()) return false;
    int slot = live_slot();
    if (slot < 0) return false;

    uint8_t *v; size_t n;
    if (read_field(slot, "ssid", &v, &n) && n <= CAIRN_PROV_SSID_MAX) {
        memcpy(out->ssid, v, n); out->ssid[n] = 0; out->ssid_len = n;
        scrub(v, n); free(v);
        if (read_field(slot, "pass", &v, &n) && n >= 1 && v[0] == 'p' && n - 1 <= CAIRN_PROV_PASS_MAX) {
            memcpy(out->pass, v + 1, n - 1); out->pass[n - 1] = 0; out->pass_len = n - 1;
            out->have_wifi = true;
            scrub(v, n); free(v);
        }
    }
    uint8_t *crt = NULL, *key = NULL; size_t cn = 0, kn = 0;
    if (read_field(slot, "crt", &crt, &cn) && read_field(slot, "key", &key, &kn)) {
        out->cert = (char *)crt;
        out->key = (char *)key;
        out->have_tls = true;
    } else {
        if (crt) free(crt);
        if (key) { scrub(key, kn); free(key); }
    }
    return out->have_wifi || out->have_tls;
}

void cairn_prov_creds_free(cairn_prov_creds_t *c)
{
    if (c->cert) { scrub(c->cert, strlen(c->cert)); free(c->cert); }
    if (c->key)  { scrub(c->key, strlen(c->key));   free(c->key); }
    scrub(c, sizeof(*c));
    memset(c, 0, sizeof(*c));
}

bool cairn_prov_has_credentials(void)
{
    cairn_prov_creds_t c;
    bool have = cairn_prov_creds_load(&c) && c.have_wifi && c.have_tls;
    cairn_prov_creds_free(&c);
    return have;
}

static bool write_field(int slot, const char *field, const void *data, size_t len)
{
    char k[16];
    keyname(k, slot, field);
    if (!cairn_kv_set_blob(k, data, len)) return false;

    /* "The call returned" is not "a reboot will see it". Read it back. */
    size_t n = 0;
    uint8_t *back = (uint8_t *)malloc(len ? len : 1);
    if (back == NULL) return false;
    bool ok = cairn_kv_get_blob_var(k, back, len, &n) && n == len && memcmp(back, data, len) == 0;
    scrub(back, len);
    free(back);
    return ok;
}

bool cairn_prov_creds_apply(const cairn_prov_staged_t *st)
{
    if (!cairn_kv_begin()) return false;

    /* Start from the live set so a value not staged is carried over. */
    cairn_prov_creds_t cur;
    (void)cairn_prov_creds_load(&cur);

    int live = live_slot();
    int next = (live == 0) ? 1 : 0;
    erase_slot(next);

    bool ok = true;

    const uint8_t *ssid = st->have_ssid ? st->ssid : cur.ssid;
    size_t ssid_len = st->have_ssid ? st->ssid_len : cur.ssid_len;
    const char *pass = st->have_pass ? st->pass : cur.pass;
    size_t pass_len = st->have_pass ? st->pass_len : cur.pass_len;
    if (ssid_len > 0) {
        uint8_t pbuf[CAIRN_PROV_PASS_MAX + 2];
        pbuf[0] = 'p';
        memcpy(pbuf + 1, pass, pass_len);
        ok = ok && write_field(next, "ssid", ssid, ssid_len) && write_field(next, "pass", pbuf, pass_len + 1);
        scrub(pbuf, sizeof(pbuf));
    }

    const char *crt = st->cert ? st->cert : cur.cert;
    const char *key = st->key ? st->key : cur.key;
    if (crt && key) {
        ok = ok && write_field(next, "crt", crt, strlen(crt)) && write_field(next, "key", key, strlen(key));
    }

    cairn_prov_creds_free(&cur);

    if (!ok) {
        erase_slot(next);   /* the live slot was never touched */
        return false;
    }

    /* One u32 write makes the whole set live. */
    if (!cairn_kv_set_u32(KV_SLOT, (uint32_t)next) || live_slot() != next) {
        erase_slot(next);
        return false;
    }
    if (live >= 0) erase_slot(live);
    return true;
}
