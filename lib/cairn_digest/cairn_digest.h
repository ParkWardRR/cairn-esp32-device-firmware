/*
 * The LTE digest generator: a streaming trip reduction with a hard byte budget.
 *
 * Context: issue #20 (this repo) and the upstream contract issue
 * ParkWardRR/cairn-driving-log-selfhosted#26. The owner's requirement is that LTE
 * carries a very small, separate artifact about one trip, never the full bundle.
 *
 * *** PROVISIONAL. *** contracts/digest/v1 is not released. Nothing here is a final
 * wire format: the byte layout in cairn_digest_wire.c is a placeholder (its first
 * byte is a DRAFT marker and it will be replaced by the contract's), the numeric
 * limits are starting points, and no contract vector has been checked. What IS
 * meant to survive the contract is the shape of the work, which is format
 * independent and lives in this header:
 *
 *   1. Route reduction. Fixes stream in; the reducer keeps at most K points
 *      (Visvalingam-style: when full, drop the interior point whose removal
 *      changes the path least, measured as its perpendicular deviation from the
 *      segment between its neighbours, which is the same quantity Ramer-Douglas-
 *      Peucker thresholds on). RAM is fixed by the context size, there is no
 *      allocation, and a long trip costs time but not memory.
 *   2. Summary statistics, as accumulators: distance, duration, max speed,
 *      moving time, per-channel count/min/max/mean (boost, trim, lambda...),
 *      health flags, a short list of notable events.
 *   3. A hard byte budget. The encoder is a bounded writer; when the result would
 *      exceed the budget it reduces the route further (then drops the event list)
 *      rather than exceed it. The budget a caller asks for is clamped to a
 *      compiled-in ceiling that no configuration value can exceed.
 *
 * Compress-before-encrypt is deliberately NOT here. The plaintext digest bytes are
 * produced into a caller buffer; compressing them, AEAD-encrypting under the
 * digest's own domain-separated key, and signing are the sealing step, left as the
 * documented hook cairn_digest_seal(). There is no default: without a sealer the
 * digest is not shippable. The plaintext contains locations, so this library never
 * writes it to flash or a card (see security gate #18): the buffer is the caller's
 * RAM and stays there until the hook has consumed it.
 *
 * Portable C, no Arduino, no allocation.
 */

#ifndef CAIRN_DIGEST_H
#define CAIRN_DIGEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── limits: compiled in, none of them a config value ─────────────────────── */

/* No digest is ever longer than this, whatever the config asks for. */
#define CAIRN_DIGEST_BYTES_HARD_CEILING 4096u
/*
 * The smallest budget honoured. Below this the fixed part of a worst-case digest
 * (identity, statistics, health, a full event list, two route points) might not
 * fit, and a digest with no route is not worth sending. A request under the floor
 * is raised to it, not rejected: a bad config must not silently disable the
 * feature or, worse, the budget.
 */
#define CAIRN_DIGEST_BYTES_FLOOR        384u
#define CAIRN_DIGEST_BYTES_DEFAULT      2048u

#define CAIRN_DIGEST_POINTS_HARD_CEILING 192u
#define CAIRN_DIGEST_POINTS_DEFAULT      96u

#define CAIRN_DIGEST_NCHAN       4  /* summarised channels */
#define CAIRN_DIGEST_MAX_EVENTS  8  /* notable events kept (the rest are counted) */

/* The first two channels' meaning is a convention for now; the engine profile
 * issue decides which are meaningful. Units are the caller's, kept as integers. */
#define CAIRN_DIGEST_CH_BOOST   0
#define CAIRN_DIGEST_CH_TRIM    1
#define CAIRN_DIGEST_CH_LAMBDA  2
#define CAIRN_DIGEST_CH_SPARE   3

#define CAIRN_DIGEST_SPEED_UNKNOWN 0xFFFFu

/* ── configuration ────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t byte_budget;   /* clamped to [FLOOR, HARD_CEILING] */
    uint32_t max_points;    /* clamped to [2, POINTS_HARD_CEILING] */
    uint32_t min_spacing_m; /* streaming pre-filter: fixes closer than this to the
                               last kept point are not candidates (0: default) */
    bool     route_times;   /* carry a per-point time offset (a few bytes each) */
} cairn_digest_config_t;

void cairn_digest_config_defaults(cairn_digest_config_t *c);

/* The budget that will really be enforced for this config. */
uint32_t cairn_digest_effective_budget(const cairn_digest_config_t *c);
uint32_t cairn_digest_effective_points(const cairn_digest_config_t *c);

/* ── input ────────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t t_ms;            /* any clock monotonic over the trip (UTC when known) */
    bool     has_fix;
    int32_t  lat_e7;          /* degrees * 1e7 */
    int32_t  lon_e7;
    uint16_t speed_cmps;      /* cm/s, or CAIRN_DIGEST_SPEED_UNKNOWN */
    uint8_t  ch_mask;         /* bit i: ch[i] is valid in this sample */
    int32_t  ch[CAIRN_DIGEST_NCHAN];
} cairn_digest_sample_t;

/* What the digest says about itself; supplied at finish, once the trip is sealed. */
typedef struct {
    uint8_t  trip_root[32];     /* content root of the bundle this summarises */
    uint8_t  vehicle_id[16];
    uint8_t  assignment_id[16];
    uint16_t engine_profile_id;
    uint16_t engine_profile_version;
} cairn_digest_meta_t;

/* ── state ────────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t first_t_ms, last_t_ms;
    bool     started;
    uint32_t samples;
    uint32_t fixes_used;
    uint32_t fixes_rejected;    /* out of range, or implausibly far for the time */
    uint64_t distance_mm;       /* stationary jitter filtered out */
    uint32_t max_speed_cmps;
    uint32_t moving_time_ms;
    uint32_t health_flags;
    uint32_t events_dropped;    /* events beyond CAIRN_DIGEST_MAX_EVENTS */
    struct {
        uint32_t count;
        int32_t  min, max;
        int64_t  sum;
    } ch[CAIRN_DIGEST_NCHAN];
} cairn_digest_stats_t;

typedef struct {
    int32_t  lat_e7, lon_e7;
    uint32_t t_s;               /* seconds since the first sample */
    uint32_t importance;        /* deviation if removed; UINT32_MAX = pinned */
    uint16_t prev, next;
    bool     used;
} cairn_digest_pt_t;

typedef struct {
    uint8_t  type;
    uint32_t t_s;
} cairn_digest_event_t;

/*
 * The whole working state. Fixed size, caller-allocated: put it in static storage,
 * not on a task stack (about 6 KiB at the compiled ceilings). Nothing else is
 * ever allocated.
 */
typedef struct {
    cairn_digest_config_t cfg;
    cairn_digest_stats_t  st;

    cairn_digest_pt_t pt[CAIRN_DIGEST_POINTS_HARD_CEILING + 2];
    uint16_t head, tail;        /* list ends; 0xFFFF when empty */
    uint32_t count;
    uint32_t cap;               /* effective max_points */
    uint32_t spacing_units;     /* min spacing, in scaled 1e-7 degree units */

    int32_t  cos_q15;           /* cos(latitude of the first fix), Q15 */
    bool     have_anchor, have_prev, have_last;
    uint8_t  reject_run;               /* consecutive implausible fixes */
    int32_t  anchor_lat, anchor_lon;   /* distance jitter filter */
    int32_t  prev_lat, prev_lon;       /* last accepted fix, for plausibility */
    uint64_t prev_t_ms;
    int32_t  last_lat, last_lon;       /* newest accepted fix (route tail) */
    uint32_t last_t_s;

    cairn_digest_event_t ev[CAIRN_DIGEST_MAX_EVENTS];
    uint8_t  ev_count;

    bool     route_reduced_by_budget;
    bool     events_dropped_by_budget;
    bool     finished;
} cairn_digest_t;

typedef enum {
    CAIRN_DIGEST_OK = 0,
    CAIRN_DIGEST_BAD_ARG,
    CAIRN_DIGEST_FINISHED,          /* add() after finish() */
    CAIRN_DIGEST_BUDGET_TOO_SMALL,  /* unreachable at or above the floor; defensive */
    CAIRN_DIGEST_BUFFER_TOO_SMALL,  /* caller buffer is under the effective budget */
    CAIRN_DIGEST_SEAL_UNAVAILABLE,  /* no sealer: the digest must not be shipped */
    CAIRN_DIGEST_SEAL_FAILED,
} cairn_digest_status_t;

const char *cairn_digest_status_name(cairn_digest_status_t s);

/* ── streaming API ────────────────────────────────────────────────────────── */

void cairn_digest_init(cairn_digest_t *d, const cairn_digest_config_t *cfg);

/* Feed one sample. O(K) worst case, no allocation. */
cairn_digest_status_t cairn_digest_add(cairn_digest_t *d, const cairn_digest_sample_t *s);

void cairn_digest_note_health(cairn_digest_t *d, uint32_t flag_bits);

/* A notable event (type is the caller's code). Returns false when the list is full;
 * the loss is counted and reported, never silent. */
bool cairn_digest_note_event(cairn_digest_t *d, uint8_t type, uint64_t t_ms);

const cairn_digest_stats_t *cairn_digest_stats(const cairn_digest_t *d);
uint32_t cairn_digest_route_points(const cairn_digest_t *d);

/*
 * Produce the PLAINTEXT digest bytes. The result never exceeds the effective
 * budget (which never exceeds CAIRN_DIGEST_BYTES_HARD_CEILING): if the first
 * encoding is too large the route is reduced further, then the event list is
 * dropped, and the encoding retried. `out_cap` must be at least the effective
 * budget. Ends the stream; calling it again re-encodes the same reduced state.
 */
cairn_digest_status_t cairn_digest_finish(cairn_digest_t *d, const cairn_digest_meta_t *meta,
                                          uint8_t *out, size_t out_cap, size_t *out_len);

/* The reduced route, for tests and for a future second encoding. */
size_t cairn_digest_route(const cairn_digest_t *d, int32_t *lat_e7, int32_t *lon_e7,
                          size_t cap);

/* ── the sealing hook (NOT IMPLEMENTED: documented seam) ──────────────────── */

/*
 * The later step, per contracts/digest/v1: compress the plaintext, AEAD-encrypt it
 * under a key derived from the escrowed storage root with the digest's own
 * domain-separation label, sign it with the device key. Compress FIRST: ciphertext
 * does not compress. The sealer owns the result; this library only hands it the
 * plaintext and guarantees nothing else has been written anywhere.
 *
 * With no sealer, cairn_digest_seal returns CAIRN_DIGEST_SEAL_UNAVAILABLE and the
 * caller has nothing it may send.
 */
typedef bool (*cairn_digest_seal_fn)(void *ctx, const uint8_t *plain, size_t len);

cairn_digest_status_t cairn_digest_seal(const uint8_t *plain, size_t len,
                                        cairn_digest_seal_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_DIGEST_H */
