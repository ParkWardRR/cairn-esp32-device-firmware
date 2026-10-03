/*
 * PID validation build. Compiled only when CAIRN_PIDTEST is defined.
 *
 * This exists because the boost and mixture record was written against
 * assumptions about what the vendor library returns, and two of those
 * assumptions were wrong — the equivalence ratio arrives on a 0..200 scale
 * rather than as the raw 16-bit ratio, and absolute load is read as one byte
 * through a helper that caps it at 100%. Both were found by reading
 * normalizeData rather than by testing, which means the rest of the set is
 * equally unproven.
 *
 * So rather than trust a second round of reasoning, this prints three things
 * side by side for every PID of interest:
 *
 *   1. whether the ECU claims to support it, from the Mode 01 bitmaps
 *   2. the raw bytes the ECU actually returned
 *   3. the value the library produced after its own conversion
 *
 * With the raw bytes in hand the conversion can be checked against the
 * standard formula instead of inferred. The build still captures normally, so
 * one drive yields both the diagnostic and a usable bundle.
 */

#include "config.h"

#if CAIRN_PIDTEST

#include <Arduino.h>
#include <FreematicsPlus.h>

#include "cairn_log.h"
#include "pidtest.h"
#include "sensors.h"

static const char *TAG = "PIDTEST";

static uint32_t s_next_ms;
static bool     s_dumped_map;

/*
 * The PIDs the boost and mixture record depends on, plus the ones already in
 * OBD_SNAPSHOT so a single pass covers everything the firmware reads.
 *
 * `std_bytes` is what SAE J1979 specifies, carried here so the raw reply can be
 * checked for length. A reply shorter than the standard says is the signature of
 * a library helper reading half a value, which is exactly the absolute-load bug.
 */
typedef struct {
    uint8_t     pid;
    const char *name;
    uint8_t     std_bytes;
    const char *std_formula;
} pid_probe_t;

static const pid_probe_t k_probes[] = {
    /* Already recorded in OBD_SNAPSHOT. */
    { 0x04, "ENGINE_LOAD",      1, "A*100/255 %" },
    { 0x05, "COOLANT_TEMP",     1, "A-40 C" },
    { 0x0A, "FUEL_PRESSURE",    1, "A*3 kPa" },
    { 0x0C, "RPM",              2, "((A*256)+B)/4 rpm" },
    { 0x0D, "SPEED",            1, "A km/h" },
    { 0x0E, "TIMING_ADVANCE",   1, "(A/2)-64 deg" },
    { 0x0F, "INTAKE_TEMP",      1, "A-40 C" },
    { 0x11, "THROTTLE",         1, "A*100/255 %" },

    /* The boost and mixture set, where the assumptions are unproven. */
    { 0x0B, "INTAKE_MAP",       1, "A kPa absolute" },
    { 0x33, "BAROMETRIC",       1, "A kPa" },
    { 0x10, "MAF_FLOW",         2, "((A*256)+B)/100 g/s" },
    { 0x44, "EQUIV_RATIO",      2, "((A*256)+B)/32768 lambda" },
    { 0x43, "ABSOLUTE_LOAD",    2, "((A*256)+B)*100/255 %" },
    { 0x46, "AMBIENT_TEMP",     1, "A-40 C" },
    { 0x06, "FUEL_TRIM_SHORT",  1, "(A-128)*100/128 %" },
    { 0x07, "FUEL_TRIM_LONG",   1, "(A-128)*100/128 %" },

    /*
     * Probed because 0x0B saturates at 255 kPa absolute — about 22.3 psi gauge
     * — which is inside the range a tuned car runs in, so the primary boost
     * signal may flatten exactly where it is being judged.
     *
     * 0x4F byte D declares the vehicle's own manifold-pressure maximum as
     * D * 10 kPa, which is the standard way to learn the real ceiling instead
     * of assuming 255. 0x87 is defined as intake manifold absolute pressure
     * with a wider range, but its scaling could not be sourced with
     * confidence, so this prints the raw bytes and nothing is read from it yet.
     * 0x70 is included for the same reason.
     *
     * 0x04 sits beside 0x43 deliberately: calculated load really is one byte
     * and 0..100%, and conflating the two is the likely origin of the vendor
     * library reading absolute load a byte at a time.
     */
    /*
     * Fuel composition. On a blended tank these two decide how the mixture
     * data can be read at all: stoichiometric air-fuel ratio falls from about
     * 14.7:1 on gasoline to roughly 9.8:1 on E85, so an AFR figure is
     * meaningless without knowing the blend. Lambda is recorded instead
     * precisely because it is blend-independent — 0.85 is fifteen per cent rich
     * whatever is in the tank — but converting it to an AFR for display needs
     * the ethanol fraction, and 0x52 is the standard place to ask for it.
     *
     * Whether an N20 answers is doubtful without a flex-fuel sensor, which is
     * exactly why this prints rather than assumes.
     */
    { 0x51, "FUEL_TYPE",        1, "enum; 23 = diesel, 1 = gasoline" },
    { 0x52, "ETHANOL_PCT",      1, "A*100/255 %" },

    { 0x4F, "MAX_VALUES",       4, "D*10 kPa = declared IMAP max" },
    { 0x87, "IMAP_WIDE",        5, "unverified; raw bytes only" },
    { 0x70, "BOOST_CONTROL",    5, "0-2047.97 kPa; x0.03125 per bit" },
};

/*
 * Dump the Mode 01 support bitmaps the library collected during init().
 *
 * Read with one caveat that matters: init() memsets pidmap to 0xff before
 * querying, so every PID reads as supported until a reply corrects it, and the
 * query loop breaks on the first failure. A map of all-ones therefore means
 * "nothing was learned" rather than "everything is supported", and the honest
 * reading of a fully-set map is that the bitmaps were never answered.
 */
static void dump_support_map(void)
{
    char line[96];
    int  n = 0;

    CAIRN_LOGI(TAG, "Mode 01 support bitmaps, as collected at init:");

    for (int i = 0; i < 8; i++) {
        n = snprintf(line, sizeof(line), "  PIDs %02X-%02X:",
                     (unsigned)(i * 0x20 + 1), (unsigned)(i * 0x20 + 0x20));
        for (int b = 0; b < 4; b++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %02X",
                          sensors_obd_pidmap_byte((uint8_t)(i * 4 + b)));
        }
        CAIRN_LOGI(TAG, "%s", line);
    }

    CAIRN_LOGW(TAG, "all-FF above means the bitmaps were never answered, not "
                    "that every PID is supported — init() defaults the map to "
                    "0xff and only corrects what replies");
}

void pidtest_tick(void)
{
    uint32_t now = millis();
    if ((int32_t)(now - s_next_ms) < 0) return;
    s_next_ms = now + CAIRN_PIDTEST_PERIOD_MS;

    if (!sensors_obd_ready()) {
        CAIRN_LOGW(TAG, "ECU not answering; nothing to probe. Turn the ignition "
                        "on — this prints once per interval until it does.");
        return;
    }

    if (!s_dumped_map) {
        dump_support_map();
        s_dumped_map = true;
    }

    CAIRN_LOGI(TAG, "---- PID probe ----");

    for (size_t i = 0; i < sizeof(k_probes) / sizeof(k_probes[0]); i++) {
        const pid_probe_t *p = &k_probes[i];

        char raw[64];
        raw[0] = '\0';
        int  raw_bytes = sensors_obd_raw_pid(p->pid, raw, sizeof(raw));

        int  conv = 0;
        bool got  = sensors_obd_converted_pid(p->pid, &conv);

        /*
         * Printed on one line per PID so a drive log can be read with grep.
         * The raw reply is the authoritative part: everything else is either a
         * claim (support) or an interpretation (converted).
         */
        CAIRN_LOGI(TAG, "%02X %-17s support=%d raw=[%s] bytes=%d/%d conv=%s%d  std: %s",
                   (unsigned)p->pid, p->name,
                   sensors_obd_pid_supported(p->pid) ? 1 : 0,
                   raw[0] ? raw : "no reply",
                   raw_bytes, (int)p->std_bytes,
                   got ? "" : "(none) ", conv,
                   p->std_formula);
    }

    cairn_log_flush();
}

#endif /* CAIRN_PIDTEST */
