#include "cairn_retention.h"

#include <string.h>

/*
 * Insertion sort by (counter, id). n is bounded by scratch_cap, which the
 * device sizes in the hundreds; this is not a hot path, and avoiding qsort
 * keeps the comparison logic readable next to the policy it serves.
 */
static void sort_oldest_first(cairn_retention_entry_t *e, int n)
{
    for (int i = 1; i < n; i++) {
        cairn_retention_entry_t k = e[i];
        int j = i - 1;
        while (j >= 0 && (e[j].counter > k.counter ||
                          (e[j].counter == k.counter &&
                           strcmp(e[j].id, k.id) > 0))) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = k;
    }
}

/*
 * Delete from `entries` those matching `want_receipted`, oldest first, until
 * free space (as re-measured through the hook after every delete) reaches
 * `stop_at`. Returns true when the target was reached.
 */
static bool drain(const cairn_retention_io_t *io,
                  cairn_retention_entry_t *entries, int n,
                  bool want_receipted, uint64_t stop_at,
                  cairn_retention_stats_t *stats)
{
    for (int i = 0; i < n; i++) {
        if (io->free_bytes(io->ctx) >= stop_at)
            return true;
        cairn_retention_entry_t *e = &entries[i];
        if (e->receipted != want_receipted)
            continue;

        /*
         * The unreceipted drop is logged before the delete: if power dies
         * between the two, the log claims a drop that did not happen, which
         * is the honest direction — a logged bundle that still exists is a
         * curiosity; a vanished bundle with no log line is a cover-up.
         */
        if (!want_receipted)
            io->log_drop(io->ctx, e);

        if (!io->delete_bundle(io->ctx, e)) {
            stats->delete_errors++;
            continue;
        }
        if (want_receipted) {
            stats->receipted_dropped++;
            io->log_drop(io->ctx, e);
        } else {
            stats->unreceipted_dropped++;
        }
        stats->bytes_freed += e->bytes;
    }
    return io->free_bytes(io->ctx) >= stop_at;
}

bool cairn_retention_run(const cairn_retention_io_t *io,
                         const cairn_retention_policy_t *policy,
                         cairn_retention_entry_t *scratch, int scratch_cap,
                         cairn_retention_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));

    uint64_t free_now = io->free_bytes(io->ctx);
    if (free_now >= policy->low_water_bytes)
        return true; /* healthy: rule 1, nothing is even listed */

    int n = io->list_sealed(io->ctx, scratch, scratch_cap);
    if (n < 0)
        return false; /* unreadable card: a fault, not license to delete */
    if (n > scratch_cap)
        n = scratch_cap;

    sort_oldest_first(scratch, n);

    /* Rule 2: receipted bundles, oldest first, up to the high-water mark. */
    if (drain(io, scratch, n, true, policy->high_water_bytes, stats))
        return true;

    /*
     * Rule 3: the receipted supply is exhausted and the card is still under
     * pressure. Unreceipted bundles go oldest-first — but only back up to the
     * LOW water mark, not the high one: each of these deletes destroys the
     * only copy, so the round takes the minimum that lets capture continue
     * and leaves the rest for a phone that might still appear.
     */
    if (policy->allow_unreceipted &&
        drain(io, scratch, n, false, policy->low_water_bytes, stats))
        return true;

    stats->still_under_low = io->free_bytes(io->ctx) < policy->low_water_bytes;
    return true;
}

void cairn_retention_default_policy(uint64_t total_bytes,
                                    cairn_retention_policy_t *out)
{
    const uint64_t MIB = 1024ull * 1024ull;
    uint64_t low = 64ull * MIB;
    uint64_t five_pct = total_bytes / 20ull;
    if (five_pct > low)
        low = five_pct;
    out->low_water_bytes = low;
    out->high_water_bytes = low + 64ull * MIB;
    out->allow_unreceipted = true;
}
