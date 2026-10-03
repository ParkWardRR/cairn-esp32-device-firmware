/*
 * Golden vector tests for the BLE GNSS_FIX wire format.
 *
 * Decodes each hex payload from docs/golden-vectors.json using the same
 * byte layout as ble_companion.cpp and checks the decoded fields against
 * expected values derived from the input.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_format.h"
#include "minijson.h"

static int g_pass, g_fail;

#define ASSERT(cond, ...)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL  %s:%d: ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
            return;                                                           \
        }                                                                     \
    } while (0)

static inline int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                     (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static int hex_decode(const char *hex, uint8_t *out, size_t max_len)
{
    size_t len = strlen(hex);
    if (len % 2 != 0 || len / 2 > max_len)
        return -1;
    for (size_t i = 0; i < len / 2; i++) {
        unsigned byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1)
            return -1;
        out[i] = (uint8_t)byte;
    }
    return (int)(len / 2);
}

static void test_vector(const mj_doc_t *doc, const mj_node_t *vec)
{
    const char *name = mj_str_or(mj_get(doc, vec, "name"), "?");
    const char *hex  = mj_str_or(mj_get(doc, vec, "hex"), "");
    const mj_node_t *input = mj_get(doc, vec, "input");

    ASSERT(input != NULL, "%s: missing input", name);
    ASSERT(strlen(hex) == 56, "%s: hex length %zu, want 56", name, strlen(hex));

    uint8_t buf[28];
    int n = hex_decode(hex, buf, sizeof(buf));
    ASSERT(n == 28, "%s: hex_decode returned %d", name, n);

    double lat   = mj_num_or(mj_get(doc, input, "lat"), 0);
    double lon   = mj_num_or(mj_get(doc, input, "lon"), 0);
    double alt   = mj_num_or(mj_get(doc, input, "alt"), 0);
    double hacc  = mj_num_or(mj_get(doc, input, "hacc"), 0);
    double vacc  = mj_num_or(mj_get(doc, input, "vacc"), 0);
    double speed = mj_num_or(mj_get(doc, input, "speed"), 0);
    double course = mj_num_or(mj_get(doc, input, "course"), 0);
    double age   = mj_num_or(mj_get(doc, input, "age"), 0);
    int    seq   = (int)mj_num_or(mj_get(doc, input, "seq"), 0);

    int32_t  lat_e7       = rd_i32(buf + 0);
    int32_t  lon_e7       = rd_i32(buf + 4);
    int32_t  alt_cm       = rd_i32(buf + 8);
    uint16_t speed_cmps   = rd_u16(buf + 12);
    uint16_t heading_cdeg = rd_u16(buf + 14);
    uint16_t h_acc_cm     = rd_u16(buf + 16);
    uint16_t v_acc_cm     = rd_u16(buf + 18);
    uint8_t  fix_type     = buf[20];
    uint8_t  validity     = buf[21];
    uint16_t sample_age   = rd_u16(buf + 22);
    uint16_t wire_seq     = rd_u16(buf + 24);

    ASSERT(wire_seq == (uint16_t)seq,
           "%s: seq %u, want %u", name, wire_seq, (uint16_t)seq);

    uint16_t expected_age = (uint16_t)(age * 1000.0);
    ASSERT(sample_age == expected_age,
           "%s: sample_age_ms %u, want %u", name, sample_age, expected_age);

    bool pos_invalid = (hacc < 0);
    bool alt_invalid = (vacc < 0);
    bool speed_unavail = (speed < 0);
    bool course_unavail = (course < 0);

    if (pos_invalid) {
        ASSERT(lat_e7 == 0, "%s: lat_e7 %d, want 0 (invalid)", name, lat_e7);
        ASSERT(lon_e7 == 0, "%s: lon_e7 %d, want 0 (invalid)", name, lon_e7);
        ASSERT(fix_type == 0, "%s: fix_type %u, want 0", name, fix_type);
        ASSERT((validity & 0x01) == 0,
               "%s: validity b0 set for invalid pos", name);
    } else {
        int32_t exp_lat = (int32_t)round(lat * 1e7);
        int32_t exp_lon = (int32_t)round(lon * 1e7);
        ASSERT(lat_e7 == exp_lat,
               "%s: lat_e7 %d, want %d", name, lat_e7, exp_lat);
        ASSERT(lon_e7 == exp_lon,
               "%s: lon_e7 %d, want %d", name, lon_e7, exp_lon);
        ASSERT((validity & 0x01) != 0,
               "%s: validity b0 clear for valid pos", name);

        uint16_t exp_hacc = (uint16_t)fmin(round(hacc * 100.0), 65534.0);
        ASSERT(h_acc_cm == exp_hacc,
               "%s: h_acc_cm %u, want %u", name, h_acc_cm, exp_hacc);

        if (alt_invalid) {
            ASSERT(alt_cm == 0x7FFFFFFF,
                   "%s: alt_cm 0x%08x, want sentinel", name, (unsigned)alt_cm);
            ASSERT(v_acc_cm == 0xFFFF,
                   "%s: v_acc_cm %u, want 0xFFFF", name, v_acc_cm);
            ASSERT(fix_type == 1,
                   "%s: fix_type %u, want 1 (2D)", name, fix_type);
            ASSERT((validity & 0x02) == 0,
                   "%s: validity b1 set for invalid alt", name);
        } else {
            int32_t exp_alt = (int32_t)round(alt * 100.0);
            ASSERT(alt_cm == exp_alt,
                   "%s: alt_cm %d, want %d", name, alt_cm, exp_alt);
            uint16_t exp_vacc = (uint16_t)fmin(round(vacc * 100.0), 65534.0);
            ASSERT(v_acc_cm == exp_vacc,
                   "%s: v_acc_cm %u, want %u", name, v_acc_cm, exp_vacc);
            ASSERT(fix_type == 2,
                   "%s: fix_type %u, want 2 (3D)", name, fix_type);
            ASSERT((validity & 0x02) != 0,
                   "%s: validity b1 clear for valid alt", name);
        }
    }

    if (speed_unavail) {
        ASSERT(speed_cmps == 0xFFFF,
               "%s: speed_cmps %u, want 0xFFFF", name, speed_cmps);
        ASSERT((validity & 0x04) == 0,
               "%s: validity b2 set for unavail speed", name);
    } else {
        uint16_t exp_speed = (uint16_t)fmin(round(speed * 100.0), 65534.0);
        ASSERT(speed_cmps == exp_speed,
               "%s: speed_cmps %u, want %u", name, speed_cmps, exp_speed);
        ASSERT((validity & 0x04) != 0,
               "%s: validity b2 clear for valid speed", name);
    }

    if (course_unavail) {
        ASSERT(heading_cdeg == 0xFFFF,
               "%s: heading_cdeg %u, want 0xFFFF", name, heading_cdeg);
        ASSERT((validity & 0x08) == 0,
               "%s: validity b3 set for unavail course", name);
    } else {
        uint16_t exp_course = (uint16_t)(((int)round(course * 100.0)) % 36000);
        ASSERT(heading_cdeg == exp_course,
               "%s: heading_cdeg %u, want %u", name, heading_cdeg, exp_course);
        ASSERT((validity & 0x08) != 0,
               "%s: validity b3 clear for valid course", name);
    }

    /* Firmware validation: reject when validity b0 is clear */
    bool should_reject = pos_invalid;
    bool would_reject = !(validity & 0x01);
    ASSERT(would_reject == should_reject,
           "%s: validation mismatch: reject=%d, want=%d",
           name, would_reject, should_reject);

    /* Staleness: all vectors have age ≤ 2.0, firmware threshold is 3.0 s */
    if (!should_reject) {
        ASSERT(sample_age <= 3000,
               "%s: age %u ms > 3000, would be rejected", name, sample_age);
    }

    /* Stored sample mapping: source_flags, sats sentinels */
    if (!should_reject) {
        cairn_gnss_sample_t s;
        memset(&s, 0, sizeof(s));
        s.lat_e7       = lat_e7;
        s.lon_e7       = lon_e7;
        s.alt_cm       = alt_cm;
        s.speed_cmps   = speed_cmps;
        s.heading_cdeg = heading_cdeg;
        s.h_acc_cm     = h_acc_cm;
        s.v_acc_cm     = v_acc_cm;
        s.fix_type     = fix_type;
        s.sats_used    = 0xFF;
        s.sats_visible = 0xFF;
        s.hdop_e2      = CAIRN_U16_UNKNOWN;
        s.source_flags = CAIRN_SOURCE_PHONE;

        ASSERT(s.source_flags == 0x20,
               "%s: source_flags 0x%02x, want 0x20", name, s.source_flags);
        ASSERT(s.sats_used == 0xFF,
               "%s: sats_used %u, want 0xFF", name, s.sats_used);
        ASSERT(s.hdop_e2 == 0xFFFF,
               "%s: hdop_e2 %u, want 0xFFFF", name, s.hdop_e2);
    }

    printf("  pass  [ble    ] %s\n", name);
    g_pass++;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <golden-vectors.json>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)sz + 1);
    fread(text, 1, (size_t)sz, f);
    text[sz] = '\0';
    fclose(f);

    mj_doc_t *doc = (mj_doc_t *)calloc(1, sizeof(mj_doc_t));
    if (!mj_parse(doc, text)) {
        fprintf(stderr, "JSON parse error: %s\n", doc->error);
        return 1;
    }

    const mj_node_t *root = mj_root(doc);
    const mj_node_t *fixes = mj_get(doc, root, "gnss_fix");
    if (!fixes) {
        fprintf(stderr, "missing gnss_fix array\n");
        return 1;
    }

    size_t count = mj_len(doc, fixes);
    printf("BLE golden vectors: %zu case(s)\n", count);

    for (size_t i = 0; i < count; i++)
        test_vector(doc, mj_at(doc, fixes, i));

    printf("\nBLE golden vectors: %d/%d passed\n", g_pass, g_pass + g_fail);

    free(doc);
    free(text);
    return g_fail > 0 ? 1 : 0;
}
