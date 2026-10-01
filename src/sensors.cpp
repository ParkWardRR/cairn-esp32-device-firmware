#include "sensors.h"

#include <Arduino.h>
#include <FreematicsPlus.h>
#include <math.h>

#include "cairn_log.h"
#include "config.h"

static const char *TAG = "SENS";

static FreematicsESP32 s_sys;
static COBD            s_obd;
static MEMS_I2C       *s_mems = nullptr;
static GPS_DATA       *s_gps = nullptr;

static SensorStatus   *s_status = nullptr;

/* Accelerometer bias, measured at boot while the vehicle is presumed still. */
static float s_acc_bias[3] = { 0, 0, 0 };

/* IMU accumulation state for the current window. */
static struct {
    uint32_t count;
    float    sum_sq;        /* of the bias-corrected magnitude, in g */
    float    peak[3];       /* signed peaks, g */
    float    gyro_peak;     /* deg/s */
    float    mean;          /* running mean magnitude, for variance */
    float    sum;
    uint32_t window_start_ms;
    uint16_t last_rms_mg;
} s_imu;

static uint32_t s_last_gps_ts = 0;
static uint32_t s_last_gps_update_ms = 0;

/* ── narrowing ────────────────────────────────────────────────────────────── */

/*
 * Saturate into the specification's i8 fields instead of letting C wrap.
 *
 * This matters most for coolant temperature. The driver returns
 * `hex2uint8(data) - 40`, so the range is -40..+215 C, while the field is i8
 * with 0x80 reserved for "unavailable". A bare cast turns 128 C into -128,
 * which *is* the unavailable sentinel, and 130 C into -126 C — so an
 * overheating engine would be recorded either as a missing sensor or as a
 * plausible cold reading. Both are worse than useless: they are indistinguishable
 * from real observations.
 *
 * Saturating at +127 is still obviously extreme and cannot be mistaken for
 * normal, and -128 stays reserved for genuinely absent data.
 */
static int8_t sat_i8(int v)
{
    if (v > 127) return 127;
    if (v < -127) return -127; /* -128 is the unavailable sentinel */
    return (int8_t)v;
}

/* ── init ─────────────────────────────────────────────────────────────────── */

static void calibrate_imu(void)
{
    if (s_mems == nullptr) return;

    float sum[3] = { 0, 0, 0 };
    int   n = 0;
    uint32_t t = millis();

    while (millis() - t < 1000) {
        float a[3];
        if (!s_mems->read(a)) continue;
        sum[0] += a[0];
        sum[1] += a[1];
        sum[2] += a[2];
        n++;
    }

    if (n > 0) {
        /*
         * Gravity is left in deliberately. Motion scoring uses the deviation of
         * the magnitude from its resting value, so removing the full vector —
         * including gravity — makes a stationary device read near zero whatever
         * its mounting angle.
         */
        s_acc_bias[0] = sum[0] / n;
        s_acc_bias[1] = sum[1] / n;
        s_acc_bias[2] = sum[2] / n;
        CAIRN_LOGI(TAG, "IMU bias: %.3f %.3f %.3f g (%d samples)",
                   s_acc_bias[0], s_acc_bias[1], s_acc_bias[2], n);
    } else {
        CAIRN_LOGW(TAG, "IMU calibration read nothing; using zero bias");
    }
}

static void init_obd(SensorStatus *status)
{
    /*
     * begin() defaults to begin(useCoProc = true, useCellular = true), and the
     * defaults are kept deliberately even though no cellular module is fitted.
     *
     * Passing useCellular = false looks like the obvious tidy-up and is a trap:
     * in FreematicsPlus.cpp that branch is the only thing that sets
     * FLAG_GNSS_SOFT_SERIAL, which selects which GNSS transport gpsBeginExt()
     * uses. The hardware profile in docs/flashing-and-testing.md was measured
     * with the defaults and recorded GNSS as 38400 soft serial, so changing
     * them would quietly move the firmware off the configuration this board was
     * validated under.
     *
     * The cost of the default is a UART1 init on pins 35/2 and GPIO27 driven
     * low, for a modem that is not there. Nothing here ever calls the cellular
     * API, so that is wasted setup rather than a conflict — the coprocessor
     * link is UART2 and the GNSS in use reaches the receiver over that link.
     */
    if (s_sys.begin()) {
        status->coprocessor = true;
        status->device_type = s_sys.devType;
        CAIRN_LOGI(TAG, "coprocessor up, device type %u", s_sys.devType);

        s_obd.begin(s_sys.link);

        if (s_obd.init()) {
            status->obd = true;
            CAIRN_LOGI(TAG, "ECU connected");

            char buf[128] = { 0 };
            if (s_obd.getVIN(buf, sizeof(buf))) {
                /* A VIN is 17 characters; the driver's buffer is larger, so the
                 * copy is bounded to the field and deliberately truncating. */
                snprintf(status->vin, sizeof(status->vin), "%.17s", buf);
                CAIRN_LOGI(TAG, "VIN %s", status->vin);
            }
        } else {
            /* Not an error: the ignition may simply be off. */
            CAIRN_LOGW(TAG, "ECU not responding (ignition off?)");
        }
    } else {
        CAIRN_LOGE(TAG, "coprocessor init failed; retrying without OBD");
        s_sys.begin(false, false);
    }
}

static void init_imu(SensorStatus *status)
{
    s_mems = new ICM_42627;
    if (s_mems->begin()) {
        status->imu = true;
        CAIRN_LOGI(TAG, "IMU ICM-42627 ready");
        calibrate_imu();
    } else {
        delete s_mems;
        s_mems = nullptr;
        status->imu = false;
        CAIRN_LOGW(TAG, "no IMU found");
    }
}

static void init_gnss(SensorStatus *status)
{
    if (s_sys.gpsBeginExt()) {
        status->gnss = true;
        CAIRN_LOGI(TAG, "GNSS ready (external)");
    } else if (s_sys.gpsBegin()) {
        status->gnss = true;
        CAIRN_LOGI(TAG, "GNSS ready (internal)");
    } else {
        status->gnss = false;
        CAIRN_LOGW(TAG, "GNSS not available");
    }
}

bool sensors_begin(SensorStatus *status)
{
    memset(status, 0, sizeof(*status));
    memset(&s_imu, 0, sizeof(s_imu));
    s_imu.window_start_ms = millis();
    s_status = status;

    init_obd(status);
    init_imu(status);
    init_gnss(status);

    return status->coprocessor;
}

void sensors_retry_failed(SensorStatus *status)
{
    if (!status->obd && status->coprocessor) {
        if (s_obd.init()) {
            status->obd = true;
            CAIRN_LOGI(TAG, "ECU became available");
        }
    }
    if (!status->gnss) init_gnss(status);
    if (!status->imu) init_imu(status);
}

/* ── GNSS ─────────────────────────────────────────────────────────────────── */

bool sensors_read_gnss(cairn_gnss_sample_t *out, uint32_t *fix_age_ms)
{
    if (s_status == nullptr || !s_status->gnss) return false;
    if (!s_sys.gpsGetData(&s_gps) || s_gps == nullptr) return false;

    /* The driver returns the same buffer each call; an unchanged timestamp means
     * no new fix, and reporting it again would fabricate a sample. */
    if (s_gps->ts == s_last_gps_ts) {
        if (fix_age_ms != nullptr) *fix_age_ms = millis() - s_last_gps_update_ms;
        return false;
    }
    s_last_gps_ts = s_gps->ts;
    s_last_gps_update_ms = millis();
    if (fix_age_ms != nullptr) *fix_age_ms = 0;

    memset(out, 0, sizeof(*out));

    /* Degrees x 1e7, as the specification requires. */
    out->lat_e7 = (int32_t)lroundf(s_gps->lat * 1e7f);
    out->lon_e7 = (int32_t)lroundf(s_gps->lng * 1e7f);
    out->alt_cm = (int32_t)lroundf(s_gps->alt * 100.0f);

    /* The driver reports knots; the format wants cm/s. */
    out->speed_cmps   = (uint16_t)lroundf(s_gps->speed * 51.4444f);
    out->heading_cdeg = (uint16_t)(s_gps->heading * 100u);
    out->hdop_e2      = (s_gps->hdop > 0) ? (uint16_t)(s_gps->hdop * 10u)
                                          : CAIRN_U16_UNKNOWN;

    /*
     * This receiver reports no accuracy estimates, so they are sentinels rather
     * than a guess derived from HDOP. A fabricated accuracy is worse than an
     * absent one: it would be indistinguishable from a measured value.
     */
    out->h_acc_cm = CAIRN_U16_UNKNOWN;
    out->v_acc_cm = CAIRN_U16_UNKNOWN;

    out->sats_used    = s_gps->sat;
    out->sats_visible = CAIRN_U8_UNKNOWN;

    /* Fix type is inferred from what is actually present. */
    if (s_gps->sat >= 4 && s_gps->alt != 0) out->fix_type = 3;
    else if (s_gps->sat >= 3)               out->fix_type = 2;
    else                                    out->fix_type = 0;

    out->source_flags = 0;

    /*
     * UTC offset relative to the bundle's basis is filled in by the caller,
     * which owns the basis. Until then it is unknown, with the accuracy
     * sentinel saying so.
     */
    out->utc_offset_ms = 0;
    out->utc_acc_ms    = CAIRN_U16_UNKNOWN;

    return true;
}

bool sensors_gnss_utc(uint64_t *utc_ms, uint32_t *acc_ms)
{
    if (s_gps == nullptr || s_gps->date == 0) return false;

    /* date is DDMMYY, time is HHMMSSss in the driver's encoding. */
    uint32_t d = s_gps->date;
    uint32_t t = s_gps->time;

    int day   = (int)(d / 10000);
    int month = (int)((d / 100) % 100);
    int year  = 2000 + (int)(d % 100);

    int hour = (int)(t / 1000000);
    int min  = (int)((t / 10000) % 100);
    int sec  = (int)((t / 100) % 100);
    int cs   = (int)(t % 100);

    if (month < 1 || month > 12 || day < 1 || day > 31) return false;

    /* Days since the Unix epoch, via a civil-from-days algorithm. */
    int y = year;
    int m = month;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m - 3) + 2) / 5 + day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;

    uint64_t ms = (uint64_t)days * 86400000ULL +
                  (uint64_t)hour * 3600000ULL +
                  (uint64_t)min * 60000ULL +
                  (uint64_t)sec * 1000ULL +
                  (uint64_t)cs * 10ULL;

    *utc_ms = ms;

    /*
     * One second, honestly. The receiver gives no accuracy figure, and the
     * basis is only as good as the moment it was sampled relative to the
     * monotonic clock.
     */
    *acc_ms = 1000;
    return true;
}

/* ── IMU ──────────────────────────────────────────────────────────────────── */

void sensors_imu_accumulate(void)
{
    if (s_mems == nullptr) return;

    float a[3], g[3];
    if (!s_mems->read(a, g)) return;

    float dx = a[0] - s_acc_bias[0];
    float dy = a[1] - s_acc_bias[1];
    float dz = a[2] - s_acc_bias[2];
    float mag = sqrtf(dx * dx + dy * dy + dz * dz);

    s_imu.count++;
    s_imu.sum    += mag;
    s_imu.sum_sq += mag * mag;

    if (fabsf(dx) > fabsf(s_imu.peak[0])) s_imu.peak[0] = dx;
    if (fabsf(dy) > fabsf(s_imu.peak[1])) s_imu.peak[1] = dy;
    if (fabsf(dz) > fabsf(s_imu.peak[2])) s_imu.peak[2] = dz;

    float gmax = fmaxf(fabsf(g[0]), fmaxf(fabsf(g[1]), fabsf(g[2])));
    if (gmax > s_imu.gyro_peak) s_imu.gyro_peak = gmax;

    /* Keep a live RMS so motion scoring does not have to wait for a window. */
    if (s_imu.count > 0) {
        float rms = sqrtf(s_imu.sum_sq / s_imu.count);
        s_imu.last_rms_mg = (uint16_t)lroundf(rms * 1000.0f);
    }
}

uint16_t sensors_recent_accel_rms_mg(void)
{
    return s_imu.last_rms_mg;
}

bool sensors_imu_summarize(uint32_t window_ms, cairn_imu_summary_t *out)
{
    if (s_mems == nullptr || s_imu.count == 0) return false;

    memset(out, 0, sizeof(*out));

    float rms  = sqrtf(s_imu.sum_sq / s_imu.count);
    float mean = s_imu.sum / s_imu.count;
    float var  = (s_imu.sum_sq / s_imu.count) - (mean * mean);
    if (var < 0) var = 0;

    out->window_ms       = (uint16_t)window_ms;
    out->accel_rms_mg    = (uint16_t)lroundf(rms * 1000.0f);
    out->accel_peak_x_mg = (int16_t)lroundf(s_imu.peak[0] * 1000.0f);
    out->accel_peak_y_mg = (int16_t)lroundf(s_imu.peak[1] * 1000.0f);
    out->accel_peak_z_mg = (int16_t)lroundf(s_imu.peak[2] * 1000.0f);
    out->gyro_peak_dps_e1 = (int16_t)lroundf(s_imu.gyro_peak * 10.0f);
    out->variance        = (uint16_t)lroundf(var * 1000000.0f);
    out->sample_count    = (uint16_t)((s_imu.count > 0xFFFF) ? 0xFFFF : s_imu.count);
    out->event_flags     = 0;

    /* Reset for the next window, keeping the last RMS for scoring continuity. */
    uint16_t keep = s_imu.last_rms_mg;
    memset(&s_imu, 0, sizeof(s_imu));
    s_imu.window_start_ms = millis();
    s_imu.last_rms_mg = keep;

    return true;
}

/* ── OBD ──────────────────────────────────────────────────────────────────── */

/* Read one PID, mapping a non-answer onto the specification's sentinel. */
static bool pid_value(byte pid, int *value)
{
    return s_obd.readPID(pid, *value);
}

bool sensors_read_obd(cairn_obd_snapshot_t *out)
{
    if (s_status == nullptr || !s_status->obd) return false;

    memset(out, 0, sizeof(*out));

    uint32_t requested = 0, answered = 0;
    uint8_t  errors = 0;
    int      v = 0;

    /* speed, km/h */
    requested++;
    if (pid_value(PID_SPEED, &v)) {
        out->speed_kph = (int16_t)v;
        answered++;
    } else {
        out->speed_kph = CAIRN_I16_UNKNOWN;
        errors++;
    }

    requested++;
    if (pid_value(PID_RPM, &v)) {
        out->rpm = (int16_t)((v > 32767) ? 32767 : v);
        answered++;
    } else {
        out->rpm = CAIRN_I16_UNKNOWN;
        errors++;
    }

    requested++;
    if (pid_value(PID_FUEL_PRESSURE, &v)) {
        out->fuel_pressure_kpa = (uint16_t)v;
        answered++;
    } else {
        out->fuel_pressure_kpa = CAIRN_U16_UNKNOWN;
        errors++;
    }

    requested++;
    if (pid_value(PID_THROTTLE, &v)) {
        out->throttle_pct = (uint8_t)v;
        answered++;
    } else {
        out->throttle_pct = CAIRN_U8_UNKNOWN;
        errors++;
    }

    requested++;
    if (pid_value(PID_ENGINE_LOAD, &v)) {
        out->engine_load_pct = (uint8_t)v;
        answered++;
    } else {
        out->engine_load_pct = CAIRN_U8_UNKNOWN;
        errors++;
    }

    requested++;
    if (pid_value(PID_COOLANT_TEMP, &v)) {
        out->coolant_temp_c = sat_i8(v);
        answered++;
    } else {
        out->coolant_temp_c = CAIRN_I8_UNKNOWN;
        errors++;
    }

    requested++;
    if (pid_value(PID_INTAKE_TEMP, &v)) {
        out->intake_temp_c = sat_i8(v);
        answered++;
    } else {
        out->intake_temp_c = CAIRN_I8_UNKNOWN;
        errors++;
    }

    requested++;
    if (pid_value(PID_TIMING_ADVANCE, &v)) {
        out->timing_advance_deg = sat_i8(v);
        answered++;
    } else {
        out->timing_advance_deg = CAIRN_I8_UNKNOWN;
        errors++;
    }

    /*
     * The counts are recorded, not just the values. A snapshot where two of
     * eight PIDs answered is a different observation from one where all eight
     * did, even if the values that arrived look identical.
     */
    out->pids_requested  = requested;
    out->pids_answered   = answered;
    out->pid_error_count = errors;
    out->poll_cadence_ms = CAIRN_OBD_PERIOD_MS;

    /* A snapshot where nothing answered is a gap, not a reading. */
    return answered > 0;
}

uint16_t sensors_battery_mv(void)
{
    if (s_status == nullptr || !s_status->coprocessor) return CAIRN_U16_UNKNOWN;

    /* On COBD rather than the system object, and documented to work without an
     * ECU — so this reads even with the ignition off, which is exactly when a
     * low-battery decision matters. */
    float v = s_obd.getVoltage();
    if (v <= 0.0f) return CAIRN_U16_UNKNOWN;
    return (uint16_t)lroundf(v * 1000.0f);
}

void sensors_fill_health(cairn_device_health_t *out, uint8_t health_state,
                         uint8_t reboot_count, int rssi_dbm)
{
    memset(out, 0, sizeof(*out));

    out->battery_mv = sensors_battery_mv();

    cairn_log_stats_t log_stats;
    cairn_log_get_stats(&log_stats);
    out->sd_write_errors = (uint16_t)((log_stats.sd_write_errors > 0xFFFF)
                                          ? 0xFFFF
                                          : log_stats.sd_write_errors);

    out->sd_free_mib = CAIRN_U16_UNKNOWN; /* filled by the caller, which owns SD */

    float temp = 0;
    if (s_mems != nullptr && s_mems->read(nullptr, nullptr, nullptr, &temp)) {
        out->device_temp_c = sat_i8((int)lroundf(temp));
    } else {
        out->device_temp_c = CAIRN_I8_UNKNOWN;
    }

    out->rssi_dbm = (rssi_dbm != 0) ? sat_i8(rssi_dbm) : CAIRN_I8_UNKNOWN;

    out->ext_sensor_1 = CAIRN_U16_UNKNOWN;
    out->ext_sensor_2 = CAIRN_U16_UNKNOWN;
    out->health_state = health_state;
    out->reboot_count = reboot_count;
}
