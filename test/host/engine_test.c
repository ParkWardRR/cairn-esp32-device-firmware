/*
 * Host test for lib/cairn_engine: the expression evaluator against the shared
 * vectors, and the generated N20 tables against the values the firmware hard-coded
 * before profiles existed.
 *
 * The "legacy" functions below are copies of that behaviour, taken from
 * src/sensors.cpp (sensors_read_obd_batch, sensors_read_obd, sensors_read_obd_extended)
 * and the vendored COBD::normalizeData as of commit 28fc13e, and kept here as the
 * oracle. The N20 tables must reproduce them over EVERY possible reply (all 256 values
 * of a one-byte PID, all 65536 of a two-byte PID), not a sample.
 *
 *   engine_test <expr-vectors.txt> <engines-dir> [expected-selection]
 *
 * It runs against whichever tables the build carries: the committed all-engines
 * header, or, with the third argument, a generated selection (make engines-check
 * does one-engine builds this way). Checks that need an engine that is not installed
 * are skipped and said to be.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_engine.h"
#include "cairn_format.h"
#include "config.h"

/* The vendored library's PID numbers, from the real header rather than retyped. */
#include "../../third_party/freematics-base/lib/FreematicsPlus/utility/OBD.h"

static int g_checks;
static int g_fails;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_fails++;                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        long long a_ = (long long)(a), b_ = (long long)(b);                      \
        g_checks++;                                                              \
        if (a_ != b_) {                                                          \
            g_fails++;                                                           \
            fprintf(stderr, "FAIL %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__,  \
                    __LINE__, #a, #b, a_, b_);                                   \
        }                                                                        \
    } while (0)

/* ── the expression vectors ───────────────────────────────────────────────── */

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int    hi = -1;
    for (; *s; s++) {
        int d;
        if (isspace((unsigned char)*s)) continue;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return (size_t)-1;
        if (hi < 0) {
            hi = d;
        } else {
            if (n >= cap) return (size_t)-1;
            out[n++] = (uint8_t)(hi << 4 | d);
            hi = -1;
        }
    }
    return hi < 0 ? n : (size_t)-1;
}

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void parse_inputs(const char *s, uint8_t in[4])
{
    char *end;
    int   i = 0;
    memset(in, 0, 4);
    while (i < 4) {
        long v = strtol(s, &end, 10);
        if (end == s) break;
        in[i++] = (uint8_t)v;
        s = end;
    }
}

static void run_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    char  line[1024];
    int   lineno = 0, ran = 0;

    if (f == NULL) {
        fprintf(stderr, "cannot open the expression vectors %s\n", path);
        g_fails++;
        return;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char *fld[8];
        int   nf = 0;
        char *p = line;
        char *t;

        lineno++;
        t = trim(line);
        if (*t == '\0' || *t == '#') continue;

        /* Split on ';' (expressions never contain one). */
        fld[nf++] = t;
        for (p = t; *p && nf < 8; p++) {
            if (*p == ';') {
                *p = '\0';
                fld[nf++] = p + 1;
            }
        }
        {
            int i;
            for (i = 0; i < nf; i++) fld[i] = trim(fld[i]);
        }

        if (fld[0][0] == 'X') continue; /* compile-time rejections: the Rust side's */

        {
            uint8_t code[256], in[4];
            size_t  len;
            int32_t v = 0;
            cairn_expr_status_t st;
            const char *expect;

            if (fld[0][0] == 'E' && nf == 6) {
                len = unhex(fld[5], code, sizeof(code));
                parse_inputs(fld[3], in);
                expect = fld[4];
            } else if (fld[0][0] == 'B' && nf == 4) {
                len = unhex(fld[1], code, sizeof(code));
                parse_inputs(fld[2], in);
                expect = fld[3];
            } else {
                fprintf(stderr, "vectors line %d: unrecognised\n", lineno);
                g_fails++;
                continue;
            }
            if (len == (size_t)-1) {
                fprintf(stderr, "vectors line %d: bad hex\n", lineno);
                g_fails++;
                continue;
            }

            st = cairn_expr_eval(code, len, in, &v);
            ran++;
            g_checks++;
            if (strncmp(expect, "ERR_", 4) == 0) {
                if (strcmp(cairn_expr_status_name(st), expect) != 0) {
                    g_fails++;
                    fprintf(stderr, "vectors line %d: got %s, want %s\n", lineno,
                            cairn_expr_status_name(st), expect);
                }
            } else if (st != CAIRN_EXPR_OK || (long long)v != strtoll(expect, NULL, 10)) {
                g_fails++;
                fprintf(stderr, "vectors line %d: got %s %d, want %s\n", lineno,
                        cairn_expr_status_name(st), (int)v, expect);
            }
        }
    }
    fclose(f);
    printf("  expression vectors: %d evaluated\n", ran);
    CHECK(ran >= 80);
}

static void test_evaluator_args(void)
{
    uint8_t in[4] = { 1, 2, 3, 4 };
    uint8_t code[] = { CAIRN_OP_LOAD_A };
    int32_t v = 77;

    CHECK_EQ(cairn_expr_eval(NULL, 1, in, &v), CAIRN_EXPR_ERR_ARG);
    CHECK_EQ(cairn_expr_eval(code, 1, NULL, &v), CAIRN_EXPR_ERR_ARG);
    CHECK_EQ(cairn_expr_eval(code, 1, in, NULL), CAIRN_EXPR_ERR_ARG);
    CHECK_EQ(cairn_expr_eval(NULL, 0, in, &v), CAIRN_EXPR_ERR_RESULT);
    CHECK_EQ(v, 77); /* untouched on every error */
    CHECK_EQ(cairn_expr_eval(code, 1, in, &v), CAIRN_EXPR_OK);
    CHECK_EQ(v, 1);
}

/* ── the legacy behaviour, as the oracle ──────────────────────────────────── */

static int L_sat_i8(int v)
{
    if (v > 127) return 127;
    if (v < -127) return -127;
    return (int8_t)v;
}

/* COBD::getPercentageValue, and its truncation to uint8_t on return. */
static int L_pct(unsigned a) { return (uint8_t)((uint16_t)a * 100 / 255); }
static unsigned L_u16(unsigned a, unsigned b) { return (uint16_t)((uint16_t)a << 8 | b); }

typedef int (*legacy_fn)(unsigned a, unsigned b);

/* sensors_read_obd_batch(): the six hot PIDs, parsed from one ISO-TP reply. */
static int Lb_rpm(unsigned a, unsigned b)      { return (int16_t)(L_u16(a, b) / 4u); }
static int Lb_speed(unsigned a, unsigned b)    { (void)b; return (int16_t)a; }
static int Lb_throttle(unsigned a, unsigned b) { (void)b; return (uint8_t)((uint16_t)a * 100u / 255u); }
static int Lb_timing(unsigned a, unsigned b)   { (void)b; return L_sat_i8((int)a / 2 - 64); }
static int Lb_map(unsigned a, unsigned b)      { (void)b; return (uint16_t)a; }
/* sensors_read_obd_extended(): lambda_e4 from the raw 16 bits. */
static int Lb_lambda(unsigned a, unsigned b)
{
    uint32_t e4 = ((uint32_t)L_u16(a, b) * 10000u) / 32768u;
    return (uint16_t)((e4 > 65534u) ? 65534u : e4);
}

/*
 * The sequential fallback and the cold channels: the library converts
 * (normalizeData) and sensors_read_obd* narrows. Written as the whole chain.
 */
static int Ls_rpm(unsigned a, unsigned b)      { int v = L_u16(a, b) >> 2; return (int16_t)((v > 32767) ? 32767 : v); }
static int Ls_speed(unsigned a, unsigned b)    { (void)b; return (int16_t)a; }
static int Ls_throttle(unsigned a, unsigned b) { (void)b; return (uint8_t)L_pct(a); }
static int Ls_timing(unsigned a, unsigned b)   { (void)b; return L_sat_i8((int)(a / 2) - 64); }
static int Ls_map(unsigned a, unsigned b)      { int v = (int)a; (void)b; return (v < 0) ? 0 : (uint16_t)((v > 65534) ? 65534 : v); }
static int Ls_load(unsigned a, unsigned b)     { (void)b; return (uint8_t)L_pct(a); }
static int Ls_temp(unsigned a, unsigned b)     { (void)b; return L_sat_i8((int)a - 40); }
static int Ls_maf(unsigned a, unsigned b)
{
    int  v  = (int)(L_u16(a, b) / 100);
    long cg = (long)v * 100L;
    return (uint16_t)((cg > 65534L) ? 65534L : ((cg < 0) ? 0 : cg));
}
static int Ls_trim(unsigned a, unsigned b)     { (void)b; return L_sat_i8(((int)a - 128) * 100 / 128); }
static int Ls_baro(unsigned a, unsigned b)     { int v = (int)a; (void)b; return (uint8_t)((v < 0) ? 0 : ((v > 254) ? 254 : v)); }
static int Ls_absload(unsigned a, unsigned b)  { return (uint16_t)L_u16(a, b); } /* pid_raw_u16 */
static int Ls_fuel(unsigned a, unsigned b)     { int v = L_pct(a); (void)b; return (uint8_t)((v < 0) ? 0 : ((v > 100) ? 100 : v)); }

typedef struct {
    cairn_field_t field;
    uint8_t       pid;       /* the library's PID_* constant */
    uint8_t       nbytes;
    uint8_t       tier;
    uint8_t       slot;      /* cold rotation slot, sensors.cpp cold_turn(n) */
    legacy_fn     batch;     /* NULL for cold */
    legacy_fn     seq;
    const char   *name;
} legacy_pid_t;

/* Order is the batch order of k_batch_pids[], then cold slots 0..9. */
static const legacy_pid_t L_PIDS[] = {
    { CAIRN_FIELD_RPM,                PID_RPM,                  2, CAIRN_TIER_HOT,  0, Lb_rpm,      Ls_rpm,      "rpm" },
    { CAIRN_FIELD_SPEED_KPH,          PID_SPEED,                1, CAIRN_TIER_HOT,  0, Lb_speed,    Ls_speed,    "speed" },
    { CAIRN_FIELD_THROTTLE_PCT,       PID_THROTTLE,             1, CAIRN_TIER_HOT,  0, Lb_throttle, Ls_throttle, "throttle" },
    { CAIRN_FIELD_TIMING_ADVANCE_DEG, PID_TIMING_ADVANCE,       1, CAIRN_TIER_HOT,  0, Lb_timing,   Ls_timing,   "timing" },
    { CAIRN_FIELD_MAP_KPA,            PID_INTAKE_MAP,           1, CAIRN_TIER_HOT,  0, Lb_map,      Ls_map,      "map" },
    { CAIRN_FIELD_LAMBDA_E4,          PID_AIR_FUEL_EQUIV_RATIO, 2, CAIRN_TIER_HOT,  0, Lb_lambda,   Lb_lambda,   "lambda" },
    { CAIRN_FIELD_ENGINE_LOAD_PCT,    PID_ENGINE_LOAD,          1, CAIRN_TIER_COLD, 0, NULL,        Ls_load,     "engine_load" },
    { CAIRN_FIELD_COOLANT_TEMP_C,     PID_COOLANT_TEMP,         1, CAIRN_TIER_COLD, 1, NULL,        Ls_temp,     "coolant" },
    { CAIRN_FIELD_INTAKE_TEMP_C,      PID_INTAKE_TEMP,          1, CAIRN_TIER_COLD, 2, NULL,        Ls_temp,     "intake_temp" },
    { CAIRN_FIELD_MAF_CGPS,           PID_MAF_FLOW,             2, CAIRN_TIER_COLD, 3, NULL,        Ls_maf,      "maf" },
    { CAIRN_FIELD_AMBIENT_TEMP_C,     PID_AMBIENT_TEMP,         1, CAIRN_TIER_COLD, 4, NULL,        Ls_temp,     "ambient" },
    { CAIRN_FIELD_FUEL_TRIM_SHORT_PCT, PID_SHORT_TERM_FUEL_TRIM_1, 1, CAIRN_TIER_COLD, 5, NULL,     Ls_trim,     "stft" },
    { CAIRN_FIELD_FUEL_TRIM_LONG_PCT, PID_LONG_TERM_FUEL_TRIM_1, 1, CAIRN_TIER_COLD, 6, NULL,       Ls_trim,     "ltft" },
    { CAIRN_FIELD_BARO_KPA,           PID_BAROMETRIC,           1, CAIRN_TIER_COLD, 7, NULL,        Ls_baro,     "baro" },
    { CAIRN_FIELD_ABS_LOAD_RAW,       PID_ABSOLUTE_ENGINE_LOAD, 2, CAIRN_TIER_COLD, 8, NULL,        Ls_absload,  "abs_load" },
    { CAIRN_FIELD_FUEL_LEVEL_PCT,     PID_FUEL_LEVEL,           1, CAIRN_TIER_COLD, 9, NULL,        Ls_fuel,     "fuel_level" },
};
#define L_NPIDS (sizeof(L_PIDS) / sizeof(L_PIDS[0]))

/* sensors.cpp: CAIRN_OBD_COLD_SLOTS, k_batch_pids[], k_batch_dbytes[] */
#define L_COLD_SLOTS 10
static const uint8_t L_BATCH_PIDS[]   = { 0x0C, 0x0D, 0x11, 0x0E, 0x0B, 0x44 };
static const uint8_t L_BATCH_DBYTES[] = {    2,    1,    1,    1,    1,    2 };

/* ── the N20 tables against the oracle ────────────────────────────────────── */

static void test_n20_pid_table(const cairn_engine_profile_t *n20)
{
    size_t i;

    CHECK_EQ(n20->status, CAIRN_ENGINE_DERIVED);
    CHECK_EQ(n20->n_pids, L_NPIDS);
    CHECK_EQ(n20->n_hot, sizeof(L_BATCH_PIDS));
    CHECK_EQ(n20->cold_slots, L_COLD_SLOTS);

    /* The batch request: same PIDs, same order, same data lengths. */
    for (i = 0; i < sizeof(L_BATCH_PIDS); i++) {
        CHECK_EQ(n20->pids[i].tier, CAIRN_TIER_HOT);
        CHECK_EQ(n20->pids[i].service, 0x01);
        CHECK_EQ(n20->pids[i].pid, L_BATCH_PIDS[i]);
        CHECK_EQ(n20->pids[i].nbytes, L_BATCH_DBYTES[i]);
    }

    for (i = 0; i < L_NPIDS; i++) {
        const legacy_pid_t *l = &L_PIDS[i];
        const cairn_pid_t  *p = cairn_engine_pid_for_field(n20, l->field);
        unsigned a, b, bmax;
        long long bad = 0;

        CHECK(p != NULL);
        if (p == NULL) continue;
        CHECK_EQ(p, &n20->pids[i]);          /* same position as the legacy table */
        CHECK_EQ(p->pid, l->pid);
        CHECK_EQ(p->nbytes, l->nbytes);
        CHECK_EQ(p->tier, l->tier);
        if (l->tier == CAIRN_TIER_COLD) CHECK_EQ(p->cold_slot, l->slot);
        CHECK_EQ(p->log, 1);

        bmax = (l->nbytes > 1) ? 255u : 0u;
        for (a = 0; a <= 255u; a++) {
            for (b = 0; b <= bmax; b++) {
                uint8_t in[4];
                int32_t v = 0;
                cairn_expr_status_t st;

                in[0] = (uint8_t)a; in[1] = (uint8_t)b; in[2] = 0; in[3] = 0;
                st = cairn_engine_eval_pid(n20, p, in, &v);
                if (st != CAIRN_EXPR_OK) { bad++; continue; }

                /* The table's stated range is a promise; it must hold. */
                if (v < p->range_min || v > p->range_max) bad++;
                if (l->seq(a, b) != v) bad++;
                if (l->batch != NULL && l->batch(a, b) != v) bad++;
            }
        }
        g_checks++;
        if (bad != 0) {
            g_fails++;
            fprintf(stderr, "FAIL N20 %s: %lld of the possible replies differ from the legacy conversion\n",
                    l->name, bad);
        }
    }
    printf("  N20 PID table: %zu PIDs, every possible reply checked against the legacy conversion\n",
           (size_t)L_NPIDS);
}

static void test_n20_params(const cairn_engine_profile_t *n20)
{
    CHECK_EQ(n20->obd_period_ms, CAIRN_OBD_PERIOD_MS);
    CHECK_EQ(n20->obd_batch_period_ms, CAIRN_OBD_BATCH_PERIOD_MS);
    CHECK_EQ(n20->engine_on_mv, CAIRN_ENGINE_ON_MV);
    CHECK_EQ(n20->standby_idle_ms, CAIRN_STANDBY_IDLE_MS);
    CHECK_EQ(n20->standby_heartbeat_ms, CAIRN_STANDBY_HEARTBEAT_MS);
    CHECK_EQ(n20->drive_voltage_dwell_ms, CAIRN_DRIVE_VOLTAGE_DWELL_MS);
    CHECK_EQ(n20->drive_motion_dwell_ms, CAIRN_DRIVE_MOTION_DWELL_MS);
    CHECK_EQ(n20->drive_both_dwell_ms, CAIRN_DRIVE_BOTH_DWELL_MS);

    /* Documented, not acted on: the 8 minute first sleep phase (bmw_obd_bus_sleep.md),
     * which the 5 minute standby dwell must stay inside. */
    CHECK_EQ(n20->bus_first_sleep_phase_ms, 8u * 60u * 1000u);
    CHECK(n20->standby_idle_ms < n20->bus_first_sleep_phase_ms);

    /* sensors_battery_mv(): v >= 9.0f && v <= 18.0f */
    CHECK_EQ(n20->supply_min_mv, 9000);
    CHECK_EQ(n20->supply_max_mv, 18000);

    {
        uint32_t all = CAIRN_K_PIDS | CAIRN_K_OBD_PERIOD | CAIRN_K_OBD_BATCH_PERIOD |
                       CAIRN_K_COLD_SLOTS | CAIRN_K_ENGINE_ON_MV | CAIRN_K_STANDBY_IDLE |
                       CAIRN_K_STANDBY_HEARTBEAT | CAIRN_K_DRIVE_VOLTAGE_DWELL |
                       CAIRN_K_DRIVE_MOTION_DWELL | CAIRN_K_DRIVE_BOTH_DWELL |
                       CAIRN_K_BUS_SLEEP_PHASE | CAIRN_K_SUPPLY_RANGE;
        CHECK_EQ(n20->known, all);
    }
}

/* The probe list lives in src/pidtest.cpp and is not wired; hold the profile to it. */
static void test_n20_probes_match_pidtest(const cairn_engine_profile_t *n20, const char *pidtest_path)
{
    FILE  *f = fopen(pidtest_path, "r");
    char   line[512];
    bool   in_block = false;
    size_t n = 0;

    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", pidtest_path);
        g_fails++;
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned pid, std_bytes;
        char     name[40];

        if (strstr(line, "k_probes[] = {") != NULL) { in_block = true; continue; }
        if (in_block && line[0] == '}') break;
        if (!in_block) continue;

        if (sscanf(line, " { 0x%x, \"%39[^\"]\", %u,", &pid, name, &std_bytes) == 3) {
            CHECK(n < n20->n_probes);
            if (n < n20->n_probes) {
                CHECK_EQ(n20->probes[n].pid, pid);
                CHECK_EQ(n20->probes[n].std_bytes, std_bytes);
                CHECK(strcmp(n20->probes[n].name, name) == 0);
            }
            n++;
        }
    }
    fclose(f);
    CHECK_EQ(n, n20->n_probes);
    printf("  N20 probes: %zu entries equal src/pidtest.cpp k_probes[]\n", n);
}

static void test_supply_literal_in_source(const char *sensors_path)
{
    FILE *f = fopen(sensors_path, "r");
    char  line[512];
    bool  found = false;

    if (f == NULL) { fprintf(stderr, "cannot open %s\n", sensors_path); g_fails++; return; }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "v >= 9.0f && v <= 18.0f") != NULL) found = true;
    }
    fclose(f);
    /* Not wired (a float compare cannot be shown identical to an integer one), so
     * this tripwire says when the literal moves and the profile must follow. */
    CHECK(found);
}

/* ── the batch, request and parse, over the real tables ───────────────────── */

typedef struct {
    int rpm, speed, throttle, timing, map, lambda_e4;
} legacy_batch_t;

/* sensors_read_obd_batch(), from the byte buffer on: the echo checks and the parse. */
static bool L_batch_parse(const uint8_t *bytes, int nbytes, legacy_batch_t *out)
{
    int     expected = 0, pos = 0, i, j;
    uint8_t data[6][2];

    for (i = 0; i < 6; i++) expected += 1 + L_BATCH_DBYTES[i];
    if (nbytes < expected) return false;
    for (i = 0; i < 6; i++) {
        if (bytes[pos] != L_BATCH_PIDS[i]) return false;
        pos++;
        for (j = 0; j < L_BATCH_DBYTES[i]; j++) data[i][j] = bytes[pos++];
    }
    out->rpm       = (int16_t)(((uint16_t)data[0][0] << 8 | data[0][1]) / 4u);
    out->speed     = (int16_t)data[1][0];
    out->throttle  = (uint8_t)((uint16_t)data[2][0] * 100u / 255u);
    out->timing    = L_sat_i8((int)data[3][0] / 2 - 64);
    out->map       = data[4][0];
    {
        uint16_t raw = (uint16_t)((uint16_t)data[5][0] << 8 | data[5][1]);
        uint32_t e4  = ((uint32_t)raw * 10000u) / 32768u;
        out->lambda_e4 = (uint16_t)((e4 > 65534u) ? 65534u : e4);
    }
    return true;
}

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void test_n20_batch(const cairn_engine_profile_t *n20)
{
    char   req[24];
    char   legacy[24];
    size_t k, i, n;
    long   frames = 0;

    /* The request string: sensors_read_obd_batch built "01" + %02X per PID + '\r'. */
    k = (size_t)snprintf(legacy, sizeof(legacy), "01");
    for (i = 0; i < sizeof(L_BATCH_PIDS); i++)
        k += (size_t)snprintf(legacy + k, sizeof(legacy) - k, "%02X", (unsigned)L_BATCH_PIDS[i]);
    legacy[k++] = '\r';
    legacy[k]   = '\0';
    n = cairn_engine_batch_request(n20, req, sizeof(req));
    CHECK_EQ(n, k);
    CHECK(strcmp(req, legacy) == 0);
    CHECK(strcmp(req, "010C0D110E0B44\r") == 0);
    CHECK_EQ(cairn_engine_batch_request(n20, req, 4), 0); /* too small: refuses, no overrun */

    /* Parse: random replies, valid and not, against the legacy parse. */
    for (frames = 0; frames < 300000; frames++) {
        uint8_t        buf[16];
        size_t         len = 0;
        uint32_t       r = rnd();
        legacy_batch_t lb;
        int32_t        v[CAIRN_FIELD_COUNT];
        uint32_t       present = 0;
        bool           lok, nok;

        for (i = 0; i < sizeof(L_BATCH_PIDS); i++) {
            size_t j;
            buf[len++] = L_BATCH_PIDS[i];
            for (j = 0; j < L_BATCH_DBYTES[i]; j++) buf[len++] = (uint8_t)rnd();
        }
        /* Corrupt some: a wrong echo, a truncation, trailing bytes. */
        switch (r & 7u) {
        case 0: buf[(r >> 8) % len] ^= (uint8_t)(1u << ((r >> 16) & 7u)); break;
        case 1: len = (r >> 8) % (len + 1); break;
        case 2: buf[len++] = (uint8_t)rnd(); break;
        default: break;
        }

        lok = L_batch_parse(buf, (int)len, &lb);
        nok = cairn_engine_batch_parse(n20, buf, len, v, &present);
        if (lok != nok) { CHECK(lok == nok); break; }
        if (lok) {
            if (lb.rpm != v[CAIRN_FIELD_RPM] || lb.speed != v[CAIRN_FIELD_SPEED_KPH] ||
                lb.throttle != v[CAIRN_FIELD_THROTTLE_PCT] ||
                lb.timing != v[CAIRN_FIELD_TIMING_ADVANCE_DEG] ||
                lb.map != v[CAIRN_FIELD_MAP_KPA] || lb.lambda_e4 != v[CAIRN_FIELD_LAMBDA_E4] ||
                present != ((1u << CAIRN_FIELD_RPM) | (1u << CAIRN_FIELD_SPEED_KPH) |
                            (1u << CAIRN_FIELD_THROTTLE_PCT) |
                            (1u << CAIRN_FIELD_TIMING_ADVANCE_DEG) |
                            (1u << CAIRN_FIELD_MAP_KPA) | (1u << CAIRN_FIELD_LAMBDA_E4))) {
                CHECK(!"batch parse differs from the legacy parse");
                break;
            }
        }
    }
    g_checks++;
    printf("  N20 batch: request string and %ld random replies (some corrupt) match the legacy parse\n", frames);
}

/* ── resolved values, with and without a profile's data ───────────────────── */

static void test_params_follow_active(const cairn_engine_profile_t *n20, const cairn_engine_profile_t *stub)
{
    if (n20 != NULL) {
        const cairn_engine_params_t *p;
        CHECK(cairn_engine_select("bmw-n20"));
        p = cairn_engine_params();
        CHECK_EQ(p->obd_period_ms, CAIRN_OBD_PERIOD_MS);
        CHECK_EQ(p->obd_batch_period_ms, CAIRN_OBD_BATCH_PERIOD_MS);
        CHECK_EQ(p->cold_slots, L_COLD_SLOTS);
        CHECK_EQ(p->engine_on_mv, CAIRN_ENGINE_ON_MV);
        CHECK_EQ(p->standby_idle_ms, CAIRN_STANDBY_IDLE_MS);
        CHECK_EQ(p->standby_heartbeat_ms, CAIRN_STANDBY_HEARTBEAT_MS);
        CHECK_EQ(p->drive_voltage_dwell_ms, CAIRN_DRIVE_VOLTAGE_DWELL_MS);
        CHECK_EQ(p->drive_motion_dwell_ms, CAIRN_DRIVE_MOTION_DWELL_MS);
        CHECK_EQ(p->drive_both_dwell_ms, CAIRN_DRIVE_BOTH_DWELL_MS);
        CHECK(p->from_profile != 0);
        CHECK(cairn_engine_has_pids());
    }
    if (stub != NULL) {
        const cairn_engine_params_t *p;
        CHECK(cairn_engine_select(stub->id));
        p = cairn_engine_params();
        /* Nothing claimed: every value is the firmware's own default, and says so. */
        CHECK_EQ(p->from_profile, 0);
        CHECK_EQ(p->obd_period_ms, CAIRN_OBD_PERIOD_MS);
        CHECK_EQ(p->engine_on_mv, CAIRN_ENGINE_ON_MV);
        CHECK_EQ(p->standby_idle_ms, CAIRN_STANDBY_IDLE_MS);
        CHECK(p->cold_slots >= 1);
        CHECK(!cairn_engine_has_pids());
        CHECK(cairn_engine_pid_for_field(stub, CAIRN_FIELD_RPM) == NULL);
        {
            char req[24];
            int32_t v[CAIRN_FIELD_COUNT];
            uint32_t present;
            uint8_t b[8] = { 0 };
            CHECK_EQ(cairn_engine_batch_request(stub, req, sizeof(req)), 0);
            CHECK(!cairn_engine_batch_parse(stub, b, sizeof(b), v, &present));
        }
    }

    CHECK(!cairn_engine_select("no-such-engine"));
    CHECK(!cairn_engine_select(NULL));
    cairn_engine_select_default();
    {
        /* The default is the first installed engine that claims something. */
        size_t i;
        const cairn_engine_profile_t *want = cairn_engine_installed(0);
        for (i = 0; i < cairn_engine_installed_count(); i++) {
            if (cairn_engine_installed(i)->status != CAIRN_ENGINE_STUB) {
                want = cairn_engine_installed(i);
                break;
            }
        }
        CHECK(cairn_engine_active() == want);
    }
}

/* ── the vehicle gate ─────────────────────────────────────────────────────── */

#define Q13 "?????" "?????" "???"
#define A13 "AAAAA" "AAAAA" "AAA"

static void test_gate(void)
{
    static const char *const x_vin[] = { "XXX" Q13 "1", "XXX" Q13 "2" };
    static const char *const y_vin[] = { "YYY" Q13 "3" };
    static const char *const o_vin[] = { "?????" "?????" "?????" "???" }; /* 18 long: never matches */
    static const char *const z_vin[] = { "XXXYYY" "??????????" "1" };     /* overlaps x */
    const cairn_engine_catalogue_entry_t cat[] = {
        { "x", x_vin, 2, NULL, 0, 1 },
        { "y", y_vin, 1, NULL, 0, 0 },
        { "w", NULL, 0, NULL, 0, 0 },
    };
    const cairn_engine_catalogue_entry_t overlap[] = {
        { "x", x_vin, 2, NULL, 0, 1 },
        { "z", z_vin, 1, NULL, 0, 0 },
    };
    const cairn_engine_catalogue_entry_t odd[] = { { "o", o_vin, 1, NULL, 0, 1 } };
    const char *id = NULL;

    CHECK(cairn_vin_matches("XXX" Q13 "1", "XXX" A13 "1"));
    CHECK(!cairn_vin_matches("XXX" Q13 "1", "XXX" A13 "2"));
    CHECK(!cairn_vin_matches("XXX" Q13 "1", "XXX" "AAAAAAAAAAAA" "1")); /* 16 long */
    CHECK(!cairn_vin_matches("XXX" Q13 "1", NULL));
    CHECK(!cairn_vin_matches(NULL, "XXX" A13 "1"));
    CHECK(!cairn_vin_matches("short", "short"));

    /* Declared engine. */
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, "x", NULL, &id), CAIRN_VEHICLE_SERVED);
    CHECK(id != NULL && strcmp(id, "x") == 0);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, "y", NULL, &id), CAIRN_VEHICLE_REFUSED_NOT_INSTALLED);
    CHECK(id != NULL && strcmp(id, "y") == 0);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, "w", NULL, &id), CAIRN_VEHICLE_REFUSED_NOT_INSTALLED);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, "nope", NULL, &id), CAIRN_VEHICLE_REFUSED_UNKNOWN_ENGINE);
    CHECK(id == NULL);

    /* By VIN. */
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, NULL, "XXX" A13 "1", &id), CAIRN_VEHICLE_SERVED);
    CHECK(id != NULL && strcmp(id, "x") == 0);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, NULL, "YYY" A13 "3", &id), CAIRN_VEHICLE_REFUSED_NOT_INSTALLED);
    CHECK(id != NULL && strcmp(id, "y") == 0);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, "", "YYY" A13 "3", &id), CAIRN_VEHICLE_REFUSED_NOT_INSTALLED);

    /* Not enough to go on is not a refusal: today's behaviour is to record. */
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, NULL, NULL, &id), CAIRN_VEHICLE_UNIDENTIFIED);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, NULL, "", &id), CAIRN_VEHICLE_UNIDENTIFIED);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, NULL, "TOOSHORT", &id), CAIRN_VEHICLE_UNIDENTIFIED);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, NULL, "QQQ" A13 "9", &id), CAIRN_VEHICLE_UNIDENTIFIED);
    CHECK(id == NULL);
    CHECK_EQ(cairn_engine_check_vehicle_in(odd, 1, NULL, "ABCDEFGHJKLMNPRST", &id), CAIRN_VEHICLE_UNIDENTIFIED);
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, NULL, NULL, NULL), CAIRN_VEHICLE_UNIDENTIFIED);

    /* The declaration beats the VIN. */
    CHECK_EQ(cairn_engine_check_vehicle_in(cat, 3, "x", "YYY" A13 "3", &id), CAIRN_VEHICLE_SERVED);
    CHECK(id != NULL && strcmp(id, "x") == 0);

    /* Two engines claiming one VIN is a contradiction, not evidence. */
    CHECK_EQ(cairn_engine_check_vehicle_in(overlap, 2, NULL, "XXXYYY" "AAAAAAAAAA" "1", &id),
             CAIRN_VEHICLE_UNIDENTIFIED);
}


static void test_gate_real(const char *expect_selection)
{
    const cairn_engine_identity_t *idn = cairn_engine_identity();
    const char *id;
    size_t i;

    /* Every engine in the catalogue is either served or refused as not installed,
     * agreeing with the install list; one nobody has a profile for is refused. */
    CHECK_EQ(cairn_engine_check_vehicle("bmw-n20", NULL, &id) != CAIRN_VEHICLE_REFUSED_UNKNOWN_ENGINE, 1);
    CHECK_EQ(cairn_engine_check_vehicle("bmw-b58", NULL, &id) != CAIRN_VEHICLE_REFUSED_UNKNOWN_ENGINE, 1);
    for (i = 0; i < 2; i++) {
        static const char *const names[2] = { "bmw-n20", "bmw-b58" };
        bool inst = cairn_engine_find(names[i]) != NULL;
        CHECK_EQ(cairn_engine_check_vehicle(names[i], NULL, &id),
                 inst ? CAIRN_VEHICLE_SERVED : CAIRN_VEHICLE_REFUSED_NOT_INSTALLED);
    }
    CHECK_EQ(cairn_engine_check_vehicle("vag-ea888", NULL, &id), CAIRN_VEHICLE_REFUSED_UNKNOWN_ENGINE);
    /* No VIN rule is documented for any shipped engine, so a VIN identifies nothing. */
    CHECK_EQ(cairn_engine_check_vehicle(NULL, "WBA00000000000000", &id), CAIRN_VEHICLE_UNIDENTIFIED);

    if (expect_selection != NULL) {
        CHECK(strcmp(idn->selection, expect_selection) == 0);
        if (strcmp(expect_selection, "bmw-n20") == 0) {
            CHECK_EQ(cairn_engine_installed_count(), 1);
            CHECK(cairn_engine_find("bmw-b58") == NULL);
            CHECK_EQ(cairn_engine_check_vehicle("bmw-b58", NULL, &id), CAIRN_VEHICLE_REFUSED_NOT_INSTALLED);
            CHECK(strcmp(id, "bmw-b58") == 0);
            CHECK_EQ(cairn_engine_check_vehicle("bmw-n20", NULL, &id), CAIRN_VEHICLE_SERVED);
            CHECK(!cairn_engine_select("bmw-b58"));
        }
        if (strcmp(expect_selection, "bmw-b58") == 0) {
            CHECK_EQ(cairn_engine_installed_count(), 1);
            CHECK(cairn_engine_find("bmw-n20") == NULL);
            CHECK_EQ(cairn_engine_check_vehicle("bmw-n20", NULL, &id), CAIRN_VEHICLE_REFUSED_NOT_INSTALLED);
            CHECK(cairn_engine_active() == cairn_engine_find("bmw-b58"));
        }
        if (strcmp(expect_selection, "bmw-b58,bmw-n20") == 0) {
            CHECK_EQ(cairn_engine_installed_count(), 2);
            CHECK(cairn_engine_active() == cairn_engine_find("bmw-n20"));
        }
    }
}

/* ── the build identity ───────────────────────────────────────────────────── */

static void hex32(const uint8_t *h, char *out)
{
    static const char d[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[2 * i]     = d[h[i] >> 4];
        out[2 * i + 1] = d[h[i] & 15];
    }
    out[64] = '\0';
}

/* SHA-256 of a file's bytes with CRLF read as LF, using the firmware's own SHA-256
 * rather than the generator's, so the two ends are independent. */
static bool file_hash(const char *path, uint8_t out[32])
{
    FILE   *f = fopen(path, "rb");
    uint8_t *buf, *norm;
    long    sz;
    size_t  i, n = 0;

    if (f == NULL) return false;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf  = (uint8_t *)malloc((size_t)sz + 1);
    norm = (uint8_t *)malloc((size_t)sz + 1);
    if (buf == NULL || norm == NULL || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        free(norm);
        return false;
    }
    fclose(f);
    for (i = 0; i < (size_t)sz; i++) {
        if (buf[i] == '\r' && i + 1 < (size_t)sz && buf[i + 1] == '\n') continue;
        norm[n++] = buf[i];
    }
    cairn_sha256(norm, n, out);
    free(buf);
    free(norm);
    return true;
}

static void test_identity(const char *dir)
{
    const cairn_engine_identity_t *idn = cairn_engine_identity();
    cairn_sha256_t ctx;
    uint8_t        build[32];
    char           hexb[65], strbuf[512], sel[128];
    size_t         i;

    CHECK_EQ(idn->count, cairn_engine_installed_count());

    cairn_sha256_init(&ctx);
    cairn_sha256_update(&ctx, (const uint8_t *)"cairn.engine-build/v1-draft\n", 28);
    strbuf[0] = '\0';
    sel[0] = '\0';
    strcat(strbuf, "engines=");

    for (i = 0; i < cairn_engine_installed_count(); i++) {
        const cairn_engine_profile_t *p = cairn_engine_installed(i);
        char     path[256], line[128], h[65];
        uint8_t  fh[32];

        snprintf(path, sizeof(path), "%s/%s.yaml", dir, p->id);
        CHECK(file_hash(path, fh));
        /* The hash baked into the image is the hash of the YAML on disk. */
        CHECK(memcmp(fh, p->sha256, 32) == 0);

        hex32(p->sha256, h);
        snprintf(line, sizeof(line), "%s\n%u\n%s\n", p->id, (unsigned)p->version, h);
        cairn_sha256_update(&ctx, (const uint8_t *)line, strlen(line));

        if (i > 0) { strcat(strbuf, ","); strcat(sel, ","); }
        snprintf(line, sizeof(line), "%s@%u/%.16s", p->id, (unsigned)p->version, h);
        strcat(strbuf, line);
        strcat(sel, p->id);
    }
    cairn_sha256_final(&ctx, build);
    CHECK(memcmp(build, idn->build_sha256, 32) == 0);

    hex32(build, hexb);
    {
        char tail[40];
        snprintf(tail, sizeof(tail), " build=%.16s", hexb);
        strcat(strbuf, tail);
    }
    CHECK(strcmp(strbuf, idn->string) == 0);
    CHECK(strcmp(sel, idn->selection) == 0);
    printf("  build identity: %s\n", idn->string);
}

/* ── the profile with nothing claimed ─────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *vectors, *dir, *expect = NULL;
    const cairn_engine_profile_t *n20, *b58;
    char pidtest[300], sensors[300];

    if (argc < 3) {
        fprintf(stderr, "usage: %s <expr-vectors.txt> <engines-dir> [expected-selection]\n", argv[0]);
        return 2;
    }
    vectors = argv[1];
    dir = argv[2];
    if (argc > 3) expect = argv[3];

    printf("engine_test: installed = %s\n", cairn_engine_identity()->selection);

    run_vectors(vectors);
    test_evaluator_args();
    test_gate();
    test_gate_real(expect);
    test_identity(dir);

    n20 = cairn_engine_find("bmw-n20");
    b58 = cairn_engine_find("bmw-b58");

    if (n20 != NULL) {
        test_n20_pid_table(n20);
        test_n20_params(n20);
        test_n20_batch(n20);
    } else {
        printf("  bmw-n20 is not installed in this build: equivalence checks skipped\n");
    }
    if (b58 != NULL) {
        CHECK_EQ(b58->status, CAIRN_ENGINE_STUB);
        CHECK_EQ(b58->known, 0);
        CHECK_EQ(b58->n_pids, 0);
    }
    test_params_follow_active(n20, b58);

    /* Source tripwires: relative to test/host, where make runs this. */
    if (n20 != NULL) {
        snprintf(pidtest, sizeof(pidtest), "../../src/pidtest.cpp");
        snprintf(sensors, sizeof(sensors), "../../src/sensors.cpp");
        test_n20_probes_match_pidtest(n20, pidtest);
        test_supply_literal_in_source(sensors);
    }

    printf("engine_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
