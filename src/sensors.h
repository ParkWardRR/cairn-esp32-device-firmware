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

/* Re-attempt the subsystems that failed. GNSS in particular often appears only
 * after the vehicle has been powered for a while. */
void sensors_retry_failed(SensorStatus *status);

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

bool sensors_read_obd(cairn_obd_snapshot_t *out);

/* Battery voltage in millivolts, read through the coprocessor. */
uint16_t sensors_battery_mv(void);

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
