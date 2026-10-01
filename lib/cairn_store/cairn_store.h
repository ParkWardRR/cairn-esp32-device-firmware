/*
 * On-card storage: framed append, atomic seal, and boot recovery.
 *
 * Four invariants from docs/bundle-format-v2.md govern everything here:
 *
 *   1. A sealed bundle is never mutated. Sealing moves a directory; it never
 *      rewrites one in place.
 *   2. No byte is deleted without a locally verified signed receipt. This
 *      module therefore never deletes bundle data at all — see cairn_sync.
 *   3. Ordering truth is (boot_id, seq), never wall-clock UTC. UTC is recorded
 *      as an annotation with its own accuracy and may jump; seq may not.
 *   4. Honest incompleteness beats fabricated continuity. A torn tail is
 *      reported with an exact discarded byte count rather than patched over.
 *
 * Layout under CAIRN_DIR_CAPTURE/<bundle_id>/ while open:
 *
 *   seg-00000000.seg   capture frames; one chain shared across all seg-* files
 *   seg-00000001.seg   ... rotated at CAIRN_SEGMENT_MAX_BYTES
 *   journal.seg        state transitions and health, on its own chain (§3.2.1)
 *
 * Sealing adds manifest.cbor and manifest.sig, then moves the directory to
 * CAIRN_DIR_BUNDLES/<bundle_id>/.
 */

#ifndef CAIRN_STORE_H
#define CAIRN_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cairn_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 1 MiB segments. Large enough that rotation is rare, small enough that a
 * single unreadable region costs a bounded amount of a trip — the recovery scan
 * stops at the first invalid frame within a segment, so segment size is the
 * upper bound on what one torn tail can cost.
 */
#define CAIRN_SEGMENT_MAX_BYTES (1024u * 1024u)

/* Frames are staged here before hitting the card. One frame at a time: the
 * format is designed so a partially written frame is detectable, not avoided. */
#define CAIRN_STAGE_BYTES CAIRN_MAX_FRAME_LEN

/*
 * Durability cadence. Flushing every frame would triple the write load and
 * still not make power loss safe, because the card itself buffers. Instead the
 * format tolerates a torn tail exactly, and these bounds cap how much a pulled
 * fuse can cost.
 */
#define CAIRN_FLUSH_EVERY_FRAMES 32
#define CAIRN_FLUSH_EVERY_MS     5000

typedef enum {
    CAIRN_CHAIN_CAPTURE = 0,
    CAIRN_CHAIN_JOURNAL = 1,
} cairn_chain_id_t;

/* One chain's continuity state: the next sequence number and the CRC the next
 * frame must cite as prev_crc32. */
typedef struct {
    uint32_t next_seq;
    uint32_t prev_crc32;
} cairn_chain_t;

typedef struct {
    bool     active;
    uint8_t  bundle_id[16];
    uint8_t  device_id[16];
    uint8_t  boot_id[16];
    char     dir[80];

    /* Capture segments share one chain across rotations; journal.seg has its
     * own. Mixing them would make a journal write appear to be a gap in the
     * capture sequence. */
    cairn_chain_t capture_chain;
    cairn_chain_t journal_chain;

    uint32_t segment_index;
    uint32_t segment_bytes;
    uint32_t segment_first_seq;

    uint32_t journal_bytes;

    /* Tallies that become manifest fields. */
    uint32_t record_counts[256];
    uint32_t first_seq;
    uint32_t last_seq;
    bool     have_any_frame;

    uint64_t capture_started_monotonic_us;
    uint64_t capture_ended_monotonic_us;

    uint64_t utc_basis_ms;
    uint32_t utc_basis_acc_ms;

    /* Recovery outcome, reported in the manifest rather than hidden. */
    uint8_t  recovery_state;
    uint32_t discarded_tail_bytes;

    /*
     * Set when the capture cannot be safely extended — the member budget is
     * exhausted, or a segment could not be made appendable. The lifecycle seals
     * at the next opportunity; appends stop rather than fabricate continuity.
     */
    bool     needs_seal;

    uint32_t frames_since_flush;
    uint32_t last_flush_ms;

    uint32_t write_errors;
} cairn_capture_t;

/* Create the directory tree. Call once after the card mounts. */
bool cairn_store_init(void);

/*
 * A bundle id as 26 Crockford base32 characters — the directory name on the
 * card. This is the operational handle, not the bundle's identity: identity is
 * content_root, so a ULID collision would be an inconvenience rather than a
 * correctness failure.
 */
void cairn_ulid_encode(const uint8_t id[16], char out[27]);
bool cairn_ulid_decode(const char *s, uint8_t out[16]);

/*
 * Identity, persisted in NVS so it survives reboots and firmware updates.
 * device_id is derived from the efuse MAC on first boot; the signing seed is
 * generated once from the hardware RNG.
 */
bool cairn_identity_load(uint8_t device_id[16], uint8_t seed[32],
                         uint8_t pub[32], uint32_t *boot_count);

/* A fresh boot id from the hardware RNG. Ordering truth is per-boot. */
void cairn_new_boot_id(uint8_t boot_id[16]);

/*
 * Resume the open capture if one exists, else start a new one.
 *
 * Resuming runs the recovery scan over every existing segment, truncates a torn
 * tail to the last valid frame, and restores the chain state so the next append
 * continues the sequence. The discarded byte count is carried into the manifest.
 */
bool cairn_capture_open_or_resume(cairn_capture_t *cap,
                                  const uint8_t device_id[16],
                                  const uint8_t boot_id[16]);

/* Append one record. Rotates the segment when full. */
bool cairn_capture_append(cairn_capture_t *cap, cairn_chain_id_t chain,
                          uint8_t record_type, uint8_t schema_version,
                          uint16_t flags, uint32_t monotonic_ms,
                          const uint8_t *payload, size_t payload_len);

/* Record the UTC basis for the bundle: a single annotation with its own
 * accuracy, rather than a timestamp per frame that might disagree. */
void cairn_capture_set_utc_basis(cairn_capture_t *cap, uint64_t utc_ms,
                                 uint32_t acc_ms);

/* Push buffered frames to the card. Called on the cadence above and before any
 * state change that must survive power loss. */
bool cairn_capture_flush(cairn_capture_t *cap);
void cairn_capture_tick(cairn_capture_t *cap);

/*
 * Seal: hash the members, build and sign the manifest, write it into the
 * capture directory, then move the directory to CAIRN_DIR_BUNDLES.
 *
 * Crash-safe by construction rather than by assuming the move is atomic. The
 * manifest is written before the move, so an interrupted seal leaves either a
 * capture directory that already contains a valid manifest — finished by
 * cairn_store_resume_interrupted_seals() — or a completed bundle. Neither state
 * loses data, and neither produces a bundle without a manifest.
 */
bool cairn_capture_seal(cairn_capture_t *cap, const uint8_t seed[32],
                        const uint8_t pub[32], const char *firmware_version,
                        uint8_t policy_version, uint8_t out_bundle_id[16]);

/*
 * Finish any seal interrupted by power loss. Idempotent: run at every boot
 * before opening a capture.
 */
int cairn_store_resume_interrupted_seals(void);

/* Count sealed bundles awaiting a receipt, and their total bytes. */
bool cairn_store_pending_stats(uint32_t *bundle_count, uint64_t *total_bytes);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_STORE_H */
