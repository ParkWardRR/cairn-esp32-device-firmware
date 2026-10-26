/*
 * GNSS fix vetting: what the device writes as a position, and what it refuses.
 *
 * Two jobs, both portable C with the clock and the other sensors passed in, so the
 * rules are host-testable (test/host/gnss_test.c) and the Arduino glue in
 * src/sensors.cpp and src/sensor_task.cpp only feeds them.
 *
 * 1. The plausibility gate. The receiver is reached through the co-processor, which
 *    reports a position, a speed and a satellite count and nothing about whether the
 *    solution is any good. On the 2026-10-14 evening drive it reported 37 fixes, over
 *    47 s, tracing a 2.7 km run north at 33-49 m/s from where the car had just parked.
 *    The car was creeping and then idling: OBD read 0-3 kph and the accelerometer sat
 *    at 8-9 mg, its engine-idle floor. The fixes were internally consistent -- each
 *    position agreed with the speed before it -- so no check on the positions alone
 *    could see them, and they drew a spur on the map, a phantom leg and a 109 mph top
 *    speed. They also had half the satellites of a real fix (median 6 against 12).
 *
 *    A fix is refused when the car's own sensors or physics contradict it:
 *      - SETTLING  the first fix after more than CAIRN_GNSS_SETTLE_GAP_MS without an
 *                  accepted one needs at least CAIRN_GNSS_SETTLE_MIN_SATS satellites.
 *                  A receiver re-acquiring with four or five satellites is exactly
 *                  where the run above began.
 *      - FEW_SATS  any fix with fewer than CAIRN_GNSS_SETTLE_MIN_SATS satellites in use.
 *                  Added 2026-10-25: pulling into a garage, the M240i's receiver fell to
 *                  3-5 satellites and ran 8 -> 28 km/h, 70 m off, while OBD read 3-6
 *                  km/h -- not after a silence, so SETTLING never applied.
 *      - OBD       the fix claims more than a fresh
 *                  OBD reading (no older than CAIRN_GNSS_OBD_FRESH_MS) allows: the OBD
 *                  speed, plus what the car could have gained since at
 *                  CAIRN_GNSS_MAX_ACCEL_CMPS2, plus CAIRN_GNSS_OBD_MARGIN_CMPS. The wheel
 *                  speed sensors are the better witness, and the allowance keeps a hard
 *                  launch from a standstill from being refused.
 *      - STILL     the fix claims at least CAIRN_GNSS_MOVING_CMPS while the last IMU
 *                  window's RMS is under CAIRN_GNSS_STILL_MG. Measured on every stored
 *                  drive: driving at 8 m/s or more never read below 87 mg at the 2nd
 *                  percentile, idling reads 8-9.
 *      - ACCEL     the speed changed by more than CAIRN_GNSS_MAX_ACCEL_CMPS2 (just over
 *                  1 g) since the last accepted fix, no more than
 *                  CAIRN_GNSS_ACCEL_WINDOW_MS earlier. A road car cannot; a receiver
 *                  jumping between solutions can.
 *    The server applies the same physics afterwards (store/v1.6 v_position_trusted),
 *    with the advantage of seeing the seconds after a fix as well as before. This gate
 *    is the device refusing to write what it can already tell is wrong; the server
 *    remains the backstop for what only hindsight shows.
 *
 * 2. The 5 Hz NMEA path (ATGRR). The co-processor's raw NMEA buffer carries several
 *    fixes per poll. It used to be parsed for GGA only, every fix in a buffer was
 *    stamped with the moment of the poll, a sentence could be read twice across polls,
 *    and speed and heading -- which GGA does not carry -- were written as 0, which a
 *    reader cannot tell from a stationary car (the format has no unknown sentinel for
 *    speed). Now a fix is a GGA paired with the RMC of the same time of day, so speed
 *    and course are the receiver's own; each fix's age comes from its time of day; a
 *    sentence whose checksum fails, or that was already emitted, or a buffer whose
 *    newest fix is stale against the receiver's current time, is dropped.
 */
#ifndef CAIRN_GNSS_H
#define CAIRN_GNSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cairn_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAIRN_GNSS_MOVING_CMPS        800u    /* 8 m/s, 29 kph */
#define CAIRN_GNSS_STILL_MG           35u
#define CAIRN_GNSS_OBD_MARGIN_CMPS    300u    /* 3 m/s: OBD speed resolution and lag */
#define CAIRN_GNSS_OBD_FRESH_MS       2000u
#define CAIRN_GNSS_MAX_ACCEL_CMPS2    1000u   /* 10 m/s^2 */
#define CAIRN_GNSS_ACCEL_WINDOW_MS    2000u
#define CAIRN_GNSS_SETTLE_GAP_MS      30000u
#define CAIRN_GNSS_SETTLE_MIN_SATS    6u

typedef enum {
    CAIRN_GNSS_ACCEPT = 0,
    CAIRN_GNSS_REJECT_SETTLING,
    CAIRN_GNSS_REJECT_OBD,
    CAIRN_GNSS_REJECT_STILL,
    CAIRN_GNSS_REJECT_ACCEL,
    CAIRN_GNSS_REJECT_FEW_SATS,
    CAIRN_GNSS_VERDICTS
} cairn_gnss_verdict_t;

/* What the rest of the car says at the moment of the fix. */
typedef struct {
    bool     obd_valid;     /* an OBD speed reading exists */
    int16_t  obd_speed_kph;
    uint32_t obd_age_ms;    /* how long before the fix it was read */
    bool     imu_valid;     /* an IMU window has completed */
    uint16_t imu_rms_mg;    /* RMS of the last completed window, as IMU_SUMMARY reports it */
} cairn_gnss_context_t;

typedef struct {
    bool     have_accepted;
    uint32_t accepted_ms;        /* monotonic time of the last accepted fix */
    uint16_t accepted_speed_cmps;
    uint32_t rejected[CAIRN_GNSS_VERDICTS];
} cairn_gnss_gate_t;

void cairn_gnss_gate_reset(cairn_gnss_gate_t *g);

/* Judges one fix taken at monotonic time fix_ms; an accepted fix becomes the
 * reference for the next. ctx may be NULL when nothing else is known. */
cairn_gnss_verdict_t cairn_gnss_gate(cairn_gnss_gate_t *g, const cairn_gnss_sample_t *s,
                                     uint32_t fix_ms, const cairn_gnss_context_t *ctx);

const char *cairn_gnss_verdict_name(cairn_gnss_verdict_t v);

/* ── NMEA ─────────────────────────────────────────────────────────────────── */

#define CAIRN_NMEA_DAY_MS 86400000u
#define CAIRN_NMEA_NO_TOD 0xFFFFFFFFu
/* A buffer whose newest fix is older than this against the receiver's current time is
 * stale: the co-processor handed back something it read a while ago. */
#define CAIRN_NMEA_STALE_MS 2000u

typedef struct {
    cairn_gnss_sample_t sample;
    uint32_t tod_ms;   /* UTC time of day of the fix */
    uint32_t age_ms;   /* how long before the reference instant the fix was taken */
} cairn_nmea_fix_t;

typedef struct {
    uint32_t last_tod_ms;  /* newest time of day already emitted, or CAIRN_NMEA_NO_TOD */
    uint32_t bad_checksum;
    uint32_t unpaired;     /* GGA with no RMC of the same instant */
    uint32_t stale_buffers;
    uint32_t repeats;      /* already emitted in an earlier poll */
} cairn_nmea_state_t;

void cairn_nmea_reset(cairn_nmea_state_t *st);

/*
 * Extracts the fixes in an NMEA buffer, oldest first, at most max_out.
 *
 * ref_tod_ms is the receiver's current UTC time of day if known (from its last parsed
 * fix, advanced by the time since), else CAIRN_NMEA_NO_TOD, in which case the newest
 * fix in the buffer is taken as current. Each fix's age_ms is measured from that
 * reference, so the caller stamps it at (now - age_ms).
 */
int cairn_nmea_collect(const char *buf, size_t len, uint32_t ref_tod_ms,
                       cairn_nmea_state_t *st, cairn_nmea_fix_t *out, int max_out);

/* Parses the time-of-day field "hhmmss[.sss]" to milliseconds, or CAIRN_NMEA_NO_TOD. */
uint32_t cairn_nmea_parse_tod(const char *field);

/* (a - b) on a 24 h clock, taking the shorter way round: negative when a is earlier. */
int32_t cairn_nmea_tod_diff(uint32_t a, uint32_t b);

#ifdef __cplusplus
}
#endif

#endif
