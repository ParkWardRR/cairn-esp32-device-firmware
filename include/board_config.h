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

#define CAIRN_PIN_SD_CS          5   /* microSD over SPI */
#define CAIRN_PIN_LED            4
#define CAIRN_PIN_BUZZER         25

#define CAIRN_PIN_LINK_UART_RX   13  /* OBD coprocessor */
#define CAIRN_PIN_LINK_UART_TX   14

#define CAIRN_PIN_GPS_POWER      12
#define CAIRN_PIN_GPS_UART_RXD   34
#define CAIRN_PIN_GPS_UART_TXD   26

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
