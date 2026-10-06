#include "cairn_devinfo.h"

#include <string.h>

/* ── little-endian helpers ────────────────────────────────────────────────── */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── sizes ────────────────────────────────────────────────────────────────── */

enum {
    LEN_FIRMWARE  = 16,
    LEN_IDENTITY  = 25,
    LEN_STORAGE   = 8,
    LEN_TRANSPORT = 4,
    LEN_ENGINE_FIXED = 11,
    LEN_BOOT      = 16
};

static size_t engine_record_size(const cairn_di_engine_t *e) { return 2u + LEN_ENGINE_FIXED + e->id_len; }

static bool engine_valid(const cairn_di_engine_t *e)
{
    if (e->id_len < 1 || e->id_len > CAIRN_DI_ENGINE_ID_MAX) return false;
    for (uint8_t i = 0; i < e->id_len; i++) {
        unsigned char c = (unsigned char)e->id[i];
        if (c < 0x21 || c > 0x7E) return false;    /* printable ASCII, no spaces: an identifier */
    }
    return true;
}

/* ── encode ───────────────────────────────────────────────────────────────── */

size_t cairn_devinfo_encode(const cairn_devinfo_t *info, uint8_t *out, size_t cap)
{
    if (info->capabilities & CAIRN_CAP_RESERVED) return 0;
    if (info->n_transports > CAIRN_DI_MAX_TRANSPORTS || info->n_engines > CAIRN_DI_MAX_ENGINES) return 0;
    for (uint8_t i = 0; i < info->n_engines; i++)
        if (!engine_valid(&info->engines[i])) return 0;

    /* Everything but the engines is never dropped, so size it first. */
    size_t fixed = CAIRN_DI_HEADER_LEN;
    if (info->has_firmware) fixed += 2 + LEN_FIRMWARE;
    if (info->has_identity) fixed += 2 + LEN_IDENTITY;
    if (info->has_storage)  fixed += 2 + LEN_STORAGE;
    fixed += (size_t)info->n_transports * (2 + LEN_TRANSPORT);
    if (info->has_boot)     fixed += 2 + LEN_BOOT;

    /* The most engines that fit alongside the 0x7F marker once any are dropped. */
    uint8_t keep = info->n_engines;
    for (;;) {
        size_t eng = 0;
        for (uint8_t i = 0; i < keep; i++) eng += engine_record_size(&info->engines[i]);
        bool marker = info->truncated || keep < info->n_engines;
        if (fixed + eng + (marker ? 2 : 0) <= CAIRN_DI_MAX_LEN) break;
        if (keep == 0) return 0;                    /* the fixed part alone does not fit: impossible by construction */
        keep--;
    }
    bool marker = info->truncated || keep < info->n_engines;

    size_t total = fixed + (marker ? 2 : 0);
    for (uint8_t i = 0; i < keep; i++) total += engine_record_size(&info->engines[i]);
    if (cap < total) return 0;

    uint8_t *p = out;
    p[0] = CAIRN_DI_VERSION;
    p[1] = CAIRN_DI_MINOR;
    put16(p + 2, (uint16_t)total);
    put32(p + 4, info->capabilities);
    p += CAIRN_DI_HEADER_LEN;

    if (info->has_firmware) {
        *p++ = CAIRN_DI_REC_FIRMWARE; *p++ = LEN_FIRMWARE;
        *p++ = info->firmware.major; *p++ = info->firmware.minor;
        *p++ = info->firmware.patch; *p++ = info->firmware.flags;
        memcpy(p, info->firmware.commit, 8); p += 8;
        put32(p, info->firmware.build_unix); p += 4;
    }
    if (info->has_identity) {
        *p++ = CAIRN_DI_REC_IDENTITY; *p++ = LEN_IDENTITY;
        memcpy(p, info->identity.device_id, 16); p += 16;
        memcpy(p, info->identity.fingerprint, 4); p += 4;
        *p++ = info->identity.enrol_state;
        put32(p, info->identity.storage_key_version); p += 4;
    }
    if (info->has_storage) {
        *p++ = CAIRN_DI_REC_STORAGE; *p++ = LEN_STORAGE;
        *p++ = info->storage.state;
        *p++ = 0;                                   /* reserved */
        put16(p, info->storage.pending_bundles); p += 2;
        put32(p, info->storage.free_mib); p += 4;
    }
    for (uint8_t i = 0; i < info->n_transports; i++) {
        *p++ = CAIRN_DI_REC_TRANSPORT; *p++ = LEN_TRANSPORT;
        *p++ = info->transports[i].kind;
        *p++ = info->transports[i].state;
        put16(p, info->transports[i].last_error); p += 2;
    }
    for (uint8_t i = 0; i < keep; i++) {
        const cairn_di_engine_t *e = &info->engines[i];
        *p++ = CAIRN_DI_REC_ENGINE; *p++ = (uint8_t)(LEN_ENGINE_FIXED + e->id_len);
        put16(p, e->profile_version); p += 2;
        memcpy(p, e->hash, 8); p += 8;
        *p++ = e->id_len;
        memcpy(p, e->id, e->id_len); p += e->id_len;
    }
    if (info->has_boot) {
        *p++ = CAIRN_DI_REC_BOOT; *p++ = LEN_BOOT;
        put32(p, info->boot.to_ble_ms); p += 4;
        put32(p, info->boot.to_ready_ms); p += 4;
        put32(p, info->boot.to_first_fix_ms); p += 4;
        *p++ = info->boot.reset_reason;
        *p++ = 0; *p++ = 0; *p++ = 0;               /* reserved */
    }
    if (marker) { *p++ = CAIRN_DI_REC_TRUNCATED; *p++ = 0; }

    return (size_t)(p - out);                       /* equals total */
}

/* ── decode ───────────────────────────────────────────────────────────────── */

cairn_di_err_t cairn_devinfo_decode(cairn_devinfo_t *info, const uint8_t *in, size_t len)
{
    memset(info, 0, sizeof *info);
    if (len < CAIRN_DI_HEADER_LEN) return CAIRN_DI_ERR_SHORT;
    if (in[0] != CAIRN_DI_VERSION) return CAIRN_DI_ERR_VERSION;

    size_t total = get16(in + 2);
    if (total != len || total > CAIRN_DI_MAX_LEN) return CAIRN_DI_ERR_LENGTH;

    info->version = in[0];
    info->minor = in[1];
    info->capabilities = get32(in + 4);

    size_t i = CAIRN_DI_HEADER_LEN;
    int last_known = -1;
    while (i < len) {
        if (len - i < 2) return CAIRN_DI_ERR_SHORT;
        uint8_t type = in[i], rlen = in[i + 1];
        i += 2;
        if (len - i < rlen) return CAIRN_DI_ERR_SHORT;
        const uint8_t *v = in + i;
        i += rlen;

        bool known = (type >= CAIRN_DI_REC_FIRMWARE && type <= CAIRN_DI_REC_BOOT) || type == CAIRN_DI_REC_TRUNCATED;
        if (!known) {
            /* A newer minor version's record: skipped by its length. */
            if (info->unknown_records < 255) info->unknown_records++;
            continue;
        }

        /* Known types arrive in ascending order; only transport and engine repeat. */
        bool repeated = (type == CAIRN_DI_REC_TRANSPORT || type == CAIRN_DI_REC_ENGINE);
        if ((int)type < last_known || ((int)type == last_known && !repeated)) return CAIRN_DI_ERR_RECORD;
        last_known = type;

        switch (type) {
        case CAIRN_DI_REC_FIRMWARE:
            if (rlen != LEN_FIRMWARE) return CAIRN_DI_ERR_RECORD;
            info->has_firmware = true;
            info->firmware.major = v[0]; info->firmware.minor = v[1];
            info->firmware.patch = v[2]; info->firmware.flags = v[3];
            memcpy(info->firmware.commit, v + 4, 8);
            info->firmware.build_unix = get32(v + 12);
            break;
        case CAIRN_DI_REC_IDENTITY:
            if (rlen != LEN_IDENTITY) return CAIRN_DI_ERR_RECORD;
            info->has_identity = true;
            memcpy(info->identity.device_id, v, 16);
            memcpy(info->identity.fingerprint, v + 16, 4);
            info->identity.enrol_state = v[20];
            info->identity.storage_key_version = get32(v + 21);
            break;
        case CAIRN_DI_REC_STORAGE:
            if (rlen != LEN_STORAGE) return CAIRN_DI_ERR_RECORD;
            info->has_storage = true;
            info->storage.state = v[0];
            info->storage.pending_bundles = get16(v + 2);
            info->storage.free_mib = get32(v + 4);
            break;
        case CAIRN_DI_REC_TRANSPORT:
            if (rlen != LEN_TRANSPORT) return CAIRN_DI_ERR_RECORD;
            if (info->n_transports >= CAIRN_DI_MAX_TRANSPORTS) return CAIRN_DI_ERR_OVERFLOW;
            info->transports[info->n_transports].kind = v[0];
            info->transports[info->n_transports].state = v[1];
            info->transports[info->n_transports].last_error = get16(v + 2);
            info->n_transports++;
            break;
        case CAIRN_DI_REC_ENGINE: {
            if (rlen < LEN_ENGINE_FIXED + 1) return CAIRN_DI_ERR_RECORD;
            uint8_t id_len = v[10];
            if (id_len < 1 || id_len > CAIRN_DI_ENGINE_ID_MAX || rlen != LEN_ENGINE_FIXED + id_len)
                return CAIRN_DI_ERR_RECORD;
            if (info->n_engines >= CAIRN_DI_MAX_ENGINES) return CAIRN_DI_ERR_OVERFLOW;
            cairn_di_engine_t *e = &info->engines[info->n_engines];
            e->profile_version = get16(v);
            memcpy(e->hash, v + 2, 8);
            e->id_len = id_len;
            memcpy(e->id, v + 11, id_len);
            e->id[id_len] = '\0';
            info->n_engines++;
            break;
        }
        case CAIRN_DI_REC_BOOT:
            if (rlen != LEN_BOOT) return CAIRN_DI_ERR_RECORD;
            info->has_boot = true;
            info->boot.to_ble_ms = get32(v);
            info->boot.to_ready_ms = get32(v + 4);
            info->boot.to_first_fix_ms = get32(v + 8);
            info->boot.reset_reason = v[12];
            break;
        case CAIRN_DI_REC_TRUNCATED:
            if (rlen != 0) return CAIRN_DI_ERR_RECORD;
            info->truncated = true;
            break;
        default:
            break;
        }
    }
    return CAIRN_DI_OK;
}

/* ── UPLINK_EVENT ─────────────────────────────────────────────────────────── */

size_t cairn_uplink_event_encode(const cairn_uplink_event_t *e, uint8_t *out, size_t cap)
{
    if (cap < CAIRN_UE_LEN || e->kind < CAIRN_UE_LEAVING || e->kind > CAIRN_UE_ABORTED) return 0;
    memset(out, 0, CAIRN_UE_LEN);
    out[0] = e->kind; out[1] = e->path; out[2] = e->reason; out[3] = e->outcome;
    put32(out + 4, e->slot);
    put16(out + 8, e->max_seconds);
    put16(out + 10, e->committed);
    put16(out + 12, e->failed);
    /* bytes 14..15 are reserved and stay zero */
    put32(out + 16, e->bytes_sent);
    put32(out + 20, e->duration_ms);
    return CAIRN_UE_LEN;
}

bool cairn_uplink_event_decode(cairn_uplink_event_t *e, const uint8_t *in, size_t len)
{
    memset(e, 0, sizeof *e);
    if (len != CAIRN_UE_LEN) return false;
    if (in[0] < CAIRN_UE_LEAVING || in[0] > CAIRN_UE_ABORTED) return false;
    if (in[14] != 0 || in[15] != 0) return false;

    e->kind = in[0]; e->path = in[1]; e->reason = in[2]; e->outcome = in[3];
    e->slot = get32(in + 4);
    e->max_seconds = get16(in + 8);
    e->committed = get16(in + 10);
    e->failed = get16(in + 12);
    e->bytes_sent = get32(in + 16);
    e->duration_ms = get32(in + 20);
    return true;
}
