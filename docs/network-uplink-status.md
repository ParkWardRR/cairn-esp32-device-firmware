# Network uplink: what is proven, what is not, and is it ready to drive

State as of 2026-10-07, all of it measured on the car's dongle (ESP32-D0WDQ6 rev 1,
device `8777228e31648b23f6f783d28eee3e26`) against the live server. Nothing below is
inferred from a datasheet unless it says so.

## Short answer: yes, drive it

The capture, seal, BLE and LTE paths have each been exercised on this hardware. The one
link that has never run is the *automatic* LTE trigger, and a bench cannot test it — it
needs a trip-end edge, which only a drive produces. That makes a drive the right next
step rather than a risk.

What a drive should produce, in order:

1. a trip captured and sealed when you park (proven many times)
2. about ten minutes later, parked and away from home, an LTE send that uploads the
   bundle and prunes it on a verified receipt (**never yet triggered automatically**)
3. the trip visible on the dashboard

If step 2 does not happen, the first thing to check is the serial log for
`[UPLINK] LTE send`. If that line never appears the schedule's preconditions were not
met; if it appears and fails, the reason is logged.

## Proven on this hardware

| path | evidence |
|---|---|
| Capture and seal | Bundles sealed repeatedly, including a 1.17 MB one. `resuming interrupted capture` works across reboots. |
| BLE -> phone -> server | **9 bundles** receipted on the server, all `"path":"ble-relay"`. |
| Wi-Fi -> server, mTLS | **4 bundles** delivered and pruned, `env:cairn-wifiup`. |
| LTE -> server, mTLS over Funnel | **1 bundle**, 158,906 bytes, delivered and pruned. T-Mobile US (PLMN 311480), PDP address assigned, TLSv1.2 ECDHE-ECDSA-AES128-GCM in 5.2 s, 3 requests over 1 connection. |
| TLS actually verifies | Correct CA -> VERIFIED. Wrong CA (`env:cairn-tlsneg`) -> REJECTED, flags `0x8` NOT_TRUSTED, mbedTLS `-0x2700`. |
| Server rejects bad clients | No client cert -> TLS failure. Cert with the right CommonName from an untrusted CA -> TLS failure. |
| Uplink schedule in production | Arms, scans, opens a slot, honours the abort, holds standby. Boots clean, no panics. |

## Not proven

- **The automatic LTE trigger.** Needs a trip-end edge. `cairn_uplink` deliberately does
  not start LTE after a power cycle because it has not seen a trip end — the conservative
  direction, and the reason a bench cannot exercise it.
- **Per-chunk LTE cost at the 8 KiB production chunk size** (#35). The one datapoint is a
  single-chunk bundle: ~3 KB/s. Do not extrapolate from the UART's 11.5 kB/s.
- **Resume across two sessions over cellular.** The code path is host-tested (a killed
  link resends only what is missing) but has not been exercised on a real interrupted
  cellular transfer.
- **Parked current draw** with the modem in the loop (#10 — needs a meter).

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
