/*
 * On-card storage: sealed append, atomic seal, and boot recovery.
 *
 * Every frame written here is encrypted (bundle format v3): the card holds
 * ciphertext, and the keys live in on-chip NVS, because the card is never the
 * security boundary. Recovery does not need the key — torn tails, CRCs and the
 * chain are all checked over the stored bytes — so a device that has lost its
 * key still recovers and seals what is on the card rather than discarding it.
 *
 * Four invariants from docs/bundle-format-v3.md govern everything here:
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

/*
 * The storage identity: the root the segment keys derive from, the vehicle
 * assignment every segment and manifest is bound to, and the device's
 * monotonic bundle counter.
 *
 * An interface rather than globals so the store does not decide where any of
 * it lives. On the device cairn_storage_identity_load() backs it with NVS; a
 * host test can wrap the counter hooks to cut power between them and the
 * signature, which is the property that matters most here.
 *
 * The counter is per bundle and is written into every segment header, where
 * the AAD binds it into every frame — so it has to be fixed when the bundle's
 * first segment is created, not when the bundle is sealed. It is therefore
 * *reserved* (made durable) before the first header naming it is written, and
 * *committed* again before the manifest naming it is signed. Between the two,
 * no power cut can hand the same counter to different content: a reserved
 * counter whose bundle never seals is a hole the server reports, and a hole is
 * the harmless direction.
 */
typedef struct {
    uint8_t  root_key[CAIRN_ROOT_KEY_SIZE];
    uint32_t storage_key_version;

    /*
     * The assignment as provisioned. With none provisioned both are all-zero
     * and `assigned` is false: the device still captures and seals — data
     * first — but the server will refuse such a bundle until an assignment
     * exists, which is the correct direction for that failure.
     */
    uint8_t  vehicle_id[16];
    uint8_t  assignment_id[16];
    bool     assigned;

    /* Allocate the next counter, durably, before returning true. */
    bool (*counter_reserve)(void *ctx, uint64_t *out);
    /* Ensure `counter` is recorded as spent, durably, before returning true. */
    bool (*counter_commit)(void *ctx, uint64_t counter);
    void *counter_ctx;
} cairn_storage_identity_t;

typedef struct {
    bool     active;
    uint8_t  bundle_id[16];
    uint8_t  device_id[16];
    uint8_t  boot_id[16];
    char     dir[80];

    /*
     * This bundle's binding, as written into its segment headers. For a
     * resumed capture it is read back from those headers rather than taken from
     * the current identity: a manifest must agree with every header it covers
     * (§5.4), so a capture opened under one boot or assignment is sealed under
     * the same one even if the device has since rebooted or been reassigned.
     */
    uint8_t  vehicle_id[16];
    uint8_t  assignment_id[16];
    bool     assigned;
    uint32_t storage_key_version;
    uint64_t device_counter;

    /* Not owned. Supplies the counter hooks at seal. */
    const cairn_storage_identity_t *identity;

    /*
     * The ciphers of the two segments currently being appended to. False
     * `can_encrypt` means the device holds no key that matches this bundle —
     * a different storage_key_version, or a root that cannot authenticate what
     * is already on the card — so nothing more may be appended and the bundle
     * is sealed as it stands.
     */
    bool                   can_encrypt;
    cairn_segment_cipher_t segment_cipher;
    cairn_segment_cipher_t journal_cipher;

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

/*
 * Log the device id and public key in full, plus the exact command that enrols
 * them.
 *
 * The device generates its own signing key on first boot, so it cannot be
 * enrolled in advance — the server has to be told a key that does not exist
 * until the hardware has run once. Printing the full values here is what closes
 * that loop; a truncated prefix in a log line is not something an operator can
 * enrol.
 */
void cairn_identity_print_enrolment(const uint8_t device_id[16],
                                    const uint8_t pub[32]);

/* A fresh boot id from the hardware RNG. Ordering truth is per-boot. */
void cairn_new_boot_id(uint8_t boot_id[16]);

/*
 * The storage identity from NVS: K_root (generated from the hardware RNG on
 * first boot), its storage_key_version, the provisioned vehicle assignment, and
 * counter hooks backed by NVS. Logs loudly when no assignment is provisioned.
 */
bool cairn_storage_identity_load(cairn_storage_identity_t *out);

/*
 * Provision the vehicle assignment. Takes effect for the next bundle opened; an
 * open capture keeps the assignment its headers already carry.
 */
bool cairn_storage_set_assignment(const uint8_t vehicle_id[16],
                                  const uint8_t assignment_id[16]);

/*
 * Raise the device counter to at least `floor`. Never lowers it.
 *
 * Used when a device is re-enrolled: the server returns the highest counter it
 * has accepted from this device, and the next bundle must be numbered above it.
 * A floor below the current value is a no-op, because lowering a counter is
 * precisely the rollback the counter exists to defeat. Returns false only if the
 * value could not be made durable.
 */
bool cairn_storage_raise_counter(uint64_t floor);

/* The highest device counter ever reserved, 0 if none. For logs and tests. */
uint64_t cairn_storage_counter_high_water(void);

/*
 * Resume the open capture if one exists, else start a new one.
 *
 * Resuming runs the recovery scan over every existing segment, truncates a torn
 * tail to the last valid frame, and restores the chain state so the next append
 * continues the sequence. The discarded byte count is carried into the manifest.
 * The scan is structural, so it needs no key; the key is then used only to
 * confirm that the segments about to be extended authenticate under it.
 *
 * `identity` must outlive the capture.
 */
bool cairn_capture_open_or_resume(cairn_capture_t *cap,
                                  const uint8_t device_id[16],
                                  const uint8_t boot_id[16],
                                  const cairn_storage_identity_t *identity);

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
 *
 * The bundle's device counter is committed through the identity's hook before
 * the manifest is signed; if that fails the seal does not proceed, because a
 * signed manifest naming a counter the device could forget is how one counter
 * ends up on two different bundles.
 */
bool cairn_capture_seal(cairn_capture_t *cap, const uint8_t seed[32],
                        const uint8_t pub[32], const char *firmware_version,
                        uint8_t policy_version, uint32_t trip_seq,
                        uint8_t out_bundle_id[16]);

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
