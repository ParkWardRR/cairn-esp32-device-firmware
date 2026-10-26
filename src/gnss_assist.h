/*
 * Assisted GNSS on the device: AssistNow Predictive Orbits and time aiding for the
 * receiver behind the co-processor. The decisions are lib/cairn_agnss (host-tested);
 * this file moves bytes between the card, the server and the receiver.
 *
 * Why: measured 2026-10-25 from the card, a cold receiver in the M240i (OBD port low
 * under the dash) saw no satellites for minutes and gave its first usable fix after
 * 8.5 and 10.5 minutes (30 on 2026-10-24). It lacks orbits and time at power-up.
 *
 * Three parts, each on the task that owns what it touches:
 *
 *   - The controller (lifecycle, core 1) owns the card. It loads the stored orbits
 *     into PSRAM at boot, records the wall-clock observations the phone and the
 *     network give it, and writes a newly read receiver identity to the card.
 *   - An upload session (net_upload, the same task) reports the receiver's identity
 *     to the server once per boot and fetches fresh orbits when the copy is missing
 *     or getting short, after the bundles, on the connection already open
 *     (format/v3 §6.4: POST /api/v2/gnss-assist/receiver, GET /api/v2/gnss-assist
 *     with If-None-Match).
 *   - The sensing task (core 0) owns the co-processor link. On every start of the
 *     receiver it injects, paced one ATGDS line per 25 ms and never blocking: the
 *     time (once trustworthy UTC is known) and the position hint again after it,
 *     then today's predictions, then the almanacs, and stops at the first fix. If
 *     the identity is not known it asks the receiver for it (UBX-SEC-UNIQID,
 *     UBX-MON-VER) and looks for the replies in the ATGRR buffer.
 *
 * One INFO line per injection run says what was sent, how old the data was, and
 * whether time went with it and from where.
 */

#ifndef CAIRN_GNSS_ASSIST_H
#define CAIRN_GNSS_ASSIST_H

#include <stdbool.h>
#include <stdint.h>

#include "net_http.h"

/* ── controller ───────────────────────────────────────────────────────────── */

/* Load what the card holds. Call once, after the card is mounted and before the
 * sensing task starts. */
void gnss_assist_begin(void);

/* A wall-clock observation from a source that is not the receiver itself: the
 * phone's UTC sync or the modem's network time. */
void gnss_assist_note_time(uint64_t utc_ms, uint32_t acc_ms, uint8_t source, uint32_t at_ms);

/* The controller slept this long (light sleep: millis() ran on the RTC slow clock,
 * whose drift is what the time accuracy has to allow for). */
void gnss_assist_note_standby(uint32_t ms);

/* Persist anything the sensing task learned. Cheap; call every tick. */
void gnss_assist_controller_tick(void);

/* The server exchange, inside an upload session that is already open. */
void gnss_assist_sync(cairn_http_t *h, const char *label,
                      bool (*should_abort)(void *), void *abort_ctx);

/* ── sensing task ─────────────────────────────────────────────────────────── */

/* One step: at most one ATGDS line or one ATGRR read. Call every loop. */
void gnss_assist_sense_step(uint32_t now_ms);

/* The receiver produced a fix the gate accepted. */
void gnss_assist_on_fix(void);

#endif /* CAIRN_GNSS_ASSIST_H */
