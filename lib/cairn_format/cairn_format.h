/*
 * Cairn bundle format v3 — C implementation.
 *
 * The normative specification is contracts/format/v3/spec.md. This is the third
 * implementation of it, after Go (server/format) and Rust (emulator/src/format),
 * and it is the one that matters most: a disagreement here means a device
 * writing bundles the server cannot recover.
 *
 * v3 replaces v2 outright. There is no v2 read path: v2 recordings were test
 * data, and a reader for a dead format is code that can only be wrong.
 *
 * What v3 adds is that every frame is sealed with XChaCha20-Poly1305 under a
 * per-segment key, and every segment and manifest names its vehicle, its
 * assignment and a monotonic device counter. Everything structural — torn tails,
 * CRCs, the prev_crc32 chain, the Merkle root — still works on the stored bytes
 * with no key at all, which is why the recovery scan below takes the key as an
 * optional callback rather than a requirement.
 *
 * Deliberately portable C11 with no ESP-IDF dependency, so exactly this code
 * can be compiled natively and checked against the committed conformance
 * vectors in contracts/format/v3/vectors/. Verifying firmware correctness without
 * hardware is otherwise impossible, and "it compiled" is not the same as "it
 * agrees with the other two implementations". The one thing this library does
 * not do is generate randomness: nonces are passed in, so the caller owns the
 * RNG (the hardware one on the device, a fixed stream under test).
 *
 * On ESP32, CAIRN_USE_ESP_ROM_CRC routes the checksum through esp_rom_crc32_le,
 * which is why the specification chose CRC-32 over CRC32C in the first place.
 */

#ifndef CAIRN_FORMAT_H
#define CAIRN_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── versions ─────────────────────────────────────────────────────────────── */

#define CAIRN_FORMAT_VERSION    3
#define CAIRN_MANIFEST_VERSION  3
/* Receipts did not change in v3, so neither did their version. */
#define CAIRN_RECEIPT_VERSION   2

#define CAIRN_SIGALG_ED25519 "ed25519"

/*
 * Named in the signed manifest so a future suite is an explicit, detectable
 * change rather than something inferred from key or nonce length.
 */
#define CAIRN_ENCRYPTION_SUITE_V1 "xchacha20poly1305+hkdf-sha256/v1"

/* The fixed prefix of the HKDF info string for segment keys (§3.6). */
#define CAIRN_HKDF_SEGMENT_LABEL "cairn/segment/v3"

/* ── geometry ─────────────────────────────────────────────────────────────── */

#define CAIRN_SEGMENT_HEADER_SIZE 128

/*
 * The longest header_len a *keyed* scan will accept. header_len lets a reader
 * skip a longer header from a future version, but every byte of it except the
 * CRC is AAD, so a keyed scan must hold all of it. A structural scan needs only
 * the first 128 bytes and has no such limit. Every v3 writer emits 128.
 */
#define CAIRN_MAX_HEADER_LEN      256

/* segment_index reserved for journal.seg. It is a KDF input, so the journal's
 * key can never coincide with a capture segment's. */
#define CAIRN_JOURNAL_SEGMENT_INDEX 0xFFFFFFFFu

#define CAIRN_ROOT_KEY_SIZE    32
#define CAIRN_SEGMENT_KEY_SIZE 32
#define CAIRN_NONCE_SIZE       24
#define CAIRN_TAG_SIZE         16
#define CAIRN_AEAD_OVERHEAD    (CAIRN_NONCE_SIZE + CAIRN_TAG_SIZE) /* 40 */

#define CAIRN_FRAME_HEADER_SIZE   24
#define CAIRN_FRAME_TRAILER_SIZE  4
/*
 * Every frame is sealed, so even an empty record carries the nonce and tag:
 * frame_len = 68 + N. The maximum frame is unchanged from v2, which is why the
 * largest plaintext payload shrank from 4068 to 4028.
 */
#define CAIRN_FRAME_OVERHEAD      (CAIRN_FRAME_HEADER_SIZE + CAIRN_AEAD_OVERHEAD + \
                                   CAIRN_FRAME_TRAILER_SIZE)                          /* 68 */
#define CAIRN_MIN_FRAME_LEN       CAIRN_FRAME_OVERHEAD
#define CAIRN_MAX_FRAME_LEN       4096
#define CAIRN_MAX_PAYLOAD_SIZE    (CAIRN_MAX_FRAME_LEN - CAIRN_FRAME_OVERHEAD)         /* 4028 */

/* ── record types ─────────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_REC_GNSS_SAMPLE      = 0x01,
    CAIRN_REC_IMU_SUMMARY      = 0x02,
    CAIRN_REC_IMU_RAW_WINDOW   = 0x03,
    CAIRN_REC_OBD_SNAPSHOT     = 0x04,
    CAIRN_REC_DEVICE_HEALTH    = 0x05,
    CAIRN_REC_TRIP_EVENT       = 0x06,
    CAIRN_REC_STATE_TRANSITION = 0x07,
    CAIRN_REC_GNSS_GAP         = 0x08,
    CAIRN_REC_POLICY_SNAPSHOT  = 0x09,
    CAIRN_REC_OBD_EXTENDED     = 0x0A,
} cairn_record_type_t;

bool cairn_record_type_known(uint8_t t);
const char *cairn_record_type_name(uint8_t t);

/* ── frame flags ──────────────────────────────────────────────────────────── */

#define CAIRN_FLAG_PRETRIP       (1u << 0)
#define CAIRN_FLAG_DEGRADED      (1u << 1)
#define CAIRN_FLAG_ESTIMATED_UTC (1u << 2)
#define CAIRN_FLAG_POST_RECOVERY (1u << 3)

/* ── degraded-state bitmap (§4.10) ────────────────────────────────────────── */

/*
 * DEVICE_HEALTH.health_state is a bitmap, not a severity.
 *
 * Degradation is not ordered: a vehicle can be low on battery *and* without a
 * fix *and* out of card space at once, with different causes and different
 * fixes. A scalar would force a priority between them and discard the rest —
 * reporting the battery while losing the fact that position was unavailable
 * too. Each condition gets its own bit so a reader recovers the whole set.
 */
#define CAIRN_HEALTH_OK                0x00u
#define CAIRN_HEALTH_DEGRADED_GNSS     0x01u
#define CAIRN_HEALTH_DEGRADED_STORAGE  0x02u
#define CAIRN_HEALTH_DEGRADED_TIME     0x04u
#define CAIRN_HEALTH_DEGRADED_NETWORK  0x08u
#define CAIRN_HEALTH_LOW_POWER         0x10u
#define CAIRN_HEALTH_RECOVERY_REQUIRED 0x20u
#define CAIRN_HEALTH_DEGRADED_SENSING  0x40u
/* 0x80 reserved */

/*
 * Render a bitmap into `out` as a "|"-separated list of names, for logs. Any
 * unknown bit is rendered rather than dropped, so a log from newer firmware is
 * still readable here.
 */
void cairn_health_state_names(uint8_t state, char *out, size_t cap);

/* ── results ──────────────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_OK = 0,
    CAIRN_ERR_BAD_MAGIC,
    CAIRN_ERR_BAD_HEADER_CRC,
    CAIRN_ERR_SHORT_HEADER,
    CAIRN_ERR_UNSUPPORTED_VERSION,
    CAIRN_ERR_BAD_HEADER_LEN,
    CAIRN_ERR_PAYLOAD_TOO_LARGE,
    CAIRN_ERR_BUFFER_TOO_SMALL,
    CAIRN_ERR_NON_CANONICAL,
    CAIRN_ERR_TRUNCATED,
    CAIRN_ERR_UNSUPPORTED_CBOR,
    CAIRN_ERR_MALFORMED,
    CAIRN_ERR_BAD_SIGNATURE,
    CAIRN_ERR_CONTENT_ROOT_MISMATCH,
    CAIRN_ERR_RECEIPT_ROOT_MISMATCH,
    CAIRN_ERR_TOO_MANY_MEMBERS,
    CAIRN_ERR_READ_FAILED,
    /* The key provider holds no key for this segment's device. The segment is
     * intact; it can be scanned structurally but not decrypted. */
    CAIRN_ERR_NO_KEY,
    /* The segment was sealed under a storage_key_version the provider does not
     * hold. A key is missing — this is not tampering, and is reported apart
     * from it so an operator is told which. */
    CAIRN_ERR_KEY_VERSION_MISMATCH,
    /* A frame's tag did not verify. Only cairn_open_frame returns this; a scan
     * reports the same condition as CAIRN_STOP_AUTH_FAILED. */
    CAIRN_ERR_AUTH_FAILED,
    /* A segment header disagrees with the manifest about whose data it is. */
    CAIRN_ERR_BINDING_MISMATCH,
} cairn_err_t;

const char *cairn_strerror(cairn_err_t e);

/* ── checksum ─────────────────────────────────────────────────────────────── */

/*
 * CRC-32, reflected IEEE/zlib polynomial 0xEDB88320.
 *
 * The specification chose this over CRC32C precisely because it is available in
 * ESP32 ROM, so the most constrained of the three implementations gets an
 * optimized routine for free.
 */
uint32_t cairn_crc32(const uint8_t *data, size_t len);

/* ── SHA-256 ──────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} cairn_sha256_t;

void cairn_sha256_init(cairn_sha256_t *ctx);
void cairn_sha256_update(cairn_sha256_t *ctx, const uint8_t *data, size_t len);
void cairn_sha256_final(cairn_sha256_t *ctx, uint8_t out[32]);
void cairn_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* ── HMAC and HKDF (cf_hkdf.c) ────────────────────────────────────────────── */

void cairn_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                       size_t len, uint8_t out[32]);

/*
 * HKDF-SHA256, RFC 5869: extract with `salt`, expand with `info` to `out_len`
 * bytes. Returns false only for out_len > 255 * 32, which RFC 5869 forbids.
 */
bool cairn_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                       size_t ikm_len, const uint8_t *info, size_t info_len,
                       uint8_t *out, size_t out_len);

/* ── XChaCha20-Poly1305 (cf_aead.c) ───────────────────────────────────────── */

/* HChaCha20 (draft-irtf-cfrg-xchacha §2.2): the subkey derivation step. */
void cairn_hchacha20(const uint8_t key[32], const uint8_t in[16], uint8_t out[32]);

/*
 * Seal `len` bytes of `pt` into `ct` (which may equal `pt`) and produce the
 * 16-byte tag. The AAD is authenticated but not encrypted.
 */
void cairn_xchacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[24],
                                  const uint8_t *aad, size_t aad_len,
                                  const uint8_t *pt, size_t len, uint8_t *ct,
                                  uint8_t tag[16]);

/*
 * Verify the tag, then decrypt into `pt` (which may equal `ct`). Returns false
 * and leaves `pt` untouched if the tag does not verify — nothing
 * unauthenticated is ever written out.
 */
bool cairn_xchacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[24],
                                  const uint8_t *aad, size_t aad_len,
                                  const uint8_t *ct, size_t len,
                                  const uint8_t tag[16], uint8_t *pt);

/*
 * The same with the AAD supplied in two pieces, authenticated as their
 * concatenation. A frame's AAD is its own 24-byte header followed by the
 * segment header, which live in different buffers.
 */
void cairn_xchacha20poly1305_seal2(const uint8_t key[32], const uint8_t nonce[24],
                                   const uint8_t *aad_a, size_t aad_a_len,
                                   const uint8_t *aad_b, size_t aad_b_len,
                                   const uint8_t *pt, size_t len, uint8_t *ct,
                                   uint8_t tag[16]);
bool cairn_xchacha20poly1305_open2(const uint8_t key[32], const uint8_t nonce[24],
                                   const uint8_t *aad_a, size_t aad_a_len,
                                   const uint8_t *aad_b, size_t aad_b_len,
                                   const uint8_t *ct, size_t len,
                                   const uint8_t tag[16], uint8_t *pt);

/* ── Merkle tree ──────────────────────────────────────────────────────────── */

/*
 * Domain-separated: leaf = SHA256(0x00 || d), internal = SHA256(0x01 || l || r),
 * empty = SHA256(0x02). An odd final node is promoted unchanged, never
 * duplicated — duplicating admits two distinct leaf lists with the same root.
 */
void cairn_leaf_hash(const uint8_t *data, size_t len, uint8_t out[32]);

/*
 * Build a root over `count` leaves. `leaves` is modified in place as scratch,
 * which avoids an allocation on a device with no heap to spare.
 */
void cairn_merkle_root(uint8_t (*leaves)[32], size_t count, uint8_t out[32]);

/* ── members and the content root ─────────────────────────────────────────── */

#define CAIRN_MAX_MEMBERS      16
#define CAIRN_MAX_MEMBER_NAME  32
/*
 * The chunk-descriptor ceiling.
 *
 * Raised from 64 when the chunk target dropped to 8 KiB for resumable uplinks
 * (see CAIRN_CHUNK_TARGET_BYTES). 64 descriptors at that size would have capped
 * a bundle at 512 KiB and failed the seal above it, which trades a whole trip
 * for a transport convenience — the wrong direction. The sealer now grows the
 * chunk size rather than refuse, so this is a bound on bookkeeping, not on how
 * long a drive may be.
 *
 * 96, not more, because this is the real constraint: each slot costs about 224
 * bytes of static DRAM — 40 bytes in each of the two static cairn_manifest_t
 * instances (cairn_offload.c, cairn_store.c) plus 48 in each of the three
 * manifest-sized buffers derived below. At 128 the image no longer links:
 * `.dram0.bss` overflowed `dram0_0_seg` by 1872 bytes, because NimBLE and the
 * Wi-Fi stack have already claimed most of that segment. Note that the total
 * RAM percentage PlatformIO prints is NOT the binding limit; the static DRAM
 * segment is, and it fails at link time rather than at runtime.
 *
 * Back to 64 once both network transports were in one image: the segment
 * overflowed, exactly as the paragraph above warned it would. 64 slots at the
 * 8 KiB target still covers a bundle up to 512 KiB, more than twice the
 * ~230 KB a trip has actually been measured at, and anything larger adapts to
 * coarser chunks rather than failing. Raising it again means freeing DRAM or
 * moving these buffers to the 4 MB PSRAM, which is what net_upload.cpp does
 * for the largest of them.
 */
#define CAIRN_MAX_CHUNKS       64

/*
 * An upper bound on a manifest's canonical CBOR, derived from the ceilings
 * above rather than written as a number.
 *
 * It has to be derived. The sealer's encode buffer, the offload module's
 * manifest buffer and its decode scratch were all a flat 4096, which happened
 * to fit 64 chunk descriptors; raising the chunk ceiling silently overflowed
 * the encode and the only symptom was cairn_capture_seal returning false — a
 * trip lost, with nothing in the log naming the manifest. Tying the bound to
 * the arrays means the next change to either constant carries the buffers with
 * it.
 *
 * Per entry: a chunk descriptor is a 3-element array holding two integers and a
 * 32-byte digest, so 48 bytes covers it with room for the largest integer
 * encodings; a member is a name, a length and a digest, so 96 covers it. The
 * 2048 is the scalars, the fixed strings and the record-count map.
 */
#define CAIRN_MANIFEST_ENCODED_MAX \
    (2048u + (CAIRN_MAX_MEMBERS * 96u) + (CAIRN_MAX_CHUNKS * 48u))

typedef struct {
    char     name[CAIRN_MAX_MEMBER_NAME];
    uint64_t length;
    uint8_t  sha256[32];
} cairn_member_t;

typedef struct {
    uint32_t index;
    uint32_t byte_length;
    uint8_t  sha256[32];
} cairn_chunk_t;

/* Order members canonically: by raw name bytes, ascending. */
void cairn_sort_members(cairn_member_t *members, size_t count);

/*
 * Content root over members. Identity of the *data*: independent of archive
 * framing, so repacking identical bytes yields an identical root.
 *
 * leaf_input = u16le(len(name)) || name || sha256(contents)
 */
cairn_err_t cairn_content_root(const cairn_member_t *members, size_t count,
                               uint8_t out[32]);

/* ── segment header ───────────────────────────────────────────────────────── */

/*
 * The 128-byte v3 header (§3.1). Every field but the CRC is AAD on every frame
 * of the segment, so the identity fields are bound, not merely labels — and
 * they are readable without any key, because intake must be able to authorise
 * a bundle (does this assignment exist, is this counter spent?) before it ever
 * decrypts anything.
 */
typedef struct {
    uint16_t format_version;
    uint8_t  device_id[16];
    uint8_t  boot_id[16];
    uint8_t  vehicle_id[16];
    uint8_t  assignment_id[16];
    uint32_t segment_index;       /* CAIRN_JOURNAL_SEGMENT_INDEX for journal.seg */
    uint32_t first_seq;
    uint64_t opened_monotonic_us;
    uint32_t storage_key_version; /* selects which K_root the segment key derives from */
    uint64_t device_counter;      /* identical in every segment of a bundle */
} cairn_segment_header_t;

/* Encode a 128-byte header into `out`. */
cairn_err_t cairn_encode_segment_header(const cairn_segment_header_t *h,
                                        uint8_t *out, size_t out_cap);

/*
 * Decode and verify. `header_len` receives where frame scanning begins. `len`
 * is the number of bytes available at `buf`, which must be at least 128; it is
 * not required to cover a longer header_len — callers that know the segment's
 * total size check that themselves.
 */
cairn_err_t cairn_parse_segment_header(const uint8_t *buf, size_t len,
                                       cairn_segment_header_t *out,
                                       size_t *header_len);

/* ── keys ─────────────────────────────────────────────────────────────────── */

/*
 * K_seg = HKDF-SHA256(ikm = K_root, salt = vehicle_id,
 *                     info = "cairn/segment/v3" || device_id || assignment_id
 *                            || boot_id || segment_index u32le, L = 32)
 *
 * The header, not the caller, says which key applies.
 */
void cairn_derive_segment_key(const uint8_t root[CAIRN_ROOT_KEY_SIZE],
                              const cairn_segment_header_t *h,
                              uint8_t out[CAIRN_SEGMENT_KEY_SIZE]);

/*
 * Supplies the segment key for a parsed header, or refuses with
 * CAIRN_ERR_NO_KEY / CAIRN_ERR_KEY_VERSION_MISMATCH. A server holding many
 * escrowed roots looks one up by h->device_id and h->storage_key_version; a
 * device holding its own root checks the version and derives.
 */
typedef cairn_err_t (*cairn_key_fn)(void *user, const cairn_segment_header_t *h,
                                    uint8_t key_out[CAIRN_SEGMENT_KEY_SIZE]);

/* A single storage root at a single version. */
typedef struct {
    uint8_t  root[CAIRN_ROOT_KEY_SIZE];
    uint32_t version;
} cairn_root_key_t;

/* A cairn_key_fn over a cairn_root_key_t passed as `user`. */
cairn_err_t cairn_root_key_provider(void *user, const cairn_segment_header_t *h,
                                    uint8_t key_out[CAIRN_SEGMENT_KEY_SIZE]);

/*
 * Everything needed to seal or open the frames of one segment: its key, and
 * the AAD suffix — the segment header bytes [0, header_len - 4). Binding the
 * header into every frame is what stops a valid frame being moved to another
 * segment, bundle, vehicle, device or counter.
 */
typedef struct {
    uint8_t key[CAIRN_SEGMENT_KEY_SIZE];
    uint8_t header_aad[CAIRN_MAX_HEADER_LEN - 4];
    size_t  header_aad_len;
} cairn_segment_cipher_t;

/* `header` is the encoded segment header, `header_len` bytes of it. */
cairn_err_t cairn_segment_cipher_init(cairn_segment_cipher_t *c,
                                      const uint8_t key[CAIRN_SEGMENT_KEY_SIZE],
                                      const uint8_t *header, size_t header_len);

/* Zero the key. A capture holds one per open segment; sealing drops them. */
void cairn_segment_cipher_wipe(cairn_segment_cipher_t *c);

/* ── frames ───────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  record_type;
    uint8_t  schema_version;
    uint16_t flags;
    uint32_t seq;
    uint32_t monotonic_ms;
    uint32_t prev_crc32;

    /*
     * The plaintext record body — but only after a keyed scan authenticated it
     * (`decrypted` true). After a structural scan `payload` is NULL and
     * `payload_len` is 0, deliberately rather than pointing at ciphertext:
     * ciphertext is exactly as long as plaintext, so a payload parser handed it
     * would not fail on length, it would return a plausible record of random
     * numbers.
     */
    const uint8_t *payload;
    size_t         payload_len;
    bool           decrypted;

    /*
     * nonce[24] || ciphertext || tag[16], as stored. Set by a structural scan;
     * NULL after a keyed one, which decrypts in place.
     */
    const uint8_t *sealed;
    size_t         sealed_len;

    uint32_t crc32; /* computed on encode, read on scan */
} cairn_frame_t;

/*
 * Seal `payload` and encode the frame into `out`, returning its length in
 * `written` and its CRC in `crc_out` — which is the next frame's prev_crc32.
 *
 * `nonce` must be 24 fresh random bytes. It is never derived from seq: after a
 * torn-tail truncation the same seq is legitimately written again with
 * different plaintext, and a seq-derived nonce would then encrypt two
 * plaintexts under one (key, nonce) pair — leaking their XOR and, through
 * Poly1305, the authenticator key. The library takes the nonce as an argument
 * so the caller owns the RNG.
 *
 * The CRC covers the ciphertext frame, so a holder of no key can still find a
 * torn tail, a corrupt frame or a broken chain.
 */
cairn_err_t cairn_encode_frame(const cairn_segment_cipher_t *c,
                               const uint8_t nonce[CAIRN_NONCE_SIZE],
                               uint8_t record_type, uint8_t schema_version,
                               uint16_t flags, uint32_t seq,
                               uint32_t monotonic_ms, uint32_t prev_crc32,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t *out, size_t out_cap,
                               size_t *written, uint32_t *crc_out);

/*
 * Authenticate one complete, CRC-valid frame of `frame_len` bytes and decrypt
 * its payload in place. On success `*payload` points into `frame` and
 * `*payload_len` is the plaintext length. CAIRN_ERR_AUTH_FAILED leaves the
 * frame untouched.
 */
cairn_err_t cairn_open_frame(const cairn_segment_cipher_t *c, uint8_t *frame,
                             size_t frame_len, const uint8_t **payload,
                             size_t *payload_len);

/* ── the recovery scan ────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_STOP_EOF = 0,
    CAIRN_STOP_TORN_TAIL,
    CAIRN_STOP_CORRUPT_FRAME,
    CAIRN_STOP_CHAIN_BREAK,
    CAIRN_STOP_SEQ_GAP,
    /*
     * Keyed scans only. CRC, chain and sequence all held but the tag did not:
     * the signature of a repaired-CRC edit, a frame moved from another segment,
     * or the wrong key — never of a power cut. A structural scan cannot
     * produce it.
     */
    CAIRN_STOP_AUTH_FAILED,
} cairn_stop_reason_t;

const char *cairn_stop_reason_name(cairn_stop_reason_t r);

/* Continuity across a segment boundary within one chain. */
typedef struct {
    uint32_t expected_seq;
    uint32_t expected_prev;
} cairn_scan_state_t;

typedef struct {
    cairn_segment_header_t header;
    size_t   header_len;

    /* True when every retained frame was also authenticated and decrypted. */
    bool     keyed;

    size_t   frames;               /* valid frames retained */
    uint32_t first_seq;
    uint32_t last_seq;

    cairn_stop_reason_t stop;
    size_t   stop_offset;
    uint32_t discarded_tail_bytes;

    cairn_scan_state_t next;

    size_t unknown_type_count;
    size_t record_counts[256];
} cairn_scan_result_t;

/*
 * Called for each valid frame. Returning false stops the scan early without
 * marking it damaged — used when a caller only needs a tally.
 */
typedef bool (*cairn_frame_cb)(const cairn_frame_t *f, void *user);

/*
 * How to scan. A NULL options pointer, or a NULL `key`, is a structural scan:
 * torn tails, CRCs, the chain and the sequence are verified over the stored
 * ciphertext with no key at all. A `key` provider adds authentication of every
 * frame and hands the callback plaintext.
 */
typedef struct {
    cairn_key_fn   key;
    void          *key_user;
    cairn_frame_cb on_frame;
    void          *frame_user;
} cairn_scan_opts_t;

/*
 * Walk a segment's frames per the specification's recovery algorithm (§3.3).
 *
 * Stop-at-first-invalid: every frame before the failure point is valid and
 * retained; everything from there to end-of-segment is reported as a discarded
 * tail, with the exact byte count and the reason. A header error is returned as
 * an error — the segment is unusable, but the caller must not delete it. So is
 * a key error (CAIRN_ERR_NO_KEY, CAIRN_ERR_KEY_VERSION_MISMATCH): the segment is
 * intact, merely unreadable with what the caller holds.
 *
 * Authentication runs after the structural checks, never before, so a damaged
 * frame reports as damage whether or not a key is held.
 */
cairn_err_t cairn_scan_segment(const uint8_t *buf, size_t len,
                               cairn_scan_state_t state,
                               const cairn_scan_opts_t *opts,
                               cairn_scan_result_t *out);

/*
 * Read `len` bytes at `offset`. Must fill the request completely or return
 * false; a short read is a failure, not an end-of-input signal, because the
 * segment length is known up front.
 */
typedef bool (*cairn_read_fn)(void *user, uint64_t offset, uint8_t *buf, size_t len);

/*
 * The same scan against a reader instead of a buffer, so a segment larger than
 * available RAM can be recovered. Only one frame is resident at a time.
 *
 * This is the real implementation; cairn_scan_segment is a thin wrapper over a
 * memory reader. The firmware recovers from the card through this path, and the
 * conformance vectors exercise it, so the device and the test agree on one body
 * of code rather than two that merely resemble each other.
 *
 * That includes the keyed scan: a frame is authenticated and decrypted in place
 * in the same one-frame stage, so a segment far larger than DRAM is still
 * fully authenticated with one frame resident.
 *
 * Not reentrant: the one-frame staging buffer is static, to keep a 4 KiB
 * allocation off a FreeRTOS task stack. Recovery runs once at boot on a single
 * task, which is the only caller that matters.
 */
cairn_err_t cairn_scan_segment_stream(cairn_read_fn read, void *read_user,
                                      uint64_t len,
                                      cairn_scan_state_t state,
                                      const cairn_scan_opts_t *opts,
                                      cairn_scan_result_t *out);

/* ── deterministic CBOR ───────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;
    int64_t  last_key; /* guards ascending map keys */
} cairn_cbor_enc_t;

void cairn_cbor_init(cairn_cbor_enc_t *e, uint8_t *buf, size_t cap);
void cairn_cbor_uint(cairn_cbor_enc_t *e, uint64_t v);
void cairn_cbor_bytes(cairn_cbor_enc_t *e, const uint8_t *b, size_t len);
void cairn_cbor_text(cairn_cbor_enc_t *e, const char *s);
void cairn_cbor_array(cairn_cbor_enc_t *e, size_t n);
void cairn_cbor_map(cairn_cbor_enc_t *e, size_t n);
void cairn_cbor_null(cairn_cbor_enc_t *e);
void cairn_cbor_key(cairn_cbor_enc_t *e, uint64_t k);

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
} cairn_cbor_dec_t;

void cairn_cbor_dec_init(cairn_cbor_dec_t *d, const uint8_t *buf, size_t len);
cairn_err_t cairn_cbor_uint_read(cairn_cbor_dec_t *d, uint64_t *out);
cairn_err_t cairn_cbor_map_header(cairn_cbor_dec_t *d, size_t *n);
cairn_err_t cairn_cbor_array_header(cairn_cbor_dec_t *d, size_t *n);
cairn_err_t cairn_cbor_bytes_read(cairn_cbor_dec_t *d, const uint8_t **p, size_t *len);
cairn_err_t cairn_cbor_bytes_n(cairn_cbor_dec_t *d, uint8_t *out, size_t n);
cairn_err_t cairn_cbor_text_read(cairn_cbor_dec_t *d, char *out, size_t cap);
bool        cairn_cbor_is_null(cairn_cbor_dec_t *d);

/* ── manifest ─────────────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_RECOVERY_CLEAN          = 0,
    CAIRN_RECOVERY_RECOVERED_TAIL = 1,
    CAIRN_RECOVERY_SALVAGED       = 2,
} cairn_recovery_state_t;

typedef struct {
    uint8_t  manifest_version;
    uint8_t  bundle_id[16];
    uint8_t  device_id[16];
    uint8_t  device_key_id[8];
    uint8_t  boot_id[16];
    char     firmware_version[32];
    uint8_t  schema_version;

    uint64_t capture_started_monotonic_us;
    uint64_t capture_ended_monotonic_us;

    uint64_t utc_basis_ms;
    uint32_t utc_basis_acc_ms;

    uint32_t first_seq;
    uint32_t last_seq;

    /* record_counts[type] — only non-zero entries are encoded, ascending. */
    uint32_t record_counts[256];

    cairn_member_t members[CAIRN_MAX_MEMBERS];
    size_t         member_count;

    cairn_chunk_t  chunks[CAIRN_MAX_CHUNKS];
    size_t         chunk_count;

    uint8_t  content_root[32];

    bool     has_previous_root;
    uint8_t  previous_bundle_root[32];

    uint8_t  policy_version;
    uint8_t  recovery_state;
    uint32_t discarded_tail_bytes;
    char     signature_algorithm[16];

    bool     has_trip_seq;
    uint32_t trip_seq;

    /*
     * Keys 24-28, all mandatory. They bind the bundle to a vehicle, to the
     * device-to-vehicle assignment active when it was captured, to a position
     * in the device's monotonic bundle sequence, and to the root its segments
     * were sealed under. Each is repeated in every segment header, where it is
     * also authenticated into every frame; cairn_verify_segment_binding checks
     * the two agree. They are signed here so the server can enforce assignment
     * and replay policy on the manifest alone, before fetching any content.
     */
    uint8_t  vehicle_id[16];
    uint8_t  assignment_id[16];
    uint64_t device_counter;      /* starts at 1 */
    uint32_t storage_key_version;
    char     encryption_suite[40];
} cairn_manifest_t;

/*
 * Encode in deterministic CBOR. The result is exactly the bytes of
 * manifest.cbor and exactly the bytes the signature covers, so there is
 * nothing to strip or re-encode before verifying.
 */
cairn_err_t cairn_manifest_encode(const cairn_manifest_t *m,
                                  uint8_t *out, size_t out_cap, size_t *written);

/*
 * Decode and verify canonical encoding by re-encoding and comparing bytes.
 *
 * Strict on purpose: a manifest whose bytes we would not ourselves have
 * produced cannot be safely re-serialized, and any discrepancy would silently
 * break signature verification.
 */
cairn_err_t cairn_manifest_decode(const uint8_t *buf, size_t len,
                                  cairn_manifest_t *out,
                                  uint8_t *scratch, size_t scratch_cap);

/* Recompute the content root from the members and compare to the signed value. */
cairn_err_t cairn_manifest_verify_content_root(const cairn_manifest_t *m);

/*
 * Binding rules, §5.4: check one member segment's parsed header against the
 * manifest it is listed in under `name`.
 *
 * device_id, boot_id, vehicle_id, assignment_id, device_counter and
 * storage_key_version must equal the manifest's; a capture segment's
 * segment_index must equal the index in its name, and journal.seg must carry
 * the reserved index — otherwise a segment could be renamed into another
 * position while keeping its own key-derivation inputs.
 *
 * A valid manifest signature does not make this redundant: the signature binds
 * the manifest to the device key, not the manifest to its segments. On a
 * mismatch `field` (if non-NULL) receives the name of the first field that
 * disagreed, in the same order the Go reference checks them.
 */
cairn_err_t cairn_verify_segment_binding(const cairn_manifest_t *m, const char *name,
                                         const cairn_segment_header_t *h,
                                         const char **field);

/* ── receipt ──────────────────────────────────────────────────────────────── */

#define CAIRN_MAX_OBJECT_IDS 8
#define CAIRN_MAX_OBJECT_ID_LEN 80

typedef struct {
    uint8_t  receipt_version;
    uint8_t  receipt_id[16];
    uint8_t  device_id[16];
    uint8_t  bundle_id[16];
    uint8_t  content_root[32];
    uint64_t server_ingest_utc_ms;
    uint8_t  server_key_id[8];
    uint8_t  ingest_schema_version;

    char     object_ids[CAIRN_MAX_OBJECT_IDS][CAIRN_MAX_OBJECT_ID_LEN];
    size_t   object_id_count;

    char     signature_algorithm[16];
    uint8_t  signature[64];
} cairn_receipt_t;

/* The deterministic encoding of keys 1..10 — what the signature covers. */
cairn_err_t cairn_receipt_signing_bytes(const cairn_receipt_t *r,
                                        uint8_t *out, size_t out_cap, size_t *written);

/* The complete receipt, signature included. */
cairn_err_t cairn_receipt_encode(const cairn_receipt_t *r,
                                 uint8_t *out, size_t out_cap, size_t *written);

/* Decode, verifying canonical encoding. */
cairn_err_t cairn_receipt_decode(const uint8_t *buf, size_t len,
                                 cairn_receipt_t *out,
                                 uint8_t *scratch, size_t scratch_cap);

/* ── Ed25519 ──────────────────────────────────────────────────────────────── */

#define CAIRN_ED25519_SEED_SIZE   32
#define CAIRN_ED25519_PUBLIC_SIZE 32
#define CAIRN_ED25519_SIG_SIZE    64

void cairn_ed25519_public_from_seed(const uint8_t seed[32], uint8_t pub[32]);
void cairn_ed25519_sign(const uint8_t *msg, size_t len,
                        const uint8_t seed[32], const uint8_t pub[32],
                        uint8_t sig[64]);
bool cairn_ed25519_verify(const uint8_t *msg, size_t len,
                          const uint8_t sig[64], const uint8_t pub[32]);

/*
 * X25519 (RFC 7748). `scalar` is clamped internally, so it may be raw random
 * bytes. Returns false when the result is all zero (a low-order peer point);
 * the caller must then discard it.
 */
bool cairn_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
bool cairn_x25519_public(uint8_t pub[32], const uint8_t scalar[32]);

/* The 8-byte key id: truncated SHA-256 of the public key. */
void cairn_device_key_id(const uint8_t pub[32], uint8_t out[8]);

/* ── signing and verification helpers ─────────────────────────────────────── */

cairn_err_t cairn_manifest_sign(const cairn_manifest_t *m,
                                const uint8_t seed[32], const uint8_t pub[32],
                                uint8_t *encoded, size_t encoded_cap, size_t *written,
                                uint8_t sig[64]);

cairn_err_t cairn_manifest_verify(const uint8_t *encoded, size_t len,
                                  const uint8_t sig[64], const uint8_t pub[32]);

cairn_err_t cairn_receipt_verify(const cairn_receipt_t *r, const uint8_t pub[32]);

/*
 * Both conditions are required before any byte may be pruned: the signature
 * must verify against the pinned server key, and the receipt must acknowledge
 * the content root actually uploaded. A valid signature over a different
 * bundle is not an acknowledgement of this one.
 */
cairn_err_t cairn_receipt_verify_acknowledges(const cairn_receipt_t *r,
                                              const uint8_t pub[32],
                                              const uint8_t uploaded_root[32]);

/* ── OTA update descriptor (docs/ota.md) ──────────────────────────────────── */

#define CAIRN_UPDATE_DESCRIPTOR_VERSION 1
#define CAIRN_MAX_FIRMWARE_VERSION 32

/*
 * Names a firmware image and the constraints on installing it.
 *
 * Signed by an update key kept separate from the receipt key: the receipt key
 * says "this data is safe to delete", the update key says "this code is safe to
 * run". A server compromised enough to issue false receipts costs stored trips;
 * one that could also sign firmware owns the device.
 */
typedef struct {
    uint8_t  descriptor_version;
    char     firmware_version[CAIRN_MAX_FIRMWARE_VERSION];
    uint8_t  image_sha256[32];
    uint32_t image_length;
    char     min_firmware_version[CAIRN_MAX_FIRMWARE_VERSION];
    uint64_t build_utc_ms; /* informational; never an ordering key */
    char     signature_algorithm[16];
} cairn_update_descriptor_t;

/*
 * Decode, rejecting anything this implementation would not itself have
 * produced. Strict because a descriptor whose bytes cannot be reproduced cannot
 * have its signature re-checked — and this is the one signature whose failure
 * mode is an unbootable device.
 */
cairn_err_t cairn_update_decode(const uint8_t *buf, size_t len,
                                cairn_update_descriptor_t *out,
                                uint8_t *scratch, size_t scratch_cap);

/*
 * Verify the signature, then decode.
 *
 * In that order on purpose: the device downloads megabytes on the strength of
 * this check, so an unsigned or wrongly signed descriptor must cost nothing.
 */
cairn_err_t cairn_update_verify(const uint8_t *encoded, size_t len,
                                const uint8_t sig[64], const uint8_t pub[32],
                                cairn_update_descriptor_t *out,
                                uint8_t *scratch, size_t scratch_cap);

/* Encode, for tests and for cross-checking against the reference bytes. */
cairn_err_t cairn_update_encode(const cairn_update_descriptor_t *d,
                                uint8_t *out, size_t out_cap, size_t *written);

/* ── payload builders ─────────────────────────────────────────────────────── */

/*
 * Every "unavailable" sentinel the specification defines is honoured. A
 * decoder must be able to tell "the ECU reported 0" from "the ECU did not
 * answer", and must never see a precision the receiver did not claim.
 */

#define CAIRN_U16_UNKNOWN 0xFFFF
#define CAIRN_I16_UNKNOWN ((int16_t)0x8000)
#define CAIRN_U8_UNKNOWN  0xFF
#define CAIRN_I8_UNKNOWN  ((int8_t)0x80)

#define CAIRN_SOURCE_PHONE 0x20

typedef struct {
    int32_t  lat_e7;
    int32_t  lon_e7;
    int32_t  alt_cm;
    uint16_t speed_cmps;
    uint16_t heading_cdeg;
    uint16_t hdop_e2;
    uint16_t h_acc_cm;
    uint16_t v_acc_cm;
    uint8_t  fix_type;
    uint8_t  sats_used;
    uint8_t  sats_visible;
    uint8_t  source_flags;
    int32_t  utc_offset_ms;
    uint16_t utc_acc_ms;
} cairn_gnss_sample_t;

void cairn_encode_gnss_sample(const cairn_gnss_sample_t *s, uint8_t out[32]);

typedef struct {
    uint16_t window_ms;
    uint16_t accel_rms_mg;
    int16_t  accel_peak_x_mg;
    int16_t  accel_peak_y_mg;
    int16_t  accel_peak_z_mg;
    int16_t  gyro_peak_dps_e1;
    uint16_t variance;
    uint16_t sample_count;
    uint8_t  event_flags;
} cairn_imu_summary_t;

void cairn_encode_imu_summary(const cairn_imu_summary_t *s, uint8_t out[20]);

typedef struct {
    int16_t  speed_kph;
    int16_t  rpm;
    uint16_t fuel_pressure_kpa;
    uint8_t  throttle_pct;
    uint8_t  engine_load_pct;
    int8_t   coolant_temp_c;
    int8_t   intake_temp_c;
    int8_t   timing_advance_deg;
    uint8_t  pid_error_count;
    uint32_t pids_requested;
    uint32_t pids_answered;
    uint16_t poll_cadence_ms;
} cairn_obd_snapshot_t;

void cairn_encode_obd_snapshot(const cairn_obd_snapshot_t *s, uint8_t out[24]);

/*
 * OBD_EXTENDED (§4.10) — the boosted-engine and mixture signals, 24 bytes.
 *
 * A separate record rather than more fields on OBD_SNAPSHOT, which has only two
 * reserved bytes left. Splitting also means a car that answers the basic PIDs
 * but not these still produces clean OBD_SNAPSHOT records instead of a single
 * record half full of sentinels.
 *
 * Pressures are stored absolute, as the ECU reports them. Gauge boost is
 * map_kpa minus baro_kpa, converted at decode — storing gauge would bake a
 * barometric assumption permanently into the recorded data, and the point of
 * this format is that the raw measurement survives.
 */
typedef struct {
    uint16_t map_kpa;            /* intake manifold absolute, PID 0x0B */
    uint16_t maf_cgps;           /* mass air flow, 0.01 g/s, PID 0x10 */
    uint16_t lambda_e4;          /* equivalence ratio x10000, PID 0x44 */
    uint16_t abs_load_raw;       /* raw ((A*256)+B) for PID 0x43; pct = raw*100/255 */
    uint8_t  baro_kpa;           /* PID 0x33 */
    int8_t   ambient_temp_c;     /* PID 0x46 */
    int8_t   fuel_trim_short_pct;/* PID 0x06 */
    int8_t   fuel_trim_long_pct; /* PID 0x07 */
    uint32_t pids_requested;
    uint32_t pids_answered;
    uint16_t poll_cadence_ms;
    uint8_t  fuel_level_pct;     /* PID 0x2F, 0-100 %; 0xFF = absent */
    uint8_t  _reserved_ext;
} cairn_obd_extended_t;

void cairn_encode_obd_extended(const cairn_obd_extended_t *s, uint8_t out[24]);

typedef struct {
    uint16_t battery_mv;
    uint16_t sd_write_errors;
    uint16_t sd_free_mib;
    int8_t   device_temp_c;
    int8_t   rssi_dbm;
    uint16_t ext_sensor_1;
    uint16_t ext_sensor_2;
    uint8_t  health_state;
    uint8_t  reboot_count;
} cairn_device_health_t;

void cairn_encode_device_health(const cairn_device_health_t *s, uint8_t out[16]);

typedef struct {
    uint32_t duration_ms;
    uint16_t expected_samples;
    uint8_t  cause;
} cairn_gnss_gap_t;

#define CAIRN_GAP_NO_FIX         1
#define CAIRN_GAP_RECEIVER_RESET 2
#define CAIRN_GAP_POWERED_DOWN   3
#define CAIRN_GAP_OBSTRUCTION    4
#define CAIRN_GAP_TIME_JUMP      5

void cairn_encode_gnss_gap(const cairn_gnss_gap_t *s, uint8_t out[12]);

typedef struct {
    uint8_t  region;
    uint8_t  from_state;
    uint8_t  to_state;
    uint8_t  trigger_event;
    uint8_t  reason_code;
    uint8_t  policy_version;
    uint16_t start_score_e2;
    uint16_t stop_score_e2;
    uint32_t wake_cause;
} cairn_state_transition_t;

#define CAIRN_REGION_CAPTURE      1
#define CAIRN_REGION_BUNDLE       2
#define CAIRN_REGION_CONNECTIVITY 3
#define CAIRN_REGION_HEALTH       4

/*
 * States for the health region's power dimension (§4.7.1).
 *
 * Standby entry and exit are journalled as a matched pair so a reader can
 * recover the exact windows during which the device was asleep. Both frames
 * carry monotonic_ms, so the duration is exit − enter and needs no field of its
 * own; on this platform standby is light sleep and the millisecond clock runs
 * straight through it.
 *
 * That pair is what makes parked power consumption measurable at all. A voltage
 * series alone cannot distinguish a device that slept for six hours from one
 * that sat awake, and those differ by an order of magnitude in draw. With the
 * windows recorded, the supply voltage in DEVICE_HEALTH can be attributed to
 * whichever state the device was actually in.
 */
#define CAIRN_POWER_STATE_AWAKE   0
#define CAIRN_POWER_STATE_STANDBY 1

/* trigger_event for a health-region power transition. */
#define CAIRN_TRIGGER_POWER 4

void cairn_encode_state_transition(const cairn_state_transition_t *s, uint8_t out[20]);

/* ── TRIP_EVENT (§4.6) ────────────────────────────────────────────────────── */

#define CAIRN_EVENT_TRIP_START        1
#define CAIRN_EVENT_TRIP_END          2
#define CAIRN_EVENT_HARSH_BRAKE       3
#define CAIRN_EVENT_HARSH_ACCEL       4
#define CAIRN_EVENT_HARSH_CORNERING   5
#define CAIRN_EVENT_IMPACT            6
/*
 * Decisive dynamics the device could not attribute. Attribution needs either
 * the mounting orientation, which is unknown without calibration, or a speed
 * signal, which needs the ECU answering. With neither, the motion is still real
 * and still worth recording — guessing between brake and corner would produce a
 * label indistinguishable from a measured one.
 */
#define CAIRN_EVENT_HARSH_MOTION      7
#define CAIRN_EVENT_CAPTURE_RECOVERED 8

const char *cairn_event_type_name(uint8_t t);

#define CAIRN_MAX_EVENT_DETAIL 48

/*
 * Encode a trip event. `detail` may be NULL. Position is passed in rather than
 * read from anywhere, because an event's position must be the nearest *known*
 * fix — zero when there was none, never a stale one carried forward.
 */
cairn_err_t cairn_encode_trip_event(uint8_t event_type, int32_t lat_e7,
                                    int32_t lon_e7, const char *detail,
                                    uint8_t *out, size_t out_cap,
                                    size_t *written);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_FORMAT_H */
