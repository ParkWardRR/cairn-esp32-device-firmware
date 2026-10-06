/*
 * Boot timing record: ordering, first-mark-wins, clock wrap, the console report, the
 * budget check that lets a regression fail without hardware, and the wire round trip.
 */

#include <stdio.h>
#include <string.h>

#include "cairn_boottime.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [boottime] %s\n", name); }       \
        else      { g_fail++; printf("  FAIL  [boottime] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

static void typical_boot(cairn_boottime_t *b)
{
    cairn_boottime_init(b, CAIRN_RESET_POWER_ON, 1000000u, 310000u);
    cairn_boottime_mark(b, CAIRN_BOOT_LOG_READY,       1000000u + 4000u);
    cairn_boottime_mark(b, CAIRN_BOOT_SD_MOUNTED,      1000000u + 180000u);
    cairn_boottime_mark(b, CAIRN_BOOT_STORE_READY,     1000000u + 420000u);
    cairn_boottime_mark(b, CAIRN_BOOT_BLE_ADVERTISING, 1000000u + 700000u);
    cairn_boottime_mark(b, CAIRN_BOOT_FIRST_SAMPLE,    1000000u + 950000u);
}

static void test_init_and_marks(void)
{
    cairn_boottime_t b;
    typical_boot(&b);

    CHECK("T0 is zero and reached", cairn_boottime_reached(&b, CAIRN_BOOT_APP_START) &&
                                    cairn_boottime_at(&b, CAIRN_BOOT_APP_START) == 0);
    CHECK("marks are relative to T0", cairn_boottime_at(&b, CAIRN_BOOT_SD_MOUNTED) == 180000u);
    CHECK("an unreached stage says so", !cairn_boottime_reached(&b, CAIRN_BOOT_FIRST_CHUNK) &&
                                        cairn_boottime_at(&b, CAIRN_BOOT_FIRST_CHUNK) == CAIRN_BOOT_NOT_REACHED);
    CHECK("mounting the card is remembered", b.sd_present);
    CHECK("the reset reason and pre-app time are kept", b.reset_reason == CAIRN_RESET_POWER_ON &&
                                                         b.pre_app_us == 310000u);
}

static void test_first_mark_wins(void)
{
    cairn_boottime_t b;
    typical_boot(&b);
    bool again = cairn_boottime_mark(&b, CAIRN_BOOT_BLE_ADVERTISING, 1000000u + 9000000u);
    CHECK("a second mark of a stage is refused", !again);
    CHECK("and keeps the boot-time value", cairn_boottime_at(&b, CAIRN_BOOT_BLE_ADVERTISING) == 700000u);
    CHECK("an out-of-range stage is refused", !cairn_boottime_mark(&b, CAIRN_BOOT_STAGE_COUNT, 5u));
}

static void test_clock_wrap(void)
{
    cairn_boottime_t b;
    cairn_boottime_init(&b, CAIRN_RESET_SOFTWARE, 0xFFFFF000u, 0);
    cairn_boottime_mark(&b, CAIRN_BOOT_FIRST_SAMPLE, 0x00000800u);   /* wrapped */
    CHECK("a 32-bit clock wrap between T0 and a mark gives the right delta",
          cairn_boottime_at(&b, CAIRN_BOOT_FIRST_SAMPLE) == 0x1800u);

    cairn_boottime_init(&b, CAIRN_RESET_SOFTWARE, 0, 0);
    cairn_boottime_mark(&b, CAIRN_BOOT_FIRST_SAMPLE, 0xFFFFFFFFu);
    CHECK("a delta that equals the sentinel is not mistaken for 'not reached'",
          cairn_boottime_reached(&b, CAIRN_BOOT_FIRST_SAMPLE));
}

static void test_report(void)
{
    cairn_boottime_t b;
    typical_boot(&b);

    char text[1024];
    size_t need = cairn_boottime_format(&b, text, sizeof text);
    CHECK("the report is not truncated for a normal boot", need > 0 && need < sizeof text &&
                                                          strlen(text) == need);
    CHECK("it names the reset reason", strstr(text, "reset=power_on") != NULL);
    CHECK("it shows the delta from the previous stage",
          strstr(text, "store_ready") != NULL && strstr(text, "(+240000)") != NULL);
    CHECK("stages never reached are not listed", strstr(text, "first_chunk") == NULL);

    char small[40];
    size_t need2 = cairn_boottime_format(&b, small, sizeof small);
    CHECK("a small buffer is NUL-terminated and reports the size it needed",
          need2 == need && strlen(small) < sizeof small);
    CHECK("a zero-length buffer is safe", cairn_boottime_format(&b, NULL, 0) == need);
}

/* A real boot, measured on the dongle: BLE advertised slightly BEFORE the first sample,
 * the reverse of the enum order. The report used to print the second with a delta of about
 * four billion. */
static void test_report_is_chronological(void)
{
    cairn_boottime_t b;
    cairn_boottime_init(&b, CAIRN_RESET_POWER_ON, 0, 530439u);
    cairn_boottime_mark(&b, CAIRN_BOOT_SD_MOUNTED,      540003u);
    cairn_boottime_mark(&b, CAIRN_BOOT_CAPTURE_OPEN,  53999897u);
    cairn_boottime_mark(&b, CAIRN_BOOT_BLE_ADVERTISING, 54918456u);
    cairn_boottime_mark(&b, CAIRN_BOOT_FIRST_SAMPLE,  54929151u);

    char text[1024];
    cairn_boottime_format(&b, text, sizeof text);
    const char *ble = strstr(text, "ble_advertising");
    const char *sample = strstr(text, "first_sample");
    CHECK("stages are listed in the order they were reached", ble && sample && ble < sample);
    CHECK("no delta is a wrapped negative", strstr(text, "(+42949") == NULL);
    CHECK("the delta after BLE is the real gap to the first sample", strstr(text, "(+10695)") != NULL);
}

static void test_budget(void)
{
    cairn_boottime_t b;
    typical_boot(&b);

    cairn_boot_budget_t budget;
    memset(&budget, 0, sizeof budget);
    budget.max_us[CAIRN_BOOT_FIRST_SAMPLE]    = 1000000u;
    budget.max_us[CAIRN_BOOT_BLE_ADVERTISING] = 800000u;

    cairn_boot_stage_t worst = CAIRN_BOOT_APP_START;
    CHECK("a boot inside its budget passes", cairn_boottime_check(&b, &budget, &worst));

    budget.max_us[CAIRN_BOOT_BLE_ADVERTISING] = 600000u;
    CHECK("a stage over budget fails and is named",
          !cairn_boottime_check(&b, &budget, &worst) && worst == CAIRN_BOOT_BLE_ADVERTISING);

    memset(&budget, 0, sizeof budget);
    budget.max_us[CAIRN_BOOT_FIRST_CHUNK] = 5000000u;
    CHECK("a budgeted stage that was never reached fails",
          !cairn_boottime_check(&b, &budget, &worst) && worst == CAIRN_BOOT_FIRST_CHUNK);

    memset(&budget, 0, sizeof budget);
    CHECK("no budget means no constraint", cairn_boottime_check(&b, &budget, NULL));
}

static void test_wire(void)
{
    cairn_boottime_t b, back;
    typical_boot(&b);

    uint8_t buf[CAIRN_BOOTTIME_WIRE_SIZE];
    CHECK("a short buffer is refused", cairn_boottime_encode(&b, buf, sizeof buf - 1) == 0);
    CHECK("the record has its fixed size", cairn_boottime_encode(&b, buf, sizeof buf) == sizeof buf);
    CHECK("version and stage count lead the record", buf[0] == 1 && buf[2] == CAIRN_BOOT_STAGE_COUNT);

    CHECK("it decodes", cairn_boottime_decode(&back, buf, sizeof buf));
    bool same = back.reset_reason == b.reset_reason && back.pre_app_us == b.pre_app_us &&
                back.sd_present == b.sd_present;
    for (int i = 0; i < CAIRN_BOOT_STAGE_COUNT; i++) same = same && back.at_us[i] == b.at_us[i];
    CHECK("and round-trips every field", same);

    /* A newer sender with more stages: the extra ones are ignored, none are corrupted. */
    uint8_t longer[CAIRN_BOOTTIME_WIRE_SIZE + 8];
    memset(longer, 0xAB, sizeof longer);
    memcpy(longer, buf, sizeof buf);
    longer[2] = CAIRN_BOOT_STAGE_COUNT + 2;
    CHECK("a record with extra stages still decodes the ones it knows",
          cairn_boottime_decode(&back, longer, sizeof longer) &&
          back.at_us[CAIRN_BOOT_FIRST_SAMPLE] == b.at_us[CAIRN_BOOT_FIRST_SAMPLE]);

    /* An older sender with fewer stages: the rest stay unreached. */
    uint8_t shorter[8 + 4 * 3];
    memcpy(shorter, buf, sizeof shorter);
    shorter[2] = 3;
    CHECK("a record with fewer stages leaves the rest unreached",
          cairn_boottime_decode(&back, shorter, sizeof shorter) &&
          !cairn_boottime_reached(&back, CAIRN_BOOT_FIRST_SAMPLE));

    buf[0] = 2;
    CHECK("an unknown version is refused", !cairn_boottime_decode(&back, buf, sizeof buf));
    buf[0] = 1;
    CHECK("a truncated record is refused", !cairn_boottime_decode(&back, buf, 20));
}

int main(void)
{
    test_init_and_marks();
    test_first_mark_wins();
    test_clock_wrap();
    test_report();
    test_report_is_chronological();
    test_budget();
    test_wire();

    printf("boot timing: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
