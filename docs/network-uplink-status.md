# Network uplink: what is proven, what is not, and is it ready to drive

State as of 2026-10-08, all of it measured on the car's dongle (ESP32-D0WDQ6 rev 1,
device `8777228e31648b23f6f783d28eee3e26`) against the live server. Nothing below is
inferred from a datasheet unless it says so.

## Short answer: the drive happened, and the network half worked

Updated 2026-10-08. The previous verdict here was "yes, drive it", with the automatic
LTE trigger as the one thing a bench could not test. It was driven on 2026-10-07 and
that trigger worked, unattended, end to end:

1. the trip was captured and sealed on parking (11,202 frames)
2. the trip-end edge fired, the settle delay elapsed, and LTE sent
3. **1,295,714 bytes in 386 s — 3.35 KB/s, 0.23% protocol overhead**, pruned on a
   verified receipt
4. the trip decoded on the server (`reproduced: true`) and appeared in the sync log

So every transport is now proven on this hardware, including the schedule that drives
them. Two things the drive exposed, both since fixed and neither in the network path:

- **The 300 s LTE slot limit was below the cost of one real bundle** (386 s measured),
  so the slot aborted mid-transfer every time and progress came only through resume,
  paying the attach again for nothing. Now 900 s.
- **A reboot stranded pending bundles.** `trip_ended_known` is zeroed by init, so with
  no trip edge to point at LTE would not fire until some later drive ended. Boot now
  counts as a trip end (#36), still behind the full settle delay.

### What the drive did *not* produce: usable OBD data

This matters more than anything above, because it is the point of the device. The trip
captured **239 OBD samples across 1002 s**, with 766 s of it holding no OBD data at all
in 25 outages quantised to ~23 s and small multiples. GNSS went dark in the same
windows; the IMU, read on its own path, kept sampling at ~8.5 Hz throughout — so the
device was healthy and the sensing task was blocking on itself.

`COBD::readPID` waits out `OBD_TIMEOUT_SHORT` (1000 ms) and does not retry, and a cycle
walks about nine reads. Once the DME goes quiet each one waits its full second in turn
and the loop cannot reach the GNSS poll. Bounded in `0c55fb0` to 350 ms per read plus
fail-fast after two consecutive misses — a dead cycle now costs ~700 ms rather than ~9 s.

**That fix is not verified.** Bench power reports `obd=absent`, so the read path never
runs on the desk and only a drive can confirm it. See #37, which also records two things
still open: why the six-PID batch stopped answering (225 of 239 samples fell back to the
1200 ms sequential path, against 56 ms measured for the batch on 2026-10-03), and that
wide-open throttle cannot be identified from `throttle_pct` at all — it is PID 0x11,
the throttle *plate* angle, which peaked at 77% across the whole drive and read 32-34%
during the one confirmed 8.6 psi boost event.

## Proven on this hardware

| path | evidence |
|---|---|
| Capture and seal | Bundles sealed repeatedly, including a 1.17 MB one. `resuming interrupted capture` works across reboots. |
| BLE -> phone -> server | **9 bundles** receipted on the server, all `"path":"ble-relay"`. |
| Wi-Fi -> server, mTLS | **4 bundles** delivered and pruned, `env:cairn-wifiup`. |
| LTE -> server, mTLS over Funnel | **1 bundle on the bench**, 158,906 bytes, delivered and pruned. T-Mobile US (PLMN 311480), PDP address assigned, TLSv1.2 ECDHE-ECDSA-AES128-GCM in 5.2 s, 3 requests over 1 connection. Then **a real drive, unattended**: 1,295,714 bytes in 386 s, 3.35 KB/s, 0.23% overhead. |
| The automatic LTE trigger | **Proven 2026-10-07.** Trip-end edge, settle delay, send, receipted prune — no console, no bench env. This was the last thing a bench could not test. |
| TLS actually verifies | Correct CA -> VERIFIED. Wrong CA (`env:cairn-tlsneg`) -> REJECTED, flags `0x8` NOT_TRUSTED, mbedTLS `-0x2700`. |
| Server rejects bad clients | No client cert -> TLS failure. Cert with the right CommonName from an untrusted CA -> TLS failure. |
| Uplink schedule in production | Arms, scans, opens a slot, honours the abort, holds standby. Boots clean, no panics. |

## Not proven

- **The OBD fail-fast bound** (#37). Bench power reports `obd=absent`, so the read path
  never runs on the desk. Only a drive can show whether the outages are gone.
- **Resume across two sessions over cellular.** The code path is host-tested (a killed
  link resends only what is missing) but has not been exercised on a real interrupted
  cellular transfer. Less likely to be hit now the slot limit exceeds a whole bundle.
- **Parked current draw** with the modem in the loop (#10 — needs a meter).
- **Whether this DME answers pedal position** (0x49/0x4A). Added to the `env:cairn-pidtest`
  probe list, not to the profile's `pids`, precisely because it is unknown.

## Known limitations you will meet

**Wi-Fi is switched off in `env:cairn`** (#31). It works, but not while the BLE controller
is initialised: association fails in the WPA2 four-way handshake, reason 204
`HANDSHAKE_TIMEOUT`, after 25 s, every time. Four mitigations were tried; the only one that
works (`NimBLEDevice::deinit`) panics on the next scan. LTE carries the data instead. Do
not spend time rediscovering this — read #31 first.

**One bundle on the card is stranded** (#32). 1.17 MB, sealed at 96 chunks before
`CAIRN_MAX_CHUNKS` had to drop to 64. It reports as `1 unreadable` each session, costs
nothing, and will never upload. `DROP legacy-bundles` will not remove it (that command only
drops pre-v3 manifests, deliberately).

**The phone is not told before a Wi-Fi slot** (#34). The announce and check-in fields are
unreleased BLE contract surface, so the runner reports them done and logs that it did not
actually send anything. Harmless while Wi-Fi is off.

**On USB bench power the device believes it is driving.** It reads the floating OBD rail,
latches bench mode, and the provisioning console refuses `BEGIN` with "a trip is active"
once capture starts — about 12 s after boot. Send console commands inside that window.

**BLE and Wi-Fi both need the dongle away from USB 3 hubs.** BLE failed at RSSI -94 dBm;
Wi-Fi dropped mid-upload with reason 34 `MISSING_ACKS`. The cellular modem has its own
antenna and is unaffected, which is why LTE succeeded on a bundle Wi-Fi lost.

## Things that cost hours and should not again

- **`AT+CFUN=0` powers the SIM interface down.** The module keeps its own supply after the
  UART closes, so the next bring-up reads the SIM as "unknown" and its APN write is
  refused. Teardown uses `CFUN=4`; bring-up asserts `CFUN=1` and waits for the SIM.
- **`AT+CREG?` reports the circuit-switched domain.** This SIM has no voice subscription
  and answers `0,3` (registration *denied*) while packet registration is healthy. Gate on
  `CEREG`/`CGREG` in {1,5}. Registration state 5 means roaming and is the *only* healthy
  state reachable here — the home network is in Hong Kong.
- **`AT+COPS?`'s operator name is stored on the SIM.** This card reports
  `"T-Mobile EIOTCLUB"` whatever it is attached to. Only the numeric form (`AT+COPS=3,2`)
  identifies the serving network; policy must never read the name.
- **`AT+CMEE=2` before anything that can fail**, or a refusal arrives as a bare `ERROR`
  naming neither cause nor subsystem.
- **`WiFi.setSleep(false)` aborts under coexistence**, in `pm_set_sleep_type`. Leave power
  save at its default.
- **The static DRAM segment binds, not the RAM percentage PlatformIO prints** (#33). It
  reported 36% while the link was failing.
- **Sealed bundles do not compress.** Measured: `gzip -9` reaches 90% of raw, `xz -9` 87%,
  because the payloads are AEAD ciphertext. There is nothing to win.
- **A blocking read in the sensing task costs every other sensor too.** The OBD outages
  took GNSS down with them, and nothing in the record said so — the `obd` and `position`
  tables just stopped. What identified it was the IMU continuing at full rate in the same
  windows. When a channel goes quiet, check whether its neighbours went quiet with it
  before suspecting the channel.
- **`OBD_TIMEOUT_SHORT` cannot be lowered globally.** The same 1000 ms bounds `ATZ` and
  the rest of `COBD::init()`, where a reset legitimately takes most of a second. Override
  it per read (`CairnOBD::readPIDTimed`), which is also the only way to reach the
  protected `normalizeData()` so the library's conversion is reused unchanged.

## Architecture, briefly

```
lib/cairn_uplink      the schedule: evidence in, one action out. 119 host rows.
src/uplink_runner     the other half: gathers evidence, performs the action, reports.
src/net_upload        walks the card, drives cairn_intake, applies the receipt gate.
                      Owns the shared buffers (two sets overflowed DRAM by 65 KB).
lib/cairn_intake      offer -> chunks -> commit. 63 host rows.
lib/cairn_bundle      reads a sealed bundle; borrows its decode scratch. 24 host rows.
src/net_http          HTTP/1.1 over any Client, connection held across requests.
src/tls_client        mbedTLS over any Client. Verification REQUIRED.
src/wifi_link         association + WiFiClientSecure.          [gated off, #31]
src/lte_link          modem bring-up + a Client over AT.
lib/cairn_modem       SIMCom reply parsing. 57 host rows, fixtures from this unit.
```

TLS runs on the ESP32 rather than in the modem because Tailscale Funnel requires SNI
(verified: no SNI, the edge drops the handshake) and the SIM7600's `enableSNI` is
undocumented on this firmware revision. That also removes the module's 10 KB certificate
cap, its 2 KB send cap and its TLS 1.2 ceiling, and leaves one TLS and one HTTP
implementation serving both transports.

## Bench environments

| env | what it does |
|---|---|
| `cairn` | production: capture, BLE, and the uplink schedule with LTE live |
| `cairn-netprobe` | Wi-Fi scan with exact SSID lengths, then raw AT to whatever is in the BEE socket. Answers "is a modem fitted" and "does the SIM register". |
| `cairn-wifiup` | Wi-Fi upload bench. Halts before BLE starts, which is why Wi-Fi works here. |
| `cairn-lteup` | LTE upload bench, including a TLS probe before any bytes move. |
| `cairn-tlsneg` | the same, pinning a wrong CA. Expected result: REJECTED. |

The upload benches take the radio and the card for minutes with no regard for the
schedule, so they must not run in a car.
