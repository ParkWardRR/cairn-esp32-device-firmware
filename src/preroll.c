#include "preroll.h"

#include <string.h>

#include "cairn_log.h"

static const char *TAG = "PREROLL";

void cairn_preroll_reset(cairn_preroll_t *r)
{
    r->head = 0;
    r->count = 0;
    r->overwritten = 0;
}

bool cairn_preroll_push(cairn_preroll_t *r, uint8_t record_type,
                        uint8_t schema_version, uint16_t flags,
                        uint32_t monotonic_ms, const uint8_t *payload,
                        size_t payload_len)
{
    if (payload_len > CAIRN_PREROLL_MAX_PAYLOAD) {
        /*
         * Refuse rather than truncate. A short payload would decode as a
         * different observation, which is worse than not having it: the record
         * would look valid and be wrong.
         */
        CAIRN_LOGW(TAG, "record type 0x%02x has a %u byte payload, over the "
                        "%d byte ring limit; not buffered",
                   record_type, (unsigned)payload_len,
                   CAIRN_PREROLL_MAX_PAYLOAD);
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

uint16_t cairn_preroll_flush(cairn_preroll_t *r, cairn_capture_t *cap)
{
    if (r->count == 0) return 0;

    uint32_t oldest = 0, newest = 0;
    cairn_preroll_span(r, &oldest, &newest);

    uint16_t written = 0;
    uint16_t held = r->count;

    /*
     * Oldest first, so the frames enter the capture chain in the order they
     * were observed. The chain's sequence numbers are assigned on write, so
     * flushing out of order would make the bundle claim the drive happened
     * backwards.
     */
    for (uint16_t i = 0; i < held; i++) {
        const cairn_preroll_entry_t *e = &r->entries[oldest_index(r, i)];

        if (!cairn_capture_append(cap, CAIRN_CHAIN_CAPTURE, e->record_type,
                                  e->schema_version,
                                  (uint16_t)(e->flags | CAIRN_FLAG_PRETRIP),
                                  e->monotonic_ms, e->payload, e->payload_len)) {
            /*
             * Stop at the first failure. Continuing would leave a hole in the
             * middle of the pre-roll while still writing later frames, and a
             * gap the chain cannot see is exactly what this format exists to
             * prevent.
             */
            CAIRN_LOGE(TAG, "pre-roll flush failed after %u of %u frames",
                       (unsigned)written, (unsigned)held);
            break;
        }
        written++;
    }

    CAIRN_LOGI(TAG, "flushed %u of %u pre-trip frames spanning %u ms%s",
               (unsigned)written, (unsigned)held, (unsigned)(newest - oldest),
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
