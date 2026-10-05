# Hardware round trip: capture → seal → upload → decode, encryption on

The one check the host suites cannot make: that the **real dongle** writes
encrypted bundles the **real server** accepts and decodes. Everything else about
format v3 is verified on the host (33/33 vectors in Go, Rust and C; the Rust
emulator's fault matrix against a live v3 server); this is the step that proves it
on silicon.

State at the time of writing (2026-10-05): the dongle is provisioned (enrolled,
root escrowed, assigned, Wi-Fi and mTLS credentials in NVS). **It has no SD card
in it** (`SD mount failed … GO_IDLE_STATE failed` on every attempt), so nothing
below has been run. Prerequisite: a FAT32 card, seated.

## 0. Before you start

- The server stack is deployed and healthy (`systemctl is-active cairn-server cairn-tsdb`).
- `cairn-provision --port … --reset --monitor 12` shows
  `credentials are provisioned` and, with a card, `SD mounted`.
- The server has the device enrolled and assigned: `cairn-admin device list`,
  `cairn-admin assignments <device>` show an **open** assignment. A device with no
  open assignment writes bundles the server refuses (correct, but not what you want
  to find out mid-drive).

## 1. The bench exercise (no car needed)

Flash the **self-test** build. It writes a framed, encrypted bundle to the card,
seals it, and syncs it, reporting each step:

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
| `associated, rssi …` | Wi-Fi credentials from NVS work |
| `mTLS ready: … client credentials from NVS` | the client key is **not** coming from the card |
| `sync done: 1 offered, 1 receipted, 1 pruned` | the server accepted it, issued a receipt, the device verified it against the pinned key and pruned |

Then on the server:

```sh
cairn-admin counters <device-id>          # high-water counter advanced, no missing
cairn-ledger -summary /var/lib/cairn/ledger   # committed + receipt_issued, 0 refusals
```

The self-test bundle is synthetic, so it will decode to a handful of rows, but it
must decode: `curl -s 127.0.0.1:8480/metrics` should show `bundles` incremented
and `reproduced` equal to it.

## 2. The production exercise (a real drive)

```sh
pio run -e cairn -t upload --upload-port …
```

Drive. When the car is parked and home Wi-Fi is in range the device seals, offers,
uploads and prunes by itself. Afterwards:

```sh
cairn-admin counters <device-id>                  # contiguous, none missing
ssh <host> curl -s 127.0.0.1:8480/metrics         # reproduced == bundles
cairn-provision --port … --reset --monitor 30     # (if still on the bench) no AUTH_FAILED, no UNASSIGNED
```

Open the UI: the vehicle selector lists the 428i with its trip, and the trip
detail renders. If the app is enrolled, its next pull carries a `trip_summary`
for that vehicle within ~30 s of the decode.

## 3. What a failure means

| Symptom | Almost certainly |
|---|---|
| Server: `assignment_refused` | The device's assignment id on the card is not one the server issued: re-run `cairn-provision` with the right `--assignment` |
| Server: `key_missing` | The device's root was never escrowed, or NVS was erased and a **new** root generated. Re-enrol with a new key version (never overwrite) |
| Server: `quarantined` | A bundle reused a counter with different content: a cloned unit, a restored card image presenting different bytes, or NVS rolled back. Investigate before trusting the device |
| `refused: no PROV-READY` | The 60 s window after boot has closed; use `--reset` |
| tsdb: bundles decode `AUTH_FAILED` | The server holds a different root than the device is using |
| Device: `a private key is still on the SD card` | A leftover pre-v3 `client.key`: it is ignored; delete it |

## 4. Rolling back

The v2 data was **archived**, not deleted, on the server
(`/var/lib/cairn-archive-v2-*`), and the original 4 MB flash image of the dongle
was saved before the first flash. v2 firmware cannot talk to a v3 server, so
rolling back means both.
