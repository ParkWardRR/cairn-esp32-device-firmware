/*
 * Storage: framed append, atomic seal, boot recovery.
 *
 * Portable C over cairn_fs / cairn_kv / cairn_platform, with no Arduino or IDF
 * dependency. That is not tidiness — it is what makes this file's claims
 * testable. "A torn tail is truncated to the last valid frame with an exact
 * discarded byte count" and "an interrupted seal completes idempotently at the
 * next boot" are properties about crash behaviour, and the host fault tests in
 * test/host exercise exactly this code by tearing files and re-opening them.
 */

#include "cairn_store.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cairn_fs.h"
#include "cairn_kv.h"
#include "cairn_log.h"
#include "cairn_platform.h"

static const char *TAG = "STORE";

/* 256 KiB chunks, the specification's default. With CAIRN_MAX_CHUNKS at 64 this
 * bounds a bundle at 16 MiB, above what CAIRN_MAX_MEMBERS segments can hold. */
/*
 * Target bytes per chunk.
 *
 * 8 KiB, down from 256 KiB. A chunk is the unit the server accepts or refuses
 * whole — it is stored under its own digest — so it is also the unit of lost
 * work when a link dies mid-transfer. At 256 KiB a typical ~230 KB bundle
 * sealed as a *single* chunk, which meant an interrupted cellular upload
 * restarted from zero and the offer's missing-chunks resume had nothing to
 * resume. At 8 KiB the same bundle is around 29 chunks, so progress
 * accumulates and a dead slot costs one chunk.
 *
 * 8 rather than 4 KiB: 4 doubles the per-chunk bookkeeping (a descriptor is 40
 * bytes in the signed manifest, and every chunk is a separate HTTP request) for
 * a finer resume granularity than any measurement yet justifies. Revisit if
 * poor-link measurements show 8 KiB chunks frequently failing before they are
 * acknowledged.
 *
 * The owner intends this to become a web-UI setting. Nothing outside the sealer
 * needs to agree with it: the size is recorded per chunk in the signed manifest,
 * cairn_intake derives each chunk's offset by summing the preceding
 * descriptors, the Rust emulator takes chunk_size as a parameter (its relay
 * matrix runs 128-byte chunks), and the Go server only reads
 * len(ChunkDescriptors). So this value can change per bundle without breaking
 * any reader.
 */
#define CHUNK_TARGET_BYTES (8u * 1024u)

/* Streaming I/O block: large enough to keep SPI efficient, small enough to sit
 * in DRAM alongside everything else. */
#define IO_BLOCK 2048

#define PATH_MAX_LEN 160

/* ── ULID ─────────────────────────────────────────────────────────────────── */

static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/*
 * A ULID is the operational handle for a bundle, not its identity — identity is
 * content_root. The timestamp prefix exists only so listings sort
 * chronologically, so a device with no valid clock gets a zero prefix rather
 * than a fabricated time.
 */
static void ulid_generate(uint8_t out[16], uint64_t utc_ms)
{
    out[0] = (uint8_t)(utc_ms >> 40);
    out[1] = (uint8_t)(utc_ms >> 32);
    out[2] = (uint8_t)(utc_ms >> 24);
    out[3] = (uint8_t)(utc_ms >> 16);
    out[4] = (uint8_t)(utc_ms >> 8);
    out[5] = (uint8_t)(utc_ms);

    cairn_random(out + 6, 10);
}

void cairn_ulid_encode(const uint8_t id[16], char out[27])
{
    /* 128 bits as 26 base32 characters, most significant first. The leading
     * character carries only 3 bits, which is inherent to 26 * 5 = 130. */
    for (int i = 0; i < 26; i++) {
        int      bit_pos = i * 5 - 2;
        uint32_t v = 0;

        for (int b = 0; b < 5; b++) {
            int      pos = bit_pos + b;
            uint32_t bit = 0;
            if (pos >= 0 && pos < 128) bit = (id[pos / 8] >> (7 - (pos % 8))) & 1u;
            v = (v << 1) | bit;
        }
        out[i] = CROCKFORD[v & 0x1f];
    }
    out[26] = '\0';
}

static int crockford_value(char c)
{
    for (int i = 0; i < 32; i++) {
        if (CROCKFORD[i] == c) return i;
    }
    return -1;
}

bool cairn_ulid_decode(const char *s, uint8_t out[16])
{
    if (strlen(s) != 26) return false;

    memset(out, 0, 16);
    for (int i = 0; i < 26; i++) {
        int v = crockford_value(s[i]);
        if (v < 0) return false;

        int bit_pos = i * 5 - 2;
        for (int b = 0; b < 5; b++) {
            int pos = bit_pos + b;
            if (pos < 0 || pos >= 128) continue;
            if (((uint32_t)(v >> (4 - b)) & 1u) != 0) {
                out[pos / 8] |= (uint8_t)(1u << (7 - (pos % 8)));
            }
        }
    }
    return true;
}

/* ── identity ─────────────────────────────────────────────────────────────── */

bool cairn_identity_load(uint8_t device_id[16], uint8_t seed[32],
                         uint8_t pub[32], uint32_t *boot_count)
{
    if (!cairn_kv_begin()) {
        CAIRN_LOGE(TAG, "identity store unavailable");
        return false;
    }

    /*
     * device_id is derived from a hardware-unique value, so wiping the key
     * store does not change which vehicle the data came from.
     */
    if (!cairn_kv_get_blob("device_id", device_id, 16)) {
        uint8_t unique[6];
        uint8_t digest[32];

        cairn_hw_unique_id(unique);
        cairn_sha256(unique, sizeof(unique), digest);
        memcpy(device_id, digest, 16);

        cairn_kv_set_blob("device_id", device_id, 16);
        CAIRN_LOGW(TAG, "device_id initialized from the hardware id");
    }

    /*
     * The signing seed is generated once from the hardware RNG and cannot be
     * re-derived, so losing the key store means a new key and re-enrolment on
     * the server. That is the correct outcome, and it is loud rather than
     * silent.
     *
     * Flash encryption is deliberately not enabled (see partitions-ab.csv), so
     * this seed is readable from a physically extracted chip. Accepted trade
     * for a device that must stay recoverable on a bench: the key authorizes
     * uploads, not deletions, and the server can revoke it.
     */
    if (!cairn_kv_get_blob("key_seed", seed, 32)) {
        cairn_random(seed, 32);
        cairn_kv_set_blob("key_seed", seed, 32);
        CAIRN_LOGW(TAG, "generated a new device signing key; the server must "
                        "enrol this device before it can upload");
    }

    uint32_t count = cairn_kv_get_u32("boot_count", 0) + 1;
    cairn_kv_set_u32("boot_count", count);
    *boot_count = count;

    cairn_ed25519_public_from_seed(seed, pub);

    uint8_t key_id[8];
    cairn_device_key_id(pub, key_id);
    CAIRN_LOGI(TAG, "identity: device %02x%02x%02x%02x.. key_id "
                    "%02x%02x%02x%02x%02x%02x%02x%02x boot %u",
               device_id[0], device_id[1], device_id[2], device_id[3],
               key_id[0], key_id[1], key_id[2], key_id[3], key_id[4],
               key_id[5], key_id[6], key_id[7], (unsigned)count);

    return true;
}

static void hexify(const uint8_t *b, size_t len, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = d[b[i] >> 4];
        out[i * 2 + 1] = d[b[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

void cairn_identity_print_enrolment(const uint8_t device_id[16],
                                    const uint8_t pub[32])
{
    char id_hex[33], pub_hex[65];

    hexify(device_id, 16, id_hex);
    hexify(pub, 32, pub_hex);

    CAIRN_LOGI(TAG, "device_id  %s", id_hex);
    CAIRN_LOGI(TAG, "public_key %s", pub_hex);

    /*
     * Printed as a runnable command rather than as two values to assemble.
     * Transcribing 96 hex characters off a serial console is exactly the step
     * where a bench session loses twenty minutes.
     */
    CAIRN_LOGI(TAG, "to enrol:  cairn-server -enroll %s -enroll-key %s "
                    "-enroll-name car", id_hex, pub_hex);
    CAIRN_LOGI(TAG, "until enrolled, uploads are refused with 403 and bundles "
                    "stay on the card");
}

void cairn_new_boot_id(uint8_t boot_id[16])
{
    cairn_random(boot_id, 16);
}

/* ── storage identity (format v3) ─────────────────────────────────────────── */

/*
 * NVS keys. At most 15 characters, the NVS limit. They live in internal flash,
 * not on the card: a counter or a root kept on the card would travel with a
 * copied or restored card image, which is exactly what each exists to defeat.
 */
#define KV_STORAGE_ROOT    "storage_root"
#define KV_STORAGE_VERSION "storage_ver"
#define KV_DEVICE_COUNTER  "dev_counter"
#define KV_VEHICLE_ID      "vehicle_id"
#define KV_ASSIGNMENT_ID   "assignment_id"

/* The version a freshly generated root is. The server escrows it as version 1
 * at enrolment; a rotation would issue the next. */
#define STORAGE_KEY_VERSION_INITIAL 1u

static bool kv_counter_read(uint64_t *out)
{
    uint8_t b[8];
    if (!cairn_kv_get_blob(KV_DEVICE_COUNTER, b, sizeof(b))) {
        *out = 0;
        return false;
    }
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    *out = v;
    return true;
}

/*
 * Write the counter and read it back. NVS commits on every put, but "the write
 * call returned" and "the value is what a reboot will see" are different
 * claims, and only the second one protects a counter.
 */
static bool kv_counter_write(uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    if (!cairn_kv_set_blob(KV_DEVICE_COUNTER, b, sizeof(b))) return false;

    uint64_t back = 0;
    return kv_counter_read(&back) && back == v;
}

static bool kv_counter_reserve(void *ctx, uint64_t *out)
{
    (void)ctx;
    if (!cairn_kv_begin()) return false;

    uint64_t high = 0;
    (void)kv_counter_read(&high); /* absent: no bundle yet, counters start at 1 */

    if (!kv_counter_write(high + 1)) {
        CAIRN_LOGE(TAG, "cannot persist device counter %llu",
                   (unsigned long long)(high + 1));
        return false;
    }
    *out = high + 1;
    return true;
}

static bool kv_counter_commit(void *ctx, uint64_t counter)
{
    (void)ctx;
    if (!cairn_kv_begin()) return false;

    uint64_t high = 0;
    (void)kv_counter_read(&high);
    if (high >= counter) return true;

    /*
     * The high-water mark is behind a counter already written into a bundle's
     * headers: NVS was erased or restored after the capture opened. Raise it
     * before the manifest is signed, or the next bundle would reuse this one's
     * counter for different content — which the server must quarantine.
     */
    CAIRN_LOGW(TAG, "device counter high-water %llu is behind bundle counter %llu; "
                    "raising it before signing",
               (unsigned long long)high, (unsigned long long)counter);
    return kv_counter_write(counter);
}

uint64_t cairn_storage_counter_high_water(void)
{
    uint64_t v = 0;
    if (cairn_kv_begin()) (void)kv_counter_read(&v);
    return v;
}

static bool all_zero(const uint8_t *b, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= b[i];
    return acc == 0;
}

bool cairn_storage_identity_load(cairn_storage_identity_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!cairn_kv_begin()) {
        CAIRN_LOGE(TAG, "identity store unavailable; no storage key");
        return false;
    }

    /*
     * K_root is generated once, on the device, from the hardware RNG, and never
     * leaves it in the clear. Losing it makes every bundle sealed under it
     * unreadable to anyone but the server's escrowed copy — so a regeneration
     * is logged as the event it is, not silently papered over.
     *
     * Until flash encryption is enabled (docs/esp32-hardening.md) this root is
     * plaintext in on-chip flash: it protects the card against someone who has
     * the card, not against someone who dumps the chip.
     */
    if (!cairn_kv_get_blob(KV_STORAGE_ROOT, out->root_key, CAIRN_ROOT_KEY_SIZE)) {
        cairn_rng_fill(out->root_key, CAIRN_ROOT_KEY_SIZE);
        if (!cairn_kv_set_blob(KV_STORAGE_ROOT, out->root_key, CAIRN_ROOT_KEY_SIZE) ||
            !cairn_kv_set_u32(KV_STORAGE_VERSION, STORAGE_KEY_VERSION_INITIAL)) {
            CAIRN_LOGE(TAG, "cannot persist a new storage root; refusing to seal "
                            "anything under a key that would not survive a reboot");
            memset(out->root_key, 0, sizeof(out->root_key));
            return false;
        }
        CAIRN_LOGW(TAG, "generated a new storage root (version %u); the server must "
                        "escrow it at enrolment before it can decode this device's "
                        "bundles", (unsigned)STORAGE_KEY_VERSION_INITIAL);
    }
    out->storage_key_version =
        cairn_kv_get_u32(KV_STORAGE_VERSION, STORAGE_KEY_VERSION_INITIAL);

    bool have_vehicle = cairn_kv_get_blob(KV_VEHICLE_ID, out->vehicle_id, 16);
    bool have_assign  = cairn_kv_get_blob(KV_ASSIGNMENT_ID, out->assignment_id, 16);

    /*
     * Both or neither: a vehicle without an assignment, or the reverse, is not
     * a binding the server can check, so it is treated as no assignment.
     */
    out->assigned = have_vehicle && have_assign && !all_zero(out->vehicle_id, 16) &&
                    !all_zero(out->assignment_id, 16);
    if (!out->assigned) {
        memset(out->vehicle_id, 0, 16);
        memset(out->assignment_id, 0, 16);
        CAIRN_LOGW(TAG, "UNASSIGNED: no vehicle assignment is provisioned. Capture "
                        "continues, but bundles carry an all-zero vehicle_id and "
                        "assignment_id and the server will refuse them until this "
                        "device is assigned (cairn-admin assign)");
    }

    out->counter_reserve = kv_counter_reserve;
    out->counter_commit  = kv_counter_commit;
    out->counter_ctx     = NULL;

    uint64_t high = 0;
    (void)kv_counter_read(&high);
    CAIRN_LOGI(TAG, "storage: key version %u, counter high-water %llu, %s",
               (unsigned)out->storage_key_version, (unsigned long long)high,
               out->assigned ? "assigned" : "UNASSIGNED");
    return true;
}

bool cairn_storage_set_assignment(const uint8_t vehicle_id[16],
                                  const uint8_t assignment_id[16])
{
    if (!cairn_kv_begin()) return false;
    return cairn_kv_set_blob(KV_VEHICLE_ID, vehicle_id, 16) &&
           cairn_kv_set_blob(KV_ASSIGNMENT_ID, assignment_id, 16);
}

bool cairn_storage_raise_counter(uint64_t floor)
{
    if (!cairn_kv_begin()) return false;

    uint64_t cur = 0;
    (void)kv_counter_read(&cur);
    if (floor <= cur) return true;

    return kv_counter_write(floor);
}

/* ── paths ────────────────────────────────────────────────────────────────── */

static void segment_path(const cairn_capture_t *cap, uint32_t index, char *out,
                         size_t cap_len)
{
    snprintf(out, cap_len, "%s/seg-%08u.seg", cap->dir, (unsigned)index);
}

static void journal_path(const cairn_capture_t *cap, char *out, size_t cap_len)
{
    snprintf(out, cap_len, "%s/journal.seg", cap->dir);
}

/* Defined with the rest of the basis sidecar, below; declared here because the
 * resume path needs it and sits above that. */
static void utc_basis_load(cairn_capture_t *cap);

bool cairn_store_init(void)
{
    const char *dirs[] = {
        CAIRN_DIR_ROOT, CAIRN_DIR_CAPTURE, CAIRN_DIR_BUNDLES,
        CAIRN_DIR_RECEIPTS, CAIRN_DIR_LOGS, CAIRN_DIR_STATE,
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (!cairn_fs_mkdir(dirs[i])) {
            CAIRN_LOGE(TAG, "mkdir %s failed", dirs[i]);
            return false;
        }
    }
    return true;
}

/* ── segment creation ─────────────────────────────────────────────────────── */

/*
 * Bind a cipher to a segment: derive K_seg from the root and the header's own
 * identity fields, and take the header bytes as the AAD suffix. The header,
 * not the caller, decides the key — the same rule the server applies when it
 * derives the key from the escrowed root.
 */
static bool cipher_for_header(const cairn_capture_t *cap, const uint8_t *header_bytes,
                              size_t header_len, cairn_segment_cipher_t *out)
{
    /* This store writes only 128-byte headers, and holds no more than that. */
    if (cap->identity == NULL || header_len != CAIRN_SEGMENT_HEADER_SIZE) return false;

    cairn_segment_header_t h;
    if (cairn_parse_segment_header(header_bytes, header_len, &h, NULL) != CAIRN_OK) {
        return false;
    }

    /*
     * A root of a different version cannot seal for this segment: the server
     * would derive from the version the header names and every frame would
     * fail. Refusing here is what turns that into "this bundle is sealed as
     * is" instead of a bundle of frames nobody can open.
     */
    if (h.storage_key_version != cap->identity->storage_key_version) return false;

    uint8_t key[CAIRN_SEGMENT_KEY_SIZE];
    cairn_derive_segment_key(cap->identity->root_key, &h, key);
    cairn_err_t err = cairn_segment_cipher_init(out, key, header_bytes, header_len);
    memset(key, 0, sizeof(key));
    return err == CAIRN_OK;
}

/*
 * Create a segment file holding only its header, and bind `cipher` to it.
 *
 * Every header of a bundle carries the same device, boot, vehicle, assignment,
 * key version and counter (§5.4) — taken from the capture, never from the
 * current identity, so a capture resumed after a reassignment stays one
 * consistent bundle.
 */
static bool write_segment_header(const cairn_capture_t *cap, const char *path,
                                 uint32_t segment_index, uint32_t first_seq,
                                 cairn_segment_cipher_t *cipher)
{
    cairn_segment_header_t h;
    memset(&h, 0, sizeof(h));

    h.format_version = CAIRN_FORMAT_VERSION;
    memcpy(h.device_id, cap->device_id, 16);
    memcpy(h.boot_id, cap->boot_id, 16);
    memcpy(h.vehicle_id, cap->vehicle_id, 16);
    memcpy(h.assignment_id, cap->assignment_id, 16);
    h.segment_index       = segment_index;
    h.first_seq           = first_seq;
    h.opened_monotonic_us = cairn_micros();
    h.storage_key_version = cap->storage_key_version;
    h.device_counter      = cap->device_counter;

    uint8_t     buf[CAIRN_SEGMENT_HEADER_SIZE];
    cairn_err_t err = cairn_encode_segment_header(&h, buf, sizeof(buf));
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "encode segment header: %s", cairn_strerror(err));
        return false;
    }

    if (!cipher_for_header(cap, buf, sizeof(buf), cipher)) {
        CAIRN_LOGE(TAG, "no usable storage key for %s", path);
        return false;
    }

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_WRITE);
    if (f == NULL) {
        CAIRN_LOGE(TAG, "cannot create %s", path);
        return false;
    }

    size_t n = cairn_fs_write(f, buf, sizeof(buf));
    cairn_fs_flush(f);
    cairn_fs_close(f);

    if (n != sizeof(buf)) {
        CAIRN_LOGE(TAG, "short write on %s header", path);
        return false;
    }

    CAIRN_LOGD(TAG, "opened %s (index %u, first_seq %u)", path,
               (unsigned)segment_index, (unsigned)first_seq);
    return true;
}

/* ── recovery ─────────────────────────────────────────────────────────────── */

static bool fs_read_at(void *user, uint64_t offset, uint8_t *buf, size_t len)
{
    cairn_file_t *f = (cairn_file_t *)user;

    if (!cairn_fs_seek(f, offset)) return false;
    return cairn_fs_read(f, buf, len) == len;
}

/*
 * Truncate a segment to `keep` bytes.
 *
 * There is no truncate in the filesystem abstraction because FatFs does not
 * offer one through the Arduino File API. The surviving prefix is copied to a
 * sibling and renamed over the original; the copy is fully written and flushed
 * before the original is removed, so an interruption leaves either the original
 * — recoverable again next boot — or the complete replacement. Never a
 * half-truncated segment.
 */
static bool truncate_segment(const char *path, uint32_t keep)
{
    char tmp[PATH_MAX_LEN];
    snprintf(tmp, sizeof(tmp), "%s.trunc", path);

    cairn_file_t *src = cairn_fs_open(path, CAIRN_FS_READ);
    if (src == NULL) return false;

    cairn_fs_remove(tmp);
    cairn_file_t *dst = cairn_fs_open(tmp, CAIRN_FS_WRITE);
    if (dst == NULL) {
        cairn_fs_close(src);
        return false;
    }

    uint8_t  block[IO_BLOCK];
    uint32_t copied = 0;
    bool     ok = true;

    while (copied < keep) {
        size_t want = keep - copied;
        if (want > sizeof(block)) want = sizeof(block);

        size_t got = cairn_fs_read(src, block, want);
        if (got == 0 || cairn_fs_write(dst, block, got) != got) {
            ok = false;
            break;
        }
        copied += (uint32_t)got;
    }

    cairn_fs_flush(dst);
    cairn_fs_close(dst);
    cairn_fs_close(src);

    if (!ok || copied != keep) {
        cairn_fs_remove(tmp);
        CAIRN_LOGE(TAG, "truncate copy failed for %s (%u of %u bytes)", path,
                   (unsigned)copied, (unsigned)keep);
        return false;
    }

    if (!cairn_fs_remove(path) || !cairn_fs_rename(tmp, path)) {
        CAIRN_LOGE(TAG, "truncate rename failed for %s", path);
        return false;
    }

    return true;
}

/* What a recovery pass learned about one segment. */
typedef struct {
    uint8_t  header_bytes[CAIRN_SEGMENT_HEADER_SIZE];
    size_t   header_len;
    uint32_t bytes;          /* the segment's length once any tail is gone */
    uint32_t last_frame_len; /* 0 when no frame was retained */
} recovered_t;

/* The structural scan's callback: remember the length of the last frame, so
 * the frame itself can be found again as [bytes - last_frame_len, bytes). */
static bool note_last_frame(const cairn_frame_t *f, void *user)
{
    recovered_t *r = (recovered_t *)user;
    r->last_frame_len =
        (uint32_t)(CAIRN_FRAME_HEADER_SIZE + f->sealed_len + CAIRN_FRAME_TRAILER_SIZE);
    return true;
}

/*
 * Scan one segment, truncate any torn tail, and fold its tallies into the
 * capture state.
 *
 * The scan is structural: it needs no key, so a device that has lost its root
 * still recovers exactly what a device holding it would, and never discards a
 * byte because it could not decrypt it. Whether the segment can also be
 * *extended* under the current key is a separate question, answered by
 * can_extend() for the one segment of each chain that will be appended to.
 *
 * `chain` is carried in and out so capture segments continue one sequence
 * across rotations. A header error leaves the file untouched: the segment is
 * unusable here, but deleting it would discard data the server might still
 * salvage, and nothing in this module may delete bundle data.
 */
static bool recover_segment(cairn_capture_t *cap, const char *path,
                            cairn_chain_t *chain, bool fold_counts,
                            recovered_t *rec)
{
    memset(rec, 0, sizeof(*rec));

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    if (f == NULL) return false;

    uint64_t size = cairn_fs_size(f);

    cairn_scan_state_t state;
    state.expected_seq  = chain->next_seq;
    state.expected_prev = chain->prev_crc32;

    cairn_scan_opts_t   opts = { NULL, NULL, note_last_frame, rec };
    cairn_scan_result_t res;
    cairn_err_t err = cairn_scan_segment_stream(fs_read_at, f, size, state, &opts, &res);

    /* Keep the header bytes: they are the AAD for anything appended later. */
    bool have_header = (err == CAIRN_OK) &&
                       fs_read_at(f, 0, rec->header_bytes, sizeof(rec->header_bytes));
    cairn_fs_close(f);

    if (err != CAIRN_OK || !have_header) {
        CAIRN_LOGE(TAG, "%s is unusable (%s); leaving it in place for the server",
                   path, cairn_strerror(err));
        return false;
    }
    rec->header_len = res.header_len;

    CAIRN_LOGI(TAG, "%s: %s, %u frames, seq %u..%u, %u byte tail discarded",
               path, cairn_stop_reason_name(res.stop), (unsigned)res.frames,
               (unsigned)res.first_seq, (unsigned)res.last_seq,
               (unsigned)res.discarded_tail_bytes);

    if (res.discarded_tail_bytes > 0) {
        /*
         * Honest incompleteness: the exact byte count goes into the manifest
         * and the recovery state is raised, so the server knows this bundle was
         * reconstructed rather than cleanly sealed.
         */
        cap->discarded_tail_bytes += res.discarded_tail_bytes;
        cap->recovery_state = (res.stop == CAIRN_STOP_TORN_TAIL)
                                  ? CAIRN_RECOVERY_RECOVERED_TAIL
                                  : CAIRN_RECOVERY_SALVAGED;

        if (!truncate_segment(path, (uint32_t)res.stop_offset)) {
            CAIRN_LOGW(TAG, "could not truncate %s; appending would corrupt the "
                            "chain, so this capture will be sealed as-is", path);
            return false;
        }
    }

    /*
     * Record counts describe the whole bundle, so both chains fold in.
     */
    if (fold_counts) {
        for (int t = 0; t < 256; t++) cap->record_counts[t] += res.record_counts[t];
    }

    /*
     * The sequence range and have_any_frame describe the *capture* chain alone,
     * and must not be touched when recovering the journal — the journal has its
     * own independent chain, numbered from zero, and its sequence numbers mean
     * nothing in the capture's terms.
     *
     * These used to live inside the fold_counts branch, which was fine only
     * because the journal was recovered with fold_counts false. Turning that on
     * so the journal's records would be counted therefore also let the
     * journal's sequence range overwrite the capture's, and a resumed bundle
     * sealed a manifest claiming a span its segments did not hold — observed as
     * "manifest claims seq 0..28 but the segments hold 0..2". One flag was
     * doing two unrelated jobs; now the condition says which chain it means.
     */
    if (chain == &cap->capture_chain && res.frames > 0) {
        if (!cap->have_any_frame) {
            cap->first_seq = res.first_seq;
            cap->have_any_frame = true;
        }
        cap->last_seq = res.last_seq;
    }

    chain->next_seq   = res.next.expected_seq;
    chain->prev_crc32 = res.next.expected_prev;
    rec->bytes        = (uint32_t)res.stop_offset;

    return true;
}

/*
 * May this segment be appended to under the key the device holds now?
 *
 * Builds the segment's cipher from its own header and, if the segment already
 * holds frames, authenticates the last one. One frame is enough for the
 * question being asked: it proves the root, the derived key and the header
 * binding are the ones the existing frames were sealed under, and the last
 * frame is the one the next frame chains from. Tampering further back is not a
 * reason to stop appending — it is the server's to find, with a full keyed
 * scan, and it can.
 *
 * Refusing is not losing anything: the bundle is sealed as it stands, which
 * needs no key at all.
 */
static bool can_extend(cairn_capture_t *cap, const char *path, const recovered_t *rec,
                       cairn_segment_cipher_t *cipher)
{
    if (!cipher_for_header(cap, rec->header_bytes, rec->header_len, cipher)) {
        CAIRN_LOGW(TAG, "%s was sealed under key version %u, which this device does "
                        "not hold; it will not be extended", path,
                   (unsigned)cap->storage_key_version);
        return false;
    }
    if (rec->last_frame_len == 0) return true;

    uint8_t frame[CAIRN_MAX_FRAME_LEN];
    bool    ok = false;

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    if (f != NULL) {
        ok = fs_read_at(f, rec->bytes - rec->last_frame_len, frame, rec->last_frame_len) &&
             cairn_open_frame(cipher, frame, rec->last_frame_len, NULL, NULL) == CAIRN_OK;
        cairn_fs_close(f);
    }
    memset(frame, 0, sizeof(frame)); /* it held plaintext */

    if (!ok) {
        CAIRN_LOGE(TAG, "%s does not authenticate under this device's storage key "
                        "(a different root, or an edited card); it will be sealed "
                        "as it stands rather than extended", path);
        cairn_segment_cipher_wipe(cipher);
    }
    return ok;
}

/* ── open or resume ───────────────────────────────────────────────────────── */

static bool find_open_capture(char *out, size_t cap_len)
{
    cairn_dir_t *dir = cairn_fs_opendir(CAIRN_DIR_CAPTURE);
    if (dir == NULL) return false;

    bool found = false;
    char name[64];
    bool is_dir = false;

    while (cairn_fs_readdir(dir, name, sizeof(name), &is_dir, NULL)) {
        if (!is_dir) continue;

        uint8_t probe[16];
        if (cairn_ulid_decode(name, probe)) {
            snprintf(out, cap_len, "%s", name);
            found = true;
            break;
        }
    }
    cairn_fs_closedir(dir);
    return found;
}

/*
 * Take a resumed bundle's binding from the first of its headers that parses —
 * segment 0, else the journal. The headers are the authority: they are what
 * every existing frame authenticates, and what the manifest must agree with.
 */
static bool adopt_binding_from_card(cairn_capture_t *cap)
{
    char path[PATH_MAX_LEN];

    for (int which = 0; which < 2; which++) {
        if (which == 0) {
            segment_path(cap, 0, path, sizeof(path));
        } else {
            journal_path(cap, path, sizeof(path));
        }

        cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
        if (f == NULL) continue;

        uint8_t buf[CAIRN_SEGMENT_HEADER_SIZE];
        bool    got = fs_read_at(f, 0, buf, sizeof(buf));
        cairn_fs_close(f);

        cairn_segment_header_t h;
        if (!got || cairn_parse_segment_header(buf, sizeof(buf), &h, NULL) != CAIRN_OK) {
            continue;
        }

        memcpy(cap->device_id, h.device_id, 16);
        memcpy(cap->boot_id, h.boot_id, 16);
        memcpy(cap->vehicle_id, h.vehicle_id, 16);
        memcpy(cap->assignment_id, h.assignment_id, 16);
        cap->storage_key_version = h.storage_key_version;
        cap->device_counter      = h.device_counter;
        cap->assigned = !all_zero(h.vehicle_id, 16) && !all_zero(h.assignment_id, 16);
        return true;
    }
    return false;
}

/* A new bundle's binding: the current identity, and a freshly reserved counter. */
static bool bind_new_bundle(cairn_capture_t *cap)
{
    const cairn_storage_identity_t *id = cap->identity;

    memcpy(cap->vehicle_id, id->vehicle_id, 16);
    memcpy(cap->assignment_id, id->assignment_id, 16);
    cap->assigned            = id->assigned;
    cap->storage_key_version = id->storage_key_version;

    /*
     * Reserved — durably — before any header naming it exists. Every header
     * and so every frame's AAD carries this counter, so it cannot be chosen
     * later; and if it were held only in RAM a power cut before the seal would
     * let the next boot hand the same counter to a different bundle.
     */
    if (id->counter_reserve == NULL ||
        !id->counter_reserve(id->counter_ctx, &cap->device_counter)) {
        CAIRN_LOGE(TAG, "cannot reserve a device counter; refusing to open a bundle "
                        "whose counter a power cut could reuse");
        return false;
    }
    return true;
}

bool cairn_capture_open_or_resume(cairn_capture_t *cap,
                                  const uint8_t device_id[16],
                                  const uint8_t boot_id[16],
                                  const cairn_storage_identity_t *identity)
{
    memset(cap, 0, sizeof(*cap));
    memcpy(cap->device_id, device_id, 16);
    memcpy(cap->boot_id, boot_id, 16);
    cap->identity = identity;
    cap->capture_started_monotonic_us = cairn_micros();
    cap->last_flush_ms = cairn_millis();

    if (identity == NULL) {
        CAIRN_LOGE(TAG, "no storage identity; a v3 capture cannot be opened without one");
        return false;
    }

    char id_text[27];
    char path[PATH_MAX_LEN];

    if (find_open_capture(id_text, sizeof(id_text))) {
        if (!cairn_ulid_decode(id_text, cap->bundle_id)) return false;
        snprintf(cap->dir, sizeof(cap->dir), "%s/%s", CAIRN_DIR_CAPTURE, id_text);

        CAIRN_LOGW(TAG, "resuming interrupted capture %s", id_text);

        /*
         * The bundle keeps the binding it was opened under. With no readable
         * header at all there is nothing to agree with, so it gets a fresh
         * counter like any new bundle — its unreadable segments are left for
         * the server's salvage tool either way.
         */
        if (!adopt_binding_from_card(cap)) {
            CAIRN_LOGW(TAG, "capture %s has no readable segment header", id_text);
            if (!bind_new_bundle(cap)) return false;
        }

        /*
         * Capture segments are walked in index order. A single chain spans
         * them, so out-of-order scanning would make the continuity check
         * meaningless.
         */
        uint32_t    index = 0;
        bool        chain_intact = true;
        recovered_t last;
        memset(&last, 0, sizeof(last));

        for (; index < CAIRN_MAX_MEMBERS; index++) {
            segment_path(cap, index, path, sizeof(path));
            if (!cairn_fs_exists(path)) break;

            recovered_t rec;
            if (!recover_segment(cap, path, &cap->capture_chain, true, &rec)) {
                chain_intact = false;
                break;
            }
            last = rec;
        }

        bool extendable = chain_intact;

        if (index == 0) {
            CAIRN_LOGW(TAG, "capture %s had no segments; starting segment 0",
                       id_text);
            segment_path(cap, 0, path, sizeof(path));
            if (!write_segment_header(cap, path, 0, 0, &cap->segment_cipher)) {
                extendable = false;
            }
            cap->segment_index = 0;
            cap->segment_bytes = CAIRN_SEGMENT_HEADER_SIZE;
        } else {
            cap->segment_index = index - 1;
            cap->segment_bytes = last.bytes;
            if (chain_intact) {
                segment_path(cap, cap->segment_index, path, sizeof(path));
                extendable = can_extend(cap, path, &last, &cap->segment_cipher);
            }
        }

        journal_path(cap, path, sizeof(path));
        if (cairn_fs_exists(path)) {
            recovered_t jrec;
            /*
             * Fold the journal's counts too, exactly as the capture segments
             * above do.
             *
             * This passed false, so a capture resumed after a reboot forgot
             * every DEVICE_HEALTH and STATE_TRANSITION already on the card
             * while the segments still held them. The manifest then recorded
             * the undercount and was signed over it, producing a bundle whose
             * own record_counts contradicted its own segments — permanently,
             * because the signature covers the wrong numbers.
             *
             * Caught by cairn-verify on a real drive: the journal held 75
             * transitions and 12 health records where the manifest claimed 70
             * and 11, the difference being exactly what was written before the
             * resume. The content root and signature were unaffected, so no
             * data was lost and the server still accepts the bundle — it is the
             * self-description that was wrong, which is worse than it sounds
             * for a format whose whole purpose is being checkable later.
             */
            if (recover_segment(cap, path, &cap->journal_chain, true, &jrec)) {
                cap->journal_bytes = jrec.bytes;
                if (!can_extend(cap, path, &jrec, &cap->journal_cipher)) {
                    extendable = false;
                }
            } else {
                chain_intact = false;
                extendable = false;
            }
        } else {
            if (!write_segment_header(cap, path, CAIRN_JOURNAL_SEGMENT_INDEX, 0,
                                      &cap->journal_cipher)) {
                extendable = false;
            }
            cap->journal_bytes = CAIRN_SEGMENT_HEADER_SIZE;
        }

        /*
         * If any segment could not be made appendable, the chain cannot be
         * continued without fabricating continuity. Seal what exists instead:
         * an honestly short bundle is worth more than a plausible-looking one.
         *
         * A key that cannot extend the bundle is the same outcome for a
         * different reason, and is kept distinct in the recovery state: the
         * bytes on the card are intact, so the bundle is not salvaged.
         */
        if (!chain_intact) {
            cap->needs_seal = true;
            cap->recovery_state = CAIRN_RECOVERY_SALVAGED;
            CAIRN_LOGW(TAG, "capture %s cannot be safely extended; it will be "
                            "sealed at the next opportunity", id_text);
        } else if (!extendable) {
            cap->needs_seal = true;
            CAIRN_LOGW(TAG, "capture %s cannot be extended under this device's key; "
                            "it will be sealed at the next opportunity", id_text);
        }
        cap->can_encrypt = extendable;
        if (!extendable) {
            cairn_segment_cipher_wipe(&cap->segment_cipher);
            cairn_segment_cipher_wipe(&cap->journal_cipher);
        }

        /* The basis belongs to this capture, not to the boot that is resuming
         * it. Without this the trip dates from the epoch even though GNSS found
         * a date before power was cut. */
        utc_basis_load(cap);

        cap->active = true;
        CAIRN_LOGI(TAG, "resumed: segment %u at %u bytes, next seq %u, "
                        "%u bytes discarded, recovery_state %u, counter %llu",
                   (unsigned)cap->segment_index, (unsigned)cap->segment_bytes,
                   (unsigned)cap->capture_chain.next_seq,
                   (unsigned)cap->discarded_tail_bytes,
                   (unsigned)cap->recovery_state,
                   (unsigned long long)cap->device_counter);
        return true;
    }

    /* Nothing open: start a fresh capture. */
    if (!bind_new_bundle(cap)) return false;

    ulid_generate(cap->bundle_id, 0);
    cairn_ulid_encode(cap->bundle_id, id_text);
    snprintf(cap->dir, sizeof(cap->dir), "%s/%s", CAIRN_DIR_CAPTURE, id_text);

    if (!cairn_fs_mkdir(cap->dir)) {
        CAIRN_LOGE(TAG, "mkdir %s failed", cap->dir);
        return false;
    }

    segment_path(cap, 0, path, sizeof(path));
    if (!write_segment_header(cap, path, 0, 0, &cap->segment_cipher)) return false;
    cap->segment_bytes = CAIRN_SEGMENT_HEADER_SIZE;

    journal_path(cap, path, sizeof(path));
    if (!write_segment_header(cap, path, CAIRN_JOURNAL_SEGMENT_INDEX, 0,
                              &cap->journal_cipher)) {
        return false;
    }
    cap->journal_bytes = CAIRN_SEGMENT_HEADER_SIZE;

    cap->can_encrypt = true;
    cap->active = true;
    CAIRN_LOGI(TAG, "opened capture %s, counter %llu%s", id_text,
               (unsigned long long)cap->device_counter,
               cap->assigned ? "" : " (UNASSIGNED: the server will refuse it)");
    return true;
}

/* ── append ───────────────────────────────────────────────────────────────── */

static bool rotate_segment(cairn_capture_t *cap)
{
    /*
     * A bundle holds at most CAIRN_MAX_MEMBERS members, one of which is
     * journal.seg. Exhausting the budget is a reason to seal, not a reason to
     * fail an append silently, so the lifecycle is told rather than data lost.
     */
    if (cap->segment_index + 1 >= CAIRN_MAX_MEMBERS - 1) {
        cap->needs_seal = true;
        CAIRN_LOGW(TAG, "segment budget reached at index %u; sealing is required",
                   (unsigned)cap->segment_index);
        return false;
    }

    uint32_t next = cap->segment_index + 1;
    char     path[PATH_MAX_LEN];
    segment_path(cap, next, path, sizeof(path));

    /* Build into a scratch cipher so a failed rotation leaves the current
     * segment's cipher intact for the frame that triggered it. */
    cairn_segment_cipher_t fresh;
    if (!write_segment_header(cap, path, next, cap->capture_chain.next_seq, &fresh)) {
        return false;
    }

    cairn_segment_cipher_wipe(&cap->segment_cipher);
    cap->segment_cipher    = fresh;
    cairn_segment_cipher_wipe(&fresh);
    cap->segment_index     = next;
    cap->segment_bytes     = CAIRN_SEGMENT_HEADER_SIZE;
    cap->segment_first_seq = cap->capture_chain.next_seq;

    return true;
}

bool cairn_capture_append(cairn_capture_t *cap, cairn_chain_id_t chain_id,
                          uint8_t record_type, uint8_t schema_version,
                          uint16_t flags, uint32_t monotonic_ms,
                          const uint8_t *payload, size_t payload_len)
{
    if (!cap->active) return false;

    /*
     * No key that matches this bundle means no append. Writing plaintext, or
     * sealing under a key the bundle's headers do not name, would put bytes on
     * the card nobody can authenticate; the capture is marked for sealing
     * instead, and the next bundle opens under the current key.
     */
    if (!cap->can_encrypt) {
        cap->needs_seal = true;
        return false;
    }
    if (payload_len > CAIRN_MAX_PAYLOAD_SIZE) {
        CAIRN_LOGE(TAG, "payload of %u bytes exceeds the %u-byte maximum", (unsigned)payload_len,
                   (unsigned)CAIRN_MAX_PAYLOAD_SIZE);
        return false;
    }

    cairn_chain_t *chain = (chain_id == CAIRN_CHAIN_JOURNAL) ? &cap->journal_chain
                                                             : &cap->capture_chain;

    /* Rotate first, so the frame is sealed under the segment it lands in: the
     * segment header is part of every frame's AAD. */
    size_t frame_len = CAIRN_FRAME_OVERHEAD + payload_len;
    if (chain_id != CAIRN_CHAIN_JOURNAL &&
        cap->segment_bytes + frame_len > CAIRN_SEGMENT_MAX_BYTES) {
        if (!rotate_segment(cap)) return false;
    }

    const cairn_segment_cipher_t *cipher = (chain_id == CAIRN_CHAIN_JOURNAL)
                                               ? &cap->journal_cipher
                                               : &cap->segment_cipher;

    /*
     * A fresh random nonce for every frame, from the hardware RNG — never from
     * seq. After a torn tail is truncated this same seq is written again with
     * different plaintext, and a seq-derived nonce would then reuse a
     * (key, nonce) pair: for a stream cipher that leaks the XOR of the two
     * plaintexts, and for Poly1305 the authenticator key. A 192-bit random
     * nonce makes a collision negligible with no state a power cut could lose.
     */
    uint8_t nonce[CAIRN_NONCE_SIZE];
    cairn_rng_fill(nonce, sizeof(nonce));

    uint8_t  stage[CAIRN_STAGE_BYTES];
    size_t   written = 0;
    uint32_t crc = 0;

    cairn_err_t err = cairn_encode_frame(cipher, nonce, record_type, schema_version,
                                         flags, chain->next_seq, monotonic_ms,
                                         chain->prev_crc32, payload, payload_len,
                                         stage, sizeof(stage), &written, &crc);
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "encode frame type 0x%02x: %s", record_type,
                   cairn_strerror(err));
        return false;
    }

    char path[PATH_MAX_LEN];
    if (chain_id == CAIRN_CHAIN_JOURNAL) {
        journal_path(cap, path, sizeof(path));
    } else {
        segment_path(cap, cap->segment_index, path, sizeof(path));
    }

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_APPEND);
    if (f == NULL) {
        cap->write_errors++;
        CAIRN_LOGE(TAG, "cannot append to %s", path);
        return false;
    }

    size_t n = cairn_fs_write(f, stage, written);

    /*
     * Flush on a cadence rather than per frame. The format tolerates a torn
     * tail exactly — that is what the length prefix and per-frame CRC are for —
     * so the trade is bounded loss against roughly a third of the write load.
     */
    cap->frames_since_flush++;
    uint32_t now = cairn_millis();
    if (cap->frames_since_flush >= CAIRN_FLUSH_EVERY_FRAMES ||
        now - cap->last_flush_ms >= CAIRN_FLUSH_EVERY_MS) {
        cairn_fs_flush(f);
        cap->frames_since_flush = 0;
        cap->last_flush_ms = now;
    }
    cairn_fs_close(f);

    if (n != written) {
        cap->write_errors++;
        CAIRN_LOGE(TAG, "short write to %s: %u of %u bytes", path, (unsigned)n,
                   (unsigned)written);
        /*
         * The chain is deliberately not advanced. The partial bytes become a
         * torn tail that the next scan finds and truncates, which is strictly
         * better than advancing past a record that was never fully stored.
         */
        return false;
    }

    /* Only now is the frame real. */
    if (chain_id == CAIRN_CHAIN_JOURNAL) {
        cap->journal_bytes += (uint32_t)written;
    } else {
        cap->segment_bytes += (uint32_t)written;

        if (!cap->have_any_frame) {
            cap->first_seq = chain->next_seq;
            cap->have_any_frame = true;
        }
        cap->last_seq = chain->next_seq;
    }

    cap->record_counts[record_type]++;
    chain->next_seq++;
    chain->prev_crc32 = crc;
    cap->capture_ended_monotonic_us = cairn_micros();

    CAIRN_LOGT(TAG, "frame %s seq %u, %u bytes, crc %08x",
               cairn_record_type_name(record_type),
               (unsigned)(chain->next_seq - 1), (unsigned)written, (unsigned)crc);

    return true;
}

/*
 * The basis sidecar.
 *
 * The basis cannot live only in RAM. It is established the first time GNSS has a
 * date, which on a cold start is minutes into the drive, and the capture is
 * normally not sealed in that same boot: the dongle is bus-powered, so switching
 * the car off cuts power mid-bundle and the seal happens on the next boot from a
 * resumed capture. cairn_capture_open_or_resume reconstructs segment bytes,
 * sequence numbers, record counts and ciphers from the card -- there is nowhere
 * for an in-RAM basis to come back from, so it resumed as zero and every bundle
 * was signed with utc_basis_ms = 0. Measured: all 15 trips on the server are
 * dated 1970-01-01, while the device's own health records show DEGRADED_TIME
 * clearing mid-drive, so the basis was found and then lost.
 *
 * This is the same class of bug as the record counts the journal used to forget
 * across a resume (see the comment in cairn_capture_open_or_resume), and it is
 * fixed the same way: write it down.
 *
 * A sidecar rather than a record in the chain, because the basis is a property of
 * the capture and not an observation in it. It carries a CRC because a torn write
 * here would otherwise date a whole trip wrongly, which is worse than having no
 * basis at all: a zero basis is visibly 1970 and DEGRADED_TIME says why, whereas
 * a corrupt one looks like a real date.
 */
#define UTC_BASIS_FILE_BYTES 16u

/* Little-endian, to match every other integer the format puts on the card. */
static void basis_put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void basis_put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t basis_get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t basis_get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void utc_basis_path(const cairn_capture_t *cap, char *out, size_t cap_len)
{
    snprintf(out, cap_len, "%s/utcbasis.bin", cap->dir);
}

static void utc_basis_store(const cairn_capture_t *cap)
{
    uint8_t buf[UTC_BASIS_FILE_BYTES];
    basis_put_u64(buf, cap->utc_basis_ms);
    basis_put_u32(buf + 8, cap->utc_basis_acc_ms);
    basis_put_u32(buf + 12, cairn_crc32(buf, 12));

    char path[PATH_MAX_LEN];
    utc_basis_path(cap, path, sizeof(path));

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_WRITE);
    if (f == NULL) {
        CAIRN_LOGW(TAG, "could not record the UTC basis; this trip will date from the epoch");
        return;
    }
    bool ok = cairn_fs_write(f, buf, sizeof(buf)) == sizeof(buf) && cairn_fs_flush(f);
    cairn_fs_close(f);
    if (!ok) {
        CAIRN_LOGW(TAG, "UTC basis write was short; this trip will date from the epoch");
        cairn_fs_remove(path);
    }
}

/* Restore the basis for a resumed capture. Absence is normal and not an error:
 * the receiver may never have had a date. */
static void utc_basis_load(cairn_capture_t *cap)
{
    char path[PATH_MAX_LEN];
    utc_basis_path(cap, path, sizeof(path));
    if (!cairn_fs_exists(path)) return;

    uint8_t buf[UTC_BASIS_FILE_BYTES];
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    if (f == NULL) return;
    bool read_ok = cairn_fs_read(f, buf, sizeof(buf)) == sizeof(buf);
    cairn_fs_close(f);
    if (!read_ok) return;

    if (basis_get_u32(buf + 12) != cairn_crc32(buf, 12)) {
        CAIRN_LOGW(TAG, "UTC basis sidecar failed its CRC; ignoring it");
        return;
    }

    cap->utc_basis_ms     = basis_get_u64(buf);
    cap->utc_basis_acc_ms = basis_get_u32(buf + 8);
    CAIRN_LOGI(TAG, "UTC basis recovered: %llu ms (+/- %u ms)",
               (unsigned long long)cap->utc_basis_ms,
               (unsigned)cap->utc_basis_acc_ms);
}

void cairn_capture_set_engine_profile(cairn_capture_t *cap, const char *id,
                                      uint8_t version, const uint8_t sha256[32])
{
    if (cap == NULL || id == NULL || sha256 == NULL) return;

    snprintf(cap->engine_profile_id, sizeof(cap->engine_profile_id), "%s", id);
    cap->engine_profile_version = version;
    memcpy(cap->engine_profile_sha256, sha256, 32);
    cap->has_engine_profile = true;
}

void cairn_capture_set_utc_basis(cairn_capture_t *cap, uint64_t utc_ms,
                                 uint32_t acc_ms, uint32_t sampled_monotonic_ms)
{
    /*
     * Stored as the UTC of monotonic zero, not the UTC of the sample.
     *
     * Consumers reconstruct a frame's wall-clock time as utc_basis_ms plus the
     * frame's own monotonic_ms (the server's decode.observedAt does exactly
     * that), and the firmware writes device uptime into monotonic_ms. So the
     * basis has to be the instant uptime was zero. Storing the sampled UTC
     * instead put every timestamp in the bundle late by however long the
     * receiver took to get a date -- on the 2026-10-07 drive, about 130 s.
     */
    cap->utc_basis_ms = (utc_ms > (uint64_t)sampled_monotonic_ms)
                            ? utc_ms - (uint64_t)sampled_monotonic_ms
                            : 0;
    cap->utc_basis_acc_ms = acc_ms;

    if (cap->utc_basis_ms != 0) utc_basis_store(cap);
}

bool cairn_capture_flush(cairn_capture_t *cap)
{
    /* Each append opens, writes and closes, so closing is the flush. This marker
     * exists for the durability points that must be explicit at the call site. */
    cap->frames_since_flush = 0;
    cap->last_flush_ms = cairn_millis();
    return true;
}

void cairn_capture_tick(cairn_capture_t *cap) { (void)cap; }

/* ── seal ─────────────────────────────────────────────────────────────────── */

static size_t collect_members(const char *dir, cairn_member_t *members,
                              size_t max_members)
{
    cairn_dir_t *d = cairn_fs_opendir(dir);
    if (d == NULL) return 0;

    size_t   count = 0;
    char     name[64];
    bool     is_dir = false;
    uint64_t size = 0;

    while (cairn_fs_readdir(d, name, sizeof(name), &is_dir, &size)) {
        if (is_dir) continue;

        /*
         * Only .seg files are members. A manifest written by an interrupted
         * seal must not become a member of the bundle it describes, which would
         * make the content root depend on its own signature.
         */
        size_t nlen = strlen(name);
        bool   is_seg = nlen > 4 && strcmp(name + nlen - 4, ".seg") == 0;

        if (is_seg && count < max_members && nlen + 1 <= CAIRN_MAX_MEMBER_NAME) {
            snprintf(members[count].name, CAIRN_MAX_MEMBER_NAME, "%s", name);
            members[count].length = size;
            memset(members[count].sha256, 0, 32);
            count++;
        }
    }
    cairn_fs_closedir(d);

    cairn_sort_members(members, count);
    return count;
}

/*
 * One pass over the bundle byte stream computes both the per-member digests and
 * the chunk descriptors.
 *
 * Members and chunks are different partitions of the same bytes (spec §6.1), so
 * computing them together is the only way to guarantee they describe the same
 * stream. Two separate passes could disagree if a file changed in between.
 */
/*
 * Total bytes across the members, before any hashing.
 *
 * Needed because the chunk size is chosen from the bundle's size, and a member's
 * length is otherwise only known once it has been hashed. Returns false if any
 * member cannot be opened, so the caller fails before writing a manifest that
 * describes bytes it could not read.
 */
static bool total_member_bytes(const char *dir, const cairn_member_t *members,
                               size_t member_count, uint64_t *out)
{
    uint64_t total = 0;

    for (size_t i = 0; i < member_count; i++) {
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, members[i].name);

        cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
        if (f == NULL) {
            CAIRN_LOGE(TAG, "cannot size member %s", members[i].name);
            return false;
        }
        total += cairn_fs_size(f);
        cairn_fs_close(f);
    }

    *out = total;
    return true;
}

/*
 * The chunk size for a bundle of `total` bytes.
 *
 * CHUNK_TARGET_BYTES normally, but grown when the target would need more than
 * CAIRN_MAX_CHUNKS descriptors. Growing is the only acceptable response: the
 * alternative is refusing to seal a long drive, and this firmware's whole
 * premise is that it never trades a trip for anything. An unusually long trip
 * therefore gets coarser resume granularity rather than no bundle at all, and
 * the log says so.
 */
static uint32_t chunk_size_for(uint64_t total)
{
    if (total == 0) return CHUNK_TARGET_BYTES;

    uint64_t needed = (total + CHUNK_TARGET_BYTES - 1) / CHUNK_TARGET_BYTES;
    if (needed <= (uint64_t)CAIRN_MAX_CHUNKS) return CHUNK_TARGET_BYTES;

    /* Round up to a 512-byte boundary so the size stays tidy in the manifest. */
    uint64_t size = (total + CAIRN_MAX_CHUNKS - 1) / CAIRN_MAX_CHUNKS;
    size = ((size + 511u) / 512u) * 512u;

    CAIRN_LOGW(TAG, "bundle is %llu bytes: %u-byte chunks would need %llu "
                    "descriptors (max %d), so using %llu-byte chunks instead",
               (unsigned long long)total, (unsigned)CHUNK_TARGET_BYTES,
               (unsigned long long)needed, CAIRN_MAX_CHUNKS,
               (unsigned long long)size);

    return (uint32_t)size;
}

static bool digest_members_and_chunks(const char *dir, cairn_member_t *members,
                                      size_t member_count, cairn_chunk_t *chunks,
                                      size_t *chunk_count)
{
    cairn_sha256_t chunk_ctx;
    uint32_t       chunk_bytes = 0;
    size_t         chunks_used = 0;

    uint64_t total_bytes = 0;
    if (!total_member_bytes(dir, members, member_count, &total_bytes)) return false;

    const uint32_t CHUNK_BYTES = chunk_size_for(total_bytes);

    cairn_sha256_init(&chunk_ctx);

    for (size_t i = 0; i < member_count; i++) {
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, members[i].name);

        cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
        if (f == NULL) {
            CAIRN_LOGE(TAG, "cannot read member %s", members[i].name);
            return false;
        }

        cairn_sha256_t member_ctx;
        cairn_sha256_init(&member_ctx);

        uint8_t  block[IO_BLOCK];
        uint64_t total = 0;

        for (;;) {
            size_t got = cairn_fs_read(f, block, sizeof(block));
            if (got == 0) break;

            cairn_sha256_update(&member_ctx, block, got);
            total += (uint64_t)got;

            /* The same bytes feed the chunk rolling hash; a chunk may span
             * members, so the chunk state persists across this loop. */
            size_t consumed = 0;
            while (consumed < got) {
                size_t room = CHUNK_BYTES - chunk_bytes;
                size_t take = got - consumed;
                if (take > room) take = room;

                cairn_sha256_update(&chunk_ctx, block + consumed, take);
                chunk_bytes += (uint32_t)take;
                consumed    += take;

                if (chunk_bytes == CHUNK_BYTES) {
                    if (chunks_used >= CAIRN_MAX_CHUNKS) {
                        CAIRN_LOGE(TAG, "bundle exceeds %d chunks",
                                   CAIRN_MAX_CHUNKS);
                        cairn_fs_close(f);
                        return false;
                    }
                    chunks[chunks_used].index       = (uint32_t)chunks_used;
                    chunks[chunks_used].byte_length = chunk_bytes;
                    cairn_sha256_final(&chunk_ctx, chunks[chunks_used].sha256);
                    chunks_used++;

                    cairn_sha256_init(&chunk_ctx);
                    chunk_bytes = 0;
                }
            }
        }
        cairn_fs_close(f);

        cairn_sha256_final(&member_ctx, members[i].sha256);

        /* The recorded length must be the bytes actually hashed, or the member
         * digest and the manifest would describe different things. */
        members[i].length = total;
    }

    if (chunk_bytes > 0) {
        if (chunks_used >= CAIRN_MAX_CHUNKS) {
            CAIRN_LOGE(TAG, "bundle exceeds %d chunks", CAIRN_MAX_CHUNKS);
            return false;
        }
        chunks[chunks_used].index       = (uint32_t)chunks_used;
        chunks[chunks_used].byte_length = chunk_bytes;
        cairn_sha256_final(&chunk_ctx, chunks[chunks_used].sha256);
        chunks_used++;
    }

    *chunk_count = chunks_used;
    return true;
}

static bool write_exact(const char *path, const uint8_t *data, size_t len)
{
    /*
     * Only unlink a file that is actually there. Removing unconditionally works,
     * but Arduino's VFS layer logs an ERROR line for a remove of a nonexistent
     * path, so the common case — a first seal, with no manifest to replace —
     * printed two spurious errors into the card log on every bundle. A log that
     * cries wolf on the happy path is worse than no log.
     */
    if (cairn_fs_exists(path)) cairn_fs_remove(path);

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_WRITE);
    if (f == NULL) return false;

    size_t n = cairn_fs_write(f, data, len);
    cairn_fs_flush(f);
    cairn_fs_close(f);

    return n == len;
}

/*
 * Move a capture directory that already holds a valid manifest into the sealed
 * tree. Split out so a seal interrupted after the manifest was written can be
 * completed at the next boot without re-signing anything.
 */
static bool finish_seal(const char *dir, const char *id_text)
{
    char to[PATH_MAX_LEN];
    snprintf(to, sizeof(to), "%s/%s", CAIRN_DIR_BUNDLES, id_text);

    if (cairn_fs_exists(to)) {
        CAIRN_LOGE(TAG, "%s already exists; refusing to overwrite a sealed bundle",
                   to);
        return false;
    }

    if (!cairn_fs_rename(dir, to)) {
        CAIRN_LOGE(TAG, "rename %s -> %s failed", dir, to);
        return false;
    }

    CAIRN_LOGI(TAG, "sealed bundle %s", id_text);
    return true;
}

bool cairn_capture_seal(cairn_capture_t *cap, const uint8_t seed[32],
                        const uint8_t pub[32], const char *firmware_version,
                        uint8_t policy_version, uint32_t trip_seq,
                        uint8_t out_bundle_id[16])
{
    if (!cap->active) return false;

    cairn_capture_flush(cap);
    cap->capture_ended_monotonic_us = cairn_micros();

    static cairn_manifest_t m;
    memset(&m, 0, sizeof(m));

    m.manifest_version = CAIRN_MANIFEST_VERSION;
    memcpy(m.bundle_id, cap->bundle_id, 16);
    memcpy(m.device_id, cap->device_id, 16);
    memcpy(m.boot_id, cap->boot_id, 16);
    cairn_device_key_id(pub, m.device_key_id);
    snprintf(m.firmware_version, sizeof(m.firmware_version), "%s", firmware_version);
    m.schema_version = 1;

    m.capture_started_monotonic_us = cap->capture_started_monotonic_us;
    m.capture_ended_monotonic_us   = cap->capture_ended_monotonic_us;
    m.utc_basis_ms                 = cap->utc_basis_ms;
    m.utc_basis_acc_ms             = cap->utc_basis_acc_ms;

    m.has_engine_profile = cap->has_engine_profile;
    if (cap->has_engine_profile) {
        snprintf(m.engine_profile_id, sizeof(m.engine_profile_id), "%s",
                 cap->engine_profile_id);
        m.engine_profile_version = cap->engine_profile_version;
        memcpy(m.engine_profile_sha256, cap->engine_profile_sha256, 32);
    }
    m.first_seq                    = cap->first_seq;
    m.last_seq                     = cap->last_seq;

    memcpy(m.record_counts, cap->record_counts, sizeof(m.record_counts));

    m.policy_version       = policy_version;
    m.recovery_state       = cap->recovery_state;
    m.discarded_tail_bytes = cap->discarded_tail_bytes;
    snprintf(m.signature_algorithm, sizeof(m.signature_algorithm), "%s",
             CAIRN_SIGALG_ED25519);

    m.has_trip_seq = true;
    m.trip_seq     = trip_seq;

    /*
     * The binding, exactly as the segment headers carry it — §5.4 rejects a
     * manifest that disagrees with any of them, journal included.
     */
    memcpy(m.vehicle_id, cap->vehicle_id, 16);
    memcpy(m.assignment_id, cap->assignment_id, 16);
    m.device_counter      = cap->device_counter;
    m.storage_key_version = cap->storage_key_version;
    snprintf(m.encryption_suite, sizeof(m.encryption_suite), "%s",
             CAIRN_ENCRYPTION_SUITE_V1);

    if (!cap->assigned) {
        CAIRN_LOGW(TAG, "sealing an UNASSIGNED bundle (all-zero vehicle and "
                        "assignment); the server will refuse it until this device "
                        "is assigned, and it stays on the card meanwhile");
    }

    m.member_count = collect_members(cap->dir, m.members, CAIRN_MAX_MEMBERS);
    if (m.member_count == 0) {
        CAIRN_LOGE(TAG, "no members in %s; nothing to seal", cap->dir);
        return false;
    }

    if (!digest_members_and_chunks(cap->dir, m.members, m.member_count, m.chunks,
                                   &m.chunk_count)) {
        return false;
    }

    cairn_err_t err = cairn_content_root(m.members, m.member_count, m.content_root);
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "content_root: %s", cairn_strerror(err));
        return false;
    }

    /*
     * The counter is made durable before the signature exists, never after.
     *
     * It was reserved when the bundle opened, but NVS may have been erased or
     * restored since; signing first and committing second would leave a window
     * in which a power cut produces a signed manifest under a counter the
     * device has forgotten — and the next bundle would be handed the same
     * counter for different content, which the server must quarantine. A
     * failure here aborts the seal: the capture stays intact and the seal is
     * retried, which costs nothing.
     */
    const cairn_storage_identity_t *id = cap->identity;
    if (id == NULL || id->counter_commit == NULL ||
        !id->counter_commit(id->counter_ctx, cap->device_counter)) {
        CAIRN_LOGE(TAG, "cannot make device counter %llu durable; not signing",
                   (unsigned long long)cap->device_counter);
        return false;
    }

    static uint8_t encoded[CAIRN_MANIFEST_ENCODED_MAX];
    size_t         encoded_len = 0;
    uint8_t        sig[64];

    err = cairn_manifest_sign(&m, seed, pub, encoded, sizeof(encoded),
                              &encoded_len, sig);
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "manifest_sign: %s", cairn_strerror(err));
        return false;
    }

    /*
     * Verify what was just produced before relying on it. A manifest that does
     * not verify here will not verify on the server either, and finding out now
     * keeps a broken bundle out of the upload queue.
     */
    if (cairn_manifest_verify(encoded, encoded_len, sig, pub) != CAIRN_OK) {
        CAIRN_LOGE(TAG, "freshly signed manifest fails verification; refusing to seal");
        return false;
    }

    char id_text[27];
    cairn_ulid_encode(cap->bundle_id, id_text);

    /*
     * The manifest is written into the capture directory *before* the move, so
     * an interrupted seal leaves either a capture holding a valid manifest —
     * completed by cairn_store_resume_interrupted_seals() — or a finished
     * bundle. Neither state loses data and neither yields a bundle without a
     * manifest.
     */
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/manifest.cbor", cap->dir);
    if (!write_exact(path, encoded, encoded_len)) {
        CAIRN_LOGE(TAG, "cannot write %s", path);
        return false;
    }

    snprintf(path, sizeof(path), "%s/manifest.sig", cap->dir);
    if (!write_exact(path, sig, sizeof(sig))) {
        CAIRN_LOGE(TAG, "cannot write %s", path);
        return false;
    }

    CAIRN_LOGI(TAG, "manifest written: %u members, %u chunks, %u bytes, "
                    "content_root %02x%02x%02x%02x..",
               (unsigned)m.member_count, (unsigned)m.chunk_count,
               (unsigned)encoded_len, m.content_root[0], m.content_root[1],
               m.content_root[2], m.content_root[3]);

    if (!finish_seal(cap->dir, id_text)) return false;

    if (out_bundle_id != NULL) memcpy(out_bundle_id, cap->bundle_id, 16);

    /* A sealed bundle is never appended to again, so its keys have no further
     * use in RAM. */
    cairn_segment_cipher_wipe(&cap->segment_cipher);
    cairn_segment_cipher_wipe(&cap->journal_cipher);
    cap->can_encrypt = false;

    cap->active = false;
    return true;
}

int cairn_store_resume_interrupted_seals(void)
{
    cairn_dir_t *dir = cairn_fs_opendir(CAIRN_DIR_CAPTURE);
    if (dir == NULL) return 0;

    /* Collect first, act second: moving a directory while iterating it would
     * invalidate the iterator. */
    char candidates[CAIRN_MAX_MEMBERS][27];
    int  candidate_count = 0;

    char name[64];
    bool is_dir = false;

    while (cairn_fs_readdir(dir, name, sizeof(name), &is_dir, NULL)) {
        if (!is_dir || candidate_count >= CAIRN_MAX_MEMBERS) continue;

        uint8_t probe[16];
        if (cairn_ulid_decode(name, probe)) {
            snprintf(candidates[candidate_count], 27, "%s", name);
            candidate_count++;
        }
    }
    cairn_fs_closedir(dir);

    int finished = 0;

    for (int i = 0; i < candidate_count; i++) {
        char cdir[PATH_MAX_LEN], mpath[PATH_MAX_LEN], spath[PATH_MAX_LEN];
        snprintf(cdir, sizeof(cdir), "%s/%s", CAIRN_DIR_CAPTURE, candidates[i]);
        snprintf(mpath, sizeof(mpath), "%s/manifest.cbor", cdir);
        snprintf(spath, sizeof(spath), "%s/manifest.sig", cdir);

        /* No manifest means the seal had not started: this is a live capture to
         * resume, not an interrupted seal. */
        if (!cairn_fs_exists(mpath) || !cairn_fs_exists(spath)) continue;

        CAIRN_LOGW(TAG, "capture %s already holds a manifest; finishing the "
                        "interrupted seal", candidates[i]);

        if (finish_seal(cdir, candidates[i])) finished++;
    }

    if (finished > 0) CAIRN_LOGI(TAG, "completed %d interrupted seal(s)", finished);
    return finished;
}

bool cairn_store_pending_stats(uint32_t *bundle_count, uint64_t *total_bytes)
{
    cairn_dir_t *dir = cairn_fs_opendir(CAIRN_DIR_BUNDLES);
    if (dir == NULL) return false;

    uint32_t count = 0;
    uint64_t bytes = 0;

    char name[64];
    bool is_dir = false;

    while (cairn_fs_readdir(dir, name, sizeof(name), &is_dir, NULL)) {
        if (!is_dir) continue;
        count++;

        char sub[PATH_MAX_LEN];
        snprintf(sub, sizeof(sub), "%s/%s", CAIRN_DIR_BUNDLES, name);

        cairn_dir_t *b = cairn_fs_opendir(sub);
        if (b == NULL) continue;

        char     fname[64];
        bool     fdir = false;
        uint64_t fsize = 0;

        while (cairn_fs_readdir(b, fname, sizeof(fname), &fdir, &fsize)) {
            if (!fdir) bytes += fsize;
        }
        cairn_fs_closedir(b);
    }
    cairn_fs_closedir(dir);

    *bundle_count = count;
    *total_bytes  = bytes;
    return true;
}
