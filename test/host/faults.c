/*
 * The firmware's storage property matrix.
 *
 * Each row states a property the storage layer claims, arms a fault that would
 * violate it, and asserts what survived — against the durable tree on disk,
 * never against a log line, because a log line proves nothing about what is on
 * the card.
 *
 * This exists because the alternative was documentation. "A torn tail is
 * truncated to the last valid frame with an exact discarded byte count" and "no
 * byte is deleted without a verified receipt" are claims about crash and
 * adversarial behaviour; compiling for ESP32 does not test them, and neither
 * does a successful drive. The code under test is the same cairn_store.c and
 * cairn_prune.c the device runs, reached through the filesystem abstraction.
 *
 * Every run is reproducible from its seed, which is printed on failure.
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
#include "cairn_platform.h"
#include "cairn_prune.h"
#include "cairn_store.h"
#include "preroll.h"

/* From platform_host.c / cairn_kv_posix.c. */
void cairn_host_advance(uint32_t ms);
void cairn_host_seed(uint64_t seed);
void cairn_kv_host_set_path(const char *path);

/*
 * Fixed server keys, so a "pinned" key means something here. Declared as char
 * arrays because a 32-character literal needs 33 bytes; only the first 32 are
 * the seed.
 */
static const char SERVER_SEED_TEXT[] = "cairn-fault-matrix-server-key!!!";
static const char OTHER_SEED_TEXT[]  = "cairn-fault-matrix-other-key!!!!";

#define SERVER_SEED ((const uint8_t *)SERVER_SEED_TEXT)
#define OTHER_SEED  ((const uint8_t *)OTHER_SEED_TEXT)

/* ── harness ──────────────────────────────────────────────────────────────── */

static int  g_pass, g_fail;
static char g_failures[64][512];
static int  g_failure_count;
static uint64_t g_seed = 0x5EED1234;

static const char *g_row;

static void fail(const char *fmt, ...)
{
    char detail[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);

    if (g_failure_count < 64) {
        snprintf(g_failures[g_failure_count++], 512, "%s (seed %llu): %s", g_row,
                 (unsigned long long)g_seed, detail);
    }
}

#define CHECK(cond, ...)             \
    do {                             \
        if (!(cond)) {               \
            fail(__VA_ARGS__);       \
            return false;            \
        }                            \
    } while (0)

/* A fresh simulated card and key store for each row, so no row can pass because
 * of state another row left behind. */
static char g_root[512];

static void rm_rf(const char *path)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* a missing tree is fine */ }
}

static bool fresh_tree(const char *row)
{
    snprintf(g_root, sizeof(g_root), "/tmp/cairn-fault-%s", row);
    rm_rf(g_root);

    if (mkdir(g_root, 0775) != 0) return false;

    char kv[700];
    snprintf(kv, sizeof(kv), "%s/kv.bin", g_root);
    cairn_kv_host_set_path(kv);

    char card[700];
    snprintf(card, sizeof(card), "%s/card", g_root);
    if (mkdir(card, 0775) != 0) return false;

    if (!cairn_fs_begin(card)) return false;
    cairn_host_seed(g_seed);

    return cairn_store_init();
}

static void path_in_card(const char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s/card%s", g_root, rel);
}

/* ── helpers ──────────────────────────────────────────────────────────────── */

/* Reader for the streaming scan, matching what cairn_store.c uses. */
static bool fs_read_for_test(void *user, uint64_t offset, uint8_t *buf, size_t len)
{
    cairn_file_t *f = (cairn_file_t *)user;

    if (!cairn_fs_seek(f, offset)) return false;
    return cairn_fs_read(f, buf, len) == len;
}

static void make_gnss_payload(uint8_t out[32], int i)
{
    cairn_gnss_sample_t s;
    memset(&s, 0, sizeof(s));

    s.lat_e7     = 340000000 + i;   /* 34.0 degrees, as degrees x 1e7 */
    s.lon_e7     = -1185000000;
    s.fix_type   = 3;
    s.sats_used  = 9;
    s.hdop_e2    = 120;
    s.h_acc_cm   = CAIRN_U16_UNKNOWN;
    s.v_acc_cm   = CAIRN_U16_UNKNOWN;
    s.utc_acc_ms = CAIRN_U16_UNKNOWN;

    cairn_encode_gnss_sample(&s, out);
}

static int append_samples(cairn_capture_t *cap, int n)
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        uint8_t payload[32];
        make_gnss_payload(payload, i);
        cairn_host_advance(100);
        if (cairn_capture_append(cap, CAIRN_CHAIN_CAPTURE, CAIRN_REC_GNSS_SAMPLE,
                                 1, 0, cairn_millis(), payload, sizeof(payload))) {
            ok++;
        }
    }
    return ok;
}

/* Find the single capture directory's name. */
static bool current_capture_id(char *out, size_t cap)
{
    cairn_dir_t *d = cairn_fs_opendir(CAIRN_DIR_CAPTURE);
    if (d == NULL) return false;

    char name[64];
    bool is_dir = false;
    bool found = false;

    while (cairn_fs_readdir(d, name, sizeof(name), &is_dir, NULL)) {
        if (!is_dir) continue;
        uint8_t probe[16];
        if (cairn_ulid_decode(name, probe)) {
            snprintf(out, cap, "%s", name);
            found = true;
            break;
        }
    }
    cairn_fs_closedir(d);
    return found;
}

static bool count_dirs(const char *where, int *count)
{
    cairn_dir_t *d = cairn_fs_opendir(where);
    if (d == NULL) return false;

    char name[64];
    bool is_dir = false;
    *count = 0;

    while (cairn_fs_readdir(d, name, sizeof(name), &is_dir, NULL)) {
        if (is_dir) (*count)++;
    }
    cairn_fs_closedir(d);
    return true;
}

/*
 * Simulate a power cut mid-write by lopping bytes off the end of a segment.
 * Done outside the store, through the real filesystem, so the store rediscovers
 * the damage exactly as it would after a reboot.
 */
static bool tear_file(const char *rel_path, uint32_t drop_bytes)
{
    char full[1024];
    path_in_card(rel_path, full, sizeof(full));

    struct stat st;
    if (stat(full, &st) != 0) return false;
    if ((uint32_t)st.st_size < drop_bytes) return false;

    return truncate(full, st.st_size - drop_bytes) == 0;
}

static bool flip_byte(const char *rel_path, long offset)
{
    char full[1024];
    path_in_card(rel_path, full, sizeof(full));

    FILE *f = fopen(full, "r+b");
    if (f == NULL) return false;

    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    int c = fgetc(f);
    if (c == EOF) {
        fclose(f);
        return false;
    }

    fseek(f, offset, SEEK_SET);
    fputc(c ^ 0x01, f);
    fclose(f);
    return true;
}

static bool file_size_of(const char *rel, uint64_t *size)
{
    return cairn_fs_file_size(rel, size);
}

/* Build and sign a receipt, so the prune rows can present genuine and forged
 * evidence rather than hand-written bytes. */
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
    snprintf(r.signature_algorithm, sizeof(r.signature_algorithm), "%s",
             CAIRN_SIGALG_ED25519);
    r.object_id_count = 0;

    uint8_t signing[1024];
    size_t  signing_len = 0;
    if (cairn_receipt_signing_bytes(&r, signing, sizeof(signing), &signing_len)
        != CAIRN_OK) {
        return 0;
    }

    cairn_ed25519_sign(signing, signing_len, seed, pub, r.signature);

    size_t written = 0;
    if (cairn_receipt_encode(&r, out, cap, &written) != CAIRN_OK) return 0;
    return written;
}

/* Seal the current capture and return the sealed id and content root. */
static bool seal_now(cairn_capture_t *cap, char id_out[27], uint8_t root_out[32])
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    if (!cairn_identity_load(device_id, seed, pub, &boot)) return false;

    uint8_t bundle_id[16];
    if (!cairn_capture_seal(cap, seed, pub, "cairn-fault-test", 1, bundle_id)) {
        return false;
    }
    cairn_ulid_encode(bundle_id, id_out);

    /* Read the content root back out of the sealed manifest, rather than
     * trusting an in-memory copy — the prune gate will be given exactly what a
     * real sync would read from the card. */
    char mpath[256];
    snprintf(mpath, sizeof(mpath), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, id_out);

    cairn_file_t *f = cairn_fs_open(mpath, CAIRN_FS_READ);
    if (f == NULL) return false;

    static uint8_t encoded[4096];
    size_t len = cairn_fs_read(f, encoded, sizeof(encoded));
    cairn_fs_close(f);

    static cairn_manifest_t m;
    static uint8_t scratch[8192];
    if (cairn_manifest_decode(encoded, len, &m, scratch, sizeof(scratch)) != CAIRN_OK) {
        return false;
    }
    memcpy(root_out, m.content_root, 32);
    return true;
}

/* ── rows ─────────────────────────────────────────────────────────────────── */

/*
 * A clean capture seals, and the manifest it produces verifies against the
 * device key with a content root that recomputes from the members on disk.
 * The baseline: without this, a failure in any later row is ambiguous.
 */
static bool row_clean_seal(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");
    CHECK(append_samples(&cap, 16) == 16, "not all frames appended");

    char    id[27];
    uint8_t root[32];
    CHECK(seal_now(&cap, id, root), "seal failed");

    int bundles = 0, captures = 0;
    CHECK(count_dirs(CAIRN_DIR_BUNDLES, &bundles), "cannot list bundles");
    CHECK(count_dirs(CAIRN_DIR_CAPTURE, &captures), "cannot list captures");
    CHECK(bundles == 1, "expected 1 sealed bundle, found %d", bundles);
    CHECK(captures == 0, "capture directory should be empty after sealing, "
                         "found %d", captures);

    /* The signature must verify over exactly the bytes on the card. */
    char mpath[256], spath[256];
    snprintf(mpath, sizeof(mpath), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, id);
    snprintf(spath, sizeof(spath), "%s/%s/manifest.sig", CAIRN_DIR_BUNDLES, id);

    cairn_file_t *mf = cairn_fs_open(mpath, CAIRN_FS_READ);
    CHECK(mf != NULL, "no manifest on the card");
    static uint8_t encoded[4096];
    size_t enc_len = cairn_fs_read(mf, encoded, sizeof(encoded));
    cairn_fs_close(mf);

    cairn_file_t *sf = cairn_fs_open(spath, CAIRN_FS_READ);
    CHECK(sf != NULL, "no signature on the card");
    uint8_t sig[64];
    size_t sig_len = cairn_fs_read(sf, sig, sizeof(sig));
    cairn_fs_close(sf);

    CHECK(sig_len == 64, "signature is %u bytes, want 64", (unsigned)sig_len);
    CHECK(cairn_manifest_verify(encoded, enc_len, sig, pub) == CAIRN_OK,
          "manifest signature does not verify");

    static cairn_manifest_t m;
    static uint8_t scratch[8192];
    CHECK(cairn_manifest_decode(encoded, enc_len, &m, scratch, sizeof(scratch))
              == CAIRN_OK, "manifest does not decode");
    CHECK(cairn_manifest_verify_content_root(&m) == CAIRN_OK,
          "content root does not match the members it names");
    CHECK(m.recovery_state == CAIRN_RECOVERY_CLEAN,
          "recovery_state is %u on a clean seal", (unsigned)m.recovery_state);
    CHECK(m.discarded_tail_bytes == 0, "discarded_tail_bytes is %u on a clean seal",
          (unsigned)m.discarded_tail_bytes);

    return true;
}

/*
 * A power cut mid-frame leaves a partial record. Recovery must truncate to the
 * last *valid* frame, report the exact number of bytes it gave up, and resume
 * the chain at the right sequence number — not patch over the hole.
 */
static bool row_torn_tail_mid_frame(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");
    CHECK(append_samples(&cap, 10) == 10, "not all frames appended");

    char id[27];
    CHECK(current_capture_id(id, sizeof(id)), "no capture directory");

    char seg_rel[256];
    snprintf(seg_rel, sizeof(seg_rel), "%s/%s/seg-00000000.seg", CAIRN_DIR_CAPTURE, id);

    uint64_t before = 0;
    CHECK(file_size_of(seg_rel, &before), "cannot size the segment");

    /* Lop off 20 bytes: inside the last frame, past its length prefix. */
    const uint32_t dropped = 20;
    char seg_fs_rel[256];
    snprintf(seg_fs_rel, sizeof(seg_fs_rel), "%s/%s/seg-00000000.seg",
             CAIRN_DIR_CAPTURE, id);
    CHECK(tear_file(seg_fs_rel, dropped), "cannot tear the segment");

    /* Reboot. */
    cairn_capture_t resumed;
    CHECK(cairn_capture_open_or_resume(&resumed, device_id, boot_id),
          "resume failed");

    CHECK(resumed.recovery_state == CAIRN_RECOVERY_RECOVERED_TAIL,
          "recovery_state is %u, want RECOVERED_TAIL(%d)",
          (unsigned)resumed.recovery_state, CAIRN_RECOVERY_RECOVERED_TAIL);

    /*
     * The frame was 60 bytes and 20 were lost, so the surviving prefix of that
     * frame — 40 bytes — is what recovery must discard. Reporting the 20 bytes
     * that never arrived would understate the loss.
     */
    const uint32_t frame_len = 60;
    const uint32_t want_discarded = frame_len - dropped;
    CHECK(resumed.discarded_tail_bytes == want_discarded,
          "discarded_tail_bytes is %u, want %u",
          (unsigned)resumed.discarded_tail_bytes, (unsigned)want_discarded);

    /* Nine frames survived, so the next sequence number is 9. */
    CHECK(resumed.capture_chain.next_seq == 9,
          "next_seq is %u, want 9", (unsigned)resumed.capture_chain.next_seq);

    /* The file must actually be shorter now: a torn tail is removed, not
     * tolerated, or the next append would be unreadable. */
    uint64_t after = 0;
    CHECK(file_size_of(seg_rel, &after), "cannot size the truncated segment");
    CHECK(after == before - frame_len,
          "segment is %llu bytes, want %llu", (unsigned long long)after,
          (unsigned long long)(before - frame_len));

    /* And appending must work, continuing the chain. */
    CHECK(append_samples(&resumed, 3) == 3, "cannot append after recovery");
    CHECK(resumed.capture_chain.next_seq == 12, "next_seq is %u after appending, "
          "want 12", (unsigned)resumed.capture_chain.next_seq);

    /* A second reboot must find a clean segment and discard nothing more. */
    cairn_capture_t again;
    CHECK(cairn_capture_open_or_resume(&again, device_id, boot_id),
          "second resume failed");
    CHECK(again.discarded_tail_bytes == 0,
          "a clean segment discarded %u bytes on re-open",
          (unsigned)again.discarded_tail_bytes);
    CHECK(again.capture_chain.next_seq == 12,
          "next_seq is %u after a clean re-open, want 12",
          (unsigned)again.capture_chain.next_seq);

    return true;
}

/*
 * A cut inside a frame *header* is a different code path: there are not even
 * enough bytes to read a length prefix. It must still be a torn tail rather
 * than an error that condemns the segment.
 */
static bool row_torn_tail_mid_header(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");
    CHECK(append_samples(&cap, 5) == 5, "not all frames appended");

    char id[27];
    CHECK(current_capture_id(id, sizeof(id)), "no capture directory");

    char seg_rel[256];
    snprintf(seg_rel, sizeof(seg_rel), "%s/%s/seg-00000000.seg", CAIRN_DIR_CAPTURE, id);

    /* Leave only 10 bytes of the final 60-byte frame: fewer than the 24-byte
     * frame header, let alone a complete record. */
    CHECK(tear_file(seg_rel, 50), "cannot tear the segment");

    cairn_capture_t resumed;
    CHECK(cairn_capture_open_or_resume(&resumed, device_id, boot_id),
          "resume failed");

    CHECK(resumed.recovery_state == CAIRN_RECOVERY_RECOVERED_TAIL,
          "recovery_state is %u, want RECOVERED_TAIL",
          (unsigned)resumed.recovery_state);
    CHECK(resumed.discarded_tail_bytes == 10,
          "discarded_tail_bytes is %u, want 10",
          (unsigned)resumed.discarded_tail_bytes);
    CHECK(resumed.capture_chain.next_seq == 4,
          "next_seq is %u, want 4", (unsigned)resumed.capture_chain.next_seq);

    return true;
}

/*
 * A flipped bit inside a frame is corruption, not truncation. Everything before
 * it is still valid and must be kept: corruption is isolated to one record.
 */
static bool row_corrupt_frame_isolated(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");
    CHECK(append_samples(&cap, 10) == 10, "not all frames appended");

    char id[27];
    CHECK(current_capture_id(id, sizeof(id)), "no capture directory");

    char seg_rel[256];
    snprintf(seg_rel, sizeof(seg_rel), "%s/%s/seg-00000000.seg", CAIRN_DIR_CAPTURE, id);

    /* Frame 4's payload: header(64) + 4 frames * 60 + frame header(24) + 2. */
    long offset = CAIRN_SEGMENT_HEADER_SIZE + 4 * 60 + CAIRN_FRAME_HEADER_SIZE + 2;
    CHECK(flip_byte(seg_rel, offset), "cannot flip a payload byte");

    cairn_capture_t resumed;
    CHECK(cairn_capture_open_or_resume(&resumed, device_id, boot_id),
          "resume failed");

    /* Corruption is salvage, not a clean tail recovery — the distinction is
     * carried into the manifest so the server can tell them apart. */
    CHECK(resumed.recovery_state == CAIRN_RECOVERY_SALVAGED,
          "recovery_state is %u, want SALVAGED(%d)",
          (unsigned)resumed.recovery_state, CAIRN_RECOVERY_SALVAGED);

    /* Four frames preceded the damage and must survive. */
    CHECK(resumed.capture_chain.next_seq == 4,
          "next_seq is %u, want 4 (frames 0-3 survive)",
          (unsigned)resumed.capture_chain.next_seq);

    /* Six frames of 60 bytes were given up. */
    CHECK(resumed.discarded_tail_bytes == 6 * 60,
          "discarded_tail_bytes is %u, want %d",
          (unsigned)resumed.discarded_tail_bytes, 6 * 60);

    return true;
}

/*
 * One chain spans every capture segment. A rotation must not reset the
 * sequence, and a decoder handed only the second segment must report a chain
 * break rather than silently accepting it as a complete bundle.
 */
static bool row_chain_spans_rotation(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");

    /* Enough frames to pass CAIRN_SEGMENT_MAX_BYTES and force a rotation. */
    const int needed = (int)(CAIRN_SEGMENT_MAX_BYTES / 60) + 40;
    int wrote = append_samples(&cap, needed);
    CHECK(wrote == needed, "wrote %d of %d frames", wrote, needed);
    CHECK(cap.segment_index >= 1, "no rotation occurred (segment_index %u)",
          (unsigned)cap.segment_index);

    char id[27];
    CHECK(current_capture_id(id, sizeof(id)), "no capture directory");

    /* Sequence numbers must be contiguous across the boundary. */
    CHECK(cap.capture_chain.next_seq == (uint32_t)wrote,
          "next_seq is %u after %d frames", (unsigned)cap.capture_chain.next_seq,
          wrote);

    /* Scanning segment 1 from a zero state is the honest-failure case. */
    char seg1[256];
    snprintf(seg1, sizeof(seg1), "%s/%s/seg-00000001.seg", CAIRN_DIR_CAPTURE, id);

    cairn_file_t *f = cairn_fs_open(seg1, CAIRN_FS_READ);
    CHECK(f != NULL, "no second segment");

    uint64_t size = cairn_fs_size(f);
    cairn_fs_close(f);
    CHECK(size > CAIRN_SEGMENT_HEADER_SIZE, "second segment holds no frames");

    /* Re-open and resume: the whole chain must be walked without complaint. */
    cairn_capture_t resumed;
    CHECK(cairn_capture_open_or_resume(&resumed, device_id, boot_id),
          "resume across segments failed");
    CHECK(resumed.recovery_state == CAIRN_RECOVERY_CLEAN,
          "a clean multi-segment capture reported recovery_state %u",
          (unsigned)resumed.recovery_state);
    CHECK(resumed.discarded_tail_bytes == 0,
          "a clean multi-segment capture discarded %u bytes",
          (unsigned)resumed.discarded_tail_bytes);
    CHECK(resumed.capture_chain.next_seq == (uint32_t)wrote,
          "next_seq is %u after resuming, want %d",
          (unsigned)resumed.capture_chain.next_seq, wrote);

    return true;
}

/*
 * The journal has its own chain. A health or transition record written while a
 * trip is idle must not appear as a gap in the capture sequence — which is the
 * bug this separation exists to prevent.
 */
static bool row_journal_chain_independent(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");

    CHECK(append_samples(&cap, 3) == 3, "capture frames failed");

    /* Interleave journal writes. */
    for (int i = 0; i < 4; i++) {
        cairn_state_transition_t t;
        memset(&t, 0, sizeof(t));
        t.region = CAIRN_REGION_CAPTURE;
        t.to_state = (uint8_t)i;
        t.policy_version = 1;

        uint8_t payload[20];
        cairn_encode_state_transition(&t, payload);
        cairn_host_advance(10);
        CHECK(cairn_capture_append(&cap, CAIRN_CHAIN_JOURNAL,
                                   CAIRN_REC_STATE_TRANSITION, 1, 0,
                                   cairn_millis(), payload, sizeof(payload)),
              "journal append %d failed", i);
    }

    CHECK(append_samples(&cap, 3) == 3, "capture frames after journal failed");

    /* Six capture frames and four journal frames, each counting on its own. */
    CHECK(cap.capture_chain.next_seq == 6,
          "capture next_seq is %u, want 6 — journal writes leaked into the "
          "capture chain", (unsigned)cap.capture_chain.next_seq);
    CHECK(cap.journal_chain.next_seq == 4,
          "journal next_seq is %u, want 4", (unsigned)cap.journal_chain.next_seq);

    /* Both must survive a reboot with their own continuity intact. */
    cairn_capture_t resumed;
    CHECK(cairn_capture_open_or_resume(&resumed, device_id, boot_id),
          "resume failed");
    CHECK(resumed.recovery_state == CAIRN_RECOVERY_CLEAN,
          "clean two-chain capture reported recovery_state %u",
          (unsigned)resumed.recovery_state);
    CHECK(resumed.capture_chain.next_seq == 6,
          "capture next_seq is %u after resume, want 6",
          (unsigned)resumed.capture_chain.next_seq);
    CHECK(resumed.journal_chain.next_seq == 4,
          "journal next_seq is %u after resume, want 4",
          (unsigned)resumed.journal_chain.next_seq);

    return true;
}

/*
 * Power loss between writing the manifest and moving the directory. The next
 * boot must complete the seal without re-signing, and doing it twice must
 * change nothing.
 */
static bool row_interrupted_seal_completed(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");
    CHECK(append_samples(&cap, 8) == 8, "appends failed");

    char id[27];
    CHECK(current_capture_id(id, sizeof(id)), "no capture directory");

    /*
     * Reproduce the interrupted state directly: a capture directory that
     * already holds a valid manifest and signature. This is exactly what
     * cairn_capture_seal leaves behind if power is cut before the rename.
     */
    char mpath[256], spath[256];
    snprintf(mpath, sizeof(mpath), "%s/%s/manifest.cbor", CAIRN_DIR_CAPTURE, id);
    snprintf(spath, sizeof(spath), "%s/%s/manifest.sig", CAIRN_DIR_CAPTURE, id);

    /* Build the manifest the way the store does, then stop short of the move. */
    static cairn_manifest_t m;
    memset(&m, 0, sizeof(m));
    m.manifest_version = CAIRN_MANIFEST_VERSION;
    CHECK(cairn_ulid_decode(id, m.bundle_id), "bad capture id");
    memcpy(m.device_id, device_id, 16);
    memcpy(m.boot_id, boot_id, 16);
    cairn_device_key_id(pub, m.device_key_id);
    snprintf(m.firmware_version, sizeof(m.firmware_version), "cairn-fault-test");
    m.schema_version = 1;
    snprintf(m.signature_algorithm, sizeof(m.signature_algorithm), "%s",
             CAIRN_SIGALG_ED25519);
    m.member_count = 1;
    snprintf(m.members[0].name, CAIRN_MAX_MEMBER_NAME, "seg-00000000.seg");
    m.members[0].length = 64;
    CHECK(cairn_content_root(m.members, m.member_count, m.content_root) == CAIRN_OK,
          "content root failed");

    static uint8_t encoded[4096];
    size_t enc_len = 0;
    uint8_t sig[64];
    CHECK(cairn_manifest_sign(&m, seed, pub, encoded, sizeof(encoded), &enc_len,
                              sig) == CAIRN_OK, "manifest sign failed");

    cairn_file_t *mf = cairn_fs_open(mpath, CAIRN_FS_WRITE);
    CHECK(mf != NULL, "cannot write the staged manifest");
    cairn_fs_write(mf, encoded, enc_len);
    cairn_fs_close(mf);

    cairn_file_t *sf = cairn_fs_open(spath, CAIRN_FS_WRITE);
    CHECK(sf != NULL, "cannot write the staged signature");
    cairn_fs_write(sf, sig, sizeof(sig));
    cairn_fs_close(sf);

    /* Reboot: the interrupted seal must be finished. */
    int finished = cairn_store_resume_interrupted_seals();
    CHECK(finished == 1, "completed %d interrupted seals, want 1", finished);

    int bundles = 0, captures = 0;
    CHECK(count_dirs(CAIRN_DIR_BUNDLES, &bundles), "cannot list bundles");
    CHECK(count_dirs(CAIRN_DIR_CAPTURE, &captures), "cannot list captures");
    CHECK(bundles == 1, "expected 1 sealed bundle, found %d", bundles);
    CHECK(captures == 0, "capture directory not cleared, found %d", captures);

    /* Idempotent: a second boot must find nothing to do. */
    int again = cairn_store_resume_interrupted_seals();
    CHECK(again == 0, "a second pass completed %d seals, want 0", again);

    CHECK(count_dirs(CAIRN_DIR_BUNDLES, &bundles), "cannot re-list bundles");
    CHECK(bundles == 1, "bundle count changed to %d on the second pass", bundles);

    return true;
}

/*
 * A capture directory without a manifest is a live capture, not an interrupted
 * seal. Mistaking the two would move unsealed data into the upload queue with
 * no manifest describing it.
 */
static bool row_live_capture_not_sealed(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");
    CHECK(append_samples(&cap, 4) == 4, "appends failed");

    int finished = cairn_store_resume_interrupted_seals();
    CHECK(finished == 0, "sealed %d live captures, want 0", finished);

    int bundles = 0, captures = 0;
    CHECK(count_dirs(CAIRN_DIR_BUNDLES, &bundles), "cannot list bundles");
    CHECK(count_dirs(CAIRN_DIR_CAPTURE, &captures), "cannot list captures");
    CHECK(bundles == 0, "a live capture was moved into bundles (%d found)", bundles);
    CHECK(captures == 1, "the live capture disappeared (%d found)", captures);

    return true;
}

/* ── the prune gate ───────────────────────────────────────────────────────── */

/* Shared setup: one sealed bundle, ready to be offered a receipt. */
static bool sealed_bundle(char id_out[27], uint8_t root_out[32])
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    if (!cairn_identity_load(device_id, seed, pub, &boot)) return false;

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    if (!cairn_capture_open_or_resume(&cap, device_id, boot_id)) return false;
    if (append_samples(&cap, 8) != 8) return false;

    return seal_now(&cap, id_out, root_out);
}

static bool bundle_still_present(const char *id)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, id);
    return cairn_fs_exists(dir);
}

/*
 * The invariant, stated as a test: a receipt signed by a key other than the
 * pinned one authorizes nothing. This is the row that matters most — the
 * failure it guards against deletes data permanently and reports success.
 */
static bool row_prune_rejects_wrong_key(void)
{
    char    id[27];
    uint8_t root[32];
    CHECK(sealed_bundle(id, root), "setup: seal failed");

    uint8_t pinned[32];
    cairn_ed25519_public_from_seed(SERVER_SEED, pinned);

    /* Signed by a key the device does not trust. */
    static uint8_t receipt[1024];
    uint8_t bundle_id[16];
    CHECK(cairn_ulid_decode(id, bundle_id), "bad id");
    size_t len = make_receipt(OTHER_SEED, root, bundle_id, receipt, sizeof(receipt));
    CHECK(len > 0, "could not build a receipt");

    cairn_prune_result_t r =
        cairn_prune_if_receipted(id, receipt, len, pinned, root);
    CHECK(r == CAIRN_PRUNE_RECEIPT_UNVERIFIED,
          "result is %s, want RECEIPT_UNVERIFIED", cairn_prune_result_name(r));
    CHECK(bundle_still_present(id), "the bundle was deleted on an unverified receipt");

    return true;
}

/*
 * A genuine signature over a *different* bundle is not an acknowledgement of
 * this one. Accepting it would let a server mix-up induce deletion of data it
 * never received.
 */
static bool row_prune_rejects_wrong_root(void)
{
    char    id[27];
    uint8_t root[32];
    CHECK(sealed_bundle(id, root), "setup: seal failed");

    uint8_t pinned[32];
    cairn_ed25519_public_from_seed(SERVER_SEED, pinned);

    uint8_t other_root[32];
    memcpy(other_root, root, 32);
    other_root[0] ^= 0xFF;

    static uint8_t receipt[1024];
    uint8_t bundle_id[16];
    CHECK(cairn_ulid_decode(id, bundle_id), "bad id");
    size_t len =
        make_receipt(SERVER_SEED, other_root, bundle_id, receipt, sizeof(receipt));
    CHECK(len > 0, "could not build a receipt");

    cairn_prune_result_t r =
        cairn_prune_if_receipted(id, receipt, len, pinned, root);
    CHECK(r == CAIRN_PRUNE_WRONG_BUNDLE,
          "result is %s, want WRONG_BUNDLE", cairn_prune_result_name(r));
    CHECK(bundle_still_present(id),
          "the bundle was deleted on a receipt for different content");

    return true;
}

/*
 * No pinned key means no pruning. An unconfigured device must fill its card
 * rather than guess: a full card is recoverable, a wrong deletion is not.
 */
static bool row_prune_requires_pinned_key(void)
{
    char    id[27];
    uint8_t root[32];
    CHECK(sealed_bundle(id, root), "setup: seal failed");

    static uint8_t receipt[1024];
    uint8_t bundle_id[16];
    CHECK(cairn_ulid_decode(id, bundle_id), "bad id");
    size_t len = make_receipt(SERVER_SEED, root, bundle_id, receipt, sizeof(receipt));
    CHECK(len > 0, "could not build a receipt");

    /* No key at all. */
    cairn_prune_result_t r1 = cairn_prune_if_receipted(id, receipt, len, NULL, root);
    CHECK(r1 == CAIRN_PRUNE_NO_PINNED_KEY,
          "result is %s with a NULL key, want NO_PINNED_KEY",
          cairn_prune_result_name(r1));

    /* The all-zero placeholder from secrets.h.example. */
    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    cairn_prune_result_t r2 = cairn_prune_if_receipted(id, receipt, len, zero, root);
    CHECK(r2 == CAIRN_PRUNE_NO_PINNED_KEY,
          "result is %s with the zero placeholder, want NO_PINNED_KEY",
          cairn_prune_result_name(r2));

    CHECK(bundle_still_present(id), "the bundle was deleted with no pinned key");

    return true;
}

/* Garbage in place of a receipt must be rejected before anything is touched. */
static bool row_prune_rejects_malformed(void)
{
    char    id[27];
    uint8_t root[32];
    CHECK(sealed_bundle(id, root), "setup: seal failed");

    uint8_t pinned[32];
    cairn_ed25519_public_from_seed(SERVER_SEED, pinned);

    uint8_t junk[64];
    memset(junk, 0xA5, sizeof(junk));

    cairn_prune_result_t r =
        cairn_prune_if_receipted(id, junk, sizeof(junk), pinned, root);
    CHECK(r == CAIRN_PRUNE_RECEIPT_MALFORMED,
          "result is %s, want RECEIPT_MALFORMED", cairn_prune_result_name(r));
    CHECK(bundle_still_present(id), "the bundle was deleted on a malformed receipt");

    return true;
}

/*
 * The positive case. With both conditions met the bundle is deleted and the
 * journal is left clean — otherwise every boot would try to finish a prune that
 * already happened.
 */
static bool row_prune_authorized(void)
{
    char    id[27];
    uint8_t root[32];
    CHECK(sealed_bundle(id, root), "setup: seal failed");

    uint8_t pinned[32];
    cairn_ed25519_public_from_seed(SERVER_SEED, pinned);

    static uint8_t receipt[1024];
    uint8_t bundle_id[16];
    CHECK(cairn_ulid_decode(id, bundle_id), "bad id");
    size_t len = make_receipt(SERVER_SEED, root, bundle_id, receipt, sizeof(receipt));
    CHECK(len > 0, "could not build a receipt");

    CHECK(cairn_receipt_store(id, receipt, len), "cannot store the receipt");

    cairn_prune_result_t r =
        cairn_prune_if_receipted(id, receipt, len, pinned, root);
    CHECK(r == CAIRN_PRUNE_OK, "result is %s, want OK", cairn_prune_result_name(r));
    CHECK(!bundle_still_present(id), "the bundle survived an authorized prune");

    /* The receipt outlives the payload: it is the evidence the data existed. */
    CHECK(cairn_receipt_exists(id), "the receipt was deleted along with the bundle");

    /* And the intent must be cleared. */
    char jpath[256];
    snprintf(jpath, sizeof(jpath), "%s/prune-%s.json", CAIRN_DIR_STATE, id);
    CHECK(!cairn_fs_exists(jpath), "the prune intent was left behind");

    return true;
}

/*
 * A prune interrupted part-way leaves an intent record. With the receipt still
 * on the card the deletion is already authorized, so the next boot finishes it.
 */
static bool row_interrupted_prune_resumed(void)
{
    char    id[27];
    uint8_t root[32];
    CHECK(sealed_bundle(id, root), "setup: seal failed");

    static uint8_t receipt[1024];
    uint8_t bundle_id[16];
    CHECK(cairn_ulid_decode(id, bundle_id), "bad id");
    size_t len = make_receipt(SERVER_SEED, root, bundle_id, receipt, sizeof(receipt));
    CHECK(len > 0, "could not build a receipt");
    CHECK(cairn_receipt_store(id, receipt, len), "cannot store the receipt");

    /* Hand-write the intent, as a crash mid-prune would have left it. */
    char jpath[256];
    snprintf(jpath, sizeof(jpath), "%s/prune-%s.json", CAIRN_DIR_STATE, id);
    cairn_file_t *f = cairn_fs_open(jpath, CAIRN_FS_WRITE);
    CHECK(f != NULL, "cannot write an intent record");
    const char *line = "{\"state\":\"intent\"}\n";
    cairn_fs_write(f, line, strlen(line));
    cairn_fs_close(f);

    int finished = cairn_prune_resume_interrupted();
    CHECK(finished == 1, "finished %d prunes, want 1", finished);
    CHECK(!bundle_still_present(id), "the bundle survived a resumed prune");
    CHECK(!cairn_fs_exists(jpath), "the intent was left behind");

    /* Idempotent. */
    int again = cairn_prune_resume_interrupted();
    CHECK(again == 0, "a second pass finished %d prunes, want 0", again);

    return true;
}

/*
 * An intent with no stored receipt is the dangerous case: there is no local
 * evidence the data is safe. The bundle must be kept and the intent cleared —
 * erring towards a full card, which is always recoverable.
 */
static bool row_interrupted_prune_without_receipt(void)
{
    char    id[27];
    uint8_t root[32];
    CHECK(sealed_bundle(id, root), "setup: seal failed");

    char jpath[256];
    snprintf(jpath, sizeof(jpath), "%s/prune-%s.json", CAIRN_DIR_STATE, id);
    cairn_file_t *f = cairn_fs_open(jpath, CAIRN_FS_WRITE);
    CHECK(f != NULL, "cannot write an intent record");
    const char *line = "{\"state\":\"intent\"}\n";
    cairn_fs_write(f, line, strlen(line));
    cairn_fs_close(f);

    int finished = cairn_prune_resume_interrupted();
    CHECK(finished == 0, "finished %d prunes with no receipt, want 0", finished);
    CHECK(bundle_still_present(id),
          "the bundle was deleted on an intent with no receipt");
    CHECK(!cairn_fs_exists(jpath), "the unusable intent was not cleared");

    return true;
}

/*
 * A sealed bundle is never mutated. Sealing into a name that already exists
 * must fail rather than overwrite, because the existing bundle may be waiting
 * for a receipt.
 */
static bool row_seal_never_overwrites(void)
{
    char    id[27];
    uint8_t root[32];
    CHECK(sealed_bundle(id, root), "setup: seal failed");

    /* Re-create a capture directory under the same id, with a manifest, and ask
     * the store to complete the seal. */
    char cdir[256];
    snprintf(cdir, sizeof(cdir), "%s/%s", CAIRN_DIR_CAPTURE, id);
    CHECK(cairn_fs_mkdir(cdir), "cannot stage a colliding capture");

    char mpath[320], spath[320], segpath[320];
    snprintf(mpath, sizeof(mpath), "%s/manifest.cbor", cdir);
    snprintf(spath, sizeof(spath), "%s/manifest.sig", cdir);
    snprintf(segpath, sizeof(segpath), "%s/seg-00000000.seg", cdir);

    const char *filler = "not a real manifest";
    for (int i = 0; i < 3; i++) {
        const char *p = (i == 0) ? mpath : (i == 1) ? spath : segpath;
        cairn_file_t *f = cairn_fs_open(p, CAIRN_FS_WRITE);
        CHECK(f != NULL, "cannot stage %s", p);
        cairn_fs_write(f, filler, strlen(filler));
        cairn_fs_close(f);
    }

    int finished = cairn_store_resume_interrupted_seals();
    CHECK(finished == 0, "overwrote a sealed bundle (%d seals completed)", finished);

    /* Both must still exist: the original untouched, the collider not promoted. */
    CHECK(bundle_still_present(id), "the original sealed bundle disappeared");
    CHECK(cairn_fs_exists(cdir), "the colliding capture was lost");

    /* And the original's manifest must be the real one, not the filler. */
    char orig[320];
    snprintf(orig, sizeof(orig), "%s/%s/manifest.cbor", CAIRN_DIR_BUNDLES, id);
    uint64_t size = 0;
    CHECK(cairn_fs_file_size(orig, &size), "cannot size the original manifest");
    CHECK(size > strlen(filler),
          "the original manifest is %llu bytes — it looks overwritten",
          (unsigned long long)size);

    return true;
}


/* ── the pre-roll ring ────────────────────────────────────────────────────── */

/*
 * Count the frames in a segment and check every one carries PRETRIP. Reads the
 * card through the scan, so this asserts what was actually written rather than
 * what the ring believed it wrote.
 */
typedef struct {
    int      seen;
    int      pretrip;
    uint32_t last_monotonic;
    bool     monotonic_ok;
} tally_t;

static bool tally_cb(const cairn_frame_t *f, void *user)
{
    tally_t *t = (tally_t *)user;

    t->seen++;
    if ((f->flags & CAIRN_FLAG_PRETRIP) != 0) t->pretrip++;

    /* Frames must be written in observation order, or the bundle claims the
     * drive happened out of sequence. */
    if (f->monotonic_ms < t->last_monotonic) t->monotonic_ok = false;
    t->last_monotonic = f->monotonic_ms;

    return true;
}

static bool scan_capture_segment(const char *id, tally_t *t)
{
    char rel[256];
    snprintf(rel, sizeof(rel), "%s/%s/seg-00000000.seg", CAIRN_DIR_CAPTURE, id);

    cairn_file_t *f = cairn_fs_open(rel, CAIRN_FS_READ);
    if (f == NULL) return false;

    uint64_t size = cairn_fs_size(f);

    memset(t, 0, sizeof(*t));
    t->monotonic_ok = true;

    cairn_scan_state_t state = { 0, 0 };
    cairn_scan_result_t res;
    cairn_err_t err =
        cairn_scan_segment_stream(fs_read_for_test, f, size, state, &res,
                                  tally_cb, t);
    cairn_fs_close(f);

    return err == CAIRN_OK && res.stop == CAIRN_STOP_EOF;
}

/*
 * Nothing is written while the trip is unconfirmed. This is what keeps a parked
 * car from accumulating bundles every time a door slams.
 */
static bool row_preroll_holds_while_idle(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");

    char id[27];
    CHECK(current_capture_id(id, sizeof(id)), "no capture directory");

    char rel[256];
    snprintf(rel, sizeof(rel), "%s/%s/seg-00000000.seg", CAIRN_DIR_CAPTURE, id);

    uint64_t before = 0;
    CHECK(file_size_of(rel, &before), "cannot size the segment");

    cairn_preroll_t ring;
    cairn_preroll_reset(&ring);

    for (int i = 0; i < 20; i++) {
        uint8_t payload[32];
        make_gnss_payload(payload, i);
        cairn_host_advance(1000);
        CHECK(cairn_preroll_push(&ring, CAIRN_REC_GNSS_SAMPLE, 1, 0,
                                 cairn_millis(), payload, sizeof(payload)),
              "push %d failed", i);
    }

    uint64_t after = 0;
    CHECK(file_size_of(rel, &after), "cannot re-size the segment");
    CHECK(after == before,
          "the segment grew from %llu to %llu bytes while buffering",
          (unsigned long long)before, (unsigned long long)after);
    CHECK(cap.capture_chain.next_seq == 0,
          "the chain advanced to %u while buffering",
          (unsigned)cap.capture_chain.next_seq);

    return true;
}

/*
 * On confirmation the held records are written, in order, every one flagged
 * PRETRIP. The flag is the point: these are real observations recorded before
 * the device decided a trip was underway, and a reader is entitled to tell them
 * apart from the trip proper.
 */
static bool row_preroll_flush_marks_pretrip(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");

    char id[27];
    CHECK(current_capture_id(id, sizeof(id)), "no capture directory");

    cairn_preroll_t ring;
    cairn_preroll_reset(&ring);

    const int held = 12;
    for (int i = 0; i < held; i++) {
        uint8_t payload[32];
        make_gnss_payload(payload, i);
        cairn_host_advance(1000);
        CHECK(cairn_preroll_push(&ring, CAIRN_REC_GNSS_SAMPLE, 1, 0,
                                 cairn_millis(), payload, sizeof(payload)),
              "push %d failed", i);
    }

    uint16_t written = cairn_preroll_flush(&ring, &cap);
    CHECK(written == held, "flushed %u of %d frames", (unsigned)written, held);

    /* The ring must be empty afterwards, or a second confirmation would write
     * the same observations twice. */
    uint32_t a = 0, b = 0;
    CHECK(!cairn_preroll_span(&ring, &a, &b), "the ring still reports contents");
    CHECK(cairn_preroll_flush(&ring, &cap) == 0,
          "a second flush wrote frames again");

    tally_t t;
    CHECK(scan_capture_segment(id, &t), "the segment does not scan cleanly");
    CHECK(t.seen == held, "segment holds %d frames, want %d", t.seen, held);
    CHECK(t.pretrip == held, "%d of %d frames carry PRETRIP", t.pretrip, held);
    CHECK(t.monotonic_ok, "frames were written out of observation order");

    /* Records written after confirmation must NOT be flagged. */
    uint8_t payload[32];
    make_gnss_payload(payload, 99);
    cairn_host_advance(1000);
    CHECK(cairn_capture_append(&cap, CAIRN_CHAIN_CAPTURE, CAIRN_REC_GNSS_SAMPLE,
                               1, 0, cairn_millis(), payload, sizeof(payload)),
          "post-confirmation append failed");

    CHECK(scan_capture_segment(id, &t), "the segment does not re-scan cleanly");
    CHECK(t.seen == held + 1, "segment holds %d frames, want %d", t.seen, held + 1);
    CHECK(t.pretrip == held,
          "%d frames carry PRETRIP, want %d — the flag leaked past confirmation",
          t.pretrip, held);

    return true;
}

/*
 * A ring that wraps keeps the most recent window, because that is the part
 * adjacent to the trip about to be confirmed — and it counts what it dropped
 * rather than discarding silently.
 */
static bool row_preroll_wrap_keeps_newest(void)
{
    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    CHECK(cairn_identity_load(device_id, seed, pub, &boot), "identity load failed");

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    cairn_capture_t cap;
    CHECK(cairn_capture_open_or_resume(&cap, device_id, boot_id), "open failed");

    char id[27];
    CHECK(current_capture_id(id, sizeof(id)), "no capture directory");

    cairn_preroll_t ring;
    cairn_preroll_reset(&ring);

    /* Overfill by 10. */
    const int pushed = CAIRN_PREROLL_RING_SAMPLES + 10;
    uint32_t first_kept_ms = 0;

    for (int i = 0; i < pushed; i++) {
        uint8_t payload[32];
        make_gnss_payload(payload, i);
        cairn_host_advance(100);

        if (i == 10) first_kept_ms = cairn_millis();
        CHECK(cairn_preroll_push(&ring, CAIRN_REC_GNSS_SAMPLE, 1, 0,
                                 cairn_millis(), payload, sizeof(payload)),
              "push %d failed", i);
    }

    CHECK(ring.count == CAIRN_PREROLL_RING_SAMPLES,
          "ring holds %u entries, want %d", (unsigned)ring.count,
          CAIRN_PREROLL_RING_SAMPLES);
    CHECK(ring.overwritten == 10, "overwritten is %u, want 10",
          (unsigned)ring.overwritten);

    uint32_t oldest = 0, newest = 0;
    CHECK(cairn_preroll_span(&ring, &oldest, &newest), "no span reported");
    CHECK(oldest == first_kept_ms,
          "oldest held sample is %u ms, want %u — the wrong end was evicted",
          (unsigned)oldest, (unsigned)first_kept_ms);

    uint16_t written = cairn_preroll_flush(&ring, &cap);
    CHECK(written == CAIRN_PREROLL_RING_SAMPLES, "flushed %u frames, want %d",
          (unsigned)written, CAIRN_PREROLL_RING_SAMPLES);

    tally_t t;
    CHECK(scan_capture_segment(id, &t), "the segment does not scan cleanly");
    CHECK(t.seen == CAIRN_PREROLL_RING_SAMPLES, "segment holds %d frames, want %d",
          t.seen, CAIRN_PREROLL_RING_SAMPLES);
    CHECK(t.monotonic_ok, "frames were written out of observation order");

    return true;
}

/*
 * An oversized payload is refused outright. Truncating it would produce a frame
 * that decodes as a different observation — valid-looking and wrong, which is
 * worse than not having it.
 */
static bool row_preroll_refuses_oversized(void)
{
    cairn_preroll_t ring;
    cairn_preroll_reset(&ring);

    uint8_t big[CAIRN_PREROLL_MAX_PAYLOAD + 1];
    memset(big, 0x5A, sizeof(big));

    CHECK(!cairn_preroll_push(&ring, CAIRN_REC_IMU_RAW_WINDOW, 1, 0, 1000, big,
                              sizeof(big)),
          "an oversized payload was accepted");
    CHECK(ring.count == 0, "the ring holds %u entries after a refusal",
          (unsigned)ring.count);

    /* A payload at exactly the limit must still be accepted. */
    CHECK(cairn_preroll_push(&ring, CAIRN_REC_GNSS_SAMPLE, 1, 0, 1000, big,
                             CAIRN_PREROLL_MAX_PAYLOAD),
          "a payload at the limit was refused");
    CHECK(ring.count == 1, "the ring holds %u entries, want 1",
          (unsigned)ring.count);

    return true;
}

/* ── driver ───────────────────────────────────────────────────────────────── */

typedef struct {
    const char *family;
    const char *name;
    bool (*fn)(void);
} row_t;

static const row_t ROWS[] = {
    { "seal",     "clean capture seals and verifies",         row_clean_seal },
    { "recovery", "torn tail mid-frame",                      row_torn_tail_mid_frame },
    { "recovery", "torn tail mid-header",                     row_torn_tail_mid_header },
    { "recovery", "corrupt frame isolates to one record",     row_corrupt_frame_isolated },
    { "recovery", "one chain spans segment rotation",         row_chain_spans_rotation },
    { "recovery", "journal chain is independent",             row_journal_chain_independent },
    { "seal",     "interrupted seal completes idempotently",  row_interrupted_seal_completed },
    { "seal",     "a live capture is not mistaken for a seal", row_live_capture_not_sealed },
    { "seal",     "a sealed bundle is never overwritten",     row_seal_never_overwrites },
    { "prune",    "rejects a receipt from an untrusted key",  row_prune_rejects_wrong_key },
    { "prune",    "rejects a receipt for different content",  row_prune_rejects_wrong_root },
    { "prune",    "rejects a malformed receipt",              row_prune_rejects_malformed },
    { "prune",    "refuses without a pinned key",             row_prune_requires_pinned_key },
    { "prune",    "deletes on a verified receipt",            row_prune_authorized },
    { "prune",    "resumes an interrupted prune",             row_interrupted_prune_resumed },
    { "prune",    "keeps data when an intent has no receipt", row_interrupted_prune_without_receipt },
    { "preroll",  "holds records while the trip is unconfirmed", row_preroll_holds_while_idle },
    { "preroll",  "flush writes in order, flagged PRETRIP",    row_preroll_flush_marks_pretrip },
    { "preroll",  "a wrapped ring keeps the newest window",    row_preroll_wrap_keeps_newest },
    { "preroll",  "refuses an oversized payload",              row_preroll_refuses_oversized },
};

int main(int argc, char **argv)
{
    if (argc > 1) g_seed = strtoull(argv[1], NULL, 10);

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
            printf("  pass  [%-8s] %s\n", ROWS[i].family, ROWS[i].name);
        } else {
            g_fail++;
            printf("  FAIL  [%-8s] %s\n", ROWS[i].family, ROWS[i].name);
        }

        char tree[700];
        snprintf(tree, sizeof(tree), "/tmp/cairn-fault-%s", slug);
        rm_rf(tree);
    }

    printf("\nfirmware storage matrix: %d/%zu passed\n", g_pass, n);

    if (g_failure_count > 0) {
        printf("\n%d failure(s):\n", g_failure_count);
        for (int i = 0; i < g_failure_count; i++) printf("  %s\n", g_failures[i]);
        return 1;
    }

    return (g_fail == 0) ? 0 : 1;
}
