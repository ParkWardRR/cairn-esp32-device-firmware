/*
 * The pre-trip ring.
 *
 * A trip is only declared after motion has persisted for CAIRN_START_DWELL_MS,
 * which is what stops a door slam or a passing truck from starting one. But the
 * dwell is also the first few seconds of the drive — pulling out of a parking
 * space, the first turn — and writing nothing during it would lose exactly the
 * part of a journey that is hardest to reconstruct later.
 *
 * So samples are captured continuously and held here, then flushed to the
 * bundle when a trip is confirmed. They are written with CAIRN_FLAG_PRETRIP,
 * because they are real observations that were recorded *before* the device had
 * decided a trip was underway, and a reader is entitled to know the difference.
 *
 * If motion does not persist, the ring is simply dropped. Nothing is written,
 * so a parked car does not accumulate bundles.
 */

#ifndef CAIRN_PREROLL_H
#define CAIRN_PREROLL_H

#include <stdbool.h>
#include <stdint.h>

#include "cairn_store.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One buffered record. Payloads are bounded by the largest fixed-size record
 * the firmware emits, which keeps the ring a flat array — no allocation in the
 * capture path, where a failed allocation would mean losing the data it was
 * meant to protect.
 */
#define CAIRN_PREROLL_MAX_PAYLOAD 32

typedef struct {
    uint8_t  record_type;
    uint8_t  schema_version;
    uint16_t flags;
    uint32_t monotonic_ms;
    uint8_t  payload[CAIRN_PREROLL_MAX_PAYLOAD];
    uint8_t  payload_len;
} cairn_preroll_entry_t;

typedef struct {
    cairn_preroll_entry_t entries[CAIRN_PREROLL_RING_SAMPLES];
    uint16_t head;      /* next slot to write */
    uint16_t count;     /* entries currently held, <= capacity */
    uint32_t overwritten; /* dropped because the ring wrapped */
} cairn_preroll_t;

void cairn_preroll_reset(cairn_preroll_t *r);

/*
 * Hold a record. Oldest-first eviction once full: if the ring wraps, the most
 * recent window is the one worth keeping, since it is adjacent to the trip that
 * is about to be confirmed.
 */
bool cairn_preroll_push(cairn_preroll_t *r, uint8_t record_type,
                        uint8_t schema_version, uint16_t flags,
                        uint32_t monotonic_ms, const uint8_t *payload,
                        size_t payload_len);

/*
 * Write everything held to the capture chain in capture order, each frame
 * carrying CAIRN_FLAG_PRETRIP in addition to the flags it was recorded with.
 * Returns the number of frames written and empties the ring.
 */
uint16_t cairn_preroll_flush(cairn_preroll_t *r, cairn_capture_t *cap);

/* Oldest and newest monotonic timestamps held, for logging the span recovered. */
bool cairn_preroll_span(const cairn_preroll_t *r, uint32_t *oldest_ms,
                        uint32_t *newest_ms);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_PREROLL_H */
