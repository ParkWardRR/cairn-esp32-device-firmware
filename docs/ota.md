# Secure OTA

Updating a device wired into a vehicle's OBD port means never having to pull it
out to reflash. That is the whole reason A/B slots exist from the first commit.
It also means a bad update is the most destructive thing that can happen to this
system — worse than a corrupt bundle, because a bricked device captures nothing
at all and cannot be told so.

So the question this design answers is not "how do we download firmware" but
"what has to be true before a device is allowed to replace itself".

## The update descriptor

A deterministic CBOR map (bundle format §5), signed by an **update key** that is
separate from the receipt key.

Separate because the authorities are different. The receipt key says "this data
is safe to delete"; the update key says "this code is safe to run". A server
compromised enough to issue false receipts costs stored trips; one that could
also sign firmware owns the device. Keeping them apart means the blast radius of
either key is bounded.

| Key | Field | Notes |
|---:|---|---|
| 1 | `descriptor_version` | u8, currently 1 |
| 2 | `firmware_version` | text, e.g. `cairn-v2.1.0` |
| 3 | `image_sha256` | 32 bytes, over the complete image |
| 4 | `image_length` | u32 |
| 5 | `min_firmware_version` | text, `""` for none — refuse to install over something older than this |
| 6 | `build_utc_ms` | u64, informational only; never an ordering key |
| 7 | `signature_algorithm` | text, `ed25519` |

The signature covers the canonical encoding of keys 1–7 and is carried
alongside, not inside — exactly as the manifest and receipt do, so there is
nothing to strip before verifying.

## What must be true before an update is allowed

All four, checked on the device, every time:

**1. No unreceipted bundles.** A bundle that has not been acknowledged by a
server exists only on this card. If the new image fails to boot and rollback
also fails, that data is gone. Updating with data pending trades something
irreplaceable for something that can wait.

**2. Parked.** Capture must not be interrupted mid-trip, and a reboot during a
drive loses the open capture's unflushed tail.

**3. External power.** A supply that dies during the write leaves a half-written
slot. The A/B layout means that is survivable — the old slot is untouched — but
there is no reason to take the risk on a vehicle battery that is already low.

**4. A verified signature.** Covered below.

Any of these failing is not an error. It is a reason to try later, logged as
such.

## Ordering, which is the part that matters

```
1. fetch descriptor
2. verify the descriptor signature against the pinned update key
3. refuse if firmware_version is not newer, or if this build is older
   than min_firmware_version
4. download the image into the inactive slot, writing as it arrives
5. read the slot back and hash what is actually in flash
6. compare that hash against image_sha256
7. only now: esp_ota_set_boot_partition
8. reboot
9. the new image marks itself valid only after storage checks out;
   otherwise the bootloader rolls back
```

Two steps are easy to get wrong and both are load-bearing.

**Step 2 before step 4.** Verifying the signature before downloading means an
unsigned or wrongly signed descriptor costs nothing. Verifying after would mean
a hostile server could make the device write megabytes into its spare slot on
demand.

**Step 5 reads flash back.** Hashing the bytes as they arrive proves the
*download* was intact; it does not prove the *write* succeeded. Flash can fail
to program, and a partially written slot that hashes correctly in RAM is exactly
the failure that produces a boot loop. The hash must come from the slot.

**Step 7 last.** The boot partition is only changed once the image in flash is
known to be the signed one. Before that point, an interruption at any step
leaves the device running its current firmware with a junk spare slot, which the
next attempt overwrites.

## Rollback

`esp_ota_mark_app_valid_cancel_rollback()` is called only after the card mounts
and the directory tree is confirmed — which is already how the firmware behaves,
and the reason that call is not at the top of `setup()`. An image that boots but
cannot reach its storage is not a working image, and letting it mark itself valid
would strand the device one reboot away from working.

If the image never marks itself valid, the bootloader reverts to the previous
slot on the next reset. The firmware logs loudly when it finds itself running a
slot that differs from the configured boot partition, because that is the
signature of a rollback having happened and is otherwise invisible.

## Signing

```bash
# once: create the update keypair
cairn-signfw -genkey -key update.seed

# print the public key to pin in firmware
cairn-signfw -key update.seed -print-public

# sign an image
cairn-signfw -key update.seed \
  -image .pio/build/cairn/firmware.bin \
  -version cairn-v2.1.0 \
  -out firmware-v2.1.0
```

That writes `firmware-v2.1.0.cbor` (the descriptor) and
`firmware-v2.1.0.sig`, alongside the image the server serves.

Pin the public key in `firmware/cairn-v2/include/secrets.h`:

```c
#define CAIRN_UPDATE_KEY_HEX "…64 hex characters…"
```

Leaving it undefined disables OTA entirely — the device will not fetch, let
alone install. That is the correct default: a device that cannot verify an
update has no business installing one.

## Serving

```
GET /api/v2/firmware/latest          → descriptor CBOR, signature in a header
GET /api/v2/firmware/{sha256}/image  → the image bytes
```

The image is addressed by its hash, not by version, for the same reason bundle
chunks are: a hash cannot be ambiguous about which bytes it names.

## What is not done

- **No staged rollout or per-device pinning.** Every enrolled device sees the
  same `latest`. For a homelab with one vehicle that is adequate; it would not
  be for a fleet.
- **ESP-IDF secure boot is not enabled**, so this verifies the *application*
  signature rather than the bootloader's. A physically present attacker can
  still flash over serial. Enabling secure boot is irreversible, which is a poor
  property for hardware already in a car — the same reasoning that keeps flash
  encryption off.
- **No delta updates.** Images are small enough that the whole thing fits
  comfortably in a slot and transfers in seconds on Wi-Fi.
