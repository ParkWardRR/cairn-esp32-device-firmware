#include "cairn_boottime.h"

#include <stdio.h>
#include <string.h>

static const char *const STAGE_NAMES[CAIRN_BOOT_STAGE_COUNT] = {
    "app_start", "log_ready", "sd_mounted", "store_ready", "obd_first_answer",
    "gnss_first_fix", "first_sample", "ble_advertising", "first_chunk", "capture_open", "sensors_ready",
};

static const char *const RESET_NAMES[] = {
    "unknown", "power_on", "software", "panic", "watchdog", "brownout",
    "deep_sleep_wake", "external",
};

const char *cairn_boottime_stage_name(cairn_boot_stage_t s)
{
    return ((unsigned)s < CAIRN_BOOT_STAGE_COUNT) ? STAGE_NAMES[s] : "?";
}

const char *cairn_boottime_reset_name(cairn_reset_reason_t r)
{
    return ((unsigned)r < sizeof RESET_NAMES / sizeof RESET_NAMES[0]) ? RESET_NAMES[r] : "?";
}

void cairn_boottime_init(cairn_boottime_t *b, cairn_reset_reason_t reason,
                         uint32_t now_us, uint32_t since_reset_us)
{
    memset(b, 0, sizeof *b);
    for (int i = 0; i < CAIRN_BOOT_STAGE_COUNT; i++) b->at_us[i] = CAIRN_BOOT_NOT_REACHED;
    b->reset_reason = reason;
    b->t0_us        = now_us;
    b->pre_app_us   = since_reset_us;
    b->at_us[CAIRN_BOOT_APP_START] = 0;
}

bool cairn_boottime_mark(cairn_boottime_t *b, cairn_boot_stage_t s, uint32_t now_us)
{
    if ((unsigned)s >= CAIRN_BOOT_STAGE_COUNT) return false;
    if (b->at_us[s] != CAIRN_BOOT_NOT_REACHED) return false;

    /* Modular subtraction: the platform clock is truncated to 32 bits and a boot is
     * seconds long, so a wrap between T0 and a mark still yields the right delta. A
     * delta that lands on the sentinel would read as "not reached"; nudge it down by
     * one microsecond rather than lose the stage. */
    uint32_t d = now_us - b->t0_us;
    if (d == CAIRN_BOOT_NOT_REACHED) d--;
    b->at_us[s] = d;

    if (s == CAIRN_BOOT_SD_MOUNTED) b->sd_present = true;
    return true;
}

bool cairn_boottime_reached(const cairn_boottime_t *b, cairn_boot_stage_t s)
{
    return (unsigned)s < CAIRN_BOOT_STAGE_COUNT && b->at_us[s] != CAIRN_BOOT_NOT_REACHED;
}

uint32_t cairn_boottime_at(const cairn_boottime_t *b, cairn_boot_stage_t s)
{
    return ((unsigned)s < CAIRN_BOOT_STAGE_COUNT) ? b->at_us[s] : CAIRN_BOOT_NOT_REACHED;
}

/* snprintf-style append: `used` counts what the whole text needs, which can exceed `cap`. */
static void append(char *out, size_t cap, size_t *used, const char *line, int n)
{
    if (n <= 0) return;
    if (*used < cap) snprintf(out + *used, cap - *used, "%s", line);
    *used += (size_t)n;
}

size_t cairn_boottime_format(const cairn_boottime_t *b, char *out, size_t cap)
{
    size_t used = 0;
    char   line[96];
    int    n;

    if (cap > 0) out[0] = '\0';

    n = snprintf(line, sizeof line, "boot: reset=%s pre_app=%u us sd=%s\n",
                 cairn_boottime_reset_name(b->reset_reason), (unsigned)b->pre_app_us,
                 b->sd_present ? "yes" : "no");
    append(out, cap, &used, line, n);

    /* Stages are listed in the order they were reached, not enum order: a boot does not
     * reach them in enum order (BLE can advertise before the first sample, or after), and
     * a delta against the wrong neighbour is meaningless. Ties keep enum order. */
    bool done[CAIRN_BOOT_STAGE_COUNT] = { false };
    uint32_t prev = 0;
    for (int printed = 0; printed < CAIRN_BOOT_STAGE_COUNT; printed++) {
        int next = -1;
        for (int i = 0; i < CAIRN_BOOT_STAGE_COUNT; i++) {
            if (done[i] || b->at_us[i] == CAIRN_BOOT_NOT_REACHED) continue;
            if (next < 0 || b->at_us[i] < b->at_us[next]) next = i;
        }
        if (next < 0) break;
        done[next] = true;
        n = snprintf(line, sizeof line, "boot: %-16s %9u us  (+%u)\n", STAGE_NAMES[next],
                     (unsigned)b->at_us[next], (unsigned)(b->at_us[next] - prev));
        append(out, cap, &used, line, n);
        prev = b->at_us[next];
    }
    return used;
}

bool cairn_boottime_check(const cairn_boottime_t *b, const cairn_boot_budget_t *budget,
                          cairn_boot_stage_t *worst)
{
    for (int i = 0; i < CAIRN_BOOT_STAGE_COUNT; i++) {
        if (budget->max_us[i] == 0) continue;
        if (b->at_us[i] == CAIRN_BOOT_NOT_REACHED || b->at_us[i] > budget->max_us[i]) {
            if (worst) *worst = (cairn_boot_stage_t)i;
            return false;
        }
    }
    return true;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t cairn_boottime_encode(const cairn_boottime_t *b, uint8_t *out, size_t cap)
{
    if (cap < CAIRN_BOOTTIME_WIRE_SIZE) return 0;
    out[0] = CAIRN_BOOTTIME_WIRE_VERSION;
    out[1] = (uint8_t)b->reset_reason;
    out[2] = (uint8_t)CAIRN_BOOT_STAGE_COUNT;
    out[3] = b->sd_present ? 1u : 0u;
    put_le32(out + 4, b->pre_app_us);
    for (int i = 0; i < CAIRN_BOOT_STAGE_COUNT; i++) put_le32(out + 8 + 4 * i, b->at_us[i]);
    return CAIRN_BOOTTIME_WIRE_SIZE;
}

bool cairn_boottime_decode(cairn_boottime_t *b, const uint8_t *in, size_t len)
{
    if (len < 8 || in[0] != CAIRN_BOOTTIME_WIRE_VERSION) return false;
    size_t stages = in[2];
    if (len < 8 + 4 * stages) return false;

    cairn_boottime_init(b, (cairn_reset_reason_t)in[1], 0, get_le32(in + 4));
    b->sd_present = (in[3] & 1u) != 0;
    for (size_t i = 0; i < stages && i < CAIRN_BOOT_STAGE_COUNT; i++)
        b->at_us[i] = get_le32(in + 8 + 4 * i);
    return true;
}
