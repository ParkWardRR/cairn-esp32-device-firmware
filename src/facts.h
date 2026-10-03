/*
 * Facts: what the sensors observed, as messages.
 *
 * Sensing runs in its own task and only ever *reports*. One controller drains
 * these and owns every piece of lifecycle state, decides every transition, and
 * is the only thing that touches the card.
 *
 * This is a correctness fix rather than an architectural preference. Previously
 * the IMU was read once per main-loop pass, in the same pass that wrote frames
 * to the card. An SD write takes tens of milliseconds, and during it no
 * accelerometer samples were taken — so the RMS and peak values a window
 * reported were computed over whatever moments happened not to coincide with
 * I/O. The window claimed to summarize a second of motion and did not. With
 * sensing on its own task the sample rate no longer depends on what the
 * controller is doing.
 *
 * The queue is bounded and overflow is counted, not hidden: a dropped fact is
 * reported in DEVICE_HEALTH, because a gap the data does not admit to is worse
 * than a gap it does.
 */

#ifndef CAIRN_FACTS_H
#define CAIRN_FACTS_H

#include <stdbool.h>
#include <stdint.h>

#include "cairn_format.h"

typedef enum : uint8_t {
    FACT_GNSS_SAMPLE = 1,  /* a new fix */
    FACT_GNSS_NO_FIX,      /* a sample period passed with no new fix */
    FACT_GNSS_UTC_BASIS,   /* the receiver produced a usable date for the first time */
    FACT_IMU_SUMMARY,      /* one completed accumulation window */
    FACT_MOTION,           /* live accelerometer RMS, for scoring between windows */
    FACT_OBD_SNAPSHOT,     /* at least one PID answered */
    FACT_OBD_SILENT,       /* the ECU answered nothing */
    FACT_OBD_EXTENDED,     /* boost, mixture and trims; see sensors_read_obd_extended */
    FACT_HEALTH,           /* a device health reading */
} fact_kind_t;

typedef struct {
    fact_kind_t kind;

    /*
     * When the observation was made, in monotonic milliseconds — stamped by the
     * sensor task, not by the controller. The difference matters: a fact that
     * waited in the queue must carry the time it was observed, or a backed-up
     * queue would silently relabel old samples as recent ones.
     */
    uint32_t monotonic_ms;

    union {
        cairn_gnss_sample_t   gnss;
        cairn_imu_summary_t   imu;
        cairn_obd_snapshot_t  obd;
        cairn_obd_extended_t  obd_ext;
        cairn_device_health_t health;

        struct {
            uint64_t utc_ms;
            uint32_t acc_ms;
        } utc;

        struct {
            uint16_t accel_rms_mg;
        } motion;
    } data;
} fact_t;

/*
 * Depth sized for the longest the controller can be busy. A sync can occupy it
 * for tens of seconds, so the depth has to cover that window at the current
 * fact rate.
 *
 * Raising the IMU window to 100 ms took that rate from roughly 5 facts per
 * second to about 13 — ten IMU summaries, one new GNSS fix, two OBD records —
 * which would have cut 192 slots from ~38 s of cover to ~15 s, short of a
 * sync. 512 restores about 39 s.
 *
 * Sized against the measured rate rather than rounded up: fact_t is 48 bytes,
 * so each 256 slots costs 12 KB of DRAM that the TLS handshake also wants.
 * Syncing only happens while idle, when the facts being dropped are the least
 * valuable, and anything beyond that is counted rather than hidden; the drive
 * that set these rates dropped none at the old rate, so this is headroom
 * rather than a fix.
 */
#define CAIRN_FACT_QUEUE_DEPTH 512

#endif /* CAIRN_FACTS_H */
