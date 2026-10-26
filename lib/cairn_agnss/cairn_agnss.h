/*
 * Aiding messages for the u-blox receiver behind the co-processor: the portable half.
 *
 * The receiver is not wired to the ESP32 (gpsBeginExt fails on this unit); it is
 * reached through the co-processor's "ATGDS" command, which takes space-separated hex
 * bytes and writes them to the receiver (FreematicsPlus gpsBegin). The co-processor
 * passes no UBX reply back (bench, 2026-10-26: no ACK, no poll answer), so nothing
 * sent here is ever acknowledged. Two rules follow: every message is a single
 * fire-and-forget line, and every accuracy claimed is honest, because an
 * over-confident aid is what u-blox warns can stall a start ("prolonged or even
 * failed start-ups") and nothing would tell the device it had done so.
 *
 * Taken from the parked feat/gnss-assist branch (d3fbb64), which host-tested these
 * encoders against an independent encoding (Python's struct, 2026-10-25); the
 * AssistNow orbit half of that branch is blocked and is not here.
 *
 * Pure C, host-tested in test/host/agnss_test.c.
 */

#ifndef CAIRN_AGNSS_H
#define CAIRN_AGNSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sync, class, id, length and checksum. */
#define CAIRN_UBX_OVERHEAD 8u

/* Bytes per ATGDS line, and the line buffer that holds one: "ATGDS", "XX " per byte
 * with the last space turned into '\r', and a NUL. 28 is the POS_LLH hint, the longest
 * line this firmware has ever sent (the vendor's own is 27); TIME_UTC is 32, so it goes
 * as two lines. */
#define CAIRN_AGNSS_LINE_BYTES 28u
#define CAIRN_AGNSS_LINE_CAP   (5u + 3u * CAIRN_AGNSS_LINE_BYTES + 1u)

/*
 * Time aiding. The accuracy sent is the observation's own (phone 500 ms, network
 * 2000 ms, as lifecycle records them) plus the clock's possible drift since, plus the
 * trip through the co-processor. ESP32 millis() runs from the 40 MHz crystal while
 * awake (tens of ppm; 50 is taken) but from the RTC slow clock in light sleep, whose
 * drift on this board has not been measured: 2 % is taken, pessimistic for a
 * calibrated RC oscillator. The transit allowance covers one ATGDS round trip and the
 * receiver's UART; it is not measured either and is generous for that reason. An
 * estimate looser than 15 minutes is not sent: an honest time that loose no longer
 * narrows the search.
 */
#define CAIRN_AGNSS_TIME_TRANSIT_MS  200u
#define CAIRN_AGNSS_AWAKE_DRIFT_PPM  50u
#define CAIRN_AGNSS_SLEEP_DRIFT_PPM  20000u
#define CAIRN_AGNSS_TIME_MAX_ACC_MS  (15u * 60u * 1000u)

/* Fletcher checksum over class..payload end. */
void cairn_ubx_checksum(const uint8_t *body, size_t n, uint8_t *a, uint8_t *b);

/* A frame into out; 0 when cap is too small. */
size_t cairn_ubx_encode(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len,
                        uint8_t *out, size_t cap);

int32_t cairn_agnss_days_from_civil(int y, int m, int d);
void    cairn_agnss_civil_from_days(int32_t days, int *y, int *m, int *d);

/* The accuracy an observation still has elapsed_ms later, asleep_ms of which in light
 * sleep. Saturates rather than wraps. */
uint32_t cairn_agnss_time_acc(uint32_t obs_acc_ms, uint32_t elapsed_ms, uint32_t asleep_ms);

/* UBX-MGA-INI-TIME_UTC (32 bytes), reference "on receipt", leap seconds unknown. */
size_t cairn_agnss_time_utc_frame(uint64_t utc_ms, uint32_t acc_ms, uint8_t out[32]);

/* UBX-MGA-INI-POS_LLH (28 bytes). */
size_t cairn_agnss_pos_llh_frame(int32_t lat_e7, int32_t lon_e7, int32_t alt_cm,
                                 uint32_t acc_cm, uint8_t out[28]);

/* One ATGDS line for n <= CAIRN_AGNSS_LINE_BYTES bytes; its length, 0 if it does not
 * fit. */
size_t cairn_agnss_atgds_line(const uint8_t *bytes, size_t n, char *line, size_t cap);

/*
 * A whole frame as ATGDS lines of at most CAIRN_AGNSS_LINE_BYTES: line i of it into
 * `line`, its length, or 0 past the last. The receiver parses a byte stream and does
 * not see where one ATGDS ended, so a frame may span lines.
 */
size_t cairn_agnss_frame_line(const uint8_t *frame, size_t len, unsigned i, char *line, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_AGNSS_H */
