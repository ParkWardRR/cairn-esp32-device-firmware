/*
 * LTE data accounting and limits, enforced on the dongle.
 *
 * Context: issue #21 (this repo) and the upstream contract issue
 * ParkWardRR/cairn-driving-log-selfhosted#27. The user sees consumption and sets
 * limits in the web UI and the iOS app; the dongle is the ENFORCEMENT POINT, so a
 * cap is a property of this code and not of a screen that might be out of date.
 *
 * *** PROVISIONAL. *** contracts/config/v1 is unreleased. Nothing here is a wire
 * format: the config and the report are C structs, the persisted image is private
 * to this device, and the authorisation of a config message is an input
 * (cairn_usage_auth_t) that the caller sets only after it has verified a signed
 * message. No config crypto is implemented here, and none is invented. The numeric
 * limits are starting points for the owner to set. Per security gate #18, nothing
 * secret is stored: this holds byte counts and limits, never an APN, SIM PIN, SSID
 * or key.
 *
 * What this module decides, in one place:
 *
 *   - counting: bytes up and down per path (LTE, Wi-Fi, BLE), per UTC day and per
 *     billing period (configurable billing day);
 *   - may LTE traffic start or continue (cairn_usage_gate), with a reason code for
 *     every refusal that goes into the reported state;
 *   - the caps: monthly, daily, per trip. Each is clamped by a COMPILED-IN ceiling.
 *     Only an authorised (signed) message can raise a runtime ceiling, and nothing
 *     can raise it past the compiled absolute maximum. An unsigned message can only
 *     make things stricter;
 *   - modes: LTE on/off, digests-only (default) or full bundles up to X bytes,
 *     roaming off by default, pause;
 *   - a stuck retry loop is impossible: persisted max attempts per trip, per-trip
 *     exponential backoff, and a circuit breaker that survives a reboot loop;
 *   - persistence that tolerates power loss with batched flash writes.
 *
 * The carrier's meter is the truth; this is a best-effort local count, built to
 * err on the side of OVER-counting (see the persistence notes in cairn_usage.c) so
 * that a power cut can make the dongle stop early, never run past a cap.
 *
 * Portable C, no Arduino, no allocation. Persistence goes through cairn_kv by
 * default, or through an injected store (the host tests inject torn writes).
 */

#ifndef CAIRN_USAGE_H
#define CAIRN_USAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── compiled-in limits (starting points: the owner decides the numbers) ──── */

/* The ceilings the runtime starts with. No config value, signed or not, exceeds the
 * runtime ceiling; the runtime ceiling starts here and only a signed message moves it. */
#define CAIRN_USAGE_CEIL_MONTHLY_BYTES (50u * 1024u * 1024u)
#define CAIRN_USAGE_CEIL_DAILY_BYTES   (10u * 1024u * 1024u)
#define CAIRN_USAGE_CEIL_TRIP_BYTES    (5u * 1024u * 1024u)
#define CAIRN_USAGE_CEIL_FULL_BUNDLE   (5u * 1024u * 1024u)

/* The most a signed message may ever raise a ceiling to. Beyond this needs a firmware build. */
#define CAIRN_USAGE_ABS_MONTHLY_BYTES  (1024u * 1024u * 1024u)
#define CAIRN_USAGE_ABS_DAILY_BYTES    (256u * 1024u * 1024u)
#define CAIRN_USAGE_ABS_TRIP_BYTES     (128u * 1024u * 1024u)
#define CAIRN_USAGE_ABS_FULL_BUNDLE    (128u * 1024u * 1024u)

/* Defaults when nothing has been configured. LTE is OFF until someone turns it on. */
#define CAIRN_USAGE_DEFAULT_MONTHLY_BYTES (5u * 1024u * 1024u)
#define CAIRN_USAGE_DEFAULT_DAILY_BYTES   (1024u * 1024u)
#define CAIRN_USAGE_DEFAULT_TRIP_BYTES    (256u * 1024u)
#define CAIRN_USAGE_DEFAULT_ALERT_PCT     80u

#define CAIRN_USAGE_TRIPS_TRACKED 8

typedef enum {
    CAIRN_PATH_LTE = 0,
    CAIRN_PATH_WIFI,
    CAIRN_PATH_BLE,
    CAIRN_PATH_COUNT
} cairn_usage_path_t;

typedef enum {
    CAIRN_TRAFFIC_DIGEST = 0,
    CAIRN_TRAFFIC_FULL_BUNDLE,
    CAIRN_TRAFFIC_CONTROL,      /* config/instruction sync: small, allowed in either mode */
    CAIRN_TRAFFIC_KINDS
} cairn_usage_traffic_t;

typedef enum {
    CAIRN_USAGE_MODE_DIGESTS_ONLY = 0,
    CAIRN_USAGE_MODE_FULL_UP_TO_X
} cairn_usage_mode_t;

/* Why traffic is stopped. CAIRN_STOP_NONE means it may proceed. Every refusal has
 * exactly one of these, and the reported state carries it. */
typedef enum {
    CAIRN_STOP_NONE = 0,
    CAIRN_STOP_STATE_LOST,       /* counters unreadable: fail closed until the next period */
    CAIRN_STOP_STORE_FAILED,     /* cannot persist the count: not sending what we cannot account for */
    CAIRN_STOP_DISABLED,
    CAIRN_STOP_PAUSED,
    CAIRN_STOP_ROAMING,
    CAIRN_STOP_MODE_DIGESTS_ONLY,/* a full bundle in digests-only mode */
    CAIRN_STOP_BUNDLE_TOO_LARGE, /* bigger than the "up to X" allowance */
    CAIRN_STOP_BUSY,             /* one attempt at a time */
    CAIRN_STOP_BREAKER_OPEN,
    CAIRN_STOP_TRIP_ATTEMPTS,    /* max attempts for this trip reached */
    CAIRN_STOP_BACKOFF,
    CAIRN_STOP_CAP_TRIP,
    CAIRN_STOP_CAP_DAILY,
    CAIRN_STOP_CAP_MONTHLY,
    CAIRN_STOP_REASONS
} cairn_usage_reason_t;

const char *cairn_usage_reason_name(cairn_usage_reason_t r);

/* ── configuration (what the contract will carry) ─────────────────────────── */

typedef struct {
    bool     lte_enabled;
    bool     roaming_allowed;
    bool     paused;
    uint8_t  mode;               /* cairn_usage_mode_t */
    uint8_t  billing_day;        /* 1..28: the day of the month a period starts */
    uint8_t  alert_pct;          /* 1..100 */
    uint32_t monthly_cap_bytes;
    uint32_t daily_cap_bytes;
    uint32_t trip_cap_bytes;
    uint32_t full_bundle_max_bytes;  /* the "X" of "full bundles up to X" */
} cairn_usage_config_t;

typedef struct {
    uint32_t monthly, daily, trip, full_bundle;
} cairn_usage_ceilings_t;

void cairn_usage_config_defaults(cairn_usage_config_t *c);
void cairn_usage_ceilings_defaults(cairn_usage_ceilings_t *c);

/*
 * Device-side policy that NO remote message can change (it is not in the config
 * contract). Tests shorten these; the firmware uses the defaults.
 */
typedef struct {
    uint32_t batch_bytes;        /* LTE bytes between flash writes */
    uint32_t batch_other_bytes;  /* the same for Wi-Fi/BLE (uncapped, so coarser) */
    uint16_t max_attempts_per_trip;    /* per traffic kind */
    uint16_t breaker_threshold;        /* consecutive failures that open it */
    uint32_t breaker_cooldown_s;       /* first cooldown; doubles each time it re-opens */
    uint32_t breaker_cooldown_max_s;
    uint32_t backoff_base_s;           /* after a failed attempt on a trip; doubles */
    uint32_t backoff_max_s;
} cairn_usage_tuning_t;

void cairn_usage_tuning_defaults(cairn_usage_tuning_t *t);

/*
 * Authorisation of a config message, decided by the CALLER. AUTH_SIGNED means the
 * caller verified a signature from an authorised party (enrolled client or server)
 * and its replay counter. This module does no cryptography: it models only what
 * authorisation unlocks, and it defaults to the safe reading (NONE).
 */
typedef enum {
    CAIRN_AUTH_NONE = 0,
    CAIRN_AUTH_SIGNED = 1
} cairn_usage_auth_t;

typedef enum {
    CAIRN_CFG_OK = 0,
    CAIRN_CFG_UNSIGNED_LOOSEN,   /* would loosen a limit; needs a signed message; nothing applied */
    CAIRN_CFG_ABOVE_CEILING,     /* a value exceeds the runtime ceiling; nothing applied */
    CAIRN_CFG_INVALID,           /* out of range / malformed field; nothing applied */
    CAIRN_CFG_STORE_FAILED       /* could not persist; nothing applied */
} cairn_usage_cfg_result_t;

const char *cairn_usage_cfg_result_name(cairn_usage_cfg_result_t r);

/* ── persistence seam ─────────────────────────────────────────────────────── */

typedef struct {
    /* false: key absent. Otherwise copies up to cap bytes and reports the stored length. */
    bool (*read)(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len);
    /* false: the write failed (or power was lost). Must replace the value whole. */
    bool (*write)(void *ctx, const char *key, const uint8_t *buf, size_t len);
    void *ctx;
} cairn_usage_store_t;

/* The default: cairn_kv (NVS on the device). The caller has run cairn_kv_begin(). */
const cairn_usage_store_t *cairn_usage_kv_store(void);

/* ── clock ────────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t utc_s;   /* 0 when unknown (no GNSS time yet): rollover is then skipped, never guessed */
    uint32_t mono_s;  /* seconds since boot; for backoff and breaker cooldowns */
} cairn_usage_clock_t;

/* ── state ────────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t key;                           /* 0 = unused slot */
    uint64_t bytes;                         /* LTE bytes spent on this trip, all attempts */
    uint16_t attempts[CAIRN_TRAFFIC_KINDS]; /* persisted: a reboot loop must not reset them */
    uint32_t next_ok_mono;                  /* RAM only: backoff */
    uint32_t stamp;                         /* RAM only: least-recently-used */
} cairn_usage_trip_t;

typedef enum {
    CAIRN_BREAKER_CLOSED = 0,
    CAIRN_BREAKER_OPEN,
    CAIRN_BREAKER_HALF_OPEN
} cairn_usage_breaker_t;

typedef enum {
    CAIRN_LOAD_FRESH = 0,        /* nothing stored: first boot */
    CAIRN_LOAD_OK,
    CAIRN_LOAD_RECOVERED,        /* newest copy unusable; older copy used, with extra margin */
    CAIRN_LOAD_LOST              /* stored but unreadable: counters unknown, LTE blocked */
} cairn_usage_load_t;

typedef struct {
    cairn_usage_store_t   store;
    cairn_usage_tuning_t  tune;
    cairn_usage_config_t  cfg;
    cairn_usage_ceilings_t ceil;

    uint64_t day_bytes[CAIRN_PATH_COUNT][2];     /* [path][0=up,1=down] */
    uint64_t period_bytes[CAIRN_PATH_COUNT][2];
    uint32_t day_index;      /* days since 1970-01-01 (UTC); 0 = not yet known */
    int32_t  period_id;      /* year*12 + month-index of the period start; INT32_MIN = unknown */

    cairn_usage_trip_t trips[CAIRN_USAGE_TRIPS_TRACKED];
    uint32_t trip_stamp;

    /* circuit breaker */
    uint16_t consec_failures;
    uint8_t  breaker_exp;
    bool     breaker_open;
    bool     breaker_deadline_valid;
    uint32_t breaker_open_until;
    bool     probe_inflight;

    /* the one attempt in flight */
    bool     inflight;
    int      inflight_trip;      /* index into trips, or -1 */
    cairn_usage_traffic_t inflight_kind;

    /* persistence */
    uint32_t seq;
    uint8_t  next_slot;
    bool     dirty_on_flash;     /* the newest image says "traffic may be unflushed" */
    bool     state_lost;
    uint64_t unflushed_lte;      /* LTE bytes counted since the last write */
    uint64_t unflushed_other;
    uint32_t flash_writes;       /* telemetry: flash wear is a thing we can see */
    cairn_usage_load_t loaded;

    /* not persisted */
    bool     sim_valid;
    uint64_t sim_up, sim_down;
} cairn_usage_t;

/* Load (or start) the counters. Applies the over-count margin for a dirty or torn
 * image, then persists once so the margin is not applied again by a reboot loop. */
cairn_usage_load_t cairn_usage_init(cairn_usage_t *u, const cairn_usage_store_t *store,
                                    const cairn_usage_tuning_t *tune,
                                    const cairn_usage_clock_t *now);

/* ── counting ─────────────────────────────────────────────────────────────── */

/*
 * Count bytes that crossed a path. Call per chunk at the socket/modem layer, and
 * keep a chunk no larger than tune.batch_bytes (the over-count margin assumes it).
 * Rolls the day and billing period over when `now` says so. LTE bytes are also
 * charged to the trip of the attempt in flight.
 */
void cairn_usage_record(cairn_usage_t *u, cairn_usage_path_t path, uint32_t up, uint32_t down,
                        const cairn_usage_clock_t *now);

/* Write everything now (shutdown, or after a config change). */
bool cairn_usage_flush(cairn_usage_t *u);

/* The modem's own SIM counters, when it exposes them. Reported alongside, never used to enforce. */
void cairn_usage_note_sim_counters(cairn_usage_t *u, uint64_t up, uint64_t down);

/* ── may traffic flow ─────────────────────────────────────────────────────── */

typedef struct {
    cairn_usage_traffic_t kind;
    uint64_t trip_key;       /* the trip this serves (e.g. the first 8 bytes of its content root); 0 for CONTROL */
    uint32_t bytes_wanted;   /* up + down, including protocol overhead; the whole bundle for FULL_BUNDLE */
    bool     roaming;        /* the modem's current registration */
    bool     continuing;     /* an attempt already admitted: skip attempt/backoff/breaker checks */
} cairn_usage_req_t;

/*
 * The decision. CAIRN_STOP_NONE means go. Side-effect free apart from rolling the
 * day/period over. Call it before a transfer (the attempt API below does) and
 * again before every chunk of a long one (continuing = true): crossing a cap in
 * the middle of a transfer stops the transfer.
 */
cairn_usage_reason_t cairn_usage_gate(cairn_usage_t *u, const cairn_usage_req_t *req,
                                      const cairn_usage_clock_t *now);

/*
 * Begin an attempt: gate it, count it against the trip's attempt budget, and
 * persist that BEFORE any byte moves (so a crash mid-transfer cannot erase the
 * attempt, and neither can a reboot loop). Returns CAIRN_STOP_NONE if admitted.
 */
cairn_usage_reason_t cairn_usage_attempt_begin(cairn_usage_t *u, const cairn_usage_req_t *req,
                                               const cairn_usage_clock_t *now);

/* End it. A failure feeds the breaker and the trip's backoff; a success closes the breaker. */
void cairn_usage_attempt_end(cairn_usage_t *u, bool success, const cairn_usage_clock_t *now);

/* ── configuration ────────────────────────────────────────────────────────── */

/*
 * Apply a new configuration, all or nothing. With CAIRN_AUTH_NONE only a change
 * that makes things stricter is accepted: LTE off, pause on, roaming off, caps
 * down, mode to digests-only. Anything that loosens a limit (including moving the
 * billing day, which would reset a period early) needs CAIRN_AUTH_SIGNED, and no
 * value above the runtime ceiling is ever accepted: raise the ceiling first.
 */
cairn_usage_cfg_result_t cairn_usage_apply_config(cairn_usage_t *u, const cairn_usage_config_t *c,
                                                  cairn_usage_auth_t auth,
                                                  const cairn_usage_clock_t *now);

/* Move the runtime ceilings. Signed only, and never past the compiled absolute maxima. */
cairn_usage_cfg_result_t cairn_usage_set_ceilings(cairn_usage_t *u, const cairn_usage_ceilings_t *c,
                                                  cairn_usage_auth_t auth);

/* Clear STATE_LOST and start the counters from zero. Signed only (it lifts a fail-closed stop). */
bool cairn_usage_clear_state_lost(cairn_usage_t *u, cairn_usage_auth_t auth);

/* ── report ───────────────────────────────────────────────────────────────── */

#define CAIRN_ALERT_MONTHLY_THRESHOLD 0x01u  /* at or past alert_pct of the monthly cap */
#define CAIRN_ALERT_MONTHLY_REACHED   0x02u
#define CAIRN_ALERT_DAILY_THRESHOLD   0x04u
#define CAIRN_ALERT_DAILY_REACHED     0x08u

typedef struct {
    cairn_usage_reason_t reason;       /* why LTE is stopped right now, NONE if it is not */
    bool     lte_enabled, paused, roaming_allowed;
    uint8_t  mode, billing_day, alert_pct;

    uint64_t day_up[CAIRN_PATH_COUNT], day_down[CAIRN_PATH_COUNT];
    uint64_t period_up[CAIRN_PATH_COUNT], period_down[CAIRN_PATH_COUNT];
    uint32_t day_index;
    int32_t  period_id;

    /* the caps actually enforced: min(config, runtime ceiling) */
    uint32_t monthly_cap, daily_cap, trip_cap, full_bundle_max;
    uint32_t monthly_pct, daily_pct;   /* LTE used / cap, whole percent, not clamped at 100 */
    uint32_t alerts;                   /* CAIRN_ALERT_* */

    cairn_usage_breaker_t breaker;
    uint16_t consecutive_failures;
    bool     state_lost;
    uint32_t flash_writes;
    cairn_usage_load_t loaded;

    bool     sim_valid;
    uint64_t sim_up, sim_down;
} cairn_usage_report_t;

void cairn_usage_report(cairn_usage_t *u, const cairn_usage_clock_t *now, bool roaming,
                        cairn_usage_report_t *out);

/* Introspection for tests and tooling. */
size_t cairn_usage_image_len(void);
uint32_t cairn_usage_effective_cap(const cairn_usage_t *u, uint32_t configured, uint32_t ceiling);
cairn_usage_breaker_t cairn_usage_breaker_state(cairn_usage_t *u, const cairn_usage_clock_t *now);

/* Calendar helpers, exposed so the billing-period maths is testable on its own. */
int32_t cairn_usage_period_id(uint64_t utc_s, uint8_t billing_day);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_USAGE_H */
