# Uplink manager

`lib/cairn_uplink` (issue #17) is the policy that decides which path moves a bundle and
when the radio may leave BLE. It is portable C with the clock and every outside fact
passed in, so the whole schedule runs on the host (`make -C test/host uplink`, 119 rows,
ASan/UBSan clean, 15 mutations of the safety rules all caught).

## Status

**Wired into the firmware and running in `env:cairn` since 2026-10-07.**
`src/uplink_runner.cpp` is the other half of this module: it gathers evidence from the
lifecycle, performs the one action `tick()` returns, and reports the outcome. The split is
what kept 119 rows of scheduling behaviour testable on the host, so the I/O stays on that
side of it.

| path | state |
|---|---|
| LTE | **Live.** Proven on the car's dongle: attach, TLS through the public Funnel ingress, 158,906 bytes uploaded, receipt verified, bundle pruned. |
| Wi-Fi | **Implemented and proven, but marked unavailable to the schedule.** It cannot associate while the BLE controller is initialised ([#31](https://github.com/ParkWardRR/cairn-esp32-device-firmware/issues/31)). |
| BLE | Phone-driven by `lib/cairn_offload`, as before. |

Two actions the runner cannot yet really perform, and reports as done so the schedule does
not stall: `ANNOUNCE_SLOT` and `CHECKIN`, both unreleased BLE contract fields
([#34](https://github.com/ParkWardRR/cairn-esp32-device-firmware/issues/34)).

The home scan works — it finds the configured network in about 1.7 s — but reports a miss
while Wi-Fi is unavailable, because a scan hit marks the device "at home" and being at home
is what suppresses the LTE trigger. A truthful scan would disable the only working network
path in order to enable one that cannot associate.

The credential gate was not met; it was **traded, knowingly**. Flash and NVS encryption
(#7, gate #18) need eFuse burns the car's revision v1.0 dongle may not take, so credentials
live in plaintext flash in the gitignored `include/secrets.h`. What a flash dump costs is
stated where they are defined: it permits uploading **as** this device and reading what it
uploads, but not deleting anything, because a prune still requires a receipt signed by the
server and verified on-device. The narrower protection (#18's third option, a key the chip
cannot reconstruct without the server) remains the right destination; see
[esp32-hardening.md](esp32-hardening.md).

Measured status, with the evidence, is in
[network-uplink-status.md](network-uplink-status.md).

## The schedule (the owner's, decided in #17)

```
BLE (home state)
  -> [known home network, bundles waiting, parked, battery ok]
  -> announce to the phone -> Wi-Fi slot 1 -> back on BLE: check-in
  -> Wi-Fi slot 2, only if slot 1 left bundles uncommitted -> check-in
  -> backoff before the next parked session if anything is still waiting
```

Rules the code enforces, each with a host row:

- BLE and Wi-Fi are never up together (`ble_may_advertise` / `wifi_may_run`, and the path
  order never mixes them). A home scan is Wi-Fi use too.
- A connected phone is told before the radio leaves BLE. With no phone connected there is
  nobody to tell, and the report stays pending until one connects.
- A trip start, low battery or the slot limit aborts a slot; if the caller has not returned
  within `abort_return_ms` the manager orders the radio forced off.
- Slots per session, the gap between slots, and the backoff between sessions are bounded.
  The backoff doubles on a fruitless session, is capped, and clears on a delivered bundle.
- A delivery ends in a prune only when the injected prune callback accepts a receipt
  (`cairn_prune_if_receipted` in the product). A receipt the gate refuses deletes nothing and
  the next path is tried; with no gate the module never reports delivery.
- Per bundle, paths are tried Wi-Fi (in a slot) or BLE (phone connected), then LTE only if
  the user has allowed whole bundles over LTE. A second path resumes from the chunks the
  server already holds: in the test, killing each path at several points never sends a chunk
  twice and always ends in one commit, one receipt, one prune.

## Home detection (decided in #17)

1. **Phone-asserted**: the phone says "you may use Wi-Fi now"; the dongle stores no location.
2. **Dongle-scanned** when no phone is present (the owner accepted this): a bounded,
   rate-limited scan for a provisioned network. It runs only when parked with bundles
   waiting, never during a trip, never with a phone connected, and a hit is a valid trigger
   for `scan_hit_ttl_ms` only. The list of networks is a credential-class secret and is not
   stored until #7 lands; the decision logic here holds no network names.
3. A GNSS geofence on the chip is not an option.

## LTE trigger

The owner's proposed default, still partly open: after a trip, when away from home, with no
phone and bundles waiting, `lte_after_trip_ms` after the trip ended, digests only, and always
subject to the caps in the usage module. It is `cairn_uplink_config_t.lte_auto`, not a
constant, so "never automatically" is a one-line change. After a power cycle the manager has
not seen a trip end, so it does not start LTE on its own; that is the conservative direction.

## The full state machine (from the cellular design)

The cellular design document
([lte-cellular-design.md](https://github.com/ParkWardRR/cairn-driving-log-selfhosted/blob/main/docs/lte-cellular-design.md))
defines the complete state machine that the uplink manager implements once
Wi-Fi and LTE transports exist:

| State | Behaviour | Exit condition |
|---|---|---|
| Capture | Collect OBD/GNSS locally; keep Wi-Fi off | Bundle/checkpoint ready |
| BLE preferred | Offer sealed chunks to the paired iPhone | Phone takes durable custody, or progress stalls for 30-60 s |
| Known-Wi-Fi probe | Scan only when justified by location/history or backlog; join provisioned networks only | Authenticated ingestion reachable, or 10-20 s expire |
| Wi-Fi burst | Drain eligible backlog; BLE control/GPS remains available | Queue drained, no progress, or energy/time limit reached |
| LTE fallback | Attach and upload budget-approved chunks | Queue drained, capped allowance reached, or repeated failure |
| Backoff | Wi-Fi off; LTE asleep/off; continue BLE and local recording | Next retry, new network opportunity, or priority event |

**Switch based on successful upload progress, not just RSSI.** "Strong Wi-Fi
with no server access" and "registered LTE with unusable throughput" are both
failed delivery paths. A Wi-Fi network at home should outrank LTE immediately
after a stalled/unavailable BLE relay.

### Oscillation prevention

Use transport-independent chunk IDs and receiver deduplication. When changing
paths, continue with missing chunks rather than rebuilding or resending a whole
trip. Two acknowledgement levels:

| Acknowledgement | Meaning |
|---|---|
| Phone custody | The iPhone durably stored the encrypted chunk; dongle may stop offering it over BLE |
| Server commit | The server durably accepted and verified it; dongle may apply its deletion/retention policy |

Retain the dongle copy until server commit unless storage pressure explicitly
triggers a different policy.

## Not decided here

- ~~How BLE and Wi-Fi coexist at the radio level: the owner decided time-slicing, so there
  is nothing to measure for that.~~ **Measured, and the answer was unwelcome.**
  Time-slicing by stopping advertising is not enough: with the Bluetooth controller still
  initialised, Wi-Fi association fails in the WPA2 four-way handshake (reason 204
  `HANDSHAKE_TIMEOUT`) after 25 s, every attempt. Genuinely releasing the controller
  (`NimBLEDevice::deinit`) does let Wi-Fi associate and then panics on the next scan. So
  there *was* something to measure, and the Wi-Fi path is off until it is resolved
  ([#31](https://github.com/ParkWardRR/cairn-esp32-device-firmware/issues/31)). LTE is
  unaffected: its modem has a separate antenna.
- The slot length is now measured rather than a placeholder: cellular moves about 3 KB/s on
  this hardware, so a ~230 KB bundle needs on the order of 80 s and
  `CAIRN_UPLINK_SLOT_MAX_MS` is 240 s. Slot count and backoff are still placeholders
  (issue #19).
- The wire format of the slot announcement and the check-in: it is a new field in the BLE
  contract (upstream issue 21) and is not released. The manager returns the intent only.
- Multipath (different chunks of one bundle over two paths at once): exploratory, not built,
  as the issue says to wait for measurements.
