#include "sensors.h"

#include <Arduino.h>
#include <FreematicsPlus.h>
#include <math.h>

#include "cairn_engine.h"
#include "cairn_log.h"
#include "config.h"

static const char *TAG = "SENS";

/*
 * How long one PID read may wait for the ECU, and how many consecutive misses
 * end a cycle. Both exist to stop a quiet DME from blocking the sensing task.
 *
 * COBD::readPID waits out OBD_TIMEOUT_SHORT (1000 ms) and does not retry, and a
 * capture cycle walks about nine reads: the batch request, four hot channels and
 * a cold slot for the snapshot, then MAP, lambda and another cold slot for the
 * extended record. When the ECU stops answering, each of those nine waits its
 * full second in turn, so the task blocks for most of ten seconds and cannot
 * reach the GNSS poll either.
 *
 * Measured on the 2026-10-07 drive: 25 OBD outages, quantised to ~23 s and small
 * multiples of it, covering 766 s of a 1002 s trip, with position dark in exactly
 * the same windows. The IMU is read on its own path and kept sampling at ~8.5 Hz
 * throughout, which is how we know the device was healthy and the blocking was
 * self-inflicted. It also cost the pulls the car was driven for: a single miss
 * mid-sweep stretched the effective cadence to ~1.2 s, and a 4-6 s pull leaves
 * one or two samples at that rate.
 *
 * 350 ms is comfortably clear of this DME's measured 110-140 ms reply
 * (engines/bmw-n20.yaml, cadence.measured.single_request_ms) while cutting the
 * cost of a miss to a third. OBD_TIMEOUT_SHORT itself cannot simply be lowered:
 * the same constant bounds ATZ and the rest of COBD::init(), where a reset
 * legitimately takes most of a second.
 *
 * Two consecutive misses is enough to conclude the ECU is not answering this
 * cycle; asking the remaining PIDs cannot learn anything the first two did not
 * already say, and skipping them keeps requests off the bus rather than adding
 * them. A dead cycle now costs ~700 ms instead of ~9 s.
 */
#define OBD_READ_TIMEOUT_MS  350u
#define OBD_CYCLE_MISS_LIMIT 2u

/*
 * COBD with a per-read timeout.
 *
 * Subclassed rather than reimplemented because normalizeData() and
 * checkErrorMessage() are protected, and reusing the library's own conversion is
 * the whole point: the shorter timeout then changes timing and nothing else. The
 * alternative was to convert through the engine profile's formula, which
 * test/host/engine_test.c proves equivalent over every input byte — true, but it
 * is an argument this way does not have to make.
 */
class CairnOBD : public COBD {
public:
    bool readPIDTimed(byte pid, int &result, uint32_t timeout_ms);
};

bool CairnOBD::readPIDTimed(byte pid, int &result, uint32_t timeout_ms)
{
    if (link == nullptr) return false;

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%02X%02X\r", (unsigned)dataMode, (unsigned)pid);
    link->send(buffer);
    idleTasks();

    int ret = link->receive(buffer, sizeof(buffer), (int)timeout_ms);
    if (ret <= 0 || checkErrorMessage(buffer)) {
        errors++;
        return false;
    }

    /*
     * The same reply scan COBD::readPID does: find a "41" echo whose PID matches
     * and take the first data field after it. The PID has to be checked because a
     * reply to an earlier request can still be sitting in the buffer, and reading
     * that as this PID's value is how a stale number becomes a fresh-looking
     * sample.
     */
    char *p = buffer;
    while ((p = strstr(p, "41 ")) != nullptr) {
        p += 3;
        if (hex2uint8(p) != pid) continue;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (*p == '\0') break;
        errors = 0;
        result = normalizeData(pid, p);
        return true;
    }

    errors++;
    return false;
}

/* Consecutive misses within the current cycle. */
static uint8_t s_cycle_misses;

static inline void obd_cycle_begin(void) { s_cycle_misses = 0; }
static inline bool obd_cycle_spent(void) { return s_cycle_misses >= OBD_CYCLE_MISS_LIMIT; }

static inline void obd_cycle_note(bool answered)
{
    if (answered) s_cycle_misses = 0;
    else if (s_cycle_misses < 0xFF) s_cycle_misses++;
}

static FreematicsESP32 s_sys;
static CairnOBD        s_obd;
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
     *
     * ECU init (s_obd.init()) is deferred. It times out after ~5 s when the
     * ignition is off, which is the common cold-boot case. The bus-silence
     * invariant prevents OBD traffic until a drive is confirmed anyway, so
     * attempting it at boot costs five seconds of wall time for nothing. The
     * retry path in sensors_retry_failed() handles it once the bus is open.
     */
    if (s_sys.begin()) {
        status->coprocessor = true;
        status->device_type = s_sys.devType;
        CAIRN_LOGI(TAG, "coprocessor up, device type %u", s_sys.devType);

        s_obd.begin(s_sys.link);
        CAIRN_LOGI(TAG, "ECU init deferred to drive confirmed");
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

/* ── engine profile ───────────────────────────────────────────────────────── */

/*
 * An engine id this unit has been told its vehicle has, if any. Nothing sets it by
 * default, and then the unit behaves exactly as it did before profiles: it serves
 * whatever answers, using the active profile. Set it (for instance with
 * -DCAIRN_VEHICLE_ENGINE_ID='"bmw-b58"') and a build that does not carry that engine
 * refuses to open an OBD session, and says so.
 */
#ifndef CAIRN_VEHICLE_ENGINE_ID
#define CAIRN_VEHICLE_ENGINE_ID nullptr
#endif

/* Latched: once refused, the ECU is not asked again, so a refusal costs no bus traffic. */
static bool s_engine_refused = false;
static const char *const k_declared_engine = CAIRN_VEHICLE_ENGINE_ID;

static void log_engine_identity(void)
{
    const cairn_engine_identity_t *id = cairn_engine_identity();
    const cairn_engine_profile_t  *p  = cairn_engine_active();

    CAIRN_LOGI(TAG, "%s", id->string);
    if (p == nullptr) return;

    CAIRN_LOGI(TAG, "active engine profile %s v%u (%s)", p->id, (unsigned)p->version,
               p->status == CAIRN_ENGINE_STUB      ? "stub"
               : p->status == CAIRN_ENGINE_DERIVED ? "derived"
                                                   : "verified");
    if (!cairn_engine_has_pids()) {
#if CAIRN_MTPROBE && CAIRN_MTPROBE_ALLOW_STUB
        CAIRN_LOGW(TAG, "=== DISCOVERY MODE === profile %s has no PID table; "
                        "probe will run in read-only mode, no production capture",
                   p->id);
#else
        CAIRN_LOGW(TAG, "profile %s has no PID table (unknown): no OBD request will be sent; "
                        "cadence and thresholds are the device defaults", p->id);
#endif
    }
}

/*
 * Whether this build may open an OBD session for this vehicle.
 *
 * Multi-fallback discovery chain:
 *   1. Compile-time CAIRN_VEHICLE_ENGINE_ID (no bus traffic needed)
 *   2. BLE companion engine declaration (phone tells dongle)
 *   3. VIN pattern match (full VIN against vin_patterns)
 *   4. VIN engine code match (BMW VDS positions 4-8)
 *   5. Default fallback (first non-stub profile)
 *
 * `vin` is null before the ECU has answered; the compile-time and BLE
 * paths need no bus traffic so they are checked first.
 */
static bool engine_gate(const char *vin)
{
    const char *engine = nullptr;
    cairn_vehicle_verdict_t v;

    /* ── 1. Compile-time declare ──────────────────────────────────────── */
    v = cairn_engine_check_vehicle(k_declared_engine, vin, &engine);

    /* ── 2. BLE companion declaration ─────────────────────────────────── */
    if (v == CAIRN_VEHICLE_UNIDENTIFIED) {
        const char *ble_decl = cairn_engine_get_ble_declaration();
        if (ble_decl != nullptr) {
            v = cairn_engine_check_vehicle(ble_decl, nullptr, &engine);
            if (v == CAIRN_VEHICLE_SERVED) {
                CAIRN_LOGI(TAG, "engine \"%s\" declared by companion app", engine);
            }
        }
    }

    /* ── 3. VIN pattern match (once the ECU has answered) ─────────────── */
    if (v == CAIRN_VEHICLE_UNIDENTIFIED && vin != nullptr && strlen(vin) == 17) {
        /* check_vehicle already tries vin_patterns when declared_id is NULL */
        v = cairn_engine_check_vehicle(nullptr, vin, &engine);
        if (v == CAIRN_VEHICLE_SERVED) {
            CAIRN_LOGI(TAG, "vehicle identified by VIN pattern: %s", engine);
        }
    }

    /* ── 4. VIN engine code match (BMW VDS positions 4-8) ─────────────── */
    if (v == CAIRN_VEHICLE_UNIDENTIFIED && vin != nullptr && strlen(vin) == 17) {
        const cairn_engine_catalogue_entry_t *hit =
            cairn_engine_match_code(cairn_engine_catalogue(),
                                    cairn_engine_catalogue_count(),
                                    vin);
        if (hit != nullptr) {
            engine = hit->id;
            v = hit->installed ? CAIRN_VEHICLE_SERVED
                               : CAIRN_VEHICLE_REFUSED_NOT_INSTALLED;
            if (v == CAIRN_VEHICLE_SERVED) {
                CAIRN_LOGI(TAG, "vehicle identified by VIN engine code: %s (VIN %.17s)",
                           engine, vin);
            }
        }
    }

    /* ── Handle refusals ──────────────────────────────────────────────── */
    switch (v) {
    case CAIRN_VEHICLE_REFUSED_NOT_INSTALLED:
        CAIRN_LOGE(TAG, "vehicle needs engine %s, which this build does not carry (%s): "
                        "refusing OBD, nothing will be requested from the ECU",
                   engine, cairn_engine_identity()->selection);
        s_engine_refused = true;
        return false;
    case CAIRN_VEHICLE_REFUSED_UNKNOWN_ENGINE:
        CAIRN_LOGE(TAG, "vehicle is declared as engine \"%s\", which has no profile in "
                        "engines/: refusing OBD",
                   k_declared_engine != nullptr ? k_declared_engine : "?");
        s_engine_refused = true;
        return false;
    case CAIRN_VEHICLE_SERVED:
        if (engine != nullptr && cairn_engine_select(engine)) {
            CAIRN_LOGI(TAG, "vehicle identified as engine %s: using that profile", engine);
        }
        break;
    case CAIRN_VEHICLE_UNIDENTIFIED:
    default:
        /* 5. Default fallback — first non-stub profile */
        break;
    }

    if (!cairn_engine_has_pids()) {
#if CAIRN_MTPROBE && CAIRN_MTPROBE_ALLOW_STUB
        /*
         * Discovery mode: the active profile is a stub with no PID table. The
         * MTPROBE build needs bus access to discover what the ECU supports, so
         * the gate passes — but no production OBD polling will happen because
         * there are no PIDs to request. The probe runs its own read-only
         * discovery alongside the (empty) capture path.
         *
         * This branch is deliberately compile-time gated: CAIRN_MTPROBE_ALLOW_STUB
         * must never be set in a production build.
         */
        CAIRN_LOGW(TAG, "DISCOVERY MODE: active profile %s has no PID table; "
                        "allowing OBD for probe-only operation (no production capture)",
                   cairn_engine_active()->id);
        return true;
#else
        CAIRN_LOGE(TAG, "active profile %s has no PID table: refusing OBD rather than "
                        "polling with another engine's PIDs", cairn_engine_active()->id);
        s_engine_refused = true;
        return false;
#endif
    }
    return true;
}

bool sensors_begin(SensorStatus *status)
{
    memset(status, 0, sizeof(*status));
    memset(&s_imu, 0, sizeof(s_imu));
    s_imu.window_start_ms = millis();
    s_status = status;

    cairn_engine_select_default();
    log_engine_identity();

    init_obd(status);
    init_gnss(status);
    init_imu(status);

    return status->coprocessor;
}

void sensors_retry_failed(SensorStatus *status, bool bus_open)
{
    if (bus_open && !status->obd && status->coprocessor && !s_engine_refused &&
        engine_gate(nullptr)) {
        if (s_obd.init()) {
            status->obd = true;
            CAIRN_LOGI(TAG, "ECU connected");

            char buf[128] = { 0 };
            if (s_obd.getVIN(buf, sizeof(buf))) {
                snprintf(status->vin, sizeof(status->vin), "%.17s", buf);
                CAIRN_LOGI(TAG, "VIN %s", status->vin);

                /* The VIN may say which engine this is. If it names one this build
                 * does not carry, stop using the ECU: the capture would otherwise
                 * carry values read with the wrong profile. */
                if (!engine_gate(status->vin)) status->obd = false;
            }
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

uint16_t sensors_accel_magnitude_mg(void)
{
    if (s_mems == nullptr) return 0;

    float a[3], g[3];
    if (!s_mems->read(a, g)) return 0;

    /* Bias-corrected, so the 1 g the board is sitting in does not read as
     * motion. The bias was measured at boot while the vehicle was presumed
     * still. */
    float dx = a[0] - s_acc_bias[0];
    float dy = a[1] - s_acc_bias[1];
    float dz = a[2] - s_acc_bias[2];
    float mag = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;

    if (mag < 0.0f) return 0;
    if (mag > 65534.0f) return 65534;
    return (uint16_t)lroundf(mag);
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

/*
 * Read one PID, mapping a non-answer onto the specification's sentinel.
 *
 * Returns false without touching the bus once the cycle's miss budget is spent,
 * so the caller's own "else -> UNKNOWN" branch records the field as unknown,
 * which is exactly what it is.
 */
static bool pid_value(byte pid, int *value)
{
    if (obd_cycle_spent()) return false;

    bool answered = s_obd.readPIDTimed(pid, *value, OBD_READ_TIMEOUT_MS);
    obd_cycle_note(answered);
    return answered;
}

/*
 * Read a two-byte PID as the raw 16-bit value the ECU sent, bypassing
 * normalizeData entirely.
 *
 * Needed because the library mishandles both of the two-byte PIDs this
 * firmware cares about. 0x43 absolute load is read through getPercentageValue,
 * which takes one byte and computes A * 100 / 255 — on a true 150% load
 * (raw 0x017F) that yields 0.39%, and because A increments once per 100% of
 * load the result sawtooths rather than saturating. 0x44 equivalence ratio is
 * quantised to a 0..200 scale, discarding most of a 15-bit measurement.
 *
 * Both are standard-defined as ((A*256)+B) with their own scaling, so the raw
 * pair is the honest thing to capture; the conversion belongs at decode, where
 * it can be corrected without reflashing. Confirmed against SAE J1979 /
 * ISO 15031-5 rather than inferred: see
 * research_notes/BMW N20 OBD PID support/.
 */
static bool pid_raw_u16(uint8_t pid, uint16_t *out)
{
    if (s_obd.link == nullptr) return false;
    if (obd_cycle_spent()) return false;

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "01%02X\r", (unsigned)pid);

    char buf[128];
    if (s_obd.link->sendCommand(cmd, buf, sizeof(buf), OBD_READ_TIMEOUT_MS) == 0) {
        obd_cycle_note(false);
        return false;
    }

    /* Require the mode-and-PID echo: NO DATA and "SEARCHING" both arrive on
     * this path and must not be parsed as a measurement. */
    char want[8];
    snprintf(want, sizeof(want), "41 %02X", (unsigned)pid);

    const char *p = strstr(buf, want);
    if (p == nullptr) {
        obd_cycle_note(false);
        return false;
    }
    p += strlen(want);

    /* Two hex bytes, space separated, as the ELM-style reply formats them. */
    if (!(p[0] == ' ' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
          p[3] == ' ' && isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5]))) {
        obd_cycle_note(false);
        return false;
    }

    uint8_t a = hex2uint8(p + 1);
    uint8_t b = hex2uint8(p + 4);
    *out = (uint16_t)((uint16_t)a << 8 | b);
    obd_cycle_note(true);
    return true;
}

/*
 * Multi-PID batch read for the capture path.
 *
 * Sends a single Mode 01 request carrying six hot PIDs and parses the
 * response into typed fields. Confirmed on the N20 DME on the 2026-10-03
 * drive (boot 155): all six PIDs returned in one ISO-TP frame, 56 ms average
 * across 115 clean replies. The response is a single "41" SID echo followed
 * by PID+data pairs in request order.
 *
 * Returns false on any parse failure (timeout, GNSS contamination, partial
 * response). The caller falls back to sequential reads for that cycle.
 *
 * Which PIDs, in what order, and how each reply becomes a stored value all come
 * from the active engine profile (lib/cairn_engine). The request string and the
 * parse are the profile code's, and test/host/engine_test.c holds both to the
 * behaviour this function had when the list was hard-coded here.
 */
bool sensors_read_obd_batch(obd_batch_t *out)
{
    memset(out, 0, sizeof(*out));

    /*
     * First bus access of the cycle, so this is where the miss budget resets.
     * A failed batch deliberately does not count against it: the batch being
     * unsupported says nothing about whether sequential reads will answer, and
     * on the 2026-10-07 drive that was the actual situation — 225 of 239 samples
     * came from the sequential fallback. Charging the budget here would abandon
     * cycles that were about to succeed.
     */
    obd_cycle_begin();

    if (s_status == nullptr || !s_status->obd || s_obd.link == nullptr)
        return false;

    const cairn_engine_profile_t *prof = cairn_engine_active();

    char cmd[24];
    if (cairn_engine_batch_request(prof, cmd, sizeof(cmd)) == 0)
        return false;

    char buf[192];
    if (s_obd.link->sendCommand(cmd, buf, sizeof(buf), OBD_READ_TIMEOUT_MS) == 0)
        return false;

    const char *p = strstr(buf, "41 ");
    if (p == nullptr) return false;
    p += 3;

    uint8_t bytes[32];
    int     nbytes = 0;

    while (*p && nbytes < (int)sizeof(bytes)) {
        while (*p == ' ' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;

        if (isdigit((unsigned char)*p) && p[1] == ':') {
            p += 2;
            continue;
        }

        if (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1])) {
            bytes[nbytes++] = hex2uint8(p);
            p += 2;
        } else {
            return false;
        }
    }

    int32_t  v[CAIRN_FIELD_COUNT];
    uint32_t present = 0;
    if (!cairn_engine_batch_parse(prof, bytes, (size_t)nbytes, v, &present))
        return false;

    out->rpm          = (int16_t)v[CAIRN_FIELD_RPM];
    out->speed_kph    = (int16_t)v[CAIRN_FIELD_SPEED_KPH];
    out->throttle_pct = (uint8_t)v[CAIRN_FIELD_THROTTLE_PCT];
    out->timing_deg   = (int8_t)v[CAIRN_FIELD_TIMING_ADVANCE_DEG];
    out->map_kpa      = (uint16_t)v[CAIRN_FIELD_MAP_KPA];
    out->lambda_e4    = (uint16_t)v[CAIRN_FIELD_LAMBDA_E4];
    out->present      = present;
    out->valid        = true;

    /*
     * Only a complete batch earns the fast cadence. A complete one is the ~56 ms
     * cycle the fast period was measured against; a partial one means the caller
     * has to go and fetch the rest one request at a time, so the cycle costs what
     * a sequential sweep costs and the period should reflect that.
     */
    const uint32_t hot = cairn_engine_hot_field_mask(prof);
    out->complete = (hot != 0) && ((present & hot) == hot);
    return true;
}

static inline bool batch_has(const obd_batch_t *b, cairn_field_t f)
{
    return (b->present & (1u << f)) != 0;
}

/* The PID the active profile reads for `f`, as a Mode 01 PID the library can send. */
static bool field_pid(cairn_field_t f, byte *pid)
{
    const cairn_pid_t *p = cairn_engine_pid_for_field(cairn_engine_active(), f);
    if (p == nullptr || p->service != 0x01) return false;
    *pid = (byte)p->pid;
    return true;
}


#if CAIRN_PIDTEST
/*
 * Multi-PID request probe.
 *
 * SAE J1979 permits a single Mode 01 request to carry up to six PIDs, with all
 * of their data returned in one response. If this ECU honours it, six channels
 * cost one 120 ms round trip instead of six, and the bus ceiling that currently
 * caps everything stops binding — it is the only change that would make OBD
 * "much faster" rather than merely better prioritised.
 *
 * Whether a given DME answers a multi-PID request is not something to reason
 * about. Plenty answer only the first PID, or reject the frame outright, and
 * BMW's D-CAN stub is reached through a co-processor that may not pass the
 * frame through unaltered either. So this sends one and prints exactly what
 * came back, for the same reason the single-PID probe exists.
 *
 * Returns the raw reply text; interpretation is left to the reader.
 */
int sensors_obd_multi_probe(const uint8_t *pids, int n, char *out, size_t cap)
{
    if (out == nullptr || cap == 0) return -1;
    out[0] = '\0';
    if (s_obd.link == nullptr || pids == nullptr || n <= 0 || n > 6) return -1;

    /* "01" followed by each PID, as the standard frames a multi-PID request. */
    char cmd[24];
    size_t k = (size_t)snprintf(cmd, sizeof(cmd), "01");
    for (int i = 0; i < n && k + 3 < sizeof(cmd); i++) {
        k += (size_t)snprintf(cmd + k, sizeof(cmd) - k, "%02X", (unsigned)pids[i]);
    }
    if (k + 2 >= sizeof(cmd)) return -1;
    cmd[k++] = '\r';
    cmd[k]   = '\0';

    char buf[192];
    if (s_obd.link->sendCommand(cmd, buf, sizeof(buf), OBD_TIMEOUT_SHORT) == 0) {
        return 0;
    }

    /*
     * Copied out verbatim, newlines flattened so one probe stays one log line.
     * A reply echoing "41" once per requested PID means the request worked; a
     * reply carrying only the first PID means it did not, and the difference is
     * visible without any parsing on this side.
     */
    size_t w = 0;
    for (size_t i = 0; buf[i] && w + 1 < cap; i++) {
        char c = buf[i];
        if (c == '\r' || c == '\n') {
            if (w > 0 && out[w - 1] == ' ') continue;
            c = ' ';
        }
        out[w++] = c;
    }
    out[w] = '\0';
    return (int)w;
}
#endif /* CAIRN_PIDTEST */

/*
 * Cold-channel rotation.
 *
 * Every PID is an independent request down the link and back from the ECU, and
 * that round trip measured 110..140 ms across 4263 requests on the 2026-10-03
 * drive with no observed variance. Sixteen PIDs per cycle therefore cost about
 * 1.9 s, which is why the OBD cadence sat at 2000 ms: the bus was saturated,
 * not the card.
 *
 * Polling everything at the rate the fastest channel needs is what made that
 * ceiling binding, and most of the set does not need it. Manifold pressure,
 * lambda and RPM move inside a single gear change. Coolant, ambient air,
 * barometric pressure and the long-term fuel trim move over minutes, and
 * spending 120 ms on each of them every cycle to watch them not change is what
 * was costing the channels that matter their resolution.
 *
 * So the hot set is read every cycle and the cold set rotates, one per cycle.
 * A cold channel that is not its turn is written as its unavailable sentinel
 * and is not counted as requested, because it was not: carrying the previous
 * value forward would be fabricating a measurement at a timestamp where none
 * was taken, which is the one thing this format exists to prevent. The decoder
 * already treats sentinels as absent, so a slow channel simply appears in one
 * record in seven instead of in all of them.
 */
static uint8_t s_cold_phase;

/* Slots in the rotation: the profile's, and never zero so the modulo below is safe. */
static inline uint8_t cold_slots(void)
{
    return cairn_engine_params()->cold_slots;
}

static bool cold_pid(cairn_field_t f, byte *pid)
{
    const cairn_pid_t *p = cairn_engine_pid_for_field(cairn_engine_active(), f);
    if (p == nullptr || p->service != 0x01 || p->tier != CAIRN_TIER_COLD ||
        p->cold_slot != s_cold_phase) {
        return false;
    }
    *pid = (byte)p->pid;
    return true;
}

bool sensors_read_obd(cairn_obd_snapshot_t *out, const obd_batch_t *batch)
{
    if (s_status == nullptr || !s_status->obd) return false;

    memset(out, 0, sizeof(*out));

    uint32_t requested = 0, answered = 0;
    uint8_t  errors = 0;
    int      v = 0;
    byte     pid = 0;

    const bool have_batch = (batch != nullptr && batch->valid);
    const bool batch_complete = (batch != nullptr && batch->complete);

    /*
     * The four hot channels of the snapshot. From the batch when it carried that
     * field, else one request each through the library. A channel the active
     * profile does not carry is its sentinel and is not counted as requested,
     * because it was not.
     *
     * The fallback is now per field rather than all-or-nothing. It used to be
     * gated on `!have_batch`, so a batch that came back carrying five of six PIDs
     * left the sixth as unknown even though a single request would have had it —
     * the batch was treated as the only source once it existed at all. Each field
     * is asked for exactly once either way, and the cycle's miss budget still
     * bounds what a quiet ECU can cost.
     */
    if (have_batch && batch_has(batch, CAIRN_FIELD_SPEED_KPH)) {
        out->speed_kph = batch->speed_kph;
        requested++; answered++;
    } else if (field_pid(CAIRN_FIELD_SPEED_KPH, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->speed_kph = (int16_t)v;
            answered++;
        } else {
            out->speed_kph = CAIRN_I16_UNKNOWN;
            errors++;
        }
    } else {
        out->speed_kph = CAIRN_I16_UNKNOWN;
    }

    if (have_batch && batch_has(batch, CAIRN_FIELD_RPM)) {
        out->rpm = batch->rpm;
        requested++; answered++;
    } else if (field_pid(CAIRN_FIELD_RPM, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->rpm = (int16_t)((v > 32767) ? 32767 : v);
            answered++;
        } else {
            out->rpm = CAIRN_I16_UNKNOWN;
            errors++;
        }
    } else {
        out->rpm = CAIRN_I16_UNKNOWN;
    }

    if (have_batch && batch_has(batch, CAIRN_FIELD_THROTTLE_PCT)) {
        out->throttle_pct = batch->throttle_pct;
        requested++; answered++;
    } else if (field_pid(CAIRN_FIELD_THROTTLE_PCT, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->throttle_pct = (uint8_t)v;
            answered++;
        } else {
            out->throttle_pct = CAIRN_U8_UNKNOWN;
            errors++;
        }
    } else {
        out->throttle_pct = CAIRN_U8_UNKNOWN;
    }

    if (have_batch && batch_has(batch, CAIRN_FIELD_TIMING_ADVANCE_DEG)) {
        out->timing_advance_deg = batch->timing_deg;
        requested++; answered++;
    } else if (field_pid(CAIRN_FIELD_TIMING_ADVANCE_DEG, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->timing_advance_deg = sat_i8(v);
            answered++;
        } else {
            out->timing_advance_deg = CAIRN_I8_UNKNOWN;
            errors++;
        }
    } else {
        out->timing_advance_deg = CAIRN_I8_UNKNOWN;
    }

    out->fuel_pressure_kpa = CAIRN_U16_UNKNOWN;

    out->engine_load_pct = CAIRN_U8_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_ENGINE_LOAD_PCT, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->engine_load_pct = (uint8_t)v;
            answered++;
        } else {
            errors++;
        }
    }

    out->coolant_temp_c = CAIRN_I8_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_COOLANT_TEMP_C, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->coolant_temp_c = sat_i8(v);
            answered++;
        } else {
            errors++;
        }
    }

    out->intake_temp_c = CAIRN_I8_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_INTAKE_TEMP_C, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->intake_temp_c = sat_i8(v);
            answered++;
        } else {
            errors++;
        }
    }

    out->pids_requested  = requested;
    out->pids_answered   = answered;
    out->pid_error_count = errors;
    /* The cadence this record was actually taken at, which is the fast one only
     * when the batch was complete -- the same rule the scheduler uses. */
    out->poll_cadence_ms = batch_complete ? cairn_engine_params()->obd_batch_period_ms
                                          : cairn_engine_params()->obd_period_ms;

    return answered > 0;
}

uint16_t sensors_battery_mv(void)
{
    if (s_status == nullptr || !s_status->coprocessor) return CAIRN_U16_UNKNOWN;

    /*
     * On COBD rather than the system object, and documented to work without an
     * ECU — so this reads even with the ignition off, which is exactly when a
     * low-battery decision matters.
     *
     * Retried, because the first read after the coprocessor leaves low-power
     * mode comes back as zero: resetLink() returns the link but the coprocessor
     * needs a moment before it will answer. Returning UNKNOWN there loses the
     * single most valuable sample the firmware takes — the one from the standby
     * heartbeat, whose entire purpose is recording supply voltage on a parked
     * device. Three attempts, ~60 ms worst case, against a reading that governs
     * whether an OTA write is safe.
     */
    /*
     * Accepted only inside a range a vehicle supply can actually occupy.
     *
     * The guard used to be v > 0, which treats any positive number as a
     * measurement. That is the same mistake as decoding a sentinel: a
     * coprocessor that answers mid-warm-up with a fraction of a volt would be
     * recorded as a real reading, and because a low reading is exactly what
     * LOW_POWER and the OTA rail check look for, a warm-up artifact could flag
     * a healthy car as low and refuse an update that was safe — silently,
     * since the value looks plausible in isolation.
     *
     * The floor is above a warm-up artifact but below any voltage a running
     * car produces. The 2026-10-03 drive recorded a single 6.0 V reading
     * that passed the previous v >= 6.0 guard and triggered LOW_POWER on a
     * car with a 14.8 V alternator — the co-processor ADC glitched during
     * heavy bus traffic. A floor of 9 V rejects that class of artifact while
     * remaining well below cranking sag (~10 V on a tired battery). The
     * ceiling is above any charging voltage this alternator produces,
     * measured at 14.49..14.91 V. Anything outside is the link talking, not
     * the car, and absent is the honest answer.
     */
    for (int attempt = 0; attempt < 3; attempt++) {
        float v = s_obd.getVoltage();
        if (v >= 9.0f && v <= 18.0f) return (uint16_t)lroundf(v * 1000.0f);
        if (attempt < 2) delay(20);
    }

    return CAIRN_U16_UNKNOWN;
}

uint16_t sensors_supply_mv_raw(void)
{
    if (s_status == nullptr || !s_status->coprocessor) return 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        float v = s_obd.getVoltage();
        if (v > 0.1f && v < 25.0f) return (uint16_t)lroundf(v * 1000.0f);
        if (attempt < 2) delay(20);
    }
    return 0;
}

void sensors_gnss_power_down(void)
{
    if (s_status == NULL || !s_status->gnss) return;

    s_sys.gpsEnd(true);
    s_status->gnss = false;
    s_gps = NULL;

    /* Marked unavailable rather than merely idle, so the degraded-state bitmap
     * reports DEGRADED_GNSS honestly for as long as it is off. */
    CAIRN_LOGI(TAG, "GNSS powered down for standby");
}

void sensors_link_low_power(bool enable)
{
    if (s_status == NULL || !s_status->coprocessor) return;

    if (enable) {
        s_obd.enterLowPowerMode();
        s_status->obd = false;
        CAIRN_LOGI(TAG, "coprocessor in low-power mode");
        return;
    }

    /*
     * Coming back needs a link reset, not just a mode change: the coprocessor
     * does not resume a conversation it was not part of. This mirrors the
     * vendor's resetLink() on wake.
     */
    s_sys.resetLink();
    s_obd.leaveLowPowerMode();

    if (s_obd.init()) {
        s_status->obd = true;
        CAIRN_LOGI(TAG, "coprocessor back, ECU responding");
    } else {
        CAIRN_LOGW(TAG, "coprocessor back but the ECU is not answering "
                        "(ignition may still be off)");
    }
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

bool sensors_gnss_freshness(uint32_t *age_ms, uint8_t *sats)
{
    if (s_gps == nullptr || s_status == nullptr || !s_status->gnss) return false;

    /*
     * gpsData.ts is stamped with millis() each time the driver accepts a
     * reading, on both transports — unlike the NMEA sentence counters, which
     * only move on the direct-UART branch this board does not use.
     */
    if (s_gps->ts == 0) {
        *age_ms = UINT32_MAX;
    } else {
        *age_ms = millis() - s_gps->ts;
    }

    *sats = s_gps->sat;
    return true;
}

bool sensors_read_obd_extended(cairn_obd_extended_t *out,
                               const obd_batch_t *batch)
{
    if (s_status == nullptr || !s_status->obd) return false;

    memset(out, 0, sizeof(*out));

    uint32_t requested = 0, answered = 0;
    int      v = 0;
    byte     pid = 0;

    const bool have_batch = (batch != nullptr && batch->valid);
    const bool batch_complete = (batch != nullptr && batch->complete);
    const cairn_engine_profile_t *prof = cairn_engine_active();

    /* Manifold pressure: from the batch, else one request. */
    if (have_batch && batch_has(batch, CAIRN_FIELD_MAP_KPA)) {
        out->map_kpa = batch->map_kpa;
        if (out->map_kpa >= 250)
            CAIRN_LOGW(TAG, "MAP %u kPa — nearing PID 0x0B ceiling (255)",
                       (unsigned)out->map_kpa);
        requested++;
        answered++;
    } else if (field_pid(CAIRN_FIELD_MAP_KPA, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->map_kpa = (v < 0) ? 0 : (uint16_t)((v > 65534) ? 65534 : v);
            if (out->map_kpa >= 250)
                CAIRN_LOGW(TAG, "MAP %u kPa — nearing PID 0x0B ceiling (255)",
                           (unsigned)out->map_kpa);
            answered++;
        } else {
            out->map_kpa = CAIRN_U16_UNKNOWN;
        }
    } else {
        out->map_kpa = CAIRN_U16_UNKNOWN;
    }

    /*
     * Equivalence ratio, stored as lambda x 10^4. The scaling and the clamp are the
     * profile's formula over the raw 16 bits, so the batch and the single read agree
     * by construction.
     */
    if (have_batch && batch_has(batch, CAIRN_FIELD_LAMBDA_E4)) {
        out->lambda_e4 = batch->lambda_e4;
        requested++;
        answered++;
    } else if (field_pid(CAIRN_FIELD_LAMBDA_E4, &pid)) {
        requested++;
        out->lambda_e4 = CAIRN_U16_UNKNOWN;
        uint16_t raw = 0;
        if (pid_raw_u16(pid, &raw)) {
            const uint8_t in[4] = { (uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF), 0, 0 };
            int32_t e4 = 0;
            const cairn_pid_t *lp = cairn_engine_pid_for_field(prof, CAIRN_FIELD_LAMBDA_E4);
            if (cairn_engine_eval_pid(prof, lp, in, &e4) == CAIRN_EXPR_OK) {
                out->lambda_e4 = (uint16_t)e4;
                answered++;
            }
        }
    } else {
        out->lambda_e4 = CAIRN_U16_UNKNOWN;
    }

    /* Cold slot 3. MAF is largely redundant against MAP for judging a pull. */
    out->maf_cgps = CAIRN_U16_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_MAF_CGPS, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            long cg = (long)v * 100L;
            out->maf_cgps = (uint16_t)((cg > 65534L) ? 65534L : ((cg < 0) ? 0 : cg));
            answered++;
        }
    }

    /* Cold slot 4. Outside air; constant over a drive. */
    out->ambient_temp_c = CAIRN_I8_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_AMBIENT_TEMP_C, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->ambient_temp_c = sat_i8(v);
            answered++;
        }
    }

    out->fuel_trim_short_pct = CAIRN_I8_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_FUEL_TRIM_SHORT_PCT, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->fuel_trim_short_pct = sat_i8(v);
            answered++;
        }
    }

    out->fuel_trim_long_pct = CAIRN_I8_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_FUEL_TRIM_LONG_PCT, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->fuel_trim_long_pct = sat_i8(v);
            answered++;
        }
    }

    /*
     * Cold slots 7 and 8. Baro and absolute load were hot before the batch
     * path existed, which was the right call when every PID cost 120 ms and
     * MAP and lambda needed to be in the hot set. With the batch delivering
     * six PIDs in 56 ms, the bus budget that was spent on baro and abs_load
     * is better redirected into a faster overall cadence. Barometric pressure
     * is atmospheric and moves over hours; absolute load correlates with MAP,
     * which the batch already covers.
     */
    out->baro_kpa = 0xFF;
    if (cold_pid(CAIRN_FIELD_BARO_KPA, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->baro_kpa = (uint8_t)((v < 0) ? 0 : ((v > 254) ? 254 : v));
            answered++;
        }
    }

    out->abs_load_raw = CAIRN_U16_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_ABS_LOAD_RAW, &pid)) {
        requested++;
        {
            uint16_t raw = 0;
            if (pid_raw_u16(pid, &raw)) {
                out->abs_load_raw = raw;
                answered++;
            }
        }
    }

    /* Cold slot 9. Fuel tank level; slow-moving but useful for economy. */
    out->fuel_level_pct = CAIRN_U8_UNKNOWN;
    if (cold_pid(CAIRN_FIELD_FUEL_LEVEL_PCT, &pid)) {
        requested++;
        if (pid_value(pid, &v)) {
            out->fuel_level_pct = (uint8_t)((v < 0) ? 0 : ((v > 100) ? 100 : v));
            answered++;
        }
    }

    out->pids_requested = requested;
    out->pids_answered  = answered;
    /* The cadence this record was actually taken at, which is the fast one only
     * when the batch was complete -- the same rule the scheduler uses. */
    out->poll_cadence_ms = batch_complete ? cairn_engine_params()->obd_batch_period_ms
                                          : cairn_engine_params()->obd_period_ms;

    s_cold_phase = (uint8_t)((s_cold_phase + 1u) % cold_slots());

    return true;
}

/* ── PID validation accessors ─────────────────────────────────────────────── */

bool sensors_obd_ready(void)
{
    return s_status != nullptr && s_status->obd;
}

bool sensors_obd_pid_supported(uint8_t pid)
{
    return s_obd.isValidPID(pid);
}

uint8_t sensors_obd_pidmap_byte(uint8_t index)
{
    if (index >= sizeof(s_obd.pidmap)) return 0;
    return s_obd.pidmap[index];
}

bool sensors_obd_converted_pid(uint8_t pid, int *out)
{
    int v = 0;
    if (!s_obd.readPID(pid, v)) return false;
    *out = v;
    return true;
}

int sensors_obd_raw_pid(uint8_t pid, char *out, size_t cap)
{
    if (out == nullptr || cap == 0) return -1;
    out[0] = '\0';

    if (s_obd.link == nullptr) return -1;

    /*
     * Mode 01 request, issued straight down the link so normalizeData never
     * sees it. The point of this function is to capture what the ECU actually
     * sent, which is the only way to check the library's conversion rather
     * than assume it.
     */
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "01%02X\r", pid);

    char buf[128];
    if (s_obd.link->sendCommand(cmd, buf, sizeof(buf), OBD_TIMEOUT_SHORT) == 0) {
        return 0;
    }

    /*
     * Keep only the data bytes after the "41 <pid>" echo. A reply that does not
     * echo the mode and PID is not an answer to this question — NO DATA and
     * searching messages both land here — so it is reported as zero bytes
     * rather than parsed hopefully.
     */
    char want[8];
    snprintf(want, sizeof(want), "41 %02X", pid);

    const char *p = strstr(buf, want);
    if (p == nullptr) {
        snprintf(out, cap, "%s", buf[0] ? buf : "");
        return 0;
    }

    p += strlen(want);

    int bytes = 0;
    size_t n = 0;
    while (*p && n + 3 < cap) {
        if (*p == ' ' && isxdigit((unsigned char)p[1]) &&
            isxdigit((unsigned char)p[2])) {
            out[n++] = p[1];
            out[n++] = p[2];
            out[n++] = ' ';
            bytes++;
            p += 3;
            continue;
        }
        break;
    }

    out[n] = '\0';
    return bytes;
}

#if CAIRN_MTPROBE
/*
 * Link access for the manual-transmission probe.
 *
 * Kept as three narrow calls rather than a handle to the link, so the probe can
 * neither hold a reference across a reconnect nor reach anything but the OBD
 * conversation. All of it runs on the sensing task, the only task that talks to
 * the coprocessor, so these never interleave with another request.
 */
int sensors_obd_command(const char *cmd, char *buf, size_t cap, uint32_t timeout_ms)
{
    if (s_obd.link == nullptr || buf == nullptr || cap == 0) return 0;
    buf[0] = '\0';
    return s_obd.link->sendCommand(cmd, buf, (int)cap, timeout_ms);
}

/* Reads whatever the coprocessor is streaming, without sending anything. The UART
 * link's counter is a single byte, so callers keep cap at or under 255. */
int sensors_obd_receive(char *buf, size_t cap, uint32_t timeout_ms)
{
    if (s_obd.link == nullptr || buf == nullptr || cap == 0) return 0;
    buf[0] = '\0';
    return s_obd.link->receive(buf, (int)cap, timeout_ms);
}

/* Full re-initialisation of the ECU session. Slow when the ECU is silent; used
 * only to leave sniff mode when a plain request no longer works. */
bool sensors_obd_recover(void)
{
    return s_obd.init();
}
#endif /* CAIRN_MTPROBE */
