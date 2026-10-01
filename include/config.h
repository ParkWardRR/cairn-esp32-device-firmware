/*
 * Build-time configuration, with placeholders when secrets.h is absent.
 *
 * The repository is public, so every value here is either a placeholder or a
 * policy constant. Real hostnames, credentials and the pinned server key live
 * in src/secrets.h, which is gitignored.
 */

#ifndef CAIRN_CONFIG_H
#define CAIRN_CONFIG_H

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef CAIRN_WIFI_SSID
#define CAIRN_WIFI_SSID "unconfigured"
#endif

#ifndef CAIRN_WIFI_PASSWORD
#define CAIRN_WIFI_PASSWORD ""
#endif

#ifndef CAIRN_SERVER_HOST
#define CAIRN_SERVER_HOST "cairn.example.lan"
#endif

#ifndef CAIRN_SERVER_PORT
#define CAIRN_SERVER_PORT 8080
#endif

/*
 * An all-zero key verifies nothing, so an unconfigured device uploads but never
 * prunes. That is the correct failure direction: a full card loses nothing,
 * while a wrongly authorized prune loses a trip permanently.
 */
#ifndef CAIRN_SERVER_RECEIPT_KEY_HEX
#define CAIRN_SERVER_RECEIPT_KEY_HEX \
    "0000000000000000000000000000000000000000000000000000000000000000"
#endif

#ifndef CAIRN_FIRMWARE_VERSION
#define CAIRN_FIRMWARE_VERSION "cairn-v2.0.0-dev"
#endif

/* ── capture policy ───────────────────────────────────────────────────────── */

/*
 * Bumped whenever any threshold below changes. It is recorded in every
 * STATE_TRANSITION and in the manifest, so a decision in the data can always be
 * explained by the policy that was actually in force — rather than by whatever
 * the thresholds happen to be when the data is read back.
 */
#define CAIRN_POLICY_VERSION 1

/* Sampling cadences, milliseconds. */
#define CAIRN_GNSS_PERIOD_MS      1000
#define CAIRN_IMU_WINDOW_MS       1000
#define CAIRN_OBD_PERIOD_MS       2000
#define CAIRN_HEALTH_PERIOD_MS    30000

/*
 * Trip start and stop use separate thresholds with a dwell requirement, so a
 * single noisy sample cannot start or end a trip. Scores are hundredths, to
 * match the STATE_TRANSITION payload's start_score_e2 / stop_score_e2 fields.
 */
#define CAIRN_START_SCORE_THRESHOLD_E2  150  /* 1.50 */
#define CAIRN_STOP_SCORE_THRESHOLD_E2   40   /* 0.40 */
#define CAIRN_START_DWELL_MS            3000
#define CAIRN_STOP_DWELL_MS             120000 /* 2 min of stillness ends a trip */

/* Motion evidence thresholds. */
#define CAIRN_MOTION_ACCEL_RMS_MG   120
#define CAIRN_MOTION_SPEED_CMPS     280  /* ~10 km/h */

/*
 * Pre-trip ring: samples captured before a trip is declared are kept and
 * written with CAIRN_FLAG_PRETRIP, so the beginning of a drive is not lost to
 * the dwell requirement. The flag matters — these samples are real data, but
 * they were recorded before the device had decided a trip was underway.
 */
#define CAIRN_PRETRIP_RING_SAMPLES 16

/* ── sync policy ──────────────────────────────────────────────────────────── */

#define CAIRN_SYNC_CONNECT_TIMEOUT_MS 20000
#define CAIRN_SYNC_HTTP_TIMEOUT_MS    15000

/*
 * Sync only while stopped. Uploading during a drive competes with capture for
 * both CPU and the SPI bus, and the data is not time-critical.
 */
#define CAIRN_SYNC_MIN_IDLE_MS 10000

#endif /* CAIRN_CONFIG_H */
