/*
 * Assisted GNSS for the u-blox receiver behind the co-processor: the portable half.
 *
 * Why. Measured 2026-10-25 from the dongle's card: in the M240i (OBD port low under
 * the dash) a cold receiver saw no satellites for minutes, then three or four, and
 * gave its first usable fix after 8.5 and 10.5 minutes; on 2026-10-24, 30 minutes.
 * The position hint already sent at boot (UBX-MGA-INI-POS_LLH) does not help much on
 * its own: what the receiver lacks is orbits and time. The server downloads u-blox
 * AssistNow Predictive Orbits (seven days of UBX-MGA-ANO plus almanacs) and the
 * dongle injects today's share at power-up, with UBX-MGA-INI-TIME_UTC from the
 * phone's or the network's clock.
 *
 * Everything here is pure: UBX framing and checksums, validating a downloaded
 * stream, choosing what to inject for a given UTC day, MGA-INI-TIME_UTC with an
 * honest accuracy, the pacing of injection into the co-processor's pass-through,
 * the receiver-identity scan and its JSON report, and when to ask the server
 * again. The device glue (src/gnss_assist.cpp) does I/O only, so all of this runs
 * in test/host/agnss_test.c.
 *
 * The pass-through. The receiver is not wired to the ESP32 (gpsBeginExt fails on
 * this unit); it is reached through the co-processor's "ATGDS" command, which
 * takes space-separated hex bytes and writes them to the receiver
 * (FreematicsPlus gpsBegin). The longest line this firmware has sent is 28 bytes
 * (the POS_LLH hint) and the vendor's own is 27, so injection is cut into lines
 * of at most 28 bytes. A frame may span lines: the receiver parses a byte stream
 * and has no idea where one ATGDS ended. A line never carries the tail of one
 * frame and the head of the next, so a lost line costs one message, not two.
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

/*
 * The largest stream the device downloads or keeps. Seven days of GPS, Galileo,
 * BeiDou and GLONASS predictions is 7 x 9,912 bytes and the four almanacs 7,280,
 * ~77 KB (u-blox's own table, 84 bytes per ANO message); the server trims to this
 * bound by whole days.
 */
#define CAIRN_AGNSS_MAX_BYTES (128u * 1024u)

/* Bytes per ATGDS line, and the line buffer that holds one: "ATGDS", "XX " per
 * byte with the last space turned into '\r', and a NUL. */
#define CAIRN_AGNSS_LINE_BYTES 28u
#define CAIRN_AGNSS_LINE_CAP   (5u + 3u * CAIRN_AGNSS_LINE_BYTES + 1u)

/*
 * One line every 25 ms: at most 1.1 KB/s of UBX into the receiver. Two limits
 * meet here. The co-processor writes to the receiver over its own UART, whose
 * speed is not known (38,400 baud is the receiver's default), and u-blox's host
 * guide warns that a fast host overflows a busy receiver ("adding small delays
 * during the transmission may be a simple but effective way to avoid
 * substantial loss of data"). And each line holds the co-processor link, which
 * OBD and the GNSS poll share, for one command round trip; at one line per 25 ms
 * the sensing loop (20 ms) gives up at most every other iteration. Today's
 * predictions for four constellations (~10 KB) then take ~9 s and the almanacs
 * (~7 KB) ~7 s, against a cold start measured in minutes.
 */
#define CAIRN_AGNSS_LINE_INTERVAL_MS 25u

/*
 * Time aiding. The accuracy sent is the observation's own (phone 500 ms, network
 * 2000 ms, as lifecycle records them) plus the clock's possible drift since, plus
 * the trip through the co-processor. ESP32 millis() runs from the 40 MHz crystal
 * while awake (tens of ppm; 50 is taken) but from the RTC slow clock in light
 * sleep, whose drift on this board has not been measured: 2 % is taken, which is
 * pessimistic for a calibrated RC oscillator, and the glue logs the error it
 * actually finds once the receiver reports the time, so it can be measured. The
 * transit allowance covers one ATGDS round trip and the receiver's UART; it is not
 * measured either and is generous for that reason. An estimate looser than 15
 * minutes is not sent: an honest time that loose no longer narrows the search.
 * Over-claiming is what u-blox warns about ("prolonged or even failed
 * start-ups"); a loose honest bound only costs some of the speed-up.
 */
#define CAIRN_AGNSS_TIME_TRANSIT_MS  200u
#define CAIRN_AGNSS_AWAKE_DRIFT_PPM  50u
#define CAIRN_AGNSS_SLEEP_DRIFT_PPM  20000u
#define CAIRN_AGNSS_TIME_MAX_ACC_MS  (15u * 60u * 1000u)

/* The receiver's two identity replies. SEC-UNIQID is 9 or 10 payload bytes
 * (u-blox M9: version 1, 5-byte id; M10: version 2, 6-byte id); MON-VER is
 * 40 + 30n bytes, n a handful of extension strings. */
#define CAIRN_AGNSS_SECID_MAX  (CAIRN_UBX_OVERHEAD + 10u)
#define CAIRN_AGNSS_MONVER_MAX (CAIRN_UBX_OVERHEAD + 40u + 30u * 12u)

/* "No day": UTC unknown. */
#define CAIRN_AGNSS_NO_DAY INT32_MIN

/* ── UBX framing ────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t        cls, id;
    uint16_t       len;        /* payload length */
    const uint8_t *payload;
    const uint8_t *frame;      /* sync to checksum */
    size_t         frame_len;
} cairn_ubx_t;

/* Fletcher checksum over class..payload end. */
void cairn_ubx_checksum(const uint8_t *body, size_t n, uint8_t *a, uint8_t *b);

/* A frame into out; 0 when cap is too small. */
size_t cairn_ubx_encode(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len,
                        uint8_t *out, size_t cap);

/* The frame starting exactly at *off: 1 and *off advanced past it, 0 at the clean
 * end of buf, -1 when the bytes there are not a whole valid frame. */
int cairn_ubx_next(const uint8_t *buf, size_t len, size_t *off, cairn_ubx_t *out);

/* The next valid frame at or after *off among other bytes (NMEA, noise, a frame
 * cut off by the end of a buffer). *off is left just past the frame found. */
bool cairn_ubx_find(const uint8_t *buf, size_t len, size_t *off, cairn_ubx_t *out);

/* The two polls ZTP needs answered (from the AssistNow Getting Started guide). */
extern const uint8_t CAIRN_UBX_POLL_SEC_UNIQID[8];
extern const uint8_t CAIRN_UBX_POLL_MON_VER[8];

/* ── dates ──────────────────────────────────────────────────────────────── */

int32_t cairn_agnss_days_from_civil(int y, int m, int d);
int32_t cairn_agnss_day_of_ms(uint64_t utc_ms);
void    cairn_agnss_civil_from_days(int32_t days, int *y, int *m, int *d);

/* ── a downloaded stream ────────────────────────────────────────────────── */

typedef struct {
    size_t   bytes;
    uint32_t frames;
    uint32_t ano;        /* UBX-MGA-ANO */
    uint32_t almanac;    /* every other MGA message except INI: the almanacs */
    uint32_t ini;        /* UBX-MGA-INI, which a stored stream must not contain */
    int32_t  first_day;  /* of the ANO messages; CAIRN_AGNSS_NO_DAY when none */
    int32_t  last_day;
} cairn_agnss_info_t;

/*
 * Is buf a stream worth keeping? Every byte must belong to a valid UBX-MGA frame,
 * at least one must be ANO, none INI (a stored INI-TIME_UTC would be injected
 * days late claiming its +/- 10 s), and the whole within CAIRN_AGNSS_MAX_BYTES.
 * info is filled either way, as far as the scan got.
 */
bool cairn_agnss_validate(const uint8_t *buf, size_t len, cairn_agnss_info_t *info);

/* The UTC day a UBX-MGA-ANO frame predicts for; CAIRN_AGNSS_NO_DAY otherwise. */
int32_t cairn_agnss_ano_day(const cairn_ubx_t *f);

typedef enum {
    CAIRN_AGNSS_PICK_ALL,      /* every frame (a small buffer of INI messages) */
    CAIRN_AGNSS_PICK_ANO,      /* UBX-MGA-ANO for days lo..hi */
    CAIRN_AGNSS_PICK_ALMANAC,  /* every MGA frame that is neither ANO nor INI */
} cairn_agnss_pick_t;

bool cairn_agnss_wanted(const cairn_ubx_t *f, cairn_agnss_pick_t pick, int32_t lo, int32_t hi);

/* What a pick would send. */
void cairn_agnss_count(const uint8_t *buf, size_t len, cairn_agnss_pick_t pick,
                       int32_t lo, int32_t hi, uint32_t *msgs, uint32_t *bytes);

/*
 * The UTC days to inject for an estimate of now: every day the estimate's
 * uncertainty touches (two near midnight), so a clock a few minutes off at 23:58
 * does not send tomorrow's orbits alone. Host-based Predictive Orbits per u-blox:
 * "selecting only those UBX-MGA-ANO messages with a date-stamp nearest the
 * current time".
 */
void cairn_agnss_day_window(uint64_t utc_ms, uint32_t acc_ms, int32_t *lo, int32_t *hi);

/* Narrow [lo, hi] to days the stream has, falling back to the nearest day within
 * one of the window; false when the stream has nothing that close. */
bool cairn_agnss_ano_window(const uint8_t *buf, size_t len, int32_t lo, int32_t hi,
                            int32_t *out_lo, int32_t *out_hi);

/* ── messages the device builds ─────────────────────────────────────────── */

/* The accuracy an observation still has elapsed_ms later, asleep_ms of which in
 * light sleep. Saturates rather than wraps. */
uint32_t cairn_agnss_time_acc(uint32_t obs_acc_ms, uint32_t elapsed_ms, uint32_t asleep_ms);

/* UBX-MGA-INI-TIME_UTC (32 bytes), reference "on receipt", leap seconds unknown. */
size_t cairn_agnss_time_utc_frame(uint64_t utc_ms, uint32_t acc_ms, uint8_t out[32]);

/* UBX-MGA-INI-POS_LLH (28 bytes). */
size_t cairn_agnss_pos_llh_frame(int32_t lat_e7, int32_t lon_e7, int32_t alt_cm,
                                 uint32_t acc_cm, uint8_t out[28]);

/* One ATGDS line for n <= CAIRN_AGNSS_LINE_BYTES bytes; its length, 0 if it does
 * not fit. */
size_t cairn_agnss_atgds_line(const uint8_t *bytes, size_t n, char *line, size_t cap);

/* ── pacing ─────────────────────────────────────────────────────────────── */

typedef struct {
    cairn_agnss_pick_t pick;
    int32_t  lo, hi;
    size_t   off;        /* the current frame's start */
    size_t   pos;        /* how much of it has been sent */
    bool     in_frame;
    bool     done;
    bool     malformed;
    uint32_t next_ms;
    uint32_t msgs, bytes, lines;
} cairn_agnss_pacer_t;

void cairn_agnss_pacer_start(cairn_agnss_pacer_t *p, cairn_agnss_pick_t pick,
                             int32_t lo, int32_t hi, uint32_t now_ms);

/*
 * The next line, if one is due at now_ms: its length, or 0 when it is not yet
 * time or the pick is exhausted (then p->done). buf must be the same bytes on
 * every call; a stream that turns out malformed ends the pick there.
 */
size_t cairn_agnss_pacer_next(cairn_agnss_pacer_t *p, const uint8_t *buf, size_t len,
                              uint32_t now_ms, char *line, size_t cap);

/* ── the receiver's identity ────────────────────────────────────────────── */

typedef struct {
    uint8_t  sec[CAIRN_AGNSS_SECID_MAX];
    uint8_t  sec_len;
    uint8_t  mon[CAIRN_AGNSS_MONVER_MAX];
    uint16_t mon_len;
} cairn_agnss_identity_t;

/* Take any SEC-UNIQID or MON-VER reply found among buf's bytes; the number taken. */
int  cairn_agnss_identity_scan(const uint8_t *buf, size_t len, cairn_agnss_identity_t *id);
bool cairn_agnss_identity_complete(const cairn_agnss_identity_t *id);

/* {"sec_uniqid":"<hex>","mon_ver":"<hex>"}: whole frames, upper-case hex, as ZTP
 * wants them. 0 when incomplete or cap is short. */
size_t cairn_agnss_identity_json(const cairn_agnss_identity_t *id, char *out, size_t cap);

/* "id=<hex> sw=<..> hw=<..> <extensions>", for the log. */
size_t cairn_agnss_identity_describe(const cairn_agnss_identity_t *id, char *out, size_t cap);

/* On the card the identity is just its two frames, back to back. */
bool cairn_agnss_identity_decode(const uint8_t *buf, size_t len, cairn_agnss_identity_t *id);

/* ── when to ask the server ─────────────────────────────────────────────── */

/*
 * The server refreshes once a day and the device keeps seven days, so a copy with
 * five or more days ahead of today is fresh enough; asking costs one request and
 * usually a 304 (the ETag is the copy's SHA-256). Checks are at least an hour
 * apart and at most a day, and once per boot regardless (millis() survives light
 * sleep but not a reset).
 */
#define CAIRN_AGNSS_RECHECK_MS     (24u * 3600u * 1000u)
#define CAIRN_AGNSS_MIN_RECHECK_MS (3600u * 1000u)
#define CAIRN_AGNSS_AHEAD_DAYS     5

bool cairn_agnss_fetch_due(bool have_copy, const cairn_agnss_info_t *info, int32_t today,
                           bool checked, uint32_t since_check_ms);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_AGNSS_H */
