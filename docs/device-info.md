# Device information over BLE

`lib/cairn_devinfo` (issue #14) implements the `DEVICE_INFO` characteristic and the
`UPLINK_EVENT` frame of `contracts/ble/v1/device-info.md`, as released in
**contracts-v0.2.0** (status: draft in that release). `make -C test/host devinfo` checks it
against the contract's own vectors: 72 rows, ASan/UBSan clean, 16 deliberate breakages of the
encoder and decoder all caught.

## What is done

- The encoder and decoder, byte for byte against every vector in
  `ble/v1/vectors/device-info/vectors.json`: the full value, the minimal one, a newer minor
  version with an unknown record (skipped, the rest read), the 512-byte cut (engine records
  dropped from the end, marker record `0x7F`), and the three malformed values (bad `total_len`,
  unknown major version, a known record of the wrong length), which are refused. The
  `UPLINK_EVENT` frame, including the 23-byte frame that must be refused.
- The firmware side: `src/device_info.cpp` fills the value from live state and
  `src/ble_companion.cpp` serves it as characteristic `0040` (read, encrypted and
  authenticated like every other), with `PROTOCOL_VERSION` capability bit 3 set.

## What it reports, and what it does not claim

| Record | Source | Honest limits |
|---|---|---|
| firmware | `CAIRN_FIRMWARE_VERSION`, the chip's secure-boot and flash-encryption state | `commit` is zeros unless the build defines `CAIRN_GIT_COMMIT_HEX`; `build_unix` is the compiler's `__DATE__`/`__TIME__` read as UTC, so it can be off by the builder's time zone |
| identity | device id and fingerprint from the loaded key | `enrol_state` is 2 when a vehicle assignment is provisioned, else 0: the dongle cannot tell "enrolled, no vehicle yet" from "not enrolled" |
| storage | the card, set at mount and when the pending count changes | `free_mib` is read once at mount |
| transport | BLE up; Wi-Fi: radio present, no Wi-Fi code; LTE: **not claimed**, whether the unit has a modem is unconfirmed (#16) | |
| engine | every installed profile: version, first 8 bytes of the profile's SHA-256, id | the hash is of `engines/<id>.yaml` (CRLF normalised) until `engine/v1` defines the canonical bytes |
| boot timing | `lib/cairn_boottime`: power-on to BLE advertising, to a capture being open, to the first fix | not measured on a unit yet |

Capability bits set: phone GNSS (0), bundle offload (2, only if the offload task started),
device information (3). Not set, because nothing implements them: live OBD, uplink events,
instructions, home trigger, Wi-Fi, LTE, digest, configuration. The characteristics `0041` to
`0044` are not created; a bit for something absent would be a lie the app is told to trust.

## Not verified

- A phone has not read it from a real unit. In particular, a long read (the value can be up
  to 512 bytes against a 185-byte MTU) rebuilds the value in the read callback; if the stack
  calls it once per fragment the value could change between fragments. Check on hardware.
- The vectors run only where the pinned contracts include them. A checkout pinned to
  contracts-v0.1.0 skips the row and says so; the pin bump to 0.2.0 turns it on.
