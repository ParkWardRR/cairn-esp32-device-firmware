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

#ifndef CAIRN_SERVER_TLS_PORT
#define CAIRN_SERVER_TLS_PORT 8443
#endif

/*
 * mTLS is active only when a CA is pinned at build time. There is no runtime
 * switch: a transport that could be downgraded by editing a file on the card
 * would not be worth verifying.
 */
#ifdef CAIRN_SERVER_CA_PEM
#define CAIRN_TLS_AVAILABLE 1
#else
#define CAIRN_TLS_AVAILABLE 0
#endif

/*
 * Where the device's own certificate and key live. On the card rather than in
 * firmware so they can be reissued without a reflash — which matters because
 * the certificate's CommonName must be the device id, and that is not known
 * until the hardware has booted once.
 */
#define CAIRN_PATH_CLIENT_CERT "/cairn/certs/client.crt"
#define CAIRN_PATH_CLIENT_KEY  "/cairn/certs/client.key"

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

/*
 * Event-adaptive sampling (§4.9.1). The nominal cadences below become upper
 * bounds during a trip rather than fixed rates: faster when the vehicle is
 * doing something worth resolving, never slower. Adaptation can only add
 * detail, so a reader's floor of one record per nominal period still holds.
 */
#define CAIRN_ADAPTIVE_SAMPLING 1

/* Nominal sampling cadences, milliseconds. */
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
 *
 * Sized for 45 s of pre-roll. The ring holds every record type the capture
 * chain carries, so the budget is the sum of their rates rather than GNSS
 * alone: GNSS at 1 Hz, IMU summaries at 1 Hz and OBD at 0.5 Hz comes to 2.5
 * records per second, so 45 s needs about 113 slots. 128 gives headroom for a
 * faster cadence without re-deriving this.
 *
 * At 40 bytes per entry that is ~5 KB of RAM held permanently. Worth it: the
 * first seconds of a drive are the hardest part to reconstruct afterwards, and
 * 5 KB of a 320 KB budget is cheap insurance.
 */
#define CAIRN_PREROLL_WINDOW_MS     45000
#define CAIRN_PREROLL_RING_SAMPLES  128

/* ── sync policy ──────────────────────────────────────────────────────────── */

#define CAIRN_SYNC_CONNECT_TIMEOUT_MS 20000
#define CAIRN_SYNC_HTTP_TIMEOUT_MS    15000

/*
 * Sync only while stopped. Uploading during a drive competes with capture for
 * both CPU and the SPI bus, and the data is not time-critical.
 */
#define CAIRN_SYNC_MIN_IDLE_MS 10000

#endif /* CAIRN_CONFIG_H */
