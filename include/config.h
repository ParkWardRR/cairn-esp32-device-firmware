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

/*
 * OTA is enabled only when an update key is pinned at build time. A device that
 * cannot verify an update has no business installing one, so undefined means
 * off — it will not even fetch.
 */
#ifdef CAIRN_UPDATE_KEY_HEX
#define CAIRN_OTA_AVAILABLE 1
#else
#define CAIRN_OTA_AVAILABLE 0
#endif

/* Checked while parked; see docs/ota.md for why it is not more frequent. */
#define CAIRN_OTA_CHECK_INTERVAL_MS 3600000

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

/*
 * PID validation build. Prints the Mode 01 support bitmaps and, for every PID
 * the firmware reads, the raw ECU reply beside the library's converted value —
 * the only way to check a conversion rather than reason about it. Capture is
 * unaffected, so one drive yields the diagnostic and a usable bundle.
 */
#ifndef CAIRN_PIDTEST
#define CAIRN_PIDTEST 0
#endif
#define CAIRN_PIDTEST_PERIOD_MS 5000
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

/* ── power management ─────────────────────────────────────────────────────── */

/*
 * How long the vehicle must be at rest before the device stands by. Longer than
 * the trip-stop dwell on purpose: sealing a trip and immediately sleeping would
 * miss a driver who stopped to post a letter.
 */
#define CAIRN_STANDBY_IDLE_MS 300000  /* 5 minutes */

/*
 * Voltage that means the engine is running, from the v1 firmware's measured
 * value. A resting battery sits near 12.4 V and an alternator pushes well above
 * 13 V, so this is a wide margin rather than a fine one.
 */
#define CAIRN_ENGINE_ON_MV 13200

/*
 * Dwell required before local evidence is accepted as a started drive.
 *
 * Both signals are bus-silent, which is the point: they let the device decide a
 * drive has begun without transmitting anything the vehicle network can see.
 * See the parked-silence invariant in cairn_power.h.
 *
 * The dwells reject the things that look momentarily like a drive — a door
 * slam, a tow nudge, someone leaning on the car, a central-locking actuator
 * twitching the supply rail. Either signal on its own has to persist; both
 * together are accepted promptly, since a lifted rail and sustained movement at
 * the same time is not something a parked car does.
 */
#define CAIRN_DRIVE_VOLTAGE_DWELL_MS 15000
#define CAIRN_DRIVE_MOTION_DWELL_MS  10000
#define CAIRN_DRIVE_BOTH_DWELL_MS    2000

/*
 * How often the controller samples the supply rail while awake.
 *
 * Not taken from DEVICE_HEALTH, which arrives every 30 s — too coarse to open
 * the bus promptly after an engine start, which would leave the first half
 * minute of OBD missing from every trip. Two seconds costs co-processor link
 * traffic, internal to the dongle, and nothing on the vehicle bus.
 */
#define CAIRN_BATTERY_POLL_MS 2000

/* Poll interval while standing by. The core light-sleeps in between. */
#define CAIRN_STANDBY_POLL_MS 1000

/*
 * A standby this long emits a health record anyway, so a parked device stays
 * distinguishable from a dead one.
 */
#define CAIRN_STANDBY_HEARTBEAT_MS 21600000  /* 6 hours */

/*
 * How long to stay awake after a heartbeat wake before standing by again.
 *
 * The co-processor needs roughly fifteen seconds after leaving low-power mode
 * before it will report a supply voltage — measured from the card, where the
 * health record written straight after a resume says "battery unknown". Since
 * recording that voltage is the heartbeat's only purpose, it has to wait for it.
 */
#define CAIRN_HEARTBEAT_SETTLE_MS 20000

/* Clock to drop to while standing by. 80 MHz is the lowest frequency that
 * keeps the Wi-Fi and I2C peripherals usable without re-initialisation. */
#define CAIRN_STANDBY_CPU_MHZ 80

/* ── task stacks ──────────────────────────────────────────────────────────── */

/*
 * The Arduino loop task runs the transition controller, and therefore runs the
 * seal: Merkle tree over up to CAIRN_MAX_MEMBERS members, a deterministic CBOR
 * manifest, and an Ed25519 signature. The deepest locals on that path are
 * cairn_member_t sorted[16] and signing[512] in cf_manifest.c, on top of
 * TweetNaCl's nested gf and i64[64] working arrays.
 *
 * Arduino's default is 8192, and that is not enough: the first seal attempted on
 * real hardware tripped the stack canary in loopTask and panicked, which on a
 * firmware that resumes its capture at boot turns into a reboot loop that
 * re-appends frames every cycle. Measured headroom is logged at the end of the
 * seal (see log_stack_headroom), so this number can be checked rather than
 * trusted.
 */
#define CAIRN_LOOP_STACK_BYTES 16384

/* Warn below this much free stack. Roughly a quarter of the total: enough slack
 * that the warning arrives before the canary does. */
#define CAIRN_STACK_WARN_BYTES 4096

/* ── sync policy ──────────────────────────────────────────────────────────── */

#define CAIRN_SYNC_CONNECT_TIMEOUT_MS 20000
#define CAIRN_SYNC_HTTP_TIMEOUT_MS    15000

/*
 * Sync only while stopped. Uploading during a drive competes with capture for
 * both CPU and the SPI bus, and the data is not time-critical.
 */
#define CAIRN_SYNC_MIN_IDLE_MS 10000

/*
 * How long to wait before retrying a failed sync.
 *
 * Its own timer, not idle_since_ms. Sharing that one meant every upload reset
 * the standby dwell and the device never slept.
 */
#define CAIRN_SYNC_RETRY_MS 60000

#endif /* CAIRN_CONFIG_H */
