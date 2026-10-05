/*
 * Reply-parsing checks for the manual-transmission probe.
 *
 * The replies below are the shapes the adapter produces, taken from the batch
 * parser in sensors.cpp and the ELM multi-frame convention it already handles:
 * a single "41" echo, PID+data pairs in request order, and "0:" / "1:" prefixes
 * when the reply spans frames.
 */

#include <stdio.h>
#include <string.h>

#include "mtprobe_parse.h"

static int g_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            g_failed++;                                                   \
        }                                                                 \
    } while (0)

static int parse(const char *reply, mtprobe_hit_t *hits, int cap)
{
    uint8_t bytes[64];
    int n = mtprobe_reply_bytes(reply, bytes, (int)sizeof(bytes));
    if (n < 0) return -1;
    return mtprobe_split(bytes, n, hits, cap);
}

static void test_single_pid(void)
{
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    int n = parse("41 0C 1A F8\r", h, MTPROBE_MAX_HITS);
    CHECK(n == 1);
    CHECK(h[0].pid == 0x0C && h[0].len == 2);
    CHECK(h[0].data[0] == 0x1A && h[0].data[1] == 0xF8);
}

static void test_anchors_plus_candidates(void)
{
    /* RPM, speed, throttle, then torque percent and reference torque. */
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    int n = parse("41 0C 0B B8 0D 3C 11 40 62 7D 63 02 58\r", h, MTPROBE_MAX_HITS);
    CHECK(n == 5);
    CHECK(h[0].pid == 0x0C && h[1].pid == 0x0D && h[2].pid == 0x11);
    CHECK(h[3].pid == 0x62 && h[3].data[0] == 0x7D);
    CHECK(h[4].pid == 0x63 && h[4].data[0] == 0x02 && h[4].data[1] == 0x58);
}

static void test_unsupported_pid_is_simply_absent(void)
{
    /* Asked for 0C 0D 11 61 62 63; the ECU answered only the first four it has. */
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    int n = parse("41 0C 0B B8 0D 3C 11 40 62 7D\r", h, MTPROBE_MAX_HITS);
    CHECK(n == 4);
    CHECK(h[3].pid == 0x62);
}

static void test_multiframe_prefixes(void)
{
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    const char *reply = "00F\r0: 41 0C 0B B8 0D 3C\r1: 11 40 64 01 02 03 04 05\r";
    int n = parse(reply, h, MTPROBE_MAX_HITS);
    CHECK(n == 4);
    CHECK(h[3].pid == 0x64 && h[3].len == 5);
    CHECK(h[3].data[0] == 0x01 && h[3].data[4] == 0x05);
}

static void test_transmission_gear(void)
{
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    int n = parse("41 A4 00 00 0B B8\r", h, MTPROBE_MAX_HITS);
    CHECK(n == 1);
    CHECK(h[0].pid == 0xA4 && h[0].len == 4);
}

static void test_support_bitmap(void)
{
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    int n = parse("41 00 BE 1F A8 13\r", h, MTPROBE_MAX_HITS);
    CHECK(n == 1);
    CHECK(h[0].pid == 0x00 && h[0].len == 4);
    CHECK(h[0].data[0] == 0xBE && h[0].data[3] == 0x13);
}

static void test_no_data_and_noise(void)
{
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    uint8_t bytes[16];
    CHECK(mtprobe_reply_bytes("NO DATA\r", bytes, 16) == -1);
    CHECK(mtprobe_reply_bytes("", bytes, 16) == -1);
    CHECK(mtprobe_reply_bytes("SEARCHING...\rUNABLE TO CONNECT\r", bytes, 16) == -1);
    CHECK(mtprobe_reply_bytes("41 0C ZZ\r", bytes, 16) == -1);
    CHECK(parse("NO DATA\r", h, MTPROBE_MAX_HITS) == -1);
}

static void test_truncation_keeps_what_was_whole(void)
{
    /* The 0x63 entry is cut mid-value; the complete ones before it survive. */
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    int n = parse("41 0C 0B B8 0D 3C 63 02\r", h, MTPROBE_MAX_HITS);
    CHECK(n == 2);
    CHECK(h[1].pid == 0x0D);
}

static void test_unknown_pid_stops_the_walk(void)
{
    /* 0xEE has no known length, so nothing after it can be located. */
    mtprobe_hit_t h[MTPROBE_MAX_HITS];
    int n = parse("41 0C 0B B8 EE 01 02 0D 3C\r", h, MTPROBE_MAX_HITS);
    CHECK(n == 1);
    CHECK(mtprobe_pid_len(0xEE) == 0);
}

static void test_output_is_bounded(void)
{
    uint8_t bytes[4];
    mtprobe_hit_t h[1];
    CHECK(mtprobe_reply_bytes("41 0C 0B B8 0D 3C 11 40\r", bytes, 4) == 4);
    CHECK(mtprobe_split((const uint8_t[]){ 0x0D, 0x3C, 0x11, 0x40 }, 4, h, 1) == 1);
}

int main(void)
{
    test_single_pid();
    test_anchors_plus_candidates();
    test_unsupported_pid_is_simply_absent();
    test_multiframe_prefixes();
    test_transmission_gear();
    test_support_bitmap();
    test_no_data_and_noise();
    test_truncation_keeps_what_was_whole();
    test_unknown_pid_stops_the_walk();
    test_output_is_bounded();

    if (g_failed) {
        printf("mtprobe_parse: %d check(s) failed\n", g_failed);
        return 1;
    }
    printf("mtprobe_parse: all checks passed\n");
    return 0;
}
