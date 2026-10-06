# Hardware round trip: capture → seal → offload → relay → receipt → prune

The one check the host suites cannot make: that the **real dongle** writes
encrypted bundles, the **phone** carries them, and the **real server** accepts and
decodes them. Everything else about format v3 is verified on the host (33/33 vectors
in Go, Rust and C; the Rust emulator's fault matrix against a live v3 server); this is
the step that proves it on silicon.

The dongle has **no Wi-Fi**. The leg from the card to the server is BLE to the
enrolled phone, then the phone's authenticated upload ([ble-offload.md](ble-offload.md)).
So the round trip has two stages, and only the first can be run today.

| Stage | What it proves | State (2026-10-05) |
|---|---|---|
| **A. Bench: capture and seal** | Encrypted frames, the manifest signature and the counter commit work on this silicon, and the sealed bundle is visible to the hand-off path | Capture, seal and storage checks all `[PASS]` on the real dongle with the card seated (2026-10-05). The new `awaiting hand-off` line was added after that run and has so far only been built, not flashed |
| **B. Offload: BLE → relay → receipt → prune** | The server accepts and decodes what the dongle wrote, the receipt verifies on the dongle against the pinned key, and the bundle is pruned | **Every part exists and is tested together on the host with no radio** (the firmware's protocol module, a phone client and the server's relay, `go test ./internal/offloadclient`). **Not yet run over the air**: the offload firmware has not been flashed. Run it with `cairn-phone` below |

## 0. Before you start

- The server stack is deployed and healthy (`systemctl is-active cairn-server cairn-tsdb`).
- `cairn-provision --port … --reset --monitor 12` shows `an assignment is installed`
  and, with a card, `SD mounted`.
- The server has the device enrolled and assigned: `cairn-admin device list`,
  `cairn-admin assignments <device>` show an **open** assignment. A device with no
  open assignment writes bundles the server refuses (correct, but not what you want
  to find out mid-drive).

## A. The bench exercise (no car needed)

Flash the **self-test** build. It writes a framed, encrypted bundle to the card and
seals it, reporting each step:

```sh
cd firmware/cairn-v2
pio run -e cairn-selftest -t upload --upload-port /dev/cu.usbserial-<n>
cairn-provision --port /dev/cu.usbserial-<n> --reset --monitor 60
```

Expect, in order:

| Log | Meaning |
|---|---|
| `[PASS]` known-answer checks | CRC-32, SHA-256 (and the v3 primitives) agree with the specification on this silicon |
| `storage: key version 1, counter high-water N, assigned` | the NVS identity loaded; **`UNASSIGNED` here means provisioning did not take** |
| `[PASS]` framed appends | encrypted frames written to the card |
| `sealed in … ms` | manifest signed, counter committed |
| `[PASS] N sealed bundle(s) awaiting hand-off` | the sealed bundle is visible to the offload path |
| `=== self-test PASSED ===` | all of the above |

There is no network step in the self-test and no `associated` / `mTLS` line: the
firmware has neither.

## B. The offload

The host test (`go test ./internal/offloadclient`, after `make -C firmware/cairn-v2/test/host offload-sim`)
runs the whole path with no radio. To run it over the air with a Mac standing in for the
phone:

1. Flash the production build (`pio run -e cairn -t upload …`). BLE is in every build now.
2. Enrol the Mac once: `cairn-admin client invite --role user --vehicles '*'` on the server,
   then `cairn-phone enrol --server https://<host>:8444 --code <invitation>`.
3. Get a sealed bundle: run the self-test build (it seals a synthetic one), or drive; a trip
   is sealed a few minutes after you park. The dongle refuses offload (`TRIP_ACTIVE`) until then.
4. `cairn-phone offload`. macOS asks for the pairing passkey the first time. It lists the
   sealed bundles, fetches each manifest, offers it to the server, reads and uploads the missing
   chunks, commits, and returns the receipt.
4. On the server:
   ```sh
   cairn-admin counters <device-id>                  # contiguous, none missing
   cairn-ledger -summary /var/lib/cairn/ledger       # committed + receipt_issued, relayed by <client>, 0 refusals
   ssh <host> curl -s 127.0.0.1:8480/metrics         # reproduced == bundles
   ```
5. On the dongle (`cairn-provision --port … --reset --monitor 30`): the bundle is
   gone from `/cairn/bundles`, the log shows the receipt outcome `0`, no `AUTH_FAILED`.

Open the UI: the vehicle selector lists the 428i with its trip, and the trip detail
renders. If the app is enrolled, its next pull carries a `trip_summary` for that
vehicle within ~30 s of the decode.

## What a failure means

| Symptom | Almost certainly |
|---|---|
| Server: `assignment_refused` | The device's assignment id on the card is not one the server issued: re-run `cairn-provision` with the right `--assignment` |
| Server: `key_missing` | The device's root was never escrowed, or NVS was erased and a **new** root generated. Re-enrol with a new key version (never overwrite) |
| Server: `quarantined` | A bundle reused a counter with different content: a cloned unit, a restored card image presenting different bytes, or NVS rolled back. Investigate before trusting the device |
| Server: `403 scope` on relay | The relaying app client is not scoped to this bundle's vehicle |
| `refused: no PROV-READY` | The 60 s window after boot has closed; use `--reset` |
| tsdb: bundles decode `AUTH_FAILED` | The server holds a different root than the device is using |
| App: receipt outcome 2 or 3 | The server's receipt key and the key pinned in firmware disagree (2), or the receipt names a different bundle (3). **Do not retry**; fix the key |
| Offload says `TRIP_ACTIVE` | A drive is in progress; offload only runs when parked |
| Device: `a private key is still on the SD card` | A leftover pre-v3 `client.key`: it is ignored; delete it |

## Rolling back

The v2 data was **archived**, not deleted, on the server
(`/var/lib/cairn-archive-v2-*`), and the original 4 MB flash image of the dongle
was saved before the first flash. v2 firmware cannot talk to a v3 server, so
rolling back means both.
