/*
 * Sensor access, producing format v2 payload structures.
 *
 * The drivers are the vendored FreematicsPlus library, reused rather than
 * rewritten: this repository's history already contains one hand-rolled HAL
 * that was broken and replaced by the vendored one. The job here is only to
 * translate driver output into the specification's units and sentinels.
 *
 * Every "unavailable" case is represented explicitly. A decoder must be able to
 * tell "the ECU reported zero" from "the ECU did not answer", and must never be
 * shown a precision the receiver did not claim.
 */

#ifndef CAIRN_SENSORS_H
#define CAIRN_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

#include "cairn_format.h"

struct SensorStatus {
    bool coprocessor;
    bool obd;
    bool imu;
    bool gnss;

    uint8_t device_type;
    char    vin[18];
    int     dtc_count;
};

bool sensors_begin(SensorStatus *status);

/* Re-attempt the subsystems that failed. OBD is only retried when bus_open is
 * true, because s_obd.init() blocks ~5 s when the ECU does not answer and
 * stalling the sensor task while parked gains nothing. */
void sensors_retry_failed(SensorStatus *status, bool bus_open);

/*
 * Fill a GNSS sample. Returns false when the receiver has produced nothing new,
 * which is a gap to be recorded rather than a sample to be invented.
 */
bool sensors_read_gnss(cairn_gnss_sample_t *out, uint32_t *fix_age_ms);

/* True once the receiver has delivered a date and time it considers valid. */
bool sensors_gnss_utc(uint64_t *utc_ms, uint32_t *acc_ms);

/*
 * Accumulate IMU samples continuously, then summarize a window. Peak values
 * would be meaningless if the sensor were polled only once per window.
 */
void sensors_imu_accumulate(void);
bool sensors_imu_summarize(uint32_t window_ms, cairn_imu_summary_t *out);

/* The most recent accelerometer RMS, in milli-g, for motion scoring. */
uint16_t sensors_recent_accel_rms_mg(void);

/*
 * One instantaneous bias-corrected accelerometer magnitude, in milli-g.
 *
 * For the standby loop, which runs on the controller's task while the sensing
 * task is paused. It reads the sensor once and touches none of the windowed
 * accumulator state, which is the point: calling sensors_imu_accumulate() from
 * two tasks raced on that shared window, and a torn count/sum_sq pair makes
 * sqrt(sum_sq/count) explode. The symptom was a device on a desk waking from
 * standby every fifteen seconds reporting MOTION, so standby achieved about
 * five per cent duty instead of near-continuous sleep.
 *
 * A single sample is noisier than an RMS window, which does not matter at a
 * 120 mg threshold when a stationary board reads two or three. The vendor
 * firmware polls the sensor directly here for the same reason.
 */
uint16_t sensors_accel_magnitude_mg(void);

/*
 * How recently a GNSS reading arrived, for telling two failures apart.
 *
 * A satellite count of zero is ambiguous on a bench: an unconnected antenna and
 * a receiver indoors with no sky view both report zero, and the driver exposes
 * only satellites *used in a fix* rather than satellites in view with their
 * signal strengths. Reading freshness separates "nothing is reaching us" from
 * "readings arrive and contain no fix", which is the distinction that matters
 * and which the satellite count cannot make.
 *
 * `age_ms` is UINT32_MAX when no reading has ever arrived.
 *
 * This replaced an attempt to use the driver's NMEA sentence counters, which
 * was a mistake worth recording. gpsBegin() sets FLAG_GNSS_USE_LINK, so on this
 * board GNSS arrives through the co-processor via ATGPS, and gps.stats() is
 * only called on the direct-UART branch — the counters are therefore
 * structurally always zero here. They read 0/0 whether the receiver was
 * perfect or absent, which looked like hard evidence of a dead module and was
 * no evidence at all. gpsData.ts is stamped on both transports.
 *
 * Returns false when GNSS is not initialised.
 */
bool sensors_gnss_freshness(uint32_t *age_ms, uint8_t *sats);

/*
 * Multi-PID batch result.
 *
 * When the ECU supports it, a single Mode 01 request carries all six hot PIDs
 * and comes back in ~56 ms instead of ~720 ms. The data is truly simultaneous
 * — one CAN frame, one timestamp — which is why both read functions accept it:
 * they fill their hot channels from the batch and skip their own sequential
 * requests, reading only their cold channels.
 *
 * Confirmed working on the N20 DME (2026-10-03 drive, boot 155).
 */
typedef struct {
    bool     valid;          /* at least one hot field came back */
    bool     complete;       /* every hot field the profile declares came back */
    int16_t  rpm;
    int16_t  speed_kph;
    uint8_t  throttle_pct;
    int8_t   timing_deg;
    uint16_t map_kpa;
    uint16_t lambda_e4;      /* already scaled and clamped by the profile formula */
    uint32_t present;        /* bit per cairn_field_t the active profile's batch carries */
} obd_batch_t;

bool sensors_read_obd_batch(obd_batch_t *out);

bool sensors_read_obd(cairn_obd_snapshot_t *out, const obd_batch_t *batch);

/*
 * The boosted-engine and mixture PIDs: manifold pressure, barometric, mass air
 * flow, equivalence ratio, absolute load, ambient temperature and both fuel
 * trims.
 *
 * Separate from sensors_read_obd because support varies per vehicle. Always
 * returns true: a record of sentinels with pids_answered = 0 is the evidence
 * that this ECU answers none of them, which is worth recording rather than
 * inferring from an absence.
 */
bool sensors_read_obd_extended(cairn_obd_extended_t *out, const obd_batch_t *batch);

#if CAIRN_PIDTEST
/*
 * Accessors for the PID validation build. Kept behind the flag so the
 * production firmware has no way to reach raw link traffic.
 */
bool    sensors_obd_ready(void);
bool    sensors_obd_pid_supported(uint8_t pid);
uint8_t sensors_obd_pidmap_byte(uint8_t index);
bool    sensors_obd_converted_pid(uint8_t pid, int *out);

/* Raw Mode 01 reply as space-separated hex data bytes. Returns the byte count,
 * or 0 when the ECU did not answer this PID. */
int     sensors_obd_raw_pid(uint8_t pid, char *out, size_t cap);

/* Sends one Mode 01 request carrying up to six PIDs and returns the reply
 * verbatim. Answers whether this ECU supports multi-PID requests, which is the
 * only route to a materially faster OBD cadence. */
int     sensors_obd_multi_probe(const uint8_t *pids, int n, char *out, size_t cap);
#endif

#if CAIRN_MTPROBE
/*
 * Narrow link access for the manual-transmission probe; see sensors.cpp.
 * Sensing-task only.
 */
bool sensors_obd_ready(void);
int  sensors_obd_command(const char *cmd, char *buf, size_t cap, uint32_t timeout_ms);
int  sensors_obd_receive(char *buf, size_t cap, uint32_t timeout_ms);
bool sensors_obd_recover(void);
#endif

/* Battery voltage in millivolts, read through the coprocessor. */
uint16_t sensors_battery_mv(void);

/*
 * The raw supply voltage from the OBD-II co-processor, in millivolts, with no
 * range guard. 0 if the co-processor cannot answer. Unlike sensors_battery_mv()
 * this returns readings outside a car's normal rail range: a few volts (USB
 * power on the bench, so OBD pin 16 is floating) or close to zero (co-processor
 * glitch). Only the bench-mode detector reads this; everyone else wants the
 * guarded one.
 */
uint16_t sensors_supply_mv_raw(void);

void sensors_fill_health(cairn_device_health_t *out, uint8_t health_state,
                         uint8_t reboot_count, int rssi_dbm);

/* ── power ────────────────────────────────────────────────────────────────── */

/*
 * Power the GNSS receiver down. It draws continuously and has nothing to track
 * while parked; reacquisition on wake costs seconds, which a stationary vehicle
 * can afford.
 */
void sensors_gnss_power_down(void);

/*
 * Put the OBD coprocessor into its low-power mode, or bring it back.
 *
 * This is the vendor's ATLP path, and it covers more than OBD: the internal
 * GNSS sits behind the same link. Leaving requires a link reset, which this
 * handles — the coprocessor does not resume mid-conversation.
 */
void sensors_link_low_power(bool enable);

#endif /* CAIRN_SENSORS_H */
