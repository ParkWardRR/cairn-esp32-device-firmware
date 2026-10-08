#include "cairn_offload.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cairn_fs.h"
#include "cairn_log.h"
#include "cairn_prune.h"
#include "cairn_store.h"

static const char *TAG = "OFFLOAD";

/* One module, one phone: the manifest being worked on and its scratch live here
 * rather than on a BLE task's small stack. */
static uint8_t          s_man[CAIRN_MANIFEST_ENCODED_MAX + 64];
static size_t           s_man_len;          /* manifest.cbor length; the 64-byte signature follows */
static cairn_manifest_t s_m;
static uint8_t          s_scratch[CAIRN_MANIFEST_ENCODED_MAX];

/* LIST works from a small table so pagination does not re-read every manifest. */
typedef struct {
    uint8_t  id[16];
    char     text[27];
    uint64_t stream_bytes;
    uint16_t manifest_len;
    uint16_t chunk_count;
    uint8_t  state;
} summary_t;
static summary_t s_list[CAIRN_OFFLOAD_MAX_BUNDLES];

/* ── little-endian helpers ────────────────────────────────────────────────── */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

uint32_t cairn_offload_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    /* Reflected IEEE polynomial 0xEDB88320, one nibble at a time: 64 bytes of
     * table instead of 1 KB, and the stream is slow enough that it does not matter. */
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
        0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ T[crc & 15];
        crc = (crc >> 4) ^ T[crc & 15];
    }
    return crc;
}

/* ── lifecycle ────────────────────────────────────────────────────────────── */

static void scrub(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

void cairn_offload_init(cairn_offload_t *o, const cairn_offload_io_t *io)
{
    memset(o, 0, sizeof(*o));
    o->io = *io;
    o->payload = 20;   /* the default ATT MTU of 23; refused until the MTU grows */
}

void cairn_offload_set_mtu(cairn_offload_t *o, uint16_t att_mtu)
{
    uint32_t p = (att_mtu > 3) ? (uint32_t)(att_mtu - 3) : 0;
    if (p > CAIRN_OFFLOAD_MAX_PAYLOAD) p = CAIRN_OFFLOAD_MAX_PAYLOAD;
    o->payload = (uint16_t)p;
}

static void close_file(cairn_offload_t *o)
{
    if (o->file != NULL) {
        cairn_fs_close((cairn_file_t *)o->file);
        o->file = NULL;
    }
}

static void end_operation(cairn_offload_t *o)
{
    close_file(o);
    o->state = CAIRN_OFFLOAD_IDLE;
    o->tx_len = 0;
    o->remaining = 0;
    scrub(o->receipt, sizeof(o->receipt));
    o->receipt_len = o->receipt_got = o->receipt_seq = 0;
}

bool cairn_offload_busy(const cairn_offload_t *o)
{
    return o->state != CAIRN_OFFLOAD_IDLE || o->ind_pending;
}

void cairn_offload_on_disconnect(cairn_offload_t *o)
{
    end_operation(o);
    o->ind_pending = false;
    o->ind_len = 0;
}

/* ── indications ──────────────────────────────────────────────────────────── */

static void try_send_ind(cairn_offload_t *o)
{
    if (!o->ind_pending) return;
    if (o->io.indicate(o->io.ctx, o->ind, o->ind_len)) {
        o->ind_pending = false;
        o->last_progress_ms = o->io.now_ms(o->io.ctx);
    }
}

static void respond(cairn_offload_t *o, uint8_t op, uint8_t rid, uint8_t status,
                    const uint8_t *payload, size_t plen)
{
    if (3 + plen > sizeof(o->ind)) return;   /* cannot happen: callers bound plen */
    o->ind[0] = (uint8_t)(op | 0x80);
    o->ind[1] = rid;
    o->ind[2] = status;
    if (plen > 0) memcpy(o->ind + 3, payload, plen);
    o->ind_len = 3 + plen;
    o->ind_pending = true;
    try_send_ind(o);
}

/* ── bundles on the card ──────────────────────────────────────────────────── */

/* Enumerate sealed bundles' names, sorted ascending (a ULID sorts by time). */
static size_t bundle_names(char names[][27], size_t cap)
{
    cairn_dir_t *d = cairn_fs_opendir(CAIRN_DIR_BUNDLES);
    if (d == NULL) return 0;

    size_t count = 0;
    char   name[64];
    bool   is_dir = false;
    while (cairn_fs_readdir(d, name, sizeof(name), &is_dir, NULL)) {
        uint8_t probe[16];
        if (!is_dir || count >= cap || strlen(name) != 26 || !cairn_ulid_decode(name, probe)) continue;
        memcpy(names[count], name, 27);
        count++;
    }
    cairn_fs_closedir(d);

    for (size_t i = 1; i < count; i++) {          /* insertion sort: count <= 16 */
        char tmp[27];
        memcpy(tmp, names[i], 27);
        size_t j = i;
        while (j > 0 && strcmp(names[j - 1], tmp) > 0) {
            memcpy(names[j], names[j - 1], 27);
            j--;
        }
        memcpy(names[j], tmp, 27);
    }
    return count;
}

static bool read_file_into(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    if (f == NULL) return false;
    uint64_t size = cairn_fs_size(f);
    if (size == 0 || size > cap) {
        cairn_fs_close(f);
        return false;
    }
    size_t n = cairn_fs_read(f, buf, (size_t)size);
    cairn_fs_close(f);
    *len = n;
    return n == (size_t)size;
}

/* Load and decode a bundle's manifest into the module's buffers: manifest.cbor
 * at s_man[0, s_man_len), manifest.sig after it. */
static bool load_manifest(const char *id_text)
{
    char path[160];
    size_t sig_len = 0;

    snprintf(path, sizeof(path), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, id_text);
    if (!read_file_into(path, s_man, CAIRN_MANIFEST_ENCODED_MAX, &s_man_len)) return false;

    snprintf(path, sizeof(path), "%s/%s/manifest.sig", CAIRN_DIR_BUNDLES, id_text);
    if (!read_file_into(path, s_man + s_man_len, 64, &sig_len) || sig_len != 64) return false;

    return cairn_manifest_decode(s_man, s_man_len, &s_m, s_scratch, sizeof(s_scratch)) == CAIRN_OK;
}

static uint64_t stream_bytes(const cairn_manifest_t *m)
{
    uint64_t total = 0;
    for (size_t i = 0; i < m->member_count; i++) total += m->members[i].length;
    return total;
}

/* Find a bundle by its 16-byte id and load its manifest. */
static cairn_offload_status_t find_bundle(cairn_offload_t *o, const uint8_t id[16])
{
    char names[CAIRN_OFFLOAD_MAX_BUNDLES][27];
    size_t count = bundle_names(names, CAIRN_OFFLOAD_MAX_BUNDLES);
    for (size_t i = 0; i < count; i++) {
        uint8_t got[16];
        if (!cairn_ulid_decode(names[i], got) || memcmp(got, id, 16) != 0) continue;
        snprintf(o->id_text, sizeof(o->id_text), "%s", names[i]);
        return load_manifest(names[i]) ? CAIRN_OFFLOAD_OK : CAIRN_OFFLOAD_IO_ERROR;
    }
    return CAIRN_OFFLOAD_UNKNOWN_BUNDLE;
}

/* ── transfers ────────────────────────────────────────────────────────────── */

static void begin_stream(cairn_offload_t *o, uint8_t op, uint8_t rid, uint64_t total,
                         bool from_memory)
{
    o->state = CAIRN_OFFLOAD_STREAM;
    o->op = op;
    o->request_id = rid;
    o->from_memory = from_memory;
    o->remaining = total;
    o->sent = 0;
    o->seq = 0;
    o->crc = 0xFFFFFFFFu;
    o->tx_len = 0;
    o->mem_pos = 0;
    o->member = 0;
    o->member_pos = 0;
    close_file(o);
    o->last_progress_ms = o->io.now_ms(o->io.ctx);
}

static void finish_stream(cairn_offload_t *o, uint8_t status)
{
    uint8_t p[8];
    put32(p, (uint32_t)o->sent);
    put32(p + 4, ~o->crc);
    uint8_t rid = o->request_id;
    end_operation(o);
    /* 0x86 request_id status u32 bytes_sent u32 crc32 */
    o->ind[0] = CAIRN_OFFLOAD_IND_DONE;
    o->ind[1] = rid;
    o->ind[2] = status;
    memcpy(o->ind + 3, p, 8);
    o->ind_len = 11;
    o->ind_pending = true;
    try_send_ind(o);
}

/* Fill tx[2..] with up to `want` bytes of the stream. Returns the count, or -1. */
static int read_stream(cairn_offload_t *o, uint8_t *dst, size_t want)
{
    size_t got = 0;

    if (o->from_memory) {
        memcpy(dst, s_man + o->mem_pos, want);
        o->mem_pos += want;
        return (int)want;
    }

    while (got < want) {
        if (o->member >= s_m.member_count) return -1;           /* ran off the end */
        uint64_t left_in_member = s_m.members[o->member].length - o->member_pos;
        if (left_in_member == 0) {
            close_file(o);
            o->member++;
            o->member_pos = 0;
            continue;
        }
        if (o->file == NULL) {
            char path[192];
            snprintf(path, sizeof(path), "%s/%s/%s", CAIRN_DIR_BUNDLES, o->id_text,
                     s_m.members[o->member].name);
            cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
            if (f == NULL || !cairn_fs_seek(f, o->member_pos)) {
                if (f != NULL) cairn_fs_close(f);
                return -1;
            }
            o->file = f;
        }
        size_t take = want - got;
        if ((uint64_t)take > left_in_member) take = (size_t)left_in_member;
        size_t n = cairn_fs_read((cairn_file_t *)o->file, dst + got, take);
        if (n == 0) return -1;
        got += n;
        o->member_pos += n;
    }
    return (int)got;
}

static void pump_stream(cairn_offload_t *o)
{
    for (int frames = 0; frames < 8; frames++) {
        if (o->tx_len == 0) {
            if (o->remaining == 0) {
                finish_stream(o, CAIRN_OFFLOAD_OK);
                return;
            }
            size_t room = (size_t)o->payload - 2;
            size_t want = (o->remaining < room) ? (size_t)o->remaining : room;
            int n = read_stream(o, o->tx + 2, want);
            if (n < 0 || (size_t)n != want) {
                finish_stream(o, CAIRN_OFFLOAD_IO_ERROR);
                return;
            }
            put16(o->tx, o->seq);
            o->tx_len = 2 + want;
        }

        if (!o->io.notify(o->io.ctx, o->tx, o->tx_len)) return;   /* stack is full: retry */

        size_t data = o->tx_len - 2;
        o->crc = cairn_offload_crc32_update(o->crc, o->tx + 2, data);
        o->sent += data;
        o->remaining -= data;
        o->seq++;
        o->tx_len = 0;
        o->last_progress_ms = o->io.now_ms(o->io.ctx);
    }
}

/* ── requests ─────────────────────────────────────────────────────────────── */

static void do_list(cairn_offload_t *o, uint8_t rid, const uint8_t *arg, size_t arg_len)
{
    if (arg_len != 2) {
        respond(o, CAIRN_OFFLOAD_OP_LIST, rid, CAIRN_OFFLOAD_BAD_ARGUMENT, NULL, 0);
        return;
    }
    uint16_t first = get16(arg);

    char   names[CAIRN_OFFLOAD_MAX_BUNDLES][27];
    size_t count = bundle_names(names, CAIRN_OFFLOAD_MAX_BUNDLES);

    /* Only bundles whose manifest reads and decodes are listed: an entry the
     * phone cannot act on is worse than none. */
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        uint8_t id[16];
        if (!cairn_ulid_decode(names[i], id) || !load_manifest(names[i])) continue;
        summary_t *s = &s_list[n++];
        memcpy(s->id, id, 16);
        memcpy(s->text, names[i], 27);
        s->stream_bytes = stream_bytes(&s_m);
        s->manifest_len = (uint16_t)s_man_len;
        s->chunk_count = (uint16_t)s_m.chunk_count;
        s->state = cairn_receipt_exists(names[i]) ? 1 : 0;
    }

    size_t fit = (o->payload - 8) / CAIRN_OFFLOAD_LIST_ENTRY;
    if (fit > 8) fit = 8;

    uint8_t out[5 + 8 * CAIRN_OFFLOAD_LIST_ENTRY];
    put16(out, (uint16_t)n);
    put16(out + 2, first);
    size_t k = 0;
    for (size_t i = first; i < n && k < fit; i++, k++) {
        uint8_t *e = out + 5 + k * CAIRN_OFFLOAD_LIST_ENTRY;
        memcpy(e, s_list[i].id, 16);
        put64(e + 16, s_list[i].stream_bytes);
        put16(e + 24, s_list[i].manifest_len);
        put16(e + 26, s_list[i].chunk_count);
        e[28] = s_list[i].state;
    }
    out[4] = (uint8_t)k;
    respond(o, CAIRN_OFFLOAD_OP_LIST, rid, CAIRN_OFFLOAD_OK, out, 5 + k * CAIRN_OFFLOAD_LIST_ENTRY);
}

static void do_get_manifest(cairn_offload_t *o, uint8_t rid, const uint8_t *arg, size_t arg_len)
{
    if (arg_len != 16) {
        respond(o, CAIRN_OFFLOAD_OP_GET_MANIFEST, rid, CAIRN_OFFLOAD_BAD_ARGUMENT, NULL, 0);
        return;
    }
    cairn_offload_status_t st = find_bundle(o, arg);
    if (st != CAIRN_OFFLOAD_OK) {
        respond(o, CAIRN_OFFLOAD_OP_GET_MANIFEST, rid, st, NULL, 0);
        return;
    }
    uint32_t total = (uint32_t)(s_man_len + 64);
    uint8_t p[4];
    put32(p, total);
    begin_stream(o, CAIRN_OFFLOAD_OP_GET_MANIFEST, rid, total, true);
    respond(o, CAIRN_OFFLOAD_OP_GET_MANIFEST, rid, CAIRN_OFFLOAD_OK, p, 4);
}

static void do_read(cairn_offload_t *o, uint8_t rid, const uint8_t *arg, size_t arg_len)
{
    if (arg_len != 16 + 8 + 4) {
        respond(o, CAIRN_OFFLOAD_OP_READ, rid, CAIRN_OFFLOAD_BAD_ARGUMENT, NULL, 0);
        return;
    }
    uint64_t offset = get64(arg + 16);
    uint32_t length = get32(arg + 24);

    cairn_offload_status_t st = find_bundle(o, arg);
    if (st != CAIRN_OFFLOAD_OK) {
        respond(o, CAIRN_OFFLOAD_OP_READ, rid, st, NULL, 0);
        return;
    }

    /* The bounds are written so no u64 can wrap: length is capped first, then the
     * range is compared against the total by subtraction. */
    uint64_t total = stream_bytes(&s_m);
    if (length == 0 || length > CAIRN_OFFLOAD_MAX_READ || offset >= total || (uint64_t)length > total - offset) {
        respond(o, CAIRN_OFFLOAD_OP_READ, rid, CAIRN_OFFLOAD_BAD_ARGUMENT, NULL, 0);
        return;
    }

    begin_stream(o, CAIRN_OFFLOAD_OP_READ, rid, length, false);

    /* Position at `offset`: skip whole members, then seek inside the one it falls in. */
    uint64_t pos = 0;
    while (o->member < s_m.member_count && offset >= pos + s_m.members[o->member].length) {
        pos += s_m.members[o->member].length;
        o->member++;
    }
    o->member_pos = offset - pos;

    uint8_t p[4];
    put32(p, length);
    respond(o, CAIRN_OFFLOAD_OP_READ, rid, CAIRN_OFFLOAD_OK, p, 4);
}

static void do_put_receipt(cairn_offload_t *o, uint8_t rid, const uint8_t *arg, size_t arg_len)
{
    if (arg_len != 16 + 2) {
        respond(o, CAIRN_OFFLOAD_OP_PUT_RECEIPT, rid, CAIRN_OFFLOAD_BAD_ARGUMENT, NULL, 0);
        return;
    }
    uint16_t len = get16(arg + 16);
    if (len == 0 || len > CAIRN_OFFLOAD_MAX_RECEIPT) {
        respond(o, CAIRN_OFFLOAD_OP_PUT_RECEIPT, rid, CAIRN_OFFLOAD_BAD_RECEIPT_LENGTH, NULL, 0);
        return;
    }
    cairn_offload_status_t st = find_bundle(o, arg);
    if (st != CAIRN_OFFLOAD_OK) {
        respond(o, CAIRN_OFFLOAD_OP_PUT_RECEIPT, rid, st, NULL, 0);
        return;
    }

    /* The content root the receipt must acknowledge is this bundle's own, read
     * from its manifest on the card, never from anything the phone sends. */
    memcpy(o->content_root, s_m.content_root, 32);

    o->state = CAIRN_OFFLOAD_RECEIPT_RX;
    o->op = CAIRN_OFFLOAD_OP_PUT_RECEIPT;
    o->request_id = rid;
    o->receipt_len = len;
    o->receipt_got = 0;
    o->receipt_seq = 0;
    o->last_progress_ms = o->io.now_ms(o->io.ctx);
    respond(o, CAIRN_OFFLOAD_OP_PUT_RECEIPT, rid, CAIRN_OFFLOAD_OK, NULL, 0);
}

/* The 0x84 indication ending a PUT_RECEIPT. */
static void finish_receipt(cairn_offload_t *o, uint8_t status, uint8_t outcome)
{
    uint8_t rid = o->request_id;
    end_operation(o);
    respond(o, CAIRN_OFFLOAD_OP_PUT_RECEIPT, rid, status, &outcome,
            status == CAIRN_OFFLOAD_OK ? 1 : 0);
}

static void apply_receipt(cairn_offload_t *o)
{
    /*
     * Verify BEFORE writing anything: a forged receipt must not overwrite a
     * genuine one already on the card, and must not leave a receipt file that
     * makes the bundle look "receipted". Only a receipt that verifies against the
     * pinned key AND names this bundle's content root is stored, and only then
     * is the prune attempted.
     */
    cairn_prune_result_t chk =
        cairn_receipt_check(o->receipt, o->receipt_len, o->io.pinned_key, o->content_root);

    switch (chk) {
    case CAIRN_PRUNE_OK:
        break;
    case CAIRN_PRUNE_NO_PINNED_KEY:
        CAIRN_LOGW(TAG, "no server key is pinned; the receipt for %s was not verified and "
                        "nothing was stored or deleted", o->id_text);
        finish_receipt(o, CAIRN_OFFLOAD_OK, CAIRN_OFFLOAD_OUTCOME_NO_KEY);
        return;
    case CAIRN_PRUNE_WRONG_BUNDLE:
        CAIRN_LOGE(TAG, "receipt for %s is genuine but names different content", o->id_text);
        finish_receipt(o, CAIRN_OFFLOAD_OK, CAIRN_OFFLOAD_OUTCOME_WRONG_ROOT);
        return;
    default:
        CAIRN_LOGE(TAG, "receipt for %s is malformed or does not verify (%s)", o->id_text,
                   cairn_prune_result_name(chk));
        finish_receipt(o, CAIRN_OFFLOAD_OK, CAIRN_OFFLOAD_OUTCOME_BAD_SIG);
        return;
    }

    if (!cairn_receipt_store(o->id_text, o->receipt, o->receipt_len)) {
        CAIRN_LOGW(TAG, "cannot store the receipt for %s; keeping the bundle", o->id_text);
        finish_receipt(o, CAIRN_OFFLOAD_OK, CAIRN_OFFLOAD_OUTCOME_RETAINED);
        return;
    }

    cairn_prune_result_t pr = cairn_prune_if_receipted(o->id_text, o->receipt, o->receipt_len,
                                                       o->io.pinned_key, o->content_root);
    finish_receipt(o, CAIRN_OFFLOAD_OK,
                   pr == CAIRN_PRUNE_OK ? CAIRN_OFFLOAD_OUTCOME_PRUNED
                                        : CAIRN_OFFLOAD_OUTCOME_RETAINED);
}

void cairn_offload_on_control_write(cairn_offload_t *o, const uint8_t *data, size_t len)
{
    if (len < 2) return;                 /* nothing to answer: no request id */
    uint8_t op = data[0], rid = data[1];
    const uint8_t *arg = data + 2;
    size_t arg_len = len - 2;

    if (op == CAIRN_OFFLOAD_OP_ABORT) {
        bool was_active = (o->state != CAIRN_OFFLOAD_IDLE);
        end_operation(o);
        respond(o, CAIRN_OFFLOAD_OP_ABORT, rid,
                was_active ? CAIRN_OFFLOAD_OK : CAIRN_OFFLOAD_NO_TRANSFER, NULL, 0);
        return;
    }

    if (o->ind_pending) return;          /* the stack has not taken the last response */

    if (op < CAIRN_OFFLOAD_OP_LIST || op > CAIRN_OFFLOAD_OP_PUT_RECEIPT) {
        respond(o, op, rid, CAIRN_OFFLOAD_BAD_ARGUMENT, NULL, 0);
        return;
    }
    if (o->state != CAIRN_OFFLOAD_IDLE) {
        respond(o, op, rid, CAIRN_OFFLOAD_BUSY, NULL, 0);
        return;
    }
    if (o->io.trip_active(o->io.ctx)) {
        respond(o, op, rid, CAIRN_OFFLOAD_TRIP_ACTIVE, NULL, 0);
        return;
    }
    if (o->payload < CAIRN_OFFLOAD_MIN_PAYLOAD) {
        respond(o, op, rid, CAIRN_OFFLOAD_IO_ERROR, NULL, 0);   /* negotiate a larger MTU first */
        return;
    }

    switch (op) {
    case CAIRN_OFFLOAD_OP_LIST:         do_list(o, rid, arg, arg_len);         break;
    case CAIRN_OFFLOAD_OP_GET_MANIFEST: do_get_manifest(o, rid, arg, arg_len); break;
    case CAIRN_OFFLOAD_OP_READ:         do_read(o, rid, arg, arg_len);         break;
    default:                            do_put_receipt(o, rid, arg, arg_len);  break;
    }
}

void cairn_offload_on_data_write(cairn_offload_t *o, const uint8_t *data, size_t len)
{
    if (o->state != CAIRN_OFFLOAD_RECEIPT_RX) return;

    if (len < 3 || get16(data) != o->receipt_seq) {
        finish_receipt(o, CAIRN_OFFLOAD_BAD_ARGUMENT, 0);
        return;
    }
    size_t n = len - 2;
    if ((size_t)o->receipt_got + n > o->receipt_len) {
        finish_receipt(o, CAIRN_OFFLOAD_BAD_RECEIPT_LENGTH, 0);
        return;
    }
    memcpy(o->receipt + o->receipt_got, data + 2, n);
    o->receipt_got = (uint16_t)(o->receipt_got + n);
    o->receipt_seq++;
    o->last_progress_ms = o->io.now_ms(o->io.ctx);

    if (o->receipt_got == o->receipt_len) apply_receipt(o);
}

void cairn_offload_pump(cairn_offload_t *o)
{
    uint32_t now = o->io.now_ms(o->io.ctx);

    try_send_ind(o);
    if (o->ind_pending) {
        /* A response the stack will not take must not hold the module forever. */
        if ((uint32_t)(now - o->last_progress_ms) > CAIRN_OFFLOAD_STALL_MS) {
            end_operation(o);
            o->ind_pending = false;
        }
        return;
    }

    if (o->state == CAIRN_OFFLOAD_STREAM) {
        if (o->io.trip_active(o->io.ctx)) {
            finish_stream(o, CAIRN_OFFLOAD_TRIP_ACTIVE);
            return;
        }
        if ((uint32_t)(now - o->last_progress_ms) > CAIRN_OFFLOAD_STALL_MS) {
            finish_stream(o, CAIRN_OFFLOAD_IO_ERROR);
            return;
        }
        pump_stream(o);
    } else if (o->state == CAIRN_OFFLOAD_RECEIPT_RX) {
        if (o->io.trip_active(o->io.ctx)) {
            finish_receipt(o, CAIRN_OFFLOAD_TRIP_ACTIVE, 0);
        } else if ((uint32_t)(now - o->last_progress_ms) > CAIRN_OFFLOAD_STALL_MS) {
            finish_receipt(o, CAIRN_OFFLOAD_NO_TRANSFER, 0);
        }
    }
}
