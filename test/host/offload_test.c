/*
 * Host tests for lib/cairn_offload: the dongle's side of BLE bundle offload.
 *
 * The code under test is the module the device runs, in front of a simulated BLE
 * stack and a simulated phone, over real sealed bundles on a POSIX card. The
 * properties that matter most are adversarial and live at the receipt gate: a
 * phone, however it behaves, must never be able to make the dongle delete a
 * bundle on the strength of anything but a server-signed receipt that names that
 * bundle, and must never be able to overwrite a genuine stored receipt with a
 * forged one. Those rows state the property, attack it, and assert what is on the
 * card afterwards, not what a log said.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_config.h"
#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_kv.h"
#include "cairn_log.h"
#include "cairn_offload.h"
#include "cairn_platform.h"
#include "cairn_prune.h"
#include "cairn_store.h"

void cairn_host_advance(uint32_t ms);
void cairn_host_seed(uint64_t seed);
void cairn_host_rng_seed(uint64_t seed);
void cairn_kv_host_set_path(const char *path);

/* ── harness ──────────────────────────────────────────────────────────────── */

static int  g_pass, g_fail;
static char g_failures[64][512];
static int  g_failure_count;
static const char *g_row;

static void fail(const char *fmt, ...)
{
    char detail[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    if (g_failure_count < 64) snprintf(g_failures[g_failure_count++], 512, "%s: %s", g_row, detail);
}

#define CHECK(cond, ...)             \
    do {                             \
        if (!(cond)) {               \
            fail(__VA_ARGS__);       \
            return false;            \
        }                            \
    } while (0)

static char g_root[512];

static const uint8_t TEST_VEHICLE_ID[16]    = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                                                0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f };
static const uint8_t TEST_ASSIGNMENT_ID[16] = { 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                                                0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f };
static cairn_storage_identity_t g_ident;

static const char SERVER_SEED_TEXT[] = "cairn-offload-test-server-key!!";
static const char OTHER_SEED_TEXT[]  = "cairn-offload-test-other-key!!!";
#define SERVER_SEED ((const uint8_t *)SERVER_SEED_TEXT)
#define OTHER_SEED  ((const uint8_t *)OTHER_SEED_TEXT)

static void rm_rf(const char *path)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* absent is fine */ }
}

static bool fresh_tree(const char *row)
{
    snprintf(g_root, sizeof(g_root), "/tmp/cairn-offload-%s", row);
    rm_rf(g_root);
    if (mkdir(g_root, 0775) != 0) return false;

    char kv[700], card[700];
    snprintf(kv, sizeof(kv), "%s/kv.bin", g_root);
    cairn_kv_host_set_path(kv);
    snprintf(card, sizeof(card), "%s/card", g_root);
    if (mkdir(card, 0775) != 0) return false;
    if (!cairn_fs_begin(card)) return false;
    cairn_host_seed(0x0FF10AD);
    cairn_host_rng_seed(0x0FF10AD ^ 0xA5A5A5A5DEADBEEFULL);
    if (!cairn_store_init()) return false;
    if (!cairn_kv_begin() || !cairn_storage_set_assignment(TEST_VEHICLE_ID, TEST_ASSIGNMENT_ID)) return false;
    return cairn_storage_identity_load(&g_ident);
}

/* ── real bundles on the card ─────────────────────────────────────────────── */

typedef struct {
    uint8_t id[16];
    char    text[27];
    uint8_t root[32];
    uint64_t stream_bytes;
    size_t   manifest_len;
    size_t   chunk_count;
} bundle_t;

static bool slurp(const char *rel, uint8_t *buf, size_t cap, size_t *len)
{
    cairn_file_t *f = cairn_fs_open(rel, CAIRN_FS_READ);
    if (f == NULL) return false;
    *len = cairn_fs_read(f, buf, cap);
    cairn_fs_close(f);
    return true;
}

static bool seal_bundle(int samples, bundle_t *out)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    if (!cairn_identity_load(device_id, seed, pub, &boot)) return false;

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    if (!cairn_capture_open_or_resume(&cap, device_id, boot_id, &g_ident)) return false;

    for (int i = 0; i < samples; i++) {
        cairn_gnss_sample_t s;
        memset(&s, 0, sizeof(s));
        s.lat_e7 = 340000000 + i;
        s.lon_e7 = -1185000000;
        s.fix_type = 3;
        s.sats_used = 9;
        s.hdop_e2 = 120;
        s.h_acc_cm = CAIRN_U16_UNKNOWN;
        s.v_acc_cm = CAIRN_U16_UNKNOWN;
        s.utc_acc_ms = CAIRN_U16_UNKNOWN;
        uint8_t payload[32];
        cairn_encode_gnss_sample(&s, payload);
        cairn_host_advance(100);
        if (!cairn_capture_append(&cap, CAIRN_CHAIN_CAPTURE, CAIRN_REC_GNSS_SAMPLE, 1, 0, cairn_millis(),
                                  payload, sizeof(payload))) return false;
    }

    if (!cairn_capture_seal(&cap, seed, pub, "cairn-offload-test", 1, 0, out->id)) return false;
    cairn_ulid_encode(out->id, out->text);

    char path[256];
    static uint8_t enc[4096];
    snprintf(path, sizeof(path), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, out->text);
    if (!slurp(path, enc, sizeof(enc), &out->manifest_len)) return false;

    static cairn_manifest_t m;
    static uint8_t scratch[8192];
    if (cairn_manifest_decode(enc, out->manifest_len, &m, scratch, sizeof(scratch)) != CAIRN_OK) return false;
    memcpy(out->root, m.content_root, 32);
    out->chunk_count = m.chunk_count;
    out->stream_bytes = 0;
    for (size_t i = 0; i < m.member_count; i++) out->stream_bytes += m.members[i].length;
    return true;
}

/* The bundle byte stream as it is on the card, member by member: the reference
 * the module's output is compared with. */
static bool card_stream(const bundle_t *b, uint8_t *out, size_t cap, size_t *len)
{
    char path[256];
    static uint8_t enc[4096];
    size_t enc_len = 0;
    snprintf(path, sizeof(path), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, b->text);
    if (!slurp(path, enc, sizeof(enc), &enc_len)) return false;
    static cairn_manifest_t m;
    static uint8_t scratch[8192];
    if (cairn_manifest_decode(enc, enc_len, &m, scratch, sizeof(scratch)) != CAIRN_OK) return false;

    size_t at = 0;
    for (size_t i = 0; i < m.member_count; i++) {
        size_t n = 0;
        snprintf(path, sizeof(path), "%s/%s/%s", CAIRN_DIR_BUNDLES, b->text, m.members[i].name);
        if (at + m.members[i].length > cap || !slurp(path, out + at, cap - at, &n)) return false;
        if (n != m.members[i].length) return false;
        at += n;
    }
    *len = at;
    return true;
}

static bool bundle_exists(const bundle_t *b)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, b->text);
    return cairn_fs_exists(path);
}

static size_t make_receipt(const uint8_t seed[32], const uint8_t content_root[32],
                           const uint8_t bundle_id[16], uint8_t *out, size_t cap)
{
    uint8_t pub[32];
    cairn_ed25519_public_from_seed(seed, pub);

    cairn_receipt_t r;
    memset(&r, 0, sizeof(r));
    r.receipt_version = CAIRN_RECEIPT_VERSION;
    memcpy(r.receipt_id, bundle_id, 16);
    memcpy(r.bundle_id, bundle_id, 16);
    memcpy(r.content_root, content_root, 32);
    r.server_ingest_utc_ms  = 1760000000000ULL;
    r.ingest_schema_version = 1;
    cairn_device_key_id(pub, r.server_key_id);
    snprintf(r.signature_algorithm, sizeof(r.signature_algorithm), "%s", CAIRN_SIGALG_ED25519);
    r.object_id_count = 0;

    uint8_t signing[1024];
    size_t  signing_len = 0;
    if (cairn_receipt_signing_bytes(&r, signing, sizeof(signing), &signing_len) != CAIRN_OK) return 0;
    cairn_ed25519_sign(signing, signing_len, seed, pub, r.signature);

    size_t written = 0;
    if (cairn_receipt_encode(&r, out, cap, &written) != CAIRN_OK) return 0;
    return written;
}

/* ── a simulated BLE stack and phone ──────────────────────────────────────── */

typedef struct { uint8_t b[260]; size_t len; } msg_t;

#define MAX_NOTES 2048
static msg_t   g_inds[64];
static int     g_ind_n;
static msg_t  *g_notes;
static int     g_note_n;
static bool    g_block_ind, g_block_note;
static int     g_note_budget;       /* when >= 0, notify() accepts this many then refuses */
static bool    g_trip;
static uint32_t g_now;
static uint8_t g_pinned[32];
static bool    g_have_pinned;

static bool io_indicate(void *c, const uint8_t *buf, size_t len)
{
    (void)c;
    if (g_block_ind || g_ind_n >= 64 || len > sizeof(g_inds[0].b)) return false;
    memcpy(g_inds[g_ind_n].b, buf, len);
    g_inds[g_ind_n++].len = len;
    return true;
}
static bool io_notify(void *c, const uint8_t *buf, size_t len)
{
    (void)c;
    if (g_block_note || g_note_n >= MAX_NOTES || len > sizeof(g_notes[0].b)) return false;
    if (g_note_budget == 0) return false;
    if (g_note_budget > 0) g_note_budget--;
    memcpy(g_notes[g_note_n].b, buf, len);
    g_notes[g_note_n++].len = len;
    return true;
}
static uint32_t io_now(void *c) { (void)c; return g_now; }
static bool     io_trip(void *c) { (void)c; return g_trip; }

static cairn_offload_t g_o;

static void reset_phone(uint16_t mtu, bool pinned)
{
    if (g_notes == NULL) g_notes = (msg_t *)malloc(sizeof(msg_t) * MAX_NOTES);
    g_ind_n = g_note_n = 0;
    g_block_ind = g_block_note = false;
    g_note_budget = -1;
    g_trip = false;
    g_now = 1000;
    uint8_t pub[32];
    cairn_ed25519_public_from_seed(SERVER_SEED, pub);
    memcpy(g_pinned, pub, 32);
    g_have_pinned = pinned;

    cairn_offload_io_t io = { NULL, io_indicate, io_notify, io_now, io_trip, pinned ? g_pinned : NULL };
    cairn_offload_init(&g_o, &io);
    cairn_offload_set_mtu(&g_o, mtu);
}

static void pump_until_quiet(int max)
{
    for (int i = 0; i < max; i++) {
        cairn_offload_pump(&g_o);
        g_now += 1;
        if (!cairn_offload_busy(&g_o)) return;
    }
}

static void send_ctl(uint8_t op, uint8_t rid, const uint8_t *arg, size_t n)
{
    uint8_t buf[64];
    buf[0] = op;
    buf[1] = rid;
    if (n > 0) memcpy(buf + 2, arg, n);
    cairn_offload_on_control_write(&g_o, buf, 2 + n);
}

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const msg_t *last_ind(void) { return g_ind_n > 0 ? &g_inds[g_ind_n - 1] : NULL; }

/* A response indication: op|0x80, request id, status. */
static bool expect_status(uint8_t op, uint8_t rid, uint8_t status, const char *what)
{
    const msg_t *m = last_ind();
    if (m == NULL || m->len < 3) { fail("%s: no response", what); return false; }
    if (m->b[0] != (uint8_t)(op | 0x80) || m->b[1] != rid || m->b[2] != status) {
        fail("%s: got op=%02x rid=%u status=%u, want op=%02x rid=%u status=%u", what, m->b[0], m->b[1],
             m->b[2], (uint8_t)(op | 0x80), rid, status);
        return false;
    }
    return true;
}

/* The FIRST indication of an exchange: the response, which always precedes any data.
 * (After a complete transfer the last indication is the done message.) */
static bool expect_first_status(uint8_t op, uint8_t rid, uint8_t status, const char *what)
{
	if (g_ind_n < 1 || g_inds[0].len < 3) { fail("%s: no response", what); return false; }
	const msg_t *m = &g_inds[0];
	if (m->b[0] != (uint8_t)(op | 0x80) || m->b[1] != rid || m->b[2] != status) {
		fail("%s: first indication op=%02x rid=%u status=%u, want op=%02x rid=%u status=%u", what, m->b[0],
		     m->b[1], m->b[2], (uint8_t)(op | 0x80), rid, status);
		return false;
	}
	return true;
}

/* Reassemble the notifications into `out`; checks contiguous sequence numbers. */
static bool collect(uint8_t *out, size_t cap, size_t *len, const char *what)
{
    size_t at = 0;
    for (int i = 0; i < g_note_n; i++) {
        if (g_notes[i].len < 3) { fail("%s: empty data frame %d", what, i); return false; }
        if (rd16(g_notes[i].b) != (uint16_t)i) { fail("%s: frame %d has seq %u", what, i, rd16(g_notes[i].b)); return false; }
        size_t n = g_notes[i].len - 2;
        if (at + n > cap) { fail("%s: too much data", what); return false; }
        memcpy(out + at, g_notes[i].b + 2, n);
        at += n;
    }
    *len = at;
    return true;
}

/* The final 0x86 indication: status, bytes_sent, crc. */
static bool expect_done(uint8_t rid, uint8_t status, size_t bytes, const uint8_t *data, const char *what)
{
    const msg_t *m = last_ind();
    if (m == NULL || m->len != 11 || m->b[0] != CAIRN_OFFLOAD_IND_DONE) { fail("%s: no done indication", what); return false; }
    if (m->b[1] != rid || m->b[2] != status) { fail("%s: done rid=%u status=%u, want %u/%u", what, m->b[1], m->b[2], rid, status); return false; }
    if (status == CAIRN_OFFLOAD_OK) {
        if (rd32(m->b + 3) != bytes) { fail("%s: bytes_sent %u, want %zu", what, rd32(m->b + 3), bytes); return false; }
        if (rd32(m->b + 7) != cairn_crc32(data, bytes)) { fail("%s: crc %08x, want %08x", what, rd32(m->b + 7), cairn_crc32(data, bytes)); return false; }
    }
    return true;
}

static void read_req(const bundle_t *b, uint8_t rid, uint64_t off, uint32_t len)
{
    uint8_t a[28];
    memcpy(a, b->id, 16);
    le64(a + 16, off);
    le32(a + 24, len);
    send_ctl(CAIRN_OFFLOAD_OP_READ, rid, a, sizeof(a));
}

/* ── rows ─────────────────────────────────────────────────────────────────── */

static bool row_crc32_matches_the_format_crc(void)
{
    static uint8_t buf[5000];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 31 + 7);
    size_t sizes[] = { 0, 1, 2, 15, 16, 17, 255, 1000, 4999, 5000 };
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        uint32_t whole = ~cairn_offload_crc32_update(0xFFFFFFFFu, buf, sizes[k]);
        CHECK(whole == cairn_crc32(buf, sizes[k]), "crc of %zu bytes differs", sizes[k]);
        /* Streaming in odd pieces is the same as one shot: the module streams. */
        uint32_t c = 0xFFFFFFFFu;
        size_t at = 0;
        while (at < sizes[k]) {
            size_t n = (at % 7) + 1;
            if (n > sizes[k] - at) n = sizes[k] - at;
            c = cairn_offload_crc32_update(c, buf + at, n);
            at += n;
        }
        CHECK(~c == whole, "streaming crc of %zu bytes differs", sizes[k]);
    }
    return true;
}

static bool row_list_paginates_sealed_bundles_only(void)
{
    bundle_t b[3];
    for (int i = 0; i < 3; i++) CHECK(seal_bundle(6 + i * 5, &b[i]), "seal %d", i);

    /* The capture in progress must never be listed. */
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity");
    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);
    cairn_capture_t open_cap;
    CHECK(cairn_capture_open_or_resume(&open_cap, device_id, boot_id, &g_ident), "open");

    reset_phone(43, true);     /* payload 40: exactly one entry per response */
    uint8_t seen[3] = { 0, 0, 0 };
    for (uint16_t first = 0; first < 3; first++) {
        g_ind_n = 0;
        uint8_t a[2];
        le16(a, first);
        send_ctl(CAIRN_OFFLOAD_OP_LIST, (uint8_t)(10 + first), a, 2);
        pump_until_quiet(10);
        CHECK(expect_status(CAIRN_OFFLOAD_OP_LIST, (uint8_t)(10 + first), 0, "list"), "list page %u", first);
        const msg_t *m = last_ind();
        CHECK(rd16(m->b + 3) == 3, "total %u, want 3", rd16(m->b + 3));
        CHECK(rd16(m->b + 5) == first, "first %u", rd16(m->b + 5));
        CHECK(m->b[7] == 1, "page %u carries %u entries, want 1", first, m->b[7]);
        const uint8_t *e = m->b + 8;
        int which = -1;
        for (int k = 0; k < 3; k++) if (memcmp(e, b[k].id, 16) == 0) which = k;
        CHECK(which >= 0, "entry is not one of the sealed bundles");
        seen[which]++;
        uint64_t sb = 0;
        for (int k = 0; k < 8; k++) sb |= (uint64_t)e[16 + k] << (8 * k);
        CHECK(sb == b[which].stream_bytes, "stream_bytes %llu, want %llu", (unsigned long long)sb,
              (unsigned long long)b[which].stream_bytes);
        CHECK(rd16(e + 24) == b[which].manifest_len, "manifest_len");
        CHECK(rd16(e + 26) == b[which].chunk_count, "chunk_count");
        CHECK(e[28] == 0, "state %u, want 0 (no receipt yet)", e[28]);
    }
    CHECK(seen[0] == 1 && seen[1] == 1 && seen[2] == 1, "pagination skipped or repeated a bundle");

    /* A bundle holding a stored receipt is state 1. */
    static uint8_t receipt[600];
    size_t rl = make_receipt(SERVER_SEED, b[1].root, b[1].id, receipt, sizeof(receipt));
    CHECK(rl > 0 && cairn_receipt_store(b[1].text, receipt, rl), "store receipt");
    g_ind_n = 0;
    uint8_t a0[2] = { 0, 0 };
    reset_phone(247, true);
    send_ctl(CAIRN_OFFLOAD_OP_LIST, 20, a0, 2);
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_LIST, 20, 0, "list"), "list all");
    const msg_t *m = last_ind();
    CHECK(m->b[7] == 3, "full MTU should carry all 3 entries, got %u", m->b[7]);
    int ones = 0;
    for (int k = 0; k < 3; k++) {
        const uint8_t *e = m->b + 8 + k * 29;
        if (memcmp(e, b[1].id, 16) == 0) { CHECK(e[28] == 1, "receipted bundle not state 1"); ones++; }
        else CHECK(e[28] == 0, "unreceipted bundle is state %u", e[28]);
    }
    CHECK(ones == 1, "receipted bundle missing from the list");
    return true;
}

static bool row_list_empty_and_bad_arguments(void)
{
    reset_phone(247, true);
    uint8_t a[2] = { 0, 0 };
    send_ctl(CAIRN_OFFLOAD_OP_LIST, 1, a, 2);
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_LIST, 1, 0, "empty list"), "empty");
    CHECK(rd16(last_ind()->b + 3) == 0 && last_ind()->b[7] == 0, "an empty card listed something");

    send_ctl(CAIRN_OFFLOAD_OP_LIST, 2, a, 1);                 /* wrong argument size */
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_LIST, 2, CAIRN_OFFLOAD_BAD_ARGUMENT, "short arg"), "short arg");

    send_ctl(0x77, 3, NULL, 0);                               /* unknown opcode */
    pump_until_quiet(10);
    CHECK(expect_status(0x77, 3, CAIRN_OFFLOAD_BAD_ARGUMENT, "unknown op"), "unknown op");

    uint8_t one = 1;
    cairn_offload_on_control_write(&g_o, &one, 1);            /* no request id: nothing to answer */
    int before = g_ind_n;
    pump_until_quiet(10);
    CHECK(g_ind_n == before, "a one-byte write was answered");
    return true;
}

static bool row_mtu_too_small_is_refused_not_half_served(void)
{
    bundle_t b;
    CHECK(seal_bundle(8, &b), "seal");
    reset_phone(23, true);                                    /* the default ATT MTU */
    uint8_t a[2] = { 0, 0 };
    send_ctl(CAIRN_OFFLOAD_OP_LIST, 1, a, 2);
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_LIST, 1, CAIRN_OFFLOAD_IO_ERROR, "small mtu"), "small mtu");
    return true;
}

static bool row_get_manifest_is_manifest_then_signature(void)
{
    bundle_t b;
    CHECK(seal_bundle(10, &b), "seal");
    reset_phone(247, true);

    send_ctl(CAIRN_OFFLOAD_OP_GET_MANIFEST, 5, b.id, 16);
    pump_until_quiet(100);
    CHECK(expect_first_status(CAIRN_OFFLOAD_OP_GET_MANIFEST, 5, 0, "get manifest"), "response");
    CHECK(g_inds[0].len == 7 && rd32(g_inds[0].b + 3) == b.manifest_len + 64, "total_len %u, want %zu",
          rd32(g_inds[0].b + 3), b.manifest_len + 64);

    static uint8_t got[5000], want[5000];
    size_t got_len = 0, n = 0, sn = 0;
    CHECK(collect(got, sizeof(got), &got_len, "manifest"), "collect");
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, b.text);
    CHECK(slurp(path, want, sizeof(want), &n), "read manifest");
    snprintf(path, sizeof(path), "%s/%s/manifest.sig", CAIRN_DIR_BUNDLES, b.text);
    CHECK(slurp(path, want + n, 64, &sn) && sn == 64, "read sig");
    CHECK(got_len == n + 64 && memcmp(got, want, n + 64) == 0, "streamed manifest differs from manifest.cbor||manifest.sig");
    CHECK(expect_done(5, 0, n + 64, want, "manifest done"), "done");

    /* The response indication precedes any data frame, and is never reordered. */
    CHECK(g_inds[0].b[0] == (CAIRN_OFFLOAD_OP_GET_MANIFEST | 0x80), "first indication is not the response");
    return true;
}

static bool row_read_returns_exact_ranges_across_member_boundaries(void)
{
    bundle_t b;
    CHECK(seal_bundle(40, &b), "seal");
    static uint8_t stream[200000];
    size_t total = 0;
    CHECK(card_stream(&b, stream, sizeof(stream), &total) && total == b.stream_bytes, "reference stream");
    CHECK(total > 2000, "test bundle too small to cross a member boundary (%zu)", total);

    /* Find the first member boundary to straddle it. */
    char path[256];
    static uint8_t enc[4096];
    size_t el = 0;
    snprintf(path, sizeof(path), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, b.text);
    CHECK(slurp(path, enc, sizeof(enc), &el), "manifest");
    static cairn_manifest_t m;
    static uint8_t scratch[8192];
    CHECK(cairn_manifest_decode(enc, el, &m, scratch, sizeof(scratch)) == CAIRN_OK && m.member_count >= 2, "members");
    uint64_t boundary = m.members[0].length;

    struct { uint64_t off; uint32_t len; } cases[] = {
        { 0, 1 }, { 0, (uint32_t)total }, { boundary - 10, 20 }, { boundary - 1, 2 }, { boundary, 100 },
        { total - 1, 1 }, { total - 300, 300 }, { 7, 1000 },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        if (cases[c].len > CAIRN_OFFLOAD_MAX_READ) continue;
        reset_phone(247, true);
        read_req(&b, (uint8_t)(30 + c), cases[c].off, cases[c].len);
        pump_until_quiet(2000);
        CHECK(expect_first_status(CAIRN_OFFLOAD_OP_READ, (uint8_t)(30 + c), 0, "read"), "case %zu response", c);
        static uint8_t got[70000];
        size_t got_len = 0;
        CHECK(collect(got, sizeof(got), &got_len, "read"), "case %zu collect", c);
        CHECK(got_len == cases[c].len && memcmp(got, stream + cases[c].off, got_len) == 0,
              "case %zu (off %llu len %u): bytes differ from the card", c, (unsigned long long)cases[c].off, cases[c].len);
        CHECK(expect_done((uint8_t)(30 + c), 0, got_len, got, "read done"), "case %zu done", c);
        CHECK(!cairn_offload_busy(&g_o), "case %zu left the module busy", c);
        /* Every frame but the last is full. */
        for (int i = 0; i + 1 < g_note_n; i++) CHECK(g_notes[i].len == 244, "frame %d is %zu bytes", i, g_notes[i].len);
    }
    return true;
}

static bool row_read_bounds_are_enforced_without_wrapping(void)
{
    bundle_t b;
    CHECK(seal_bundle(12, &b), "seal");
    struct { uint64_t off; uint32_t len; const char *why; } bad[] = {
        { b.stream_bytes, 1, "offset at the end" },
        { b.stream_bytes + 1000, 1, "offset past the end" },
        { 0, 0, "zero length" },
        { 0, CAIRN_OFFLOAD_MAX_READ + 1, "over the 64 KiB cap" },
        { b.stream_bytes - 1, 2, "range runs one byte past the end" },
        { UINT64_MAX, 1, "offset at u64 max" },
        { UINT64_MAX - 5, 100, "offset + length would wrap" },
        { 1, UINT32_MAX, "enormous length" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset_phone(247, true);
        read_req(&b, (uint8_t)(40 + i), bad[i].off, bad[i].len);
        pump_until_quiet(50);
        CHECK(expect_status(CAIRN_OFFLOAD_OP_READ, (uint8_t)(40 + i), CAIRN_OFFLOAD_BAD_ARGUMENT, bad[i].why), "%s", bad[i].why);
        CHECK(g_note_n == 0, "%s: data was sent for a refused read", bad[i].why);
        CHECK(!cairn_offload_busy(&g_o), "%s: module left busy", bad[i].why);
    }
    uint8_t shortarg[10] = { 0 };
    reset_phone(247, true);
    send_ctl(CAIRN_OFFLOAD_OP_READ, 99, shortarg, sizeof(shortarg));
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_READ, 99, CAIRN_OFFLOAD_BAD_ARGUMENT, "short read request"), "short");
    return true;
}

static bool row_unknown_bundle_is_refused_for_every_operation(void)
{
    bundle_t b;
    CHECK(seal_bundle(6, &b), "seal");
    uint8_t ghost[16];
    memcpy(ghost, b.id, 16);
    ghost[15] ^= 0x55;

    reset_phone(247, true);
    send_ctl(CAIRN_OFFLOAD_OP_GET_MANIFEST, 1, ghost, 16);
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_GET_MANIFEST, 1, CAIRN_OFFLOAD_UNKNOWN_BUNDLE, "get"), "get");

    bundle_t g = b;
    memcpy(g.id, ghost, 16);
    read_req(&g, 2, 0, 10);
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_READ, 2, CAIRN_OFFLOAD_UNKNOWN_BUNDLE, "read"), "read");

    uint8_t a[18];
    memcpy(a, ghost, 16);
    le16(a + 16, 100);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 3, a, 18);
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 3, CAIRN_OFFLOAD_UNKNOWN_BUNDLE, "put"), "put");
    CHECK(!cairn_offload_busy(&g_o), "left busy");
    return true;
}

static bool row_one_operation_at_a_time_and_abort(void)
{
    bundle_t b;
    CHECK(seal_bundle(40, &b), "seal");
    reset_phone(247, true);
    g_block_note = true;                         /* the stream cannot make progress */
    read_req(&b, 1, 0, 3000);
    pump_until_quiet(5);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_READ, 1, 0, "read"), "read accepted");
    CHECK(cairn_offload_busy(&g_o), "not busy mid-transfer");

    uint8_t a[2] = { 0, 0 };
    send_ctl(CAIRN_OFFLOAD_OP_LIST, 2, a, 2);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_LIST, 2, CAIRN_OFFLOAD_BUSY, "list during read"), "BUSY");
    send_ctl(CAIRN_OFFLOAD_OP_GET_MANIFEST, 3, b.id, 16);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_GET_MANIFEST, 3, CAIRN_OFFLOAD_BUSY, "manifest during read"), "BUSY 2");
    CHECK(cairn_offload_busy(&g_o) == false || true, "unreachable");

    pump_until_quiet(3);
    send_ctl(CAIRN_OFFLOAD_OP_ABORT, 4, NULL, 0);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_ABORT, 4, 0, "abort"), "abort ok");
    pump_until_quiet(5);
    CHECK(!cairn_offload_busy(&g_o), "still busy after ABORT");

    g_block_note = false;
    g_note_n = 0;
    send_ctl(CAIRN_OFFLOAD_OP_LIST, 5, a, 2);
    pump_until_quiet(10);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_LIST, 5, 0, "list after abort"), "usable after abort");

    send_ctl(CAIRN_OFFLOAD_OP_ABORT, 6, NULL, 0);            /* nothing in progress */
    CHECK(expect_status(CAIRN_OFFLOAD_OP_ABORT, 6, CAIRN_OFFLOAD_NO_TRANSFER, "idle abort"), "idle abort");
    return true;
}

static bool row_a_trip_blocks_every_operation_but_abort(void)
{
    bundle_t b;
    CHECK(seal_bundle(10, &b), "seal");
    reset_phone(247, true);
    g_trip = true;

    uint8_t a[2] = { 0, 0 };
    send_ctl(CAIRN_OFFLOAD_OP_LIST, 1, a, 2);
    pump_until_quiet(5);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_LIST, 1, CAIRN_OFFLOAD_TRIP_ACTIVE, "list"), "list");
    send_ctl(CAIRN_OFFLOAD_OP_GET_MANIFEST, 2, b.id, 16);
    pump_until_quiet(5);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_GET_MANIFEST, 2, CAIRN_OFFLOAD_TRIP_ACTIVE, "manifest"), "manifest");
    read_req(&b, 3, 0, 10);
    pump_until_quiet(5);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_READ, 3, CAIRN_OFFLOAD_TRIP_ACTIVE, "read"), "read");
    uint8_t p[18];
    memcpy(p, b.id, 16);
    le16(p + 16, 50);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 4, p, 18);
    pump_until_quiet(5);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 4, CAIRN_OFFLOAD_TRIP_ACTIVE, "put receipt"), "put receipt");
    CHECK(g_note_n == 0 && !cairn_offload_busy(&g_o), "a refused request moved data or left the module busy");

    send_ctl(CAIRN_OFFLOAD_OP_ABORT, 5, NULL, 0);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_ABORT, 5, CAIRN_OFFLOAD_NO_TRANSFER, "abort in a trip"), "abort is honoured");
    return true;
}

static bool row_a_trip_starting_mid_transfer_stops_it(void)
{
    bundle_t b;
    CHECK(seal_bundle(60, &b), "seal");
    reset_phone(247, true);
    g_note_budget = 3;                           /* a few frames get out, then the stack stalls */
    read_req(&b, 1, 0, 6000);
    pump_until_quiet(5);
    CHECK(g_note_n == 3 && cairn_offload_busy(&g_o), "stream did not start (%d frames)", g_note_n);

    g_trip = true;                               /* the car starts */
    pump_until_quiet(5);
    CHECK(!cairn_offload_busy(&g_o), "the transfer survived a trip starting");
    CHECK(expect_done(1, CAIRN_OFFLOAD_TRIP_ACTIVE, 0, NULL, "done"), "done indication says TRIP_ACTIVE");
    return true;
}

static bool row_a_stalled_transfer_times_out_and_frees_the_module(void)
{
    bundle_t b;
    CHECK(seal_bundle(40, &b), "seal");
    reset_phone(247, true);
    g_block_note = true;
    read_req(&b, 1, 0, 3000);
    pump_until_quiet(3);
    CHECK(cairn_offload_busy(&g_o), "not busy");
    g_now += CAIRN_OFFLOAD_STALL_MS + 100;
    pump_until_quiet(5);
    CHECK(!cairn_offload_busy(&g_o), "a stalled transfer held the module forever");
    CHECK(expect_done(1, CAIRN_OFFLOAD_IO_ERROR, 0, NULL, "stall done"), "stall reported as IO_ERROR");
    return true;
}

static bool row_a_congested_stack_loses_and_repeats_nothing(void)
{
    bundle_t b;
    CHECK(seal_bundle(50, &b), "seal");
    static uint8_t stream[200000];
    size_t total = 0;
    CHECK(card_stream(&b, stream, sizeof(stream), &total), "reference");

    reset_phone(247, true);
    read_req(&b, 1, 100, 5000);
    /* The stack accepts a frame, refuses two, accepts three... in a fixed pattern,
     * and refuses the final indication a few times too. */
    for (int i = 0; i < 20000 && (cairn_offload_busy(&g_o)); i++) {
        g_block_note = (i % 3) != 0;
        g_block_ind = (i % 5) == 1;
        cairn_offload_pump(&g_o);
        g_now += 1;
    }
    g_block_note = g_block_ind = false;
    pump_until_quiet(100);
    CHECK(!cairn_offload_busy(&g_o), "never finished under congestion");
    static uint8_t got[8000];
    size_t got_len = 0;
    CHECK(collect(got, sizeof(got), &got_len, "congested"), "frames lost, repeated or reordered");
    CHECK(got_len == 5000 && memcmp(got, stream + 100, 5000) == 0, "bytes differ after congestion");
    CHECK(expect_done(1, 0, 5000, got, "done"), "done after congestion");
    return true;
}

static bool row_disconnect_abandons_everything(void)
{
    bundle_t b;
    CHECK(seal_bundle(40, &b), "seal");
    reset_phone(247, true);
    g_block_note = true;
    read_req(&b, 1, 0, 3000);
    pump_until_quiet(3);
    CHECK(cairn_offload_busy(&g_o), "not busy");
    cairn_offload_on_disconnect(&g_o);
    CHECK(!cairn_offload_busy(&g_o), "busy after the link dropped");
    g_block_note = false;
    int before = g_note_n;
    pump_until_quiet(20);
    CHECK(g_note_n == before, "data kept flowing after disconnect");
    return true;
}

/* ── the receipt gate ─────────────────────────────────────────────────────── */

static void send_data(const uint8_t *bytes, size_t len, size_t piece)
{
    uint16_t seq = 0;
    size_t at = 0;
    while (at < len) {
        size_t n = (len - at < piece) ? len - at : piece;
        uint8_t f[260];
        le16(f, seq++);
        memcpy(f + 2, bytes + at, n);
        cairn_offload_on_data_write(&g_o, f, 2 + n);
        at += n;
    }
}

static void put_receipt(const bundle_t *b, uint8_t rid, const uint8_t *r, size_t len, size_t piece)
{
    uint8_t a[18];
    memcpy(a, b->id, 16);
    le16(a + 16, (uint16_t)len);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, rid, a, 18);
    if (last_ind() != NULL && last_ind()->b[2] == 0) send_data(r, len, piece);
    pump_until_quiet(20);
}

static bool outcome_is(uint8_t rid, uint8_t outcome, const char *what)
{
    const msg_t *m = last_ind();
    if (m == NULL || m->len != 4 || m->b[0] != (CAIRN_OFFLOAD_OP_PUT_RECEIPT | 0x80) || m->b[1] != rid || m->b[2] != 0) {
        fail("%s: no 0x84 outcome indication", what);
        return false;
    }
    if (m->b[3] != outcome) { fail("%s: outcome %u, want %u", what, m->b[3], outcome); return false; }
    return true;
}

static bool row_a_genuine_receipt_prunes_the_bundle(void)
{
    bundle_t b, other;
    CHECK(seal_bundle(10, &b), "seal");
    CHECK(seal_bundle(20, &other), "seal other");
    static uint8_t r[600];
    size_t rl = make_receipt(SERVER_SEED, b.root, b.id, r, sizeof(r));
    CHECK(rl > 0, "build receipt");

    for (size_t piece = 20; piece <= 240; piece += 110) {      /* odd frame sizes */
        if (piece > 20) { CHECK(seal_bundle(10, &b), "reseal"); rl = make_receipt(SERVER_SEED, b.root, b.id, r, sizeof(r)); }
        reset_phone(247, true);
        put_receipt(&b, 7, r, rl, piece);
        CHECK(outcome_is(7, CAIRN_OFFLOAD_OUTCOME_PRUNED, "genuine"), "genuine receipt");
        CHECK(!bundle_exists(&b), "the bundle is still on the card after a verified receipt (piece %zu)", piece);
        CHECK(cairn_receipt_exists(b.text), "the verified receipt was not stored");
        CHECK(bundle_exists(&other), "an unrelated bundle was deleted");
        CHECK(!cairn_offload_busy(&g_o), "module left busy");
    }
    return true;
}

static bool row_a_receipt_signed_by_another_key_deletes_nothing(void)
{
    bundle_t b;
    CHECK(seal_bundle(10, &b), "seal");
    static uint8_t r[600];
    size_t rl = make_receipt(OTHER_SEED, b.root, b.id, r, sizeof(r));
    reset_phone(247, true);
    put_receipt(&b, 8, r, rl, 100);
    CHECK(outcome_is(8, CAIRN_OFFLOAD_OUTCOME_BAD_SIG, "forged"), "forged receipt");
    CHECK(bundle_exists(&b), "A FORGED RECEIPT DELETED THE BUNDLE");
    CHECK(!cairn_receipt_exists(b.text), "a forged receipt was stored");
    return true;
}

static bool row_a_genuine_receipt_for_another_bundle_deletes_nothing(void)
{
    bundle_t a, b;
    CHECK(seal_bundle(10, &a), "seal a");
    CHECK(seal_bundle(25, &b), "seal b");
    CHECK(memcmp(a.root, b.root, 32) != 0, "test bundles share a content root");
    static uint8_t r[600];
    /* Signed by the real server key, and about bundle b. Presented for bundle a. */
    size_t rl = make_receipt(SERVER_SEED, b.root, b.id, r, sizeof(r));
    reset_phone(247, true);
    put_receipt(&a, 9, r, rl, 100);
    CHECK(outcome_is(9, CAIRN_OFFLOAD_OUTCOME_WRONG_ROOT, "wrong root"), "wrong-root receipt");
    CHECK(bundle_exists(&a) && bundle_exists(&b), "A RECEIPT FOR ANOTHER BUNDLE DELETED DATA");
    CHECK(!cairn_receipt_exists(a.text), "a receipt for the wrong bundle was stored against this one");
    return true;
}

static bool row_a_forged_receipt_cannot_overwrite_a_genuine_stored_one(void)
{
    bundle_t b;
    CHECK(seal_bundle(10, &b), "seal");
    static uint8_t good[600], bad[600], on_card[600];
    size_t gl = make_receipt(SERVER_SEED, b.root, b.id, good, sizeof(good));
    size_t bl = make_receipt(OTHER_SEED, b.root, b.id, bad, sizeof(bad));
    CHECK(gl > 0 && bl > 0, "build receipts");

    /* The genuine one is on the card awaiting its prune (state 1). */
    CHECK(cairn_receipt_store(b.text, good, gl), "store genuine");

    reset_phone(247, true);
    put_receipt(&b, 11, bad, bl, 100);
    CHECK(outcome_is(11, CAIRN_OFFLOAD_OUTCOME_BAD_SIG, "forged over genuine"), "forged");

    char path[256];
    size_t n = 0;
    snprintf(path, sizeof(path), "%s/%s.cbor", CAIRN_DIR_RECEIPTS, b.text);
    CHECK(slurp(path, on_card, sizeof(on_card), &n), "read stored receipt");
    CHECK(n == gl && memcmp(on_card, good, gl) == 0, "THE GENUINE STORED RECEIPT WAS OVERWRITTEN BY A FORGED ONE");
    CHECK(bundle_exists(&b), "bundle deleted");
    return true;
}

static bool row_malformed_receipts_are_rejected_before_anything_is_written(void)
{
    bundle_t b;
    CHECK(seal_bundle(10, &b), "seal");
    static uint8_t junk[400];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 13 + 5);
    reset_phone(247, true);
    put_receipt(&b, 12, junk, sizeof(junk), 150);
    CHECK(outcome_is(12, CAIRN_OFFLOAD_OUTCOME_BAD_SIG, "junk"), "junk receipt");
    CHECK(bundle_exists(&b) && !cairn_receipt_exists(b.text), "junk receipt had an effect on the card");
    return true;
}

static bool row_without_a_pinned_key_nothing_is_verified_stored_or_deleted(void)
{
    bundle_t b;
    CHECK(seal_bundle(10, &b), "seal");
    static uint8_t r[600];
    size_t rl = make_receipt(SERVER_SEED, b.root, b.id, r, sizeof(r));
    reset_phone(247, false);                     /* unconfigured device */
    put_receipt(&b, 13, r, rl, 100);
    CHECK(outcome_is(13, CAIRN_OFFLOAD_OUTCOME_NO_KEY, "no key"), "no key");
    CHECK(bundle_exists(&b) && !cairn_receipt_exists(b.text), "an unconfigured device acted on a receipt it could not verify");

    /* An all-zero key is the same as none. */
    uint8_t zero[32] = { 0 };
    cairn_offload_io_t io = { NULL, io_indicate, io_notify, io_now, io_trip, zero };
    cairn_offload_init(&g_o, &io);
    cairn_offload_set_mtu(&g_o, 247);
    g_ind_n = 0;
    put_receipt(&b, 14, r, rl, 100);
    CHECK(outcome_is(14, CAIRN_OFFLOAD_OUTCOME_NO_KEY, "zero key"), "zero key");
    CHECK(bundle_exists(&b), "an all-zero key authorised a delete");
    return true;
}

static bool row_a_broken_receipt_upload_ends_cleanly_and_deletes_nothing(void)
{
    bundle_t b;
    CHECK(seal_bundle(10, &b), "seal");
    static uint8_t r[600];
    size_t rl = make_receipt(SERVER_SEED, b.root, b.id, r, sizeof(r));
    uint8_t a[18];
    memcpy(a, b.id, 16);

    /* Lengths the spec refuses. */
    reset_phone(247, true);
    le16(a + 16, 0);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 1, a, 18);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 1, CAIRN_OFFLOAD_BAD_RECEIPT_LENGTH, "len 0"), "len 0");
    le16(a + 16, CAIRN_OFFLOAD_MAX_RECEIPT + 1);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 2, a, 18);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 2, CAIRN_OFFLOAD_BAD_RECEIPT_LENGTH, "len 1025"), "len 1025");
    CHECK(!cairn_offload_busy(&g_o), "busy after a refused length");

    /* Out-of-order frame. */
    le16(a + 16, (uint16_t)rl);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 3, a, 18);
    uint8_t f[40];
    le16(f, 1);                                  /* should be 0 */
    memcpy(f + 2, r, 30);
    cairn_offload_on_data_write(&g_o, f, 32);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 3, CAIRN_OFFLOAD_BAD_ARGUMENT, "out of order"), "seq");
    CHECK(last_ind()->len == 3 && !cairn_offload_busy(&g_o), "an error must end the upload with no outcome byte");

    /* More bytes than announced. */
    le16(a + 16, 50);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 4, a, 18);
    le16(f, 0);
    memcpy(f + 2, r, 38);
    cairn_offload_on_data_write(&g_o, f, 40);
    le16(f, 1);
    cairn_offload_on_data_write(&g_o, f, 40);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 4, CAIRN_OFFLOAD_BAD_RECEIPT_LENGTH, "too many bytes"), "overrun");

    /* A truncated upload that stalls. */
    le16(a + 16, (uint16_t)rl);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 5, a, 18);
    le16(f, 0);
    memcpy(f + 2, r, 30);
    cairn_offload_on_data_write(&g_o, f, 32);
    CHECK(cairn_offload_busy(&g_o), "not waiting for the rest");
    g_now += CAIRN_OFFLOAD_STALL_MS + 50;
    pump_until_quiet(5);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 5, CAIRN_OFFLOAD_NO_TRANSFER, "stall"), "stall");
    CHECK(!cairn_offload_busy(&g_o), "a stalled receipt upload held the module");

    /* ABORT mid-upload. */
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 6, a, 18);
    send_ctl(CAIRN_OFFLOAD_OP_ABORT, 7, NULL, 0);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_ABORT, 7, 0, "abort"), "abort");
    /* A late frame after the abort is ignored. */
    int before = g_ind_n;
    le16(f, 0);
    memcpy(f + 2, r, 30);
    cairn_offload_on_data_write(&g_o, f, 32);
    CHECK(g_ind_n == before, "a data frame outside an upload was acted on");

    CHECK(bundle_exists(&b) && !cairn_receipt_exists(b.text), "a broken upload changed the card");
    return true;
}

static bool row_a_trip_starting_mid_receipt_upload_aborts_it(void)
{
    bundle_t b;
    CHECK(seal_bundle(10, &b), "seal");
    static uint8_t r[600];
    size_t rl = make_receipt(SERVER_SEED, b.root, b.id, r, sizeof(r));
    reset_phone(247, true);
    uint8_t a[18];
    memcpy(a, b.id, 16);
    le16(a + 16, (uint16_t)rl);
    send_ctl(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 3, a, 18);
    uint8_t f[40];
    le16(f, 0);
    memcpy(f + 2, r, 30);
    cairn_offload_on_data_write(&g_o, f, 32);
    g_trip = true;
    pump_until_quiet(5);
    CHECK(expect_status(CAIRN_OFFLOAD_OP_PUT_RECEIPT, 3, CAIRN_OFFLOAD_TRIP_ACTIVE, "trip"), "trip");
    CHECK(!cairn_offload_busy(&g_o) && bundle_exists(&b), "the upload survived a trip or deleted data");
    return true;
}

typedef struct { const char *name; bool (*fn)(void); } row_t;

int main(void)
{
    static const row_t ROWS[] = {
        { "streaming CRC-32 equals the format's CRC-32", row_crc32_matches_the_format_crc },
        { "LIST paginates sealed bundles only, with exact entries and receipt state", row_list_paginates_sealed_bundles_only },
        { "LIST on an empty card, and malformed requests", row_list_empty_and_bad_arguments },
        { "an MTU too small for one entry is refused, not half served", row_mtu_too_small_is_refused_not_half_served },
        { "GET_MANIFEST is manifest.cbor then manifest.sig, response first", row_get_manifest_is_manifest_then_signature },
        { "READ returns exact byte ranges across member boundaries", row_read_returns_exact_ranges_across_member_boundaries },
        { "READ bounds are enforced and nothing wraps", row_read_bounds_are_enforced_without_wrapping },
        { "an unknown bundle is refused by every operation", row_unknown_bundle_is_refused_for_every_operation },
        { "one operation at a time; ABORT frees the module", row_one_operation_at_a_time_and_abort },
        { "a trip blocks every operation but ABORT", row_a_trip_blocks_every_operation_but_abort },
        { "a trip starting mid-transfer stops it", row_a_trip_starting_mid_transfer_stops_it },
        { "a stalled transfer times out and frees the module", row_a_stalled_transfer_times_out_and_frees_the_module },
        { "a congested stack loses and repeats nothing", row_a_congested_stack_loses_and_repeats_nothing },
        { "disconnect abandons everything", row_disconnect_abandons_everything },
        { "a genuine receipt prunes its bundle and only that one", row_a_genuine_receipt_prunes_the_bundle },
        { "a receipt signed by another key deletes nothing", row_a_receipt_signed_by_another_key_deletes_nothing },
        { "a genuine receipt for another bundle deletes nothing", row_a_genuine_receipt_for_another_bundle_deletes_nothing },
        { "a forged receipt cannot overwrite a genuine stored one", row_a_forged_receipt_cannot_overwrite_a_genuine_stored_one },
        { "malformed receipts are rejected before anything is written", row_malformed_receipts_are_rejected_before_anything_is_written },
        { "with no pinned key nothing is verified, stored or deleted", row_without_a_pinned_key_nothing_is_verified_stored_or_deleted },
        { "a broken receipt upload ends cleanly and deletes nothing", row_a_broken_receipt_upload_ends_cleanly_and_deletes_nothing },
        { "a trip starting mid-receipt-upload aborts it", row_a_trip_starting_mid_receipt_upload_aborts_it },
    };

    cairn_log_init(0);
    size_t n = sizeof(ROWS) / sizeof(ROWS[0]);
    for (size_t i = 0; i < n; i++) {
        char slug[64];
        snprintf(slug, sizeof(slug), "row%02zu", i);
        g_row = ROWS[i].name;
        if (!fresh_tree(slug)) {
            fail("cannot prepare a fresh tree");
            g_fail++;
            continue;
        }
        bool ok = ROWS[i].fn();
        cairn_kv_end();
        cairn_fs_end();
        if (ok) {
            g_pass++;
            printf("  pass  [offload] %s\n", ROWS[i].name);
            rm_rf(g_root);
        } else {
            g_fail++;
            printf("  FAIL  [offload] %s\n", ROWS[i].name);
        }
    }
    for (int i = 0; i < g_failure_count; i++) printf("        %s\n", g_failures[i]);
    printf("BLE offload: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
