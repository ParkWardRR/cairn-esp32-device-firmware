/*
 * Mode 01 reply parsing for the manual-transmission probe.
 *
 * Portable C with no Arduino dependency, so the host suite can run it against
 * reply text without a car. The probe asks for several PIDs in one request and
 * the ECU answers only the ones it supports, so the reply cannot be read
 * positionally: each PID's data length has to be known to find where the next
 * one starts. That is what this file carries.
 */

#ifndef CAIRN_MTPROBE_PARSE_H
#define CAIRN_MTPROBE_PARSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTPROBE_MAX_DATA 5
#define MTPROBE_MAX_HITS 8

typedef struct {
    uint8_t pid;
    uint8_t len;
    uint8_t data[MTPROBE_MAX_DATA];
} mtprobe_hit_t;

/*
 * Data bytes for a Mode 01 PID the probe knows how to walk, or 0 when it does
 * not. Zero is also the stop signal for mtprobe_split: a PID of unknown length
 * makes everything after it unlocatable.
 */
uint8_t mtprobe_pid_len(uint8_t pid);

/*
 * Extract the bytes that follow the first "41 " in an adapter reply. Multi-frame
 * replies arrive with "0:" / "1:" line prefixes, which are skipped, and a
 * length line before the first prefix is ignored because it precedes the "41".
 *
 * Returns the byte count, or -1 when there is no "41 " (NO DATA, a timeout, a
 * CAN error) or a token that is not hex.
 */
int mtprobe_reply_bytes(const char *reply, uint8_t *out, int cap);

/*
 * Walk PID,data,PID,data... as an ECU returns them for a multi-PID request.
 * Stops at the first PID of unknown length or the first truncated entry, and
 * returns the entries recovered so far. Returns the number of hits.
 */
int mtprobe_split(const uint8_t *bytes, int nbytes, mtprobe_hit_t *hits, int cap);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_MTPROBE_PARSE_H */
