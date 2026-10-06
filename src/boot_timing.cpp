#include "boot_timing.h"

#include <Arduino.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "cairn_log.h"

static const char *TAG = "BOOTTIME";

static cairn_boottime_t   s_rec;
static volatile uint32_t  s_done_mask = 0;   /* bit i set once stage i is recorded */
static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool               s_logged = false;
static uint8_t            s_reset_code = 0;

static cairn_reset_reason_t map_reason(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return CAIRN_RESET_POWER_ON;
    case ESP_RST_SW:        return CAIRN_RESET_SOFTWARE;
    case ESP_RST_PANIC:     return CAIRN_RESET_PANIC;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return CAIRN_RESET_WATCHDOG;
    case ESP_RST_BROWNOUT:  return CAIRN_RESET_BROWNOUT;
    case ESP_RST_DEEPSLEEP: return CAIRN_RESET_DEEP_SLEEP_WAKE;
    case ESP_RST_EXT:       return CAIRN_RESET_EXTERNAL;
    default:                return CAIRN_RESET_UNKNOWN;
    }
}

void boot_timing_begin(void)
{
    uint32_t now = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    /* esp_timer starts at reset, so its value at T0 is the time the ROM and bootloader took. */
    s_reset_code = (uint8_t)esp_reset_reason();
    cairn_boottime_init(&s_rec, map_reason(esp_reset_reason()), now, now);
    s_done_mask = 1u << CAIRN_BOOT_APP_START;
    portEXIT_CRITICAL(&s_lock);
}

void boot_timing_mark(cairn_boot_stage_t stage)
{
    if ((unsigned)stage >= CAIRN_BOOT_STAGE_COUNT) return;
    if (s_done_mask & (1u << stage)) return;

    uint32_t now = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    cairn_boottime_mark(&s_rec, stage, now);
    s_done_mask |= 1u << stage;
    portEXIT_CRITICAL(&s_lock);
}

uint8_t boot_timing_reset_code(void) { return s_reset_code; }

void boot_timing_snapshot(cairn_boottime_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_rec;
    portEXIT_CRITICAL(&s_lock);
}

void boot_timing_log(void)
{
    if (s_logged) return;
    s_logged = true;

    cairn_boottime_t snap;
    boot_timing_snapshot(&snap);

    char text[640];
    cairn_boottime_format(&snap, text, sizeof text);

    /* One log line per record line, so the SD log and the console both carry them. */
    for (char *line = text; *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        CAIRN_LOGI(TAG, "%s", line);
        if (!nl) break;
        line = nl + 1;
    }
}
