/*
 * The execution half: powering peripherals down and polling for a reason to
 * come back.
 *
 * Lives in src/ rather than beside the header in lib/cairn_power because it
 * drives the sensors, and sensors.h is application code. The decisions stay in
 * lib/cairn_power/cairn_power.c, which depends on nothing outside the libraries
 * and is therefore reachable by the host tests.
 *
 * The order here follows the official Freematics standby() closely, because it
 * is the sequence that is known to work on this hardware — GNSS off, then the
 * coprocessor into its own low-power mode, then poll. Two things are added that
 * the vendor does not do: the CPU is clocked down, and the core light-sleeps
 * between polls instead of spinning in delay().
 */

#include "cairn_power.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include "cairn_log.h"
#include "config.h"
#include "sensors.h"
#include "sensor_task.h"

static const char *TAG = "PWR";

void cairn_power_standby(cairn_power_result_t *out)
{
    uint32_t entered = millis();
    uint32_t polls = 0;
    cairn_wake_reason_t reason = CAIRN_WAKE_NONE;
    uint16_t wake_mv = CAIRN_U16_UNKNOWN;

    CAIRN_LOGW(TAG, "entering standby");

    /*
     * Radio first and explicitly. WiFi.mode(WIFI_OFF) powers the radio down
     * rather than merely disassociating, and it is the largest single consumer
     * on the board.
     */
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    /*
     * GNSS next, with power off. The receiver draws continuously and has
     * nothing to track while parked; it reacquires in seconds on wake, which a
     * stationary vehicle can afford.
     */
    sensors_gnss_power_down();

    /*
     * Then the coprocessor, via the vendor's own ATLP path. This covers the OBD
     * interface and, because the internal receiver sits behind the same link,
     * anything GNSS still had powered.
     */
    sensors_link_low_power(true);

    /* Nothing drains the fact queue while the controller sleeps, so sampling
     * into it would only overflow and be counted as dropped facts. */
    sensor_task_set_paused(true);

    /* Clocked down only after the peripherals that care are quiet. */
    uint32_t original_mhz = getCpuFrequencyMhz();
    if (!setCpuFrequencyMhz(CAIRN_STANDBY_CPU_MHZ)) {
        CAIRN_LOGW(TAG, "could not clock down from %u MHz; standing by at full "
                        "speed", (unsigned)original_mhz);
    }

    cairn_log_flush();

    /*
     * Light sleep, not deep: RAM and the lifecycle state survive, so waking
     * costs nothing but a few milliseconds. Entered only now that the
     * coprocessor link is idle — a light sleep with a UART mid-transfer drops
     * characters.
     */
    esp_sleep_enable_timer_wakeup((uint64_t)CAIRN_STANDBY_POLL_MS * 1000ULL);

    for (;;) {
        esp_light_sleep_start();
        polls++;

        /*
         * The accelerometer is the only thing sampled on most iterations. It is
         * on I2C and survives light sleep, so this needs no re-initialisation.
         *
         * Read directly rather than through the windowed accumulator: the
         * sensing task owns that window, and calling into it from here raced
         * on the shared count and sum_sq, producing RMS values large enough to
         * look like motion. The sensing task is paused for the duration
         * anyway.
         */
        uint16_t rms = sensors_accel_magnitude_mg();

        /*
         * Voltage goes through the coprocessor, which is in low-power mode, so
         * it is read sparingly rather than every poll — the read itself wakes
         * the link briefly.
         */
        uint16_t mv = CAIRN_U16_UNKNOWN;
        if (polls % 10 == 0) mv = sensors_battery_mv();

        uint32_t elapsed = millis() - entered;
        reason = cairn_power_should_wake(rms, mv, elapsed,
                                        CAIRN_MOTION_ACCEL_RMS_MG);
        if (reason != CAIRN_WAKE_NONE) {
            wake_mv = mv;
            break;
        }
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    /* Restore in reverse. */
    setCpuFrequencyMhz(original_mhz);
    sensor_task_set_paused(false);
    sensors_link_low_power(false);

    out->standby_ms = millis() - entered;
    out->polls = polls;
    out->wake_reason = reason;
    out->wake_battery_mv = wake_mv;

    CAIRN_LOGW(TAG, "woke after %u ms and %u polls: %s", (unsigned)out->standby_ms,
               (unsigned)polls, cairn_wake_reason_name(reason));
}
