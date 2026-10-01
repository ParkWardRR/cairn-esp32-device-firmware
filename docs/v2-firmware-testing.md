# Cairn v2 firmware — flashing and bench testing

For the hardware profile, the serial adapter wiring and the pin-level details,
see [flashing-and-testing.md](flashing-and-testing.md). This document covers
only what is different about the v2 firmware.

## What is new, in one paragraph

v2 writes a framed, chained, CRC-checked bundle format instead of loose `.bin`
files, signs a manifest over a Merkle root of the bundle's members, and deletes
nothing until it has verified an Ed25519 receipt from the server against a
pinned key. It also writes a verbose log to the card, which is the main thing
you will read on the bench.

**This firmware has never run on hardware.** It compiles for the target, and its
format implementation passes all 20 committed conformance vectors on the host —
byte-for-byte against the Go reference, including signatures. That rules out a
large class of bugs but not driver or timing problems. Treat the first boot as
the first real test.

## Before flashing

### 1. Fill in secrets

```bash
cd firmware/cairn-v2
cp include/secrets.h.example include/secrets.h
$EDITOR include/secrets.h
```

`include/secrets.h` is gitignored. The repository is public, so keep real
hostnames and keys out of everything else.

The pinned receipt key matters for pruning. Get it from the server:

```bash
cairn-server -print-receipt-key
```

If you leave the key as the all-zero placeholder, the device will still capture
and still upload — it just will never delete anything. That is the intended
failure direction: a full card loses nothing, a wrongly authorized prune loses a
trip permanently.

### 2. Verify the format on the host first

This takes two seconds and is the cheapest check available:

```bash
make -C firmware/cairn-v2/test/host
```

Expect `format v2 conformance (C): 20/20 passed`. If this fails, do not flash —
the device would write bundles the server cannot read.

`make -C firmware/cairn-v2/test/host asan` runs the same vectors under
AddressSanitizer and UBSan.

## Flashing

Flash from the Raspberry Pi, not the Mac — the USB-serial path to this board is
set up there.

```bash
# On the Mac: build both images
cd firmware/cairn-v2
pio run -e cairn-selftest
pio run -e cairn

# Copy to the flash station
rsync -av .pio/build/cairn-selftest/firmware.bin \
          .pio/build/cairn/firmware.bin \
          .pio/build/cairn/partitions.bin \
          alfa@flash-station.example.lan:~/cairn-flash/v2/
```

Then on the Pi, flash the self-test image first:

```bash
ssh alfa@flash-station.example.lan
cd ~/cairn-flash/v2
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect \
  0x10000 firmware-selftest.bin
```

PlatformIO can do the whole upload itself if the board is attached to the Pi and
the repository is checked out there:

```bash
pio run -e cairn-selftest -t upload --upload-port /dev/ttyUSB0
pio device monitor -b 115200 --filter esp32_exception_decoder
```

## Reading the self-test

The self-test image checks the things that silently make a drive worthless, then
halts instead of capturing. Expected output:

```
 BOOT INFO  Cairn cairn-v2.0.0, policy v1, built ...
 BOOT INFO  running from app0 at 0x010000 (1728 KiB)
 BOOT INFO  SD mounted: 30436 MiB total, 12 MiB used
 LOG  INFO  SD sink attached: /cairn/logs/boot-000001-000.log (boot 1, ...)
 BOOT INFO  === self-test ===
 BOOT INFO  [PASS] CRC-32("123456789") = cbf43926, want cbf43926
 BOOT INFO  [PASS] SHA-256("")
 BOOT INFO  [PASS] Ed25519 sign 24 ms, verify 48 ms, tamper rejected 1
 BOOT INFO  [PASS] wrote 8 framed records to the card
 BOOT INFO  [PASS] sealed in 118 ms
 BOOT INFO  [PASS] associated with "...", rssi -58 dBm
 BOOT INFO  [INFO] sync: OK (1 offered, 1 receipted, 1 pruned)
 BOOT INFO  === self-test PASSED ===
```

What each line is actually telling you:

| Line | Why it is checked |
|------|-------------------|
| CRC-32 | On ESP32 this routes through ROM, whose convention is *inverted* (`~esp_rom_crc32_le(~0u, …)`). A wrong wrapper corrupts every frame on the card, and only the server would ever find out. |
| Ed25519 | Hand-vendored, because mbedTLS has no Ed25519 signing. Timing is reported because signing is the slowest thing a seal does. "tamper rejected 1" confirms verification actually rejects. |
| framed records | The real encode → card → recovery-scan path, not a buffer. |
| sealed | Exercises the Merkle root, the deterministic CBOR manifest encoder and the signature over actual on-card bytes. |
| sync | Offline is a valid steady state for this device, so a missing network is reported as `[INFO]`, not a failure. |

`sync: ... 1 pruned` is the end-to-end proof: the server signed a receipt over
the content root, the device verified it against the pinned key, and only then
reclaimed the space.

If `0 pruned` with `1 receipted`, the pinned key is wrong or still the
placeholder. The log says which.

## Then flash the capture image

```bash
pio run -e cairn -t upload --upload-port /dev/ttyUSB0
pio device monitor -b 115200
```

## Reading the SD logs

Logs live in `/cairn/logs/boot-<bootcount>-<index>.log`, one file per boot, so
reboot boundaries are unambiguous. Pull the card and:

```bash
ls -la /Volumes/<card>/cairn/logs/
tail -f /Volumes/<card>/cairn/logs/boot-000003-000.log
```

Line format is `<monotonic_ms> <boot_id_prefix> [TAG] LEVEL message`:

```
     14823 a3f21c04 [LIFE] INFO  capture IDLE -> PRETRIP (start 2.40, stop 0.50)
     17851 a3f21c04 [LIFE] INFO  capture PRETRIP -> ACTIVE (start 3.10, stop 0.00)
     18002 a3f21c04 [STORE] TRACE frame GNSS_SAMPLE seq 41, 60 bytes, crc 9e2a1f03
```

Timestamps are **monotonic milliseconds, not UTC**. UTC can be absent or can
jump; a log whose timestamps run backwards is worse than one with no timestamps.
The 8-hex-digit boot id prefix is what ties log lines to the bundle they were
captured alongside, because ordering truth in the format is `(boot_id, seq)`.

Useful greps:

```bash
# Every state transition and why
grep -E '\[LIFE\]' boot-*.log

# Recovery after an interrupted write
grep -E 'torn|discard|resum|recover' boot-*.log

# Everything about receipts and pruning
grep -E '\[SYNC\]' boot-*.log
```

Logging yields to data. Below 64 MiB free the SD sink shuts itself off and
logging continues over UART only, and the whole log tree is capped at 16 MiB
oldest-first. A testing aid must not be able to cost a trip.

## The card layout

```
/cairn/capture/<ULID>/     the one open, unsealed bundle
/cairn/bundles/<ULID>/     sealed, awaiting a receipt
/cairn/receipts/<ULID>.cbor verified receipts
/cairn/state/              prune journal
/cairn/logs/               these logs
```

Inside a bundle directory:

```
seg-00000000.seg   capture frames; one chain across all seg-* files
journal.seg        state transitions and health, on its own chain
manifest.cbor      deterministic CBOR, exactly the bytes signed
manifest.sig       64-byte Ed25519 signature
```

## Things worth deliberately breaking

The point of this firmware is that failures are auditable, so the interesting
bench tests are the destructive ones.

**Pull power mid-drive.** Reboot and look for:

```
STORE WARN  resuming interrupted capture 01J...
STORE INFO  seg-00000000.seg: TORN_TAIL, 412 frames, seq 0..411, 37 byte tail discarded
STORE INFO  resumed: segment 0 at 24792 bytes, next seq 412, 37 bytes discarded, recovery_state 1
```

The exact discarded byte count is what matters, and it is carried into the
manifest as `discarded_tail_bytes` with `recovery_state` raised. The bundle
reports being short rather than looking complete.

**Pull the card mid-write.** Expect `[STORE] ERROR short write` and the chain
*not* advancing — the partial bytes become a torn tail the next boot truncates.

**Point the device at a server with the wrong receipt key.** Expect:

```
SYNC ERROR  receipt for 01J... rejected (signature verification failed); the bundle stays on the card
```

Nothing should be deleted. This is the single most important negative test here.

**Pull power during a seal.** The next boot should print `capture ... already
holds a manifest; finishing the interrupted seal`. The manifest is written
before the directory is moved, so an interrupted seal is always completable
without re-signing.

## Known gaps

- Never run on hardware. First boot is the first real test.
- OTA slots exist and the image marks itself valid after storage checks out, but
  nothing fetches an update yet.
- Flash encryption is deliberately off. Enabling it in release mode is
  irreversible, which is a poor property for a device already in a vehicle. The
  signing seed in NVS is therefore readable from an extracted chip; it
  authorizes uploads, not deletions, and the server can revoke it.
- No deep sleep yet, so parked current draw is higher than it needs to be.
- Transport is plain HTTP. The receipt signature, not the transport, is what
  authorizes deletion, so this affects who can read an upload rather than
  whether a prune is legitimate.
- `IMU_RAW_WINDOW`, `TRIP_EVENT` and `POLICY_SNAPSHOT` are defined in the format
  and exercised by the vectors, but the firmware does not emit them yet.
