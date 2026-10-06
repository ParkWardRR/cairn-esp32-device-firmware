/*
 * Boot timing record.
 *
 * Boot speed and speed to upload are owner priorities, and until now every claim
 * about them was a guess. This is the instrument: a small monotonic record of when
 * each boot stage was reached, measured from T0, kept in RAM, printed on the console,
 * and readable as a fixed-size blob for the device-info characteristic.
 *
 * Definitions (they are written down so the numbers are comparable between builds):
 *
 *   T0          the moment the application's first line runs. Time spent in the ROM
 *               and the bootloader before that is invisible to the application, so
 *               it is reported separately as `pre_app_us` when the platform can
 *               provide it (esp_timer starts at reset, so it can).
 *   T_capture   CAIRN_BOOT_FIRST_SAMPLE: the first sample is logged.
 *   T_ble       CAIRN_BOOT_BLE_ADVERTISING: the phone can find the dongle.
 *   T_uplink    CAIRN_BOOT_FIRST_CHUNK: the first chunk of a bundle is accepted.
 *
 * Wi-Fi never sits on the boot path (issue #17, #19): this record exists so that adding
 * the Wi-Fi code cannot silently regress T_capture and T_ble, and so that a regression
 * is visible where it can be measured without hardware (cairn_boottime_check).
 *
 * The wire layout below is PROVISIONAL. The device-info contract (contracts/ble/v1,
 * upstream issue 21) is not released; when it is, the encoder here moves to match it
 * and the golden vectors take over from the layout test. Nothing else depends on the
 * byte layout.
 *
 * Portable C with the clock passed in, so the logic is host-testable; the Arduino glue
 * supplies esp_timer_get_time().
 */

#ifndef CAIRN_BOOTTIME_H
#define CAIRN_BOOTTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stages in the order a normal boot reaches them. Append only: the numbers are the
 * index into the wire record. */
typedef enum {
    CAIRN_BOOT_APP_START = 0,     /* T0 */
    CAIRN_BOOT_LOG_READY,         /* logging up (UART + RAM ring) */
    CAIRN_BOOT_SD_MOUNTED,
    CAIRN_BOOT_STORE_READY,       /* NVS and counters loaded, recovery scans done */
    CAIRN_BOOT_OBD_FIRST_ANSWER,
    CAIRN_BOOT_GNSS_FIRST_FIX,
    CAIRN_BOOT_FIRST_SAMPLE,      /* T_capture */
    CAIRN_BOOT_BLE_ADVERTISING,   /* T_ble */
    CAIRN_BOOT_FIRST_CHUNK,       /* T_uplink (per path: the first one reached) */
    CAIRN_BOOT_CAPTURE_OPEN,      /* a capture bundle is open: able to start a capture */
    CAIRN_BOOT_SENSORS_READY,     /* coprocessor, GNSS and IMU brought up (sensors_begin returned) */
    CAIRN_BOOT_STAGE_COUNT
} cairn_boot_stage_t;

/* Why the chip reset. Mirrors the platform's reasons without depending on them. */
typedef enum {
    CAIRN_RESET_UNKNOWN = 0,
    CAIRN_RESET_POWER_ON,
    CAIRN_RESET_SOFTWARE,
    CAIRN_RESET_PANIC,
    CAIRN_RESET_WATCHDOG,
    CAIRN_RESET_BROWNOUT,
    CAIRN_RESET_DEEP_SLEEP_WAKE,
    CAIRN_RESET_EXTERNAL
} cairn_reset_reason_t;

/* A stage that has not been reached holds this. */
#define CAIRN_BOOT_NOT_REACHED UINT32_MAX

typedef struct {
    cairn_reset_reason_t reset_reason;
    uint32_t pre_app_us;                       /* reset to T0, 0 when not known */
    uint32_t at_us[CAIRN_BOOT_STAGE_COUNT];    /* microseconds after T0 */
    uint32_t t0_us;                            /* the platform clock at T0 */
    bool     sd_present;                       /* a card was mounted this boot */
} cairn_boottime_t;

/* Start a record. `now_us` is the platform clock at T0 and `since_reset_us` is how long
 * the chip had already been running (esp_timer_get_time() at that moment), 0 if unknown. */
void cairn_boottime_init(cairn_boottime_t *b, cairn_reset_reason_t reason,
                         uint32_t now_us, uint32_t since_reset_us);

/* Record that a stage was reached. The FIRST call wins: a stage reached twice (BLE
 * advertising resumed after a drive) keeps its boot-time value, because the question
 * being answered is how long boot took. Returns true if this call recorded it. */
bool cairn_boottime_mark(cairn_boottime_t *b, cairn_boot_stage_t s, uint32_t now_us);

bool     cairn_boottime_reached(const cairn_boottime_t *b, cairn_boot_stage_t s);

/* Microseconds from T0 to the stage, or CAIRN_BOOT_NOT_REACHED. */
uint32_t cairn_boottime_at(const cairn_boottime_t *b, cairn_boot_stage_t s);

const char *cairn_boottime_stage_name(cairn_boot_stage_t s);
const char *cairn_boottime_reset_name(cairn_reset_reason_t r);

/*
 * One line per stage reached, with the delta from the previous reached stage, which is
 * what shows the biggest term. Writes at most `cap` bytes including the NUL and returns
 * the length it would have needed (snprintf semantics).
 */
size_t cairn_boottime_format(const cairn_boottime_t *b, char *out, size_t cap);

/* ── budgets ──────────────────────────────────────────────────────────────── */

/* A ceiling per stage in microseconds; 0 means unconstrained. */
typedef struct {
    uint32_t max_us[CAIRN_BOOT_STAGE_COUNT];
} cairn_boot_budget_t;

/*
 * True when every stage that has a budget was reached within it. The first stage
 * that is over, or never reached though budgeted, is returned in *worst (may be NULL).
 * This is what lets a regression fail a test without hardware: a host test feeds it a
 * recorded boot and the budget committed in docs/.
 */
bool cairn_boottime_check(const cairn_boottime_t *b, const cairn_boot_budget_t *budget,
                          cairn_boot_stage_t *worst);

/* ── fixed wire record (provisional, little-endian) ───────────────────────── */

/*
 *   0   u8   record version (1)
 *   1   u8   reset reason
 *   2   u8   stage count (the encoder's; a reader ignores stages it does not know)
 *   3   u8   flags: bit0 = SD present
 *   4   u32  pre_app_us
 *   8   u32  at_us for each stage, in enum order (0xFFFFFFFF = not reached)
 */
#define CAIRN_BOOTTIME_WIRE_VERSION 1
#define CAIRN_BOOTTIME_WIRE_SIZE    (8 + 4 * CAIRN_BOOT_STAGE_COUNT)

/* Returns bytes written, 0 if `cap` is too small. */
size_t cairn_boottime_encode(const cairn_boottime_t *b, uint8_t *out, size_t cap);

/* Returns false on a wrong version or a short buffer. Stages beyond the ones this build
 * knows are skipped; stages the sender did not have are left unreached. */
bool cairn_boottime_decode(cairn_boottime_t *b, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_BOOTTIME_H */
