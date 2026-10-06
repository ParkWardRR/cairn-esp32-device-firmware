# Configuration receiver

`lib/cairn_config` (issue #22) is what the dongle does with a sealed configuration message
from the web UI or the iOS app, delivered at a BLE check-in or in any authenticated uplink
session. Portable C; `make -C test/host config` runs 90 rows, ASan/UBSan clean, with 14
mutations of the safety rules all caught.

## Status

**PROVISIONAL, host-tested, not wired into the firmware.** `contracts/config/v1` is not
released (an open design issue upstream), so the envelope here is built only from primitives
the enrolment contract already uses, and is isolated in `cairn_config_build()` and the open
step of `process()` so that only those move when the contract lands. The policy around it is
the part meant to survive.

It is not wired in because nothing delivers a message yet (the check-in is a BLE contract
change that is also unreleased), and because the credential half cannot be used before flash
and NVS encryption (#7).

## What it enforces, in order

1. size and shape; the message names this device
2. the signer is authorised (a callback: the pinned server key or an enrolled client)
3. the Ed25519 signature over the whole sealed envelope
4. the counter is strictly greater than the last applied (an equal one is a replay)
5. the seal opens (X25519 + HKDF-SHA256 + XChaCha20-Poly1305; the header is the AAD, so
   device, signer and counter cannot be edited, even by someone who can re-sign)
6. every field is on the allow-list, appears once, is in canonical order, and is within its
   sanity limits; trailing bytes and empty messages are refused
7. credential-class fields (APN, SIM PIN, Wi-Fi networks) are refused unless storage is
   encrypted **and** a credential store accepts them. This is whole-message: a refused
   credential also refuses the settings beside it. The configuration's own persisted slots
   never contain a credential or a network name (a row checks the raw bytes)
8. the new configuration is written to the inactive of two slots (generation + CRC); the
   newer valid slot wins at boot, so a torn write leaves the previous good one in force

A rejected message does not consume the counter. Attempts are rate limited (10 per minute,
good or bad) so the signature check cannot be used to burn the battery. Every outcome is in
a ring of the last eight, carrying a short signer id and never a key. The reported state has
the settings, the counter, a state hash (SHA-256 over canonical settings and counter),
whether credentials are accepted, and the refusal counts. Rollback restores the previous
configuration but keeps the counter, so a rolled-back message cannot be replayed.

## How a message reaches it

At a BLE check-in the phone writes an `INSTRUCTION` of type `CONFIG` and `lib/cairn_checkin`
(`docs/check-in.md`) hands the body to `cairn_config_receive`, mapping a credential refusal to
`ENCRYPTION_REQUIRED`. Both hand-offs are host-tested; nothing is wired on the device yet.
The body is capped at 440 bytes by `checkin.md`, which leaves about 240 bytes of payload after
this envelope: too little for a full list of Wi-Fi networks in one message.

## Open points the contract has to settle

- **How the server learns the device's configuration public key.** The enrolment blob carries
  only the signing key. Here the key pair is derived from the signing seed with its own HKDF
  label (never the signing key itself), but the public half has to reach the server, so
  enrolment needs a field for it. That is a change to a released contract.
- **Who may sign.** The callback takes the pinned server key or an enrolled client; the
  contract has to say which, and how the enrolled set is stored.
- **The decryption key is a secret on the chip.** It is derived from the seed that is already
  stored; until #7 it has the same exposure as the signing seed.
- **The home-network list is credential-class** (it reveals where the owner lives), so it is
  refused today. The no-phone home scan (#17) therefore has no list to scan for until #7.
- **Where credentials go.** `store_credential` is a callback to encrypted storage that does
  not exist yet.

## Scan fallback

The "done when" also asks that the scan fallback starts a Wi-Fi slot only under the stated
conditions. That logic is in `lib/cairn_uplink` (see `docs/uplink-manager.md`) and is
tested there: parked, bundles waiting, no phone, battery ok, rate limited, bounded.
