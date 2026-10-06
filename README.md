# Cairn ESP32 device firmware

Firmware for the in-car dongle of the [Cairn driving log](https://github.com/ParkWardRR/cairn-driving-log-selfhosted) (a
Freematics ONE+ Model B, ESP32). It records OBD-II, GNSS and IMU data to an SD card as encrypted,
hash-chained bundles, seals each trip, and hands the bundles to the enrolled phone over BLE. **The dongle
has no Wi-Fi, no LTE and no network credentials of any kind.** The server's signed receipt is checked on
the device before a bundle is deleted.

| Part | What it is |
|---|---|
| `src/`, `include/` | the application: capture, lifecycle, BLE companion and offload |
| `lib/` | portable C libraries: format, storage, provisioning, OTA, power, offload |
| `test/host/` | the same libraries compiled natively, with conformance vectors, fault injection and ASan/UBSan |
| `emulator/` | a Rust dongle emulator with an independent implementation of the bundle format |
| `third_party/freematics-base/` | vendored Freematics drivers |
| `research/` | measurement notes and reports behind the design decisions |

## Contracts

The bundle format, enrolment protocol and BLE offload protocol this firmware implements are specified, with
test vectors, in [Cairn Vehicle Data Protocols](https://github.com/ParkWardRR/cairn-driving-log-selfhosted/tree/main/contracts).
`contracts.lock` pins a release by tag **and** commit; `scripts/fetch-contracts.sh` fetches it into
`.contracts/` and verifies both. `CAIRN_CONTRACTS=<dir>` overrides it for changing a contract and the
firmware together; release builds refuse the override.

## Build and test

```sh
scripts/fetch-contracts.sh
make -C test/host            # format conformance, storage fault matrix, provisioning, offload
make -C test/host asan       # the same under AddressSanitizer and UBSan
(cd emulator && cargo test && cargo run --release -- conformance)

cp include/secrets.h.example include/secrets.h     # then fill in; the real file is gitignored
pio run -e cairn -j 1                                # one job: the build host is memory-constrained
```

`include/secrets.h` holds the server's public keys and the BLE passkey. Never commit it. Flashing and the
bench round trip are in `docs/flashing-and-testing.md` and `docs/hardware-roundtrip.md`.

## Related repositories

- [cairn-driving-log-selfhosted](https://github.com/ParkWardRR/cairn-driving-log-selfhosted): the front door, system docs and the contracts
- [cairn-vehicle-server](https://github.com/ParkWardRR/cairn-vehicle-server): the vehicle server
- [cairn-vehicle-web-dashboard](https://github.com/ParkWardRR/cairn-vehicle-web-dashboard): the web dashboard
- [cairn-ios-companion-app](https://github.com/ParkWardRR/cairn-companion-ios-esp32-obd2-gps-ble): the iPhone app

History before the split is preserved here; see [MIGRATION.md](MIGRATION.md).

## License

Blue Oak Model License 1.0.0, see [LICENSE](LICENSE). The vendored Freematics drivers under `third_party/` are third-party code; see their file headers.
