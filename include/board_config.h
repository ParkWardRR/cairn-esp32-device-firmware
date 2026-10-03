/*
 * Board and filesystem layout for the Freematics ONE+ Model B (classic ESP32).
 *
 * Pin assignments are taken from the working v1 firmware
 * (firmware/freematics-base/lib/FreematicsPlus/FreematicsPlus.h), not guessed.
 *
 * Nothing here is a secret. Hostnames and keys live in secrets.h, which is
 * gitignored; this repository is public, so every value in this file is either
 * hardware-fixed or a placeholder.
 */

#ifndef CAIRN_BOARD_CONFIG_H
#define CAIRN_BOARD_CONFIG_H

/* ── pins ─────────────────────────────────────────────────────────────────── */

/*
 * Only the pins this firmware drives itself belong here.
 *
 * Everything reached through the vendored FreematicsPlus library — the OBD
 * coprocessor link, both GNSS paths, the molex socket — is configured by that
 * library's own defines in FreematicsPlus.h. Repeating them here would be a
 * trap: the copies look authoritative, nothing reads them, and "fixing" a pin
 * in this file would change nothing while appearing to.
 *
 * For reference, with the values the library actually uses:
 *
 *   OBD coprocessor   UART2, RX 13 / TX 14 at 115200 (or SPI, CS 2 / READY 13)
 *   GNSS, external    UART1, RX 34 / TX 26, power 12 — these are the *same*
 *                     pins as the 4-pin molex socket (the official guide
 *                     documents it as GND / GPIO26 / VCC / GPIO34), so the
 *                     external GNSS and the external I/O header are one
 *                     connector
 *   GNSS, internal    reached over the coprocessor link via ATGPSON, soft
 *                     serial at 38400
 *
 * On this unit (device type 15) gpsBeginExt() fails and gpsBegin() succeeds, so
 * the internal receiver is the one in use. Both paths verify that NMEA actually
 * arrives before reporting success, which is why trying them in order is safe.
 */

/*
 * SD mount retries. SPI card initialisation can fail transiently right after
 * power-up and succeed moments later; a single attempt meant one bad boot cost
 * the entire drive.
 */
#define CAIRN_SD_MOUNT_ATTEMPTS 5
#define CAIRN_SD_MOUNT_RETRY_MS 250

/* Interval for the deferred retry from loop() when the boot-time mount fails
 * entirely. Longer than the in-burst delay: this is a marginal contact, not a
 * settling period, so hammering it gains nothing. */
#define CAIRN_SD_REMOUNT_RETRY_MS 15000

#define CAIRN_PIN_SD_CS  5   /* microSD over SPI; the only bus this code owns */
#define CAIRN_PIN_LED    4   /* lit while faulted, cleared once capturing */

/* ── filesystem layout ────────────────────────────────────────────────────── */

/*
 * Sealed bundles and the log tree are deliberately separate subtrees. Pruning
 * walks only /cairn/bundles, so a log file can never be mistaken for data
 * awaiting a receipt, and a receipt can never authorize deleting a log.
 */
#define CAIRN_DIR_ROOT      "/cairn"
#define CAIRN_DIR_CAPTURE   "/cairn/capture"   /* the open, unsealed bundle */
#define CAIRN_DIR_BUNDLES   "/cairn/bundles"   /* sealed, awaiting a receipt */
#define CAIRN_DIR_RECEIPTS  "/cairn/receipts"  /* verified receipts */
#define CAIRN_DIR_LOGS      "/cairn/logs"
#define CAIRN_DIR_STATE     "/cairn/state"     /* prune journal, counters */

/* ── storage budget ───────────────────────────────────────────────────────── */

/*
 * Verbose logging is a testing aid, and a testing aid must not be able to cost
 * a trip. Below this much free space the SD sink shuts itself off and logging
 * continues over UART only: unsealed capture data has the stronger claim on the
 * card, because it cannot be reproduced.
 */
#define CAIRN_LOG_FREE_SPACE_FLOOR_MIB  64

/* A single log file is rotated at this size, and the whole log tree is capped,
 * oldest-first, at the total. */
#define CAIRN_LOG_ROTATE_BYTES          (2 * 1024 * 1024)
#define CAIRN_LOG_TOTAL_BUDGET_BYTES    (16 * 1024 * 1024)

#endif /* CAIRN_BOARD_CONFIG_H */
