#include "preroll.h"

#include <string.h>

#include "cairn_log.h"

static const char *TAG = "PREROLL";

void cairn_preroll_reset(cairn_preroll_t *r)
{
    r->head = 0;
    r->count = 0;
    r->overwritten = 0;
    r->pushes = 0;
    r->large_head = 0;
    r->large_count = 0;
}

static bool push_large(cairn_preroll_t *r, uint8_t record_type, uint8_t schema_version,
                       uint16_t flags, uint32_t monotonic_ms, const uint8_t *payload,
                       size_t payload_len)
{
    cairn_preroll_large_t *e = &r->large[r->large_head];

    e->record_type    = record_type;
    e->schema_version = schema_version;
    e->flags          = flags;
    e->monotonic_ms   = monotonic_ms;
    e->after_pushes   = r->pushes;
    e->payload_len    = (uint8_t)payload_len;
    memcpy(e->payload, payload, payload_len);

    r->large_head = (uint8_t)((r->large_head + 1) % CAIRN_PREROLL_LARGE_SLOTS);
    if (r->large_count < CAIRN_PREROLL_LARGE_SLOTS) {
        r->large_count++;
    } else {
        r->overwritten++;
    }
    return true;
}

bool cairn_preroll_push(cairn_preroll_t *r, uint8_t record_type,
                        uint8_t schema_version, uint16_t flags,
                        uint32_t monotonic_ms, const uint8_t *payload,
                        size_t payload_len)
{
    if (payload_len > CAIRN_PREROLL_MAX_PAYLOAD &&
        payload_len <= CAIRN_PREROLL_LARGE_MAX_PAYLOAD) {
        return push_large(r, record_type, schema_version, flags, monotonic_ms,
                          payload, payload_len);
    }

    if (payload_len > CAIRN_PREROLL_LARGE_MAX_PAYLOAD) {
        /*
         * Refuse rather than truncate. A short payload would decode as a
         * different observation, which is worse than not having it: the record
         * would look valid and be wrong.
         */
        CAIRN_LOGW(TAG, "record type 0x%02x has a %u byte payload, over the "
                        "%d byte ring limit; not buffered",
                   record_type, (unsigned)payload_len,
                   CAIRN_PREROLL_LARGE_MAX_PAYLOAD);
        return false;
    }

    cairn_preroll_entry_t *e = &r->entries[r->head];

    e->record_type    = record_type;
    e->schema_version = schema_version;
    e->flags          = flags;
    e->monotonic_ms   = monotonic_ms;
    e->payload_len    = (uint8_t)payload_len;
    memcpy(e->payload, payload, payload_len);

    r->head = (uint16_t)((r->head + 1) % CAIRN_PREROLL_RING_SAMPLES);
    r->pushes++;

    if (r->count < CAIRN_PREROLL_RING_SAMPLES) {
        r->count++;
    } else {
        /* Wrapped: the oldest entry was just overwritten. Counted so the loss
         * is visible rather than silent. */
        r->overwritten++;
    }

    return true;
}

/* Index of the i-th oldest entry currently held. */
static uint16_t oldest_index(const cairn_preroll_t *r, uint16_t i)
{
    uint16_t start = (uint16_t)((r->head + CAIRN_PREROLL_RING_SAMPLES - r->count) %
                               CAIRN_PREROLL_RING_SAMPLES);
    return (uint16_t)((start + i) % CAIRN_PREROLL_RING_SAMPLES);
}

bool cairn_preroll_span(const cairn_preroll_t *r, uint32_t *oldest_ms,
                        uint32_t *newest_ms)
{
    if (r->count == 0) return false;

    *oldest_ms = r->entries[oldest_index(r, 0)].monotonic_ms;
    *newest_ms = r->entries[oldest_index(r, (uint16_t)(r->count - 1))].monotonic_ms;
    return true;
}

/* Index of the i-th oldest large record held. */
static uint8_t oldest_large(const cairn_preroll_t *r, uint8_t i)
{
    uint8_t start = (uint8_t)((r->large_head + CAIRN_PREROLL_LARGE_SLOTS - r->large_count) %
                              CAIRN_PREROLL_LARGE_SLOTS);
    return (uint8_t)((start + i) % CAIRN_PREROLL_LARGE_SLOTS);
}

static bool flush_one(cairn_capture_t *cap, uint8_t record_type, uint8_t schema_version,
                      uint16_t flags, uint32_t monotonic_ms, const uint8_t *payload,
                      size_t payload_len)
{
    return cairn_capture_append(cap, CAIRN_CHAIN_CAPTURE, record_type, schema_version,
                                (uint16_t)(flags | CAIRN_FLAG_PRETRIP), monotonic_ms,
                                payload, payload_len);
}

uint16_t cairn_preroll_flush(cairn_preroll_t *r, cairn_capture_t *cap)
{
    if (r->count == 0 && r->large_count == 0) return 0;

    uint32_t oldest = 0, newest = 0;
    cairn_preroll_span(r, &oldest, &newest);

    uint16_t written = 0;
    uint16_t held = r->count;
    uint16_t total = (uint16_t)(held + r->large_count);
    bool     failed = false;

    /* Ring position of the oldest record still held, in pushes since reset. */
    const uint32_t first_held = r->pushes - held;
    uint8_t li = 0;

    /*
     * A large record older than everything left in the ring is dropped with what it
     * sat beside: the ring keeps the newest window, and writing a scan whose
     * neighbours were evicted would claim a stretch of pre-roll that is not there.
     */
    while (li < r->large_count &&
           r->large[oldest_large(r, li)].after_pushes < first_held) {
        r->overwritten++;
        total--;
        li++;
    }

    /*
     * Oldest first, so the frames enter the capture chain in the order they
     * were observed. The chain's sequence numbers are assigned on write, so
     * flushing out of order would make the bundle claim the drive happened
     * backwards. A large record goes in after the ring records pushed before it.
     */
    for (uint16_t i = 0; i <= held && !failed; i++) {
        while (li < r->large_count &&
               r->large[oldest_large(r, li)].after_pushes <= first_held + i) {
            const cairn_preroll_large_t *l = &r->large[oldest_large(r, li)];
            if (!flush_one(cap, l->record_type, l->schema_version, l->flags,
                           l->monotonic_ms, l->payload, l->payload_len)) {
                failed = true;
                break;
            }
            written++;
            li++;
        }
        if (failed || i == held) break;

        const cairn_preroll_entry_t *e = &r->entries[oldest_index(r, i)];
        if (!flush_one(cap, e->record_type, e->schema_version, e->flags,
                       e->monotonic_ms, e->payload, e->payload_len)) {
            failed = true;
            break;
        }
        written++;
    }

    if (failed) {
        /*
         * Stop at the first failure. Continuing would leave a hole in the
         * middle of the pre-roll while still writing later frames, and a
         * gap the chain cannot see is exactly what this format exists to
         * prevent.
         */
        CAIRN_LOGE(TAG, "pre-roll flush failed after %u of %u frames",
                   (unsigned)written, (unsigned)total);
    }

    CAIRN_LOGI(TAG, "flushed %u of %u pre-trip frames spanning %u ms%s",
               (unsigned)written, (unsigned)total, (unsigned)(newest - oldest),
               (r->overwritten > 0) ? " (ring wrapped; older samples dropped)"
                                    : "");

    if (r->overwritten > 0) {
        CAIRN_LOGW(TAG, "%u pre-trip samples were dropped before confirmation; "
                        "the ring holds %d records",
                   (unsigned)r->overwritten, CAIRN_PREROLL_RING_SAMPLES);
    }

    cairn_preroll_reset(r);
    return written;
}
