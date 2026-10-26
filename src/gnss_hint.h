/*
 * Position and time hints for the GNSS receiver, and its time to first fix.
 *
 * A cold receiver in the M240i drew nothing for the first eight minutes of the drive of
 * 2026-10-25; at each start it is told only where the car was last fixed, at +/- 10 km
 * (sensors.cpp). This adds two hints, each sent at most once per receiver start and only
 * before that start's first accepted fix:
 *
 *   - **Wi-Fi position.** The first scan without a fix (wifiscan_runner, ~14 s after
 *     boot) is looked up in the access-point positions the server has learned
 *     (lib/cairn_wifiloc, GET /api/v2/wifi-anchors). If it places the car under the
 *     library's rules, UBX-MGA-INI-POS_LLH goes with that position and an honest 3-D
 *     accuracy. It is sent after the last-fix hint, so it is the position the receiver
 *     holds last; later scans retry until one places the car or a fix arrives.
 *   - **Time.** The first phone or network time (TIME_OBSERVATION, lifecycle) becomes
 *     UBX-MGA-INI-TIME_UTC with its accuracy grown by the clock's drift since
 *     (lib/cairn_agnss), and is not sent past 15 minutes of uncertainty.
 *
 * Neither is acknowledged: the co-processor returns no UBX replies (bench, 2026-10-26).
 * So the measure is the first fix: one INFO line per receiver start, when its first fix
 * is accepted, saying how long it took and which hints went (README, "Reading the TTFF
 * line").
 *
 * The anchor table is fetched by an upload session that is already open, after its
 * bundles, when the card copy is missing or a day old (If-None-Match: the copy's
 * SHA-256), and kept on the card sealed (cairn_wifiloc.h). It lives in PSRAM; this
 * module's own state is a PSRAM allocation too, since static DRAM is nearly full.
 *
 * Controller task only, every entry point: the scan runner, the uplink sessions and the
 * fact handlers all run there, so the table needs no lock. The receiver's start count
 * comes from sensors.cpp, which keeps it under its own lock.
 */
#ifndef CAIRN_GNSS_HINT_H
#define CAIRN_GNSS_HINT_H

#include <stdbool.h>
#include <stdint.h>

#include "cairn_format.h"
#include "cairn_store.h"

/* At boot, after the card is mounted: the storage root (the card copy's key) and the
 * table from the card. */
void gnss_hint_begin(const cairn_storage_identity_t *storage);

/* Each controller pass: notices a receiver start, sends a time hint still owed to it. */
void gnss_hint_tick(uint32_t standby_total_ms);

/* A completed scan, as the record keeps it (the strongest 16, filtered). */
void gnss_hint_on_scan(const cairn_wifi_scan_t *scan, bool learning);

/* A time observation that passed lifecycle's plausibility floor, any source. */
void gnss_hint_note_time(uint64_t utc_ms, uint32_t acc_ms, uint8_t source, uint32_t mono_ms,
                         uint32_t standby_total_ms);

/* An internal fix the gate accepted (fix_type >= 2). */
void gnss_hint_on_fix(uint32_t mono_ms, uint8_t sats);

#if CAIRN_WIFI_UPLINK || CAIRN_LTE_UPLINK
#include "net_http.h"

/* The anchor table, on a session's open connection after its bundles. Returns at once
 * when no fetch is due or the session is being aborted. */
void gnss_hint_sync(cairn_http_t *h, const char *label, bool (*should_abort)(void *), void *abort_ctx);
#endif

#endif /* CAIRN_GNSS_HINT_H */
