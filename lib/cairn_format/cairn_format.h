/*
 * Cairn bundle format v2 — C implementation.
 *
 * The normative specification is docs/bundle-format-v2.md. This is the third
 * implementation of it, after Go (server/format) and Rust (emulator/src/format),
 * and it is the one that matters most: a disagreement here means a device
 * writing bundles the server cannot recover.
 *
 * Deliberately portable C11 with no ESP-IDF dependency, so exactly this code
 * can be compiled natively and checked against the committed conformance
 * vectors in fixtures/format-v2/. Verifying firmware correctness without
 * hardware is otherwise impossible, and "it compiled" is not the same as "it
 * agrees with the other two implementations".
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

#define CAIRN_FORMAT_VERSION    2
#define CAIRN_MANIFEST_VERSION  2
#define CAIRN_RECEIPT_VERSION   2

#define CAIRN_SIGALG_ED25519 "ed25519"

/* ── geometry ─────────────────────────────────────────────────────────────── */

#define CAIRN_SEGMENT_HEADER_SIZE 64
#define CAIRN_FRAME_HEADER_SIZE   24
#define CAIRN_FRAME_TRAILER_SIZE  4
#define CAIRN_FRAME_OVERHEAD      (CAIRN_FRAME_HEADER_SIZE + CAIRN_FRAME_TRAILER_SIZE) /* 28 */
#define CAIRN_MIN_FRAME_LEN       CAIRN_FRAME_OVERHEAD
#define CAIRN_MAX_FRAME_LEN       4096
#define CAIRN_MAX_PAYLOAD_SIZE    (CAIRN_MAX_FRAME_LEN - CAIRN_FRAME_OVERHEAD)         /* 4068 */

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
} cairn_record_type_t;

bool cairn_record_type_known(uint8_t t);
const char *cairn_record_type_name(uint8_t t);

/* ── frame flags ──────────────────────────────────────────────────────────── */

#define CAIRN_FLAG_PRETRIP       (1u << 0)
#define CAIRN_FLAG_DEGRADED      (1u << 1)
#define CAIRN_FLAG_ESTIMATED_UTC (1u << 2)
#define CAIRN_FLAG_POST_RECOVERY (1u << 3)

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
#define CAIRN_MAX_CHUNKS       64

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

typedef struct {
    uint16_t format_version;
    uint8_t  device_id[16];
    uint8_t  boot_id[16];
    uint32_t segment_index;
    uint32_t first_seq;
    uint64_t opened_monotonic_us;
} cairn_segment_header_t;

/* Encode a 64-byte header into `out`. */
cairn_err_t cairn_encode_segment_header(const cairn_segment_header_t *h,
                                        uint8_t *out, size_t out_cap);

/* Decode and verify. `header_len` receives where frame scanning begins. */
cairn_err_t cairn_parse_segment_header(const uint8_t *buf, size_t len,
                                       cairn_segment_header_t *out,
                                       size_t *header_len);

/* ── frames ───────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  record_type;
    uint8_t  schema_version;
    uint16_t flags;
    uint32_t seq;
    uint32_t monotonic_ms;
    uint32_t prev_crc32;

    const uint8_t *payload;
    size_t         payload_len;

    uint32_t crc32; /* computed on encode, read on scan */
} cairn_frame_t;

/*
 * Encode a frame into `out`, returning its length in `written` and its CRC in
 * `crc_out` — which is the next frame's prev_crc32.
 */
cairn_err_t cairn_encode_frame(uint8_t record_type, uint8_t schema_version,
                               uint16_t flags, uint32_t seq,
                               uint32_t monotonic_ms, uint32_t prev_crc32,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t *out, size_t out_cap,
                               size_t *written, uint32_t *crc_out);

/* ── the recovery scan ────────────────────────────────────────────────────── */

typedef enum {
    CAIRN_STOP_EOF = 0,
    CAIRN_STOP_TORN_TAIL,
    CAIRN_STOP_CORRUPT_FRAME,
    CAIRN_STOP_CHAIN_BREAK,
    CAIRN_STOP_SEQ_GAP,
} cairn_stop_reason_t;

const char *cairn_stop_reason_name(cairn_stop_reason_t r);

/* Continuity across a segment boundary within one chain. */
typedef struct {
    uint32_t expected_seq;
    uint32_t expected_prev;
} cairn_scan_state_t;

typedef struct {
    cairn_segment_header_t header;

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
 * Walk a segment's frames per the specification's recovery algorithm.
 *
 * Stop-at-first-invalid: every frame before the failure point is valid and
 * retained; everything from there to end-of-segment is reported as a discarded
 * tail, with the exact byte count and the reason. A header error is returned as
 * an error — the segment is unusable, but the caller must not delete it.
 */
cairn_err_t cairn_scan_segment(const uint8_t *buf, size_t len,
                               cairn_scan_state_t state,
                               cairn_scan_result_t *out,
                               cairn_frame_cb cb, void *user);

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
 * Not reentrant: the one-frame staging buffer is static, to keep a 4 KiB
 * allocation off a FreeRTOS task stack. Recovery runs once at boot on a single
 * task, which is the only caller that matters.
 */
cairn_err_t cairn_scan_segment_stream(cairn_read_fn read, void *read_user,
                                      uint64_t len,
                                      cairn_scan_state_t state,
                                      cairn_scan_result_t *out,
                                      cairn_frame_cb cb, void *user);

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

void cairn_encode_state_transition(const cairn_state_transition_t *s, uint8_t out[20]);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_FORMAT_H */
