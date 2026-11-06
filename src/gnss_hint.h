/*
 * Position and time hints for the GNSS receiver, and its time to first fix.
 *
 * A cold receiver in the M240i drew nothing for the first eight minutes of the drive of
 * 2026-10-25; at each start it is told only where the car was last fixed, at +/- 10 km
 * (sensors.cpp). This adds two hints, each sent at most once per receiver start and only
 * before that start's first accepted fix:
 *
 *   - **The phone's own fix.** The companion app writes one `GNSS_FIX` at link-ready with
 *     `companion_flags` bit 1 (`HINT`, ble/v1): its last known position, minutes old by
 *     construction and never stored as a sample. A phone that has just been in the
 *     owner's pocket places the car better than the saved fix, and it is connected
 *     within seconds of boot. Its accuracy is floored at 300 m, because the
 *     phone's own figure is honest about the phone and not about how far the car may have
 *     moved since (lib/cairn_phonehint).
 *   - **Time.** The first phone or network time (TIME_OBSERVATION, lifecycle) becomes
 *     UBX-MGA-INI-TIME_UTC with its accuracy grown by the clock's drift since
 *     (lib/cairn_agnss), and is not sent past 15 minutes of uncertainty.
 *
 * (There was a third: a Wi-Fi position, from scans looked up in server-learned access
 * point anchors. It went with the Wi-Fi stack — the firmware's transports are BLE and
 * LTE, and the one 2.4 GHz radio belongs to BLE.)
 *
 * Neither is acknowledged: the co-processor returns no UBX replies (bench, 2026-10-26).
 * So the measure is the first fix: one INFO line per receiver start, when its first fix
 * is accepted, saying how long it took and which hints went (README, "Reading the TTFF
 * line").
 *
 * This module's state is a PSRAM allocation, since static DRAM is nearly full.
 *
 * Controller task only, every entry point but the phone's write: the uplink sessions and
 * the fact handlers all run there. The receiver's start count comes from sensors.cpp,
 * which keeps it under its own lock.
 */
#ifndef CAIRN_GNSS_HINT_H
#define CAIRN_GNSS_HINT_H

#include <stdbool.h>
#include <stdint.h>

/* At boot. Allocates the module's state. */
void gnss_hint_begin(void);

/* Each controller pass: notices a receiver start, drains the phone's hint mailbox, and
 * sends a time hint still owed to it. */
void gnss_hint_tick(uint32_t standby_total_ms);

/* A time observation that passed lifecycle's plausibility floor, any source. */
void gnss_hint_note_time(uint64_t utc_ms, uint32_t acc_ms, uint8_t source, uint32_t mono_ms,
                         uint32_t standby_total_ms);

/* An internal fix the gate accepted (fix_type >= 2). */
void gnss_hint_on_fix(uint32_t mono_ms, uint8_t sats);

/*
 * A `GNSS_FIX` write from the phone carrying the `HINT` flag (ble_companion.cpp).
 *
 * The only entry point here that is NOT on the controller task: this runs on NimBLE's.
 * It validates cheaply and leaves the frame in a one-slot mailbox; the next controller
 * pass decides and sends. Returns true when the frame was taken, which is what the
 * phone's accepted counter is told. Never stores anything: a hint is not a measurement.
 *
 * Which position hint a start actually ends up holding is decided by stated accuracy,
 * not by arrival order (lib/cairn_phonehint): the receiver keeps whichever arrived
 * last, and ordering by arrival would make the answer depend on when a phone happened
 * to connect.
 */
bool gnss_hint_note_phone_fix(int32_t lat_e7, int32_t lon_e7, int32_t alt_cm,
                              uint16_t h_acc_cm, uint8_t validity_flags,
                              uint8_t companion_flags);

#endif /* CAIRN_GNSS_HINT_H */
