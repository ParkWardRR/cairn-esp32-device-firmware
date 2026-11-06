/*
 * Storage retention: when the card runs low, make room — receipted bundles
 * first, oldest first.
 *
 * The owner's policy (2026-11-05), replacing "storage pressure never deletes":
 *
 *   1. Nothing is deleted while free space is at or above the low-water mark.
 *      A card that is merely filling is left alone.
 *   2. Under the low-water mark, bundles that hold a verified receipt are
 *      deleted oldest-first until free space reaches the high-water mark. A
 *      receipted bundle's bytes are safe elsewhere by definition — the receipt
 *      was verified against the pinned server key before it was stored
 *      (cairn_prune.h) — so this direction loses nothing.
 *   3. If every receipted bundle is gone and free space is still under the
 *      low-water mark, unreceipted sealed bundles are deleted oldest-first,
 *      one at a time, each logged through the drop hook BEFORE the delete.
 *      This is the owner's explicit trade: an old trip that never found a
 *      phone is worth less than the trip being driven right now. It is
 *      gated by `allow_unreceipted` so a build can still refuse.
 *
 * "Oldest" is the device counter, not the bundle id and not wall clock: ULIDs
 * built before the clock was set sort as 1970, and this repository has already
 * paid for trusting them once. The counter is monotonic by construction
 * (cairn_store.h) and is bound into every segment header.
 *
 * The open capture is never a candidate — the platform's list hook enumerates
 * sealed bundles only. Receipts are never deleted here; they are the evidence
 * and cost bytes, not megabytes.
 *
 * Portable C with injected I/O, like cairn_prune and for the same reason: a
 * wrong delete is the worst failure in the system, so the decision logic lives
 * where a host test can starve it, feed it unordered listings, and fail its
 * delete hook mid-run.
 */

#ifndef CAIRN_RETENTION_H
#define CAIRN_RETENTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One sealed bundle as the platform sees it on the card. */
typedef struct {
    char     id[27];      /* Crockford base32 ULID, NUL-terminated */
    uint64_t counter;     /* device counter from its manifest/header: the age */
    uint64_t bytes;       /* directory size on card */
    bool     receipted;   /* a verified receipt for it is stored on the card */
} cairn_retention_entry_t;

typedef struct {
    /* Free and total bytes of the filesystem holding the bundles. */
    uint64_t (*free_bytes)(void *ctx);

    /*
     * Enumerate sealed bundles into `out` (at most `max`), in any order.
     * Returns the count, or -1 if the card cannot be listed — in which case
     * nothing is deleted this round.
     */
    int (*list_sealed)(void *ctx, cairn_retention_entry_t *out, int max);

    /*
     * Delete one sealed bundle directory (its receipt, if any, stays).
     * Returning false counts as an error; the bundle is skipped, never
     * retried within the same round.
     */
    bool (*delete_bundle)(void *ctx, const cairn_retention_entry_t *e);

    /*
     * Called BEFORE each unreceipted delete, and after each receipted one.
     * This is the audit trail: on the device it appends to the retention log
     * and bumps the NVS counters DEVICE_INFO reports. Must not fail.
     */
    void (*log_drop)(void *ctx, const cairn_retention_entry_t *e);

    void *ctx;
} cairn_retention_io_t;

typedef struct {
    uint64_t low_water_bytes;   /* below this, deletion starts */
    uint64_t high_water_bytes;  /* deletion stops once free space reaches this */
    bool     allow_unreceipted; /* owner policy: true. false refuses step 3. */
} cairn_retention_policy_t;

typedef struct {
    uint32_t receipted_dropped;
    uint32_t unreceipted_dropped;
    uint32_t delete_errors;
    uint64_t bytes_freed;      /* sum of dropped entries' recorded sizes */
    bool     still_under_low;  /* true: the card is full and policy is exhausted */
} cairn_retention_stats_t;

/*
 * One retention round: measure, and delete per the policy above. Returns false
 * only when the card could not be listed or measured (stats then zeroed,
 * still_under_low false — an unreadable card is a storage fault, not license
 * to delete). Call when a write fails for space, before opening a bundle, and
 * on a periodic tick; it is cheap when free space is healthy.
 *
 * `scratch`/`scratch_cap`: caller-provided entry array (the device passes a
 * PSRAM buffer; tests pass a stack array). Bundles beyond scratch_cap are not
 * considered this round — fine, because the next round sees them.
 */
bool cairn_retention_run(const cairn_retention_io_t *io,
                         const cairn_retention_policy_t *policy,
                         cairn_retention_entry_t *scratch, int scratch_cap,
                         cairn_retention_stats_t *stats);

/*
 * The default policy for a card of `total_bytes`: low water is the larger of
 * 64 MiB and 5% of the card, high water is low + 64 MiB, unreceipted deletes
 * allowed. Stated here once so the device and the tests agree.
 */
void cairn_retention_default_policy(uint64_t total_bytes,
                                    cairn_retention_policy_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_RETENTION_H */
