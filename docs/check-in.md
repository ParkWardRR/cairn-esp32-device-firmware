# BLE check-in: instructions and the home trigger

`lib/cairn_checkin` implements `contracts/ble/v1/checkin.md` (contracts-v0.2.0, draft): the
signed `INSTRUCTION` the phone carries to the dongle, and the unsigned `HOME_TRIGGER`.
`make -C test/host checkin` runs 92 rows: all 14 instruction vectors with their exact answer
bytes, the 4 home-trigger vectors, and rows the vectors cannot hold. ASan/UBSan clean; 20
deliberate breakages of the checks all caught.

## Status

**Built and host-tested. Not wired into the firmware**, deliberately:

- it needs the pinned **instruction public key**, a build-time trust anchor like the receipt key,
  which does not exist in `secrets.h` yet (the server operator generates it);
- the GATT characteristics `0042` to `0044` are not created, so `DEVICE_INFO` leaves the
  instruction and home-trigger capability bits clear. A remote-control surface should not go
  live untested on a car.

The effects are callbacks (`upload_now`, `stop_trying`, `clear_stop`, `config`), the floor is a
`persist_floor` callback, so the glue is small: a `cairn_kv` slot for the counter floor, the
trip state, and, for `config`, `cairn_config_receive` (see below).

## What it enforces

The contract's checks, in its order: version, length and the length the header claims; the
Ed25519 signature over `"CAIRN-INSTR-V1" 0x00 device_id frame` with nothing else looked at
before it is good; a type from the closed set of four and the body size that type requires;
a counter strictly greater than the floor; the body in range (`STOP_TRYING` is 1 to 168 hours);
no trip; at most 1 per 2 s and 30 per hour; then apply, with the floor made durable before the
answer. The floor moves only when an instruction was applied. The answer is the contract's
8-byte `INSTRUCTION_RESULT`.

The home trigger: exactly 6 bytes, reserved flag bits clear, at most 900 s, a newer `seq` (with
wrap), at most one per 10 s, ignored during a trip, expiring on its own, held in RAM only.

## Where this differs from the contract, and why

- **Persist before the effect** for `UPLOAD_NOW`, `STOP_TRYING` and `CLEAR_STOP`. The contract
  says apply, then persist. If persisting fails, applying first leaves an instruction that took
  effect and can be replayed; persisting first can only lose one to a power cut. Observable
  behaviour on every vector is identical. A `CONFIG` is applied first, because the verifier may
  refuse it and a refused instruction must not move the floor.
- **`STORE_FAILED` (9)** is returned when the floor cannot be made durable. The contract's
  status table has no row for it. Raise it with the contract before relying on it.
- **`ENCRYPTION_REQUIRED` for a credential in a `CONFIG`** is decided by the configuration
  verifier, so it is reported after the rate limit rather than before it as the contract lists.
  Only the order of two refusals differs, and only when both apply.
- The rate limit counts frames that got past the signature, not every write, so a flood of
  garbage costs the dongle a signature check per write. The link is bonded, which bounds who
  can write, but a bonded hostile central can still spend battery this way. Worth a cheaper
  pre-check once the characteristic is wired.

## How it meets the configuration receiver

A `CONFIG` instruction carries up to **440 bytes** and this layer hands the body, unread, to
`cairn_config_receive` (`docs/config-receiver.md`). The two signatures are separate and both
required: the server signs the instruction, and the configuration message carries its own
signature and seal. Whether that doubling is intended is a question for `config/v1`.

A size problem to settle there: the provisional configuration envelope adds about 200 bytes of
header, tag and signature, so one `CONFIG` instruction carries at most about 240 bytes of
payload. A full list of eight Wi-Fi networks does not fit in one message; it would have to be
sent as several, each replacing or extending the list, and the receiver today replaces the
whole list.
