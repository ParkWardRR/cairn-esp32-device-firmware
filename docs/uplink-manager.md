# Uplink manager

`lib/cairn_uplink` (issue #17) is the policy that decides which path moves a bundle and
when the radio may leave BLE. It is portable C with the clock and every outside fact
passed in, so the whole schedule runs on the host (`make -C test/host uplink`, 119 rows,
ASan/UBSan clean, 15 mutations of the safety rules all caught).

## Status

**Built and host-tested. Not wired into the firmware**, because there is nothing to
schedule yet: Wi-Fi (#15) and LTE (#16) are blocked on flash and NVS encryption (#7, gate
#18), and the BLE path is phone-driven by `lib/cairn_offload`. That block does not expire
on its own — encryption is an eFuse burn the car's revision v1.0 unit may not take, so the
transports wait on a replacement unit or on #18 being decided again (see
[esp32-hardening.md](esp32-hardening.md)). When a Wi-Fi or LTE transport exists it
implements `cairn_transport_t` and the lifecycle calls `tick()`.

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

## Not decided here

- How BLE and Wi-Fi coexist at the radio level: the owner decided time-slicing, so there is
  nothing to measure for that. The slot length, slot count and backoff numbers are
  placeholders to be set from measurement (issue #19).
- The wire format of the slot announcement and the check-in: it is a new field in the BLE
  contract (upstream issue 21) and is not released. The manager returns the intent only.
- Multipath (different chunks of one bundle over two paths at once): exploratory, not built,
  as the issue says to wait for measurements.
