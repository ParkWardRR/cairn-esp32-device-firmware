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

/*
 * The one variable-length record the firmware buffers is WIFI_SCAN (format v3 section
 * 4.13), up to 132 bytes. Widening every slot to fit it would quadruple the ring for a
 * record written every ten seconds at most, so it gets a few slots of its own beside
 * the ring instead, and the flush merges them back into capture order.
 *
 * It has to be buffered at all because the scans that matter most happen exactly here:
 * pulling out of a garage with a cold receiver is a pretrip, and before 2026-10-25 a
 * scan of more than three access points would have been refused by the 32-byte limit.
 */
#define CAIRN_PREROLL_LARGE_MAX_PAYLOAD 132
#define CAIRN_PREROLL_LARGE_SLOTS       2

typedef struct {
    uint8_t  record_type;
    uint8_t  schema_version;
    uint16_t flags;
    uint32_t monotonic_ms;
    uint8_t  payload[CAIRN_PREROLL_MAX_PAYLOAD];
    uint8_t  payload_len;
} cairn_preroll_entry_t;

typedef struct {
    uint8_t  record_type;
    uint8_t  schema_version;
    uint16_t flags;
    uint32_t monotonic_ms;
    /* How many ring records had been pushed before this one: its place in capture
     * order, which survives the ring wrapping underneath it. */
    uint32_t after_pushes;
    uint8_t  payload[CAIRN_PREROLL_LARGE_MAX_PAYLOAD];
    uint8_t  payload_len;
} cairn_preroll_large_t;

typedef struct {
    cairn_preroll_entry_t entries[CAIRN_PREROLL_RING_SAMPLES];
    uint16_t head;      /* next slot to write */
    uint16_t count;     /* entries currently held, <= capacity */
    uint32_t overwritten; /* dropped because the ring wrapped */
    uint32_t pushes;    /* ring records ever pushed since the last reset */

    cairn_preroll_large_t large[CAIRN_PREROLL_LARGE_SLOTS];
    uint8_t  large_head;
    uint8_t  large_count;
} cairn_preroll_t;

void cairn_preroll_reset(cairn_preroll_t *r);

/*
 * Hold a record. Oldest-first eviction once full: if the ring wraps, the most
 * recent window is the one worth keeping, since it is adjacent to the trip that
 * is about to be confirmed. A payload over CAIRN_PREROLL_MAX_PAYLOAD and up to
 * CAIRN_PREROLL_LARGE_MAX_PAYLOAD goes to the large slots, newest kept; anything
 * larger is refused.
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
