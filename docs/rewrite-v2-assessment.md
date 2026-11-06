# Firmware rewrite v2 — assessment and architecture

Date: 2026-11-05. Baseline: `main` @ `7e93221` (contracts-v0.10.0 pinned).
Owner directive: trash the application code and start over. **BLE and LTE only — no
Wi-Fi code of any kind** (one radio; Wi-Fi slots tore BLE down, #31/#71). BLE carries
GPS quick start and trip offload. SD never deletes until full; then uploaded
(receipted) trips go first, oldest-first. Power efficient. Supports the N20 428i and
the B58 M240i and multiple phones/drivers. No approval gates; owner rolls back what
they don't like.

## 1. Verdict

Rewrite the **application layer**; keep the **proven leaf libraries and toolchain**.
The recorded stack decision (ESP-IDF via arduino-esp32 on espressif32@7.1.3, classic
ESP32 WROVER, vendored FreematicsPlus drivers, A/B partitions, no eFuse burns) stands —
this repo already paid once for a custom HAL that failed. What gets trashed is the
app layer that grew around Wi-Fi: the lifecycle monolith, the uplink slot scheduler,
the Wi-Fi link/scan/locate stack, and the BLE glue that treats the radio as something
Wi-Fi borrows.

## 2. What is broken (evidence at `7e93221`)

1. **No BLE connection parameter management anywhere.** No call to
   `updateConnParams`/`setConnectionParams` in `src/ble_companion.cpp` or
   `src/ble_offload.cpp`. Bench 2026-11-05: links die at ~21 s, HCI reason 520
   (supervision timeout) at −77..−81 dBm. The iOS app deliberately sets none (plan D2);
   the firmware owns this and doesn't do it.
2. **Single bond** (`CONFIG_BT_NIMBLE_MAX_BONDS=1`, noted at `ble_companion.cpp:399`).
   A second phone evicts the first; the first then hits `CB 14` → `needsForget`.
   Directly incompatible with multiple drivers.
3. **The lifecycle monolith.** `src/lifecycle.cpp` is 1674 lines owning trip state,
   sealing, uplink slot arbitration, radio ownership, standby, and version stamping.
   Every past regression class (1970 dates, cross-boot captures, lost drives, Wi-Fi
   slots killing BLE) ran through it.
4. **Wi-Fi is compiled out but still everywhere**: 36 files reference it (`wifi_link.*`,
   `wifiscan_runner.*`, `netprobe.*`, `lib/cairn_wifiscan`, `lib/cairn_wifiloc`, Wi-Fi
   halves of `net_upload`/`uplink_runner`/`cairn_uplink`, `CAIRN_WIFI_*` config and
   secrets). Dead code that can be re-enabled by a flag is a standing threat to the
   radio.
5. **OBD polling history**: blocking PID reads once lost 766 s of a 1002 s drive and
   starved GNSS (`sensors.cpp` + `sensor_task.cpp`, 2 kLOC). The multi-PID batch and
   cold-rotation code now works but lives tangled in the same translation units as
   engine gating, probes, and capture glue.

## 3. What is kept (contract-proven, host-tested)

- `lib/cairn_format` (+ `emulator/cairn-format` Rust twin) — format/v3 byte-exact;
  62 vectors. **Any behavioral change must land in both or CI goes red.**
- `lib/cairn_store` + `cairn_prune` — receipt-gated prune (the 9 proven BLE receipts).
- `lib/cairn_engine` + `engines/*.yaml` + `tools/enginegen` — N20 verified profile,
  B58 stub + MTPROBE discovery path (`docs/b58-data-gathering-plan.md`).
- `lib/cairn_gnss`, `cairn_trip`, `cairn_garage`, `cairn_agnss`, `cairn_phonehint`,
  `src/gnss_hint.cpp` — the new host-tested R2 set (phone HINT → `UBX-MGA-INI-POS_LLH`,
  `UTC_SYNC` → `MGA-INI-TIME_UTC`).
- `lib/cairn_offload` — offload protocol core (vectors in ble/v1).
- Enrolment console (`prov_console`), `SET garage`, VIN-based slot selection (proven on
  both cars).
- Vendored FreematicsPlus drivers, partitions-ab.csv, build identity scripts.
- LTE: `src/lte_link.*`, `tls_client` (TLS on the ESP32, not the modem), the LTE half
  of the uplink path. Second transport per the 2026-11-05 roadmap; still blocked on the
  CA key, kept compiled.

## 4. What is deleted (no flag, no return)

`src/wifi_link.*`, `src/wifiscan_runner.*`, `src/netprobe.*`, `lib/cairn_wifiscan`,
`lib/cairn_wifiloc`, the Wi-Fi paths of `net_http`/`net_upload`/`uplink_runner`/
`cairn_uplink`, `CAIRN_WIFI_*`/`CAIRN_WIFISCAN_*`/`CAIRN_WIFI_ANCHORS_*` config,
Wi-Fi credentials in `secrets.h`, Wi-Fi fields in devinfo/usage/prov stores (readers
skip unknown types; WIFI_SCAN record 0x0C stays defined in contracts and the emulator
but this firmware never emits it). DEVICE_INFO reports transports {BLE, LTE} only and
never sets capability bit 7 (Wi-Fi uplink).

## 5. New application architecture

Five tasks, one owner per resource, message-passing between them:

- **core/supervisor** — boot, recovery scan, state machine (IDLE → ARMED → CAPTURE →
  SEAL → PARKED), watchdog. Replaces lifecycle.cpp. States journaled as matched
  STATE_TRANSITION pairs (format §4.7.1).
- **capture** — owns OBD UART + GNSS stream; strictly non-blocking budgeted scheduler
  (hot batch / cold rotation); engine gate; writes frames via cairn_format. VIN picks
  the garage slot (N20 vs B58); stub engines refuse OBD, MTPROBE build unchanged.
- **ble** — owns the radio permanently. NimBLE server with the v0.10.0 GATT table;
  requests conn params on connect (30–50 ms interval, latency 4, 6 s supervision
  timeout; tightened during offload bursts); `CONFIG_BT_NIMBLE_MAX_BONDS=4`; one live
  connection, advertising always restarts on disconnect and always carries the 128-bit
  service UUID; bond dropped on PINKEY_MISSING/AUTH_FAIL; companion (GNSS_FIX/HINT/
  UTC_SYNC/GNSS_QUALITY/COMPANION_STATUS), offload (0x01–0x05 + done), DEVICE_INFO.
- **storage** — owns the SD card: bundle writer, sealing order §2.1, receipts,
  **retention** (§6 below).
- **power** — voltage sampling (BMW IGC-aware: hysteresis + trend, no single mV
  cutoff), standby = light sleep 80 MHz with BLE advertising windows, 15-min standby
  cap per connection honored, never standby during offload or with a phone connected
  and bundles pending.

## 6. Retention policy (owner-specified, replaces "pressure never deletes")

1. Nothing is ever deleted while free space ≥ `CAIRN_RETENTION_LOW_WATER` (default
   64 MiB or 5% of card, whichever is larger).
2. Below low water: prune **receipted** bundles oldest-first (receipt stays on card —
   receipts outlive bundles, per format §2) until back above high water.
3. If still below low water and capture needs space: delete **unreceipted** sealed
   bundles oldest-first, one at a time, each preceded by a journaled
   `RETENTION_DROP` policy event naming the bundle id. Never the open bundle.
4. The enrolment console gains `GET retention` / counters in DEVICE_INFO so a dropped
   bundle is visible, not silent.

Step 3 relaxes front-door invariant 2 by owner instruction (this session). The
front-door ROADMAP owns the invariant; flag the change there when this lands.

## 7. Multi-phone model

All drivers' phones bond (4 slots). One connection at a time; first to connect wins;
others see "busy" naturally (no advertising while connected). HINT accepted from any
bonded phone before first internal fix (best stated accuracy wins). Offload serves
whichever phone is connected; receipts are server-signed so double-offload is
harmless. Full arbitration (firmware #9) can layer on later without protocol change.

## 8. Verification gates (every step, on fucina)

`make host-test` (format vectors, ble vectors, engine tests, faults, gnss/trip/garage/
agnss/phonehint), `make engines-validate`, emulator `cargo test` + conformance,
firmware image builds in PlatformIO. Flash from the Mac (`/dev/cu.usbserial-110`,
USB only — no OTA). Bench: pair from 0 bonds, survive >21 s at weak RSSI, HINT/UTC
accepted, 9-receipt offload replay, retention fill test. Then fast-forward to `main`.
