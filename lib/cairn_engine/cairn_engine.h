/*
 * Per-engine profiles, compiled in.
 *
 * Engine- and vehicle-specific behaviour (which PIDs to ask for and how to read
 * them, how often, what supply voltage means "engine running", how long the bus
 * takes to sleep) lives in engines/<id>.yaml, one file per engine. tools/enginegen
 * validates the chosen files and emits the tables this module serves, so a build
 * carries all engines, or only the ones asked for, without forking the firmware.
 *
 * DRAFT AND PROVISIONAL: the schema is cairn.engine/v1-draft and should move into
 * contracts/engine/v1 once released. See engines/SPEC.draft.md.
 *
 * Three things are decided here, and each is a plain function so the host tests
 * reach it:
 *
 *   1. Which profile is active (cairn_engine_active, cairn_engine_select).
 *   2. What the active profile says, with the firmware's own default substituted
 *      for anything the profile marks `unknown`, and a record of which was which
 *      (cairn_engine_params). An unknown is never silently filled with another
 *      engine's number; it falls back to the device default that existed before
 *      profiles did, and the caller can ask which values came from the profile.
 *   3. Whether this build may serve a vehicle (cairn_engine_check_vehicle). A unit
 *      built for one engine refuses a vehicle positively identified as needing
 *      another, rather than recording it with the wrong PID table.
 */

#ifndef CAIRN_ENGINE_H
#define CAIRN_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cairn_expr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The capture-record fields a PID can feed. Order matches tools/enginegen
 * (profile.rs, Field::ALL); 0 is "none". */
typedef enum {
    CAIRN_FIELD_NONE = 0,
    CAIRN_FIELD_RPM,
    CAIRN_FIELD_SPEED_KPH,
    CAIRN_FIELD_THROTTLE_PCT,
    CAIRN_FIELD_TIMING_ADVANCE_DEG,
    CAIRN_FIELD_MAP_KPA,
    CAIRN_FIELD_LAMBDA_E4,
    CAIRN_FIELD_ENGINE_LOAD_PCT,
    CAIRN_FIELD_COOLANT_TEMP_C,
    CAIRN_FIELD_INTAKE_TEMP_C,
    CAIRN_FIELD_MAF_CGPS,
    CAIRN_FIELD_AMBIENT_TEMP_C,
    CAIRN_FIELD_FUEL_TRIM_SHORT_PCT,
    CAIRN_FIELD_FUEL_TRIM_LONG_PCT,
    CAIRN_FIELD_BARO_KPA,
    CAIRN_FIELD_ABS_LOAD_RAW,
    CAIRN_FIELD_FUEL_LEVEL_PCT,
    CAIRN_FIELD_COUNT
} cairn_field_t;

#define CAIRN_TIER_HOT  0 /* in the multi-PID batch, every cycle */
#define CAIRN_TIER_COLD 1 /* one request, on its turn in the cold rotation */

#define CAIRN_ENGINE_STUB     0 /* identity only; claims nothing */
#define CAIRN_ENGINE_DERIVED  1 /* extracted from behaviour the firmware already had */
#define CAIRN_ENGINE_VERIFIED 2 /* checked against raw replies from the real car */

/* Which values a profile states (the rest are `unknown`). */
#define CAIRN_K_PIDS                (1u << 0)
#define CAIRN_K_OBD_PERIOD          (1u << 1)
#define CAIRN_K_OBD_BATCH_PERIOD    (1u << 2)
#define CAIRN_K_COLD_SLOTS          (1u << 3)
#define CAIRN_K_ENGINE_ON_MV        (1u << 4)
#define CAIRN_K_STANDBY_IDLE        (1u << 5)
#define CAIRN_K_STANDBY_HEARTBEAT   (1u << 6)
#define CAIRN_K_DRIVE_VOLTAGE_DWELL (1u << 7)
#define CAIRN_K_DRIVE_MOTION_DWELL  (1u << 8)
#define CAIRN_K_DRIVE_BOTH_DWELL    (1u << 9)
#define CAIRN_K_BUS_SLEEP_PHASE     (1u << 10)
#define CAIRN_K_SUPPLY_RANGE        (1u << 11)

typedef struct {
    uint16_t    pid;
    uint8_t     service;   /* 0x01, or 0x22 */
    uint8_t     nbytes;    /* data bytes in the reply: A, B, C, D */
    uint8_t     tier;
    uint8_t     cold_slot; /* meaningful for CAIRN_TIER_COLD */
    uint8_t     field;     /* cairn_field_t */
    uint8_t     log;       /* record it */
    int8_t      scale_e;   /* the stored integer times 10^scale_e is the physical value */
    int32_t     range_min; /* proven bounds of the formula's result */
    int32_t     range_max;
    uint16_t    code_off;  /* into the profile's code blob */
    uint8_t     code_len;
    const char *name;
    const char *unit;
} cairn_pid_t;

typedef struct {
    uint8_t     pid;
    uint8_t     std_bytes;
    const char *name;
} cairn_probe_t;

typedef struct {
    const char *id;
    const char *name;
    uint16_t    version;
    uint8_t     status;
    uint8_t     sha256[32]; /* of engines/<id>.yaml, CRLF normalised to LF */
    uint32_t    known;      /* CAIRN_K_* */

    /* Hot PIDs first, in batch-request order, then cold PIDs by slot. */
    const cairn_pid_t *pids;
    uint8_t            n_pids;
    uint8_t            n_hot;
    const uint8_t     *code;

    uint8_t  cold_slots;
    uint16_t obd_period_ms;
    uint16_t obd_batch_period_ms;

    uint16_t engine_on_mv;
    uint32_t standby_idle_ms;
    uint32_t standby_heartbeat_ms;
    uint32_t drive_voltage_dwell_ms;
    uint32_t drive_motion_dwell_ms;
    uint32_t drive_both_dwell_ms;
    uint32_t bus_first_sleep_phase_ms; /* documented, not acted on */

    uint16_t supply_min_mv;
    uint16_t supply_max_mv;

    const cairn_probe_t *probes;
    uint8_t              n_probes;
} cairn_engine_profile_t;

/* Every engine the repository knows, installed or not. */
typedef struct {
    const char        *id;
    const char *const *vin_patterns; /* 17 characters, '?' matches any */
    uint8_t            n_vin_patterns;
    const char *const *engine_codes; /* VIN positions 4-8, e.g. "A5C5" for N20 */
    uint8_t            n_engine_codes;
    uint8_t            installed;
} cairn_engine_catalogue_entry_t;

/* What this image was built from, for the log and for the app to read. */
typedef struct {
    const char *selection;     /* "bmw-b58,bmw-n20" */
    uint8_t     count;
    uint8_t     build_sha256[32];
    const char *string;        /* "engines=bmw-n20@1/<hash16> build=<hash16>" */
} cairn_engine_identity_t;

const cairn_engine_identity_t *cairn_engine_identity(void);

size_t                        cairn_engine_installed_count(void);
const cairn_engine_profile_t *cairn_engine_installed(size_t i);
const cairn_engine_profile_t *cairn_engine_find(const char *id); /* installed only */

/* The full catalogue (installed and not-installed engines). */
const cairn_engine_catalogue_entry_t *cairn_engine_catalogue(void);
size_t                                cairn_engine_catalogue_count(void);

/* ── the active profile ───────────────────────────────────────────────────── */

/*
 * The first installed profile that is not a stub, else the first installed. With
 * several engines installed and nothing known about the vehicle this is a default,
 * not a guess about the car: select() overrides it once the vehicle is identified.
 */
const cairn_engine_profile_t *cairn_engine_active(void);
bool cairn_engine_select(const char *id); /* false (no change) when not installed */
void cairn_engine_select_default(void);

/* The active profile has a PID table the firmware can act on. */
bool cairn_engine_has_pids(void);

/* ── resolved values ──────────────────────────────────────────────────────── */

typedef struct {
    uint16_t obd_period_ms;
    uint16_t obd_batch_period_ms;
    uint8_t  cold_slots; /* never 0, so `% cold_slots` is safe */
    uint16_t engine_on_mv;
    uint32_t standby_idle_ms;
    uint32_t standby_heartbeat_ms;
    uint32_t drive_voltage_dwell_ms;
    uint32_t drive_motion_dwell_ms;
    uint32_t drive_both_dwell_ms;
    uint32_t from_profile; /* CAIRN_K_* bits that came from the profile; the rest
                              are the firmware's device defaults (config.h) */
} cairn_engine_params_t;

const cairn_engine_params_t *cairn_engine_params(void);

/* ── PID access ───────────────────────────────────────────────────────────── */

const cairn_pid_t *cairn_engine_pid_for_field(const cairn_engine_profile_t *p,
                                              cairn_field_t f);

/* Evaluate a PID's formula over its reply bytes A..D. */
cairn_expr_status_t cairn_engine_eval_pid(const cairn_engine_profile_t *p,
                                          const cairn_pid_t *pid,
                                          const uint8_t data[4], int32_t *out);

/*
 * The multi-PID Mode 01 batch: one request carrying the profile's hot PIDs, one reply.
 * The portable halves live here so the host tests can hold them to the behaviour the
 * firmware had before profiles; the serial exchange and the text-to-bytes step stay in
 * src/sensors.cpp.
 *
 * Request: "01" then each hot PID as two upper-case hex digits, then '\r'. Returns the
 * length written (not counting the NUL it also writes), or 0 when the profile has no
 * hot PIDs or `cap` is too small.
 */
size_t cairn_engine_batch_request(const cairn_engine_profile_t *p, char *buf,
                                  size_t cap);

/*
 * Parse the reply bytes that follow the "41" service echo: a sequence of
 * (PID byte, its data bytes) pairs. Fills values[field] for every field the reply
 * actually carries and sets bit `field` of *present for each.
 *
 * A partial or reordered reply is accepted — five of six PIDs is five good
 * simultaneous measurements, and `present` tells the caller exactly which. Parsing
 * stops at the first pair that cannot be read safely: a PID the profile does not
 * list as hot (its length is unknown, so the next pair cannot be located) or one
 * whose data bytes are truncated. Returns false only when nothing was parsed.
 */
/* Bitmask of every field the profile's hot PIDs cover, so a caller can tell a
 * complete batch from a partial one. */
uint32_t cairn_engine_hot_field_mask(const cairn_engine_profile_t *p);

bool cairn_engine_batch_parse(const cairn_engine_profile_t *p, const uint8_t *bytes,
                              size_t n, int32_t values[CAIRN_FIELD_COUNT],
                              uint32_t *present);

/* ── vehicle gate ─────────────────────────────────────────────────────────── */

typedef enum {
    /* Nothing says which engine this is: serve it with the active profile. */
    CAIRN_VEHICLE_UNIDENTIFIED = 0,
    /* Identified, and that engine is installed. */
    CAIRN_VEHICLE_SERVED,
    /* Identified as an engine this repository knows but this build left out. */
    CAIRN_VEHICLE_REFUSED_NOT_INSTALLED,
    /* Declared as an engine nobody has a profile for. */
    CAIRN_VEHICLE_REFUSED_UNKNOWN_ENGINE
} cairn_vehicle_verdict_t;

const char *cairn_vehicle_verdict_name(cairn_vehicle_verdict_t v);

/* True when `vin` is 17 characters and every non-'?' pattern character matches. */
bool cairn_vin_matches(const char *pattern, const char *vin);

/*
 * Decide whether to serve a vehicle. `declared_id` (an engine id the unit was told
 * about, may be NULL) wins over `vin` (may be NULL or empty). The identified engine
 * id, when there is one, is returned through `engine_id`.
 *
 * A VIN that matches two different engines' patterns is treated as unidentified: a
 * contradiction is not evidence.
 */
cairn_vehicle_verdict_t cairn_engine_check_vehicle(const char *declared_id,
                                                   const char *vin,
                                                   const char **engine_id);

/* The same over an explicit catalogue, so tests need not depend on what is shipped. */
cairn_vehicle_verdict_t cairn_engine_check_vehicle_in(
    const cairn_engine_catalogue_entry_t *cat, size_t n, const char *declared_id,
    const char *vin, const char **engine_id);

/* ── VIN engine code extraction ──────────────────────────────────────────── */

/*
 * Extract the engine code substring from a VIN. BMW encodes the engine variant
 * in VIN positions 4-8 (WMI + VDS, 0-indexed characters 3-7). The caller
 * provides the start position and length; `out` is always NUL-terminated.
 * Returns false when the VIN is too short or the position is out of range.
 */
bool cairn_vin_extract_code(const char *vin, uint8_t pos, uint8_t len,
                            char *out, size_t out_cap);

/*
 * Match a VIN's engine code substring against the catalogue's engine_codes
 * entries. Returns the first matching engine, or NULL. An engine_codes entry
 * of "A5C5" matches VIN position 4-8 == "A5C5".
 */
const cairn_engine_catalogue_entry_t *cairn_engine_match_code(
    const cairn_engine_catalogue_entry_t *cat, size_t n, const char *vin);

const cairn_engine_catalogue_entry_t *cairn_engine_match_code_in(
    const cairn_engine_catalogue_entry_t *cat, size_t n,
    const char *code, uint8_t code_len);

/* ── BLE companion engine declaration ────────────────────────────────────── */

/*
 * Set the BLE-declared engine id. The companion app writes this when the user
 * selects a vehicle. Returns true if the engine id is known in the catalogue.
 * Pass NULL or empty to clear.
 */
bool cairn_engine_set_ble_declaration(const char *engine_id);
const char *cairn_engine_get_ble_declaration(void);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_ENGINE_H */
