/*
 * Running lib/cairn_wifiscan's schedule against the real radio.
 *
 * cairn_wifiscan decides when to scan and which access points to keep; this file owns
 * the radio for the two or three seconds a scan takes. Three rules, and how:
 *
 *   - **Capture is never blocked.** The scan is asynchronous (WiFi.scanNetworks with
 *     async = true); each lifecycle tick only polls WiFi.scanComplete(). The one
 *     blocking step is bringing the driver up (WiFi.mode(WIFI_STA)), which is the same
 *     call the uplink makes, and is bounded.
 *   - **BLE stays up.** Unlike an uplink slot, a scan does not deinit NimBLE: the phone
 *     link is expected to ride through it on ESP-IDF's software coexistence, with the
 *     arbiter on its balanced split (see the .cpp for why, and for what is unverified).
 *   - **One owner for the Wi-Fi radio.** No scan starts while the uplink owns it (a slot,
 *     its home scan, or an LTE send blocking the tick), and the uplink cancels any scan
 *     in flight before it touches Wi-Fi (wifiscan_runner_cancel). Standby waits for a
 *     scan in progress, which its own timeout bounds.
 *
 * The result is a WIFI_SCAN record, handed back to the lifecycle to write through the
 * same path as every other capture record (pre-roll before a trip is confirmed).
 */

#ifndef CAIRN_WIFISCAN_RUNNER_H
#define CAIRN_WIFISCAN_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Lifecycle;

#if CAIRN_WIFI_SCAN

void wifiscan_runner_begin(void);

/*
 * One pass. Starts a scan when the schedule asks, or collects one that completed.
 * Returns true when a WIFI_SCAN payload is ready in `payload` (`*len` bytes, at most
 * CAIRN_WIFI_SCAN_MAX_PAYLOAD), completed at `*done_ms`.
 */
bool wifiscan_runner_tick(const struct Lifecycle *lc, uint8_t *payload, size_t cap,
                          size_t *len, uint32_t *done_ms);

/* Stop a scan in flight and hand the radio back off, recording nothing. Safe to call
 * when no scan is running. `why` is for the log. */
void wifiscan_runner_cancel(const char *why);

/* True from the start of a scan until it completes, fails or is cancelled. */
bool wifiscan_runner_busy(void);

#else

static inline void wifiscan_runner_begin(void) {}
static inline bool wifiscan_runner_tick(const struct Lifecycle *lc, uint8_t *payload,
                                        size_t cap, size_t *len, uint32_t *done_ms)
{
    (void)lc; (void)payload; (void)cap; (void)len; (void)done_ms;
    return false;
}
static inline void wifiscan_runner_cancel(const char *why) { (void)why; }
static inline bool wifiscan_runner_busy(void) { return false; }

#endif /* CAIRN_WIFI_SCAN */

#endif /* CAIRN_WIFISCAN_RUNNER_H */
