# ESP32 hardening — flash encryption, secure boot, and the SD key hierarchy

Plan for protecting the Freematics ONE+ Model B (classic ESP32-WROVER, 16 MB
flash, 8 MB PSRAM, SPI microSD). Context and threat table:
[trust-model-v3.md](https://github.com/ParkWardRR/cairn-driving-log-selfhosted/blob/main/docs/trust-model-v3.md). Format:
[bundle-format-v3.md](https://github.com/ParkWardRR/cairn-driving-log-selfhosted/blob/main/contracts/format/v3/spec.md).

## What the hardware can and cannot do

| Capability | Classic ESP32 |
|---|---|
| Encrypt internal SPI flash transparently (AES-256, eFuse key) | **Yes** — flash encryption |
| Refuse unsigned firmware | **Yes** — secure boot (V1 or V2 depending on chip revision) |
| Hardware RNG | **Yes** — `esp_fill_random` (needs the RF subsystem or the bootloader entropy source active for true randomness) |
| Accelerate SHA-256, AES, big-number (RSA/ECC-assist) | **Yes** — mbedTLS uses them |
| Encrypt the microSD card | **No** — it is an external SPI peripheral; nothing is transparent |
| Derive software keys from the eFuse flash key | **No** — the key is read-protected from software; only the flash-encryption engine uses it |
| HMAC / Digital Signature peripherals (non-exportable key use) | **No** — S2/S3/C3 only |
| Secure element / TPM | **No** |
| Run Tailscale | **No** — not realistically; Tailscale lives on the host and phone |

So the SD card is protected **in software, by the application**, and the root of
that protection is a random 32-byte `K_root` held in NVS inside flash that is
itself hardware-encrypted. The eFuse key protects `K_root` by protecting the
flash it lives in; it does not *derive* it.

## Key hierarchy

```text
eFuse flash-encryption key  (hardware only; never visible to software)
  └─ encrypts all of internal flash, including:
       NVS (encrypted via nvs_keys): K_root, Ed25519 seed, device counter
                                     (no network credential: the dongle has none)
            └─ K_seg = HKDF-SHA256(K_root, salt = vehicle_id,
                                   info = "cairn/segment/v3" ‖ ids ‖ segment_index)
                  └─ per-frame XChaCha20-Poly1305, random 24-byte nonce
```

## Order of work (and why the order matters)

1. **Prove signed OTA on hardware**, including failed-update rollback, power
   loss during update and a certificate rotation. Once secure boot and flash
   encryption are in release mode, signed OTA *is* the update path; an untested
   one is a brick.
2. **Migrate the production build to ESP-IDF security configuration.** The
   current build is `framework = arduino`, whose precompiled libraries ship a
   fixed `sdkconfig` — secure boot and flash encryption are compile-time
   `CONFIG_*` options and cannot be toggled there. Move to
   `framework = arduino, espidf` (Arduino as a component) with a checked-in
   `sdkconfig.defaults`. The vendored FreematicsPlus drivers keep working; this
   is the "ESP-IDF application with arduino-esp32 as a component" decision the
   ROADMAP already recorded, finally made real.
3. **Application-layer encryption (format v3)** — independent of the above and
   done first because it protects the card regardless of how the chip is
   configured.
4. ~~Move the mTLS client key and Wi-Fi credentials off the card~~ — **done by
   removal (2026-10-05).** The dongle has no Wi-Fi and no network client, so there
   is no such key to protect; earlier firmware's copies are erased from NVS at boot.
5. **Sacrificial unit.** Enable secure boot + flash encryption on a *spare* ONE+
   in **development mode** first (re-flashable a limited number of times).
   Confirm: boot, OTA install + rollback, NVS reads, SD encryption, standby and
   wake-on-motion, serial logging.
6. **Release mode** on the spare, then on the car's unit last.

## Irreversibility — read before touching a real unit

| Action | Reversible? | Consequence |
|---|---|---|
| Flash encryption, development mode | Limited (a few re-flashes) | Plaintext serial flashing still possible a bounded number of times |
| Flash encryption, **release mode** | **No** | Plaintext UART flashing is permanently disabled; signed OTA is the only update path |
| Secure boot enable | **No** | Only firmware signed with your key boots. **Lose the signing key and the unit is unrecoverable** |
| Disable JTAG / UART ROM download | **No** | Debug and recovery over serial gone |

Never do this to the only unit in the car. Keep the signing key offline (the
existing OTA update key is already kept off the server by design — reuse that
discipline), back it up, and write the recovery plan before the first eFuse burn.

## Check the chip first

Secure boot V2 (RSA-3072 / ECDSA, the stronger scheme) needs **ESP32 revision
v3.0 or later**; older silicon only supports secure boot V1, which is limited
and AES-key-derived. Read it off the real unit before choosing:

```bash
esptool chip_id        # prints "Chip is ESP32-D0WD-V3 (revision v3.x)"
esptool flash_id
```

**Measured 2026-10-05 on the car's unit** (MAC ending 7f:f8, 16 MB flash):
`ESP32-D0WDQ6`, **revision v1.0**. That is the oldest silicon, and it settles two things:

- **Secure boot V2 is not available on this unit.** V2 needs revision v3.0 or
  later. Only the V1 scheme is possible here, which is the weaker one and comes
  with its own constraints (bootloader size and layout, a one-time key burn).
  Re-read the ESP-IDF "Secure Boot (V1)" page for the exact limits against this
  repo's bootloader and partition table **before** planning the sacrificial-unit
  run, and decide whether V1's protection is worth the irreversibility for this
  hardware, or whether the right answer is a newer ONE+ for the car.
- Flash encryption is available on every revision, but on v1.0 silicon the
  number of plaintext re-flashes in development mode is the tightly limited
  7-bit counter; do not spend it casually.

**There is no spare unit.** Steps 5 and 6 above assume a sacrificial ONE+ to
rehearse on; one has never been bought, and the car's dongle is the only one.
A second unit of revision v3.0 or later would make the whole V2 path available —
until it exists, steps 5 and 6 cannot start.

**What that does to the network gate.** Issue #18 first decided that network
credentials wait for flash and NVS encryption (#7). On this unit that condition
is unreachable, not merely unfinished: the burns are ruled out, there is nothing
to rehearse on, and secure boot V2 is unavailable on revision v1.0 in any case.
Left as written it blocked Wi-Fi (#15) and LTE (#16) forever.

**Decided 2026-10-07 — #18's third option replaces it.** Network credentials
will be held under a key the chip cannot reconstruct on its own, derived with
the server's help from the already-escrowed device root, so a flash dump yields
ciphertext instead of an SSID, a password, an APN and a SIM PIN. This is a
narrower claim than flash encryption and must not be described as more: `K_root`
and the Ed25519 seed stay readable from an extracted chip, and a physically
present attacker can still reflash over serial. Nothing above in this document
is cancelled; it becomes the plan for whatever hardware follows this unit.

The open question, which has to be answered in a contract before any credential
is stored on a device:

- **Where does the unwrapping key live between boots?** RAM only is the honest
  answer and costs autonomy: a dongle that reboots in a car park cannot rejoin
  a network until a phone or the server hands the key back. Persisting it in
  NVS re-creates the plaintext secret under another name and gives up the whole
  point. A phone-assisted unwrap over the bonded BLE link fits the uplink
  schedule (BLE is the home state, the phone is usually there) and is the
  likeliest shape, but no wire format, no replay rule and no recovery path for
  "no phone for a week" exist yet.

## Device identity and what still protects a *running* device

None of this protects an unlocked, powered, stolen dongle: it holds live keys by
necessity. That case is handled by **revocation** (`devices.Registry.Revoke`,
effective immediately without a restart) and by keeping the exposure window
short. State this plainly rather than implying hardware makes theft harmless.

## Verification checklist for the sacrificial unit

- [ ] Dump flash over serial *after* enabling flash encryption: ciphertext only.
- [ ] Pull the SD card: no readable GNSS/OBD data, no key, no password.
- [ ] Copy the card to a second dongle: frames fail to authenticate.
- [ ] Flip one bit in a frame on the card: tag failure, server quarantines.
- [ ] Restore an old card image: duplicate (same content) or counter conflict.
- [ ] OTA to a good image; OTA to a deliberately bad image rolls back.
- [ ] Unsigned firmware over serial is refused.
- [ ] Revoke the device on the server; next upload is refused immediately.
