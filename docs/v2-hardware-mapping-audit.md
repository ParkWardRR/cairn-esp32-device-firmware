# v2 firmware ↔ hardware mapping audit

Checked the v2 firmware against three sources, in increasing order of authority:

1. The [official ONE+ developer guide](https://freematics.com/pages/products/freematics-one-plus/guide/)
   — thin on internals, but it does document the external 4-pin socket as
   **GND / GPIO26 / VCC(5V) / GPIO34**.
2. The vendored `FreematicsPlus` library, which is what actually configures the
   hardware.
3. [flashing-and-testing.md](flashing-and-testing.md) — values *measured on this
   unit*, which beats both of the above wherever they disagree.

## Verdict

Mapping is correct. Two real defects found, one in this firmware and one
latent trap; both fixed. Three things that look wrong and are deliberately not
changed, documented so they are not "fixed" later.

## Confirmed correct

| Item | Firmware | Source of truth |
|---|---|---|
| microSD | SPI, CS = GPIO5 | `PIN_SD_CS 5`, and v1 `SD.begin(PIN_SD_CS)` |
| LED | GPIO4 | `PIN_LED 4` (non-C3 branch, which is ours) |
| IMU | hardcoded `ICM_42627` | measured: ICM-42627 at I²C `0x68` |
| Flash mode | DIO, 40 MHz | measured: Winbond W25Q128, DIO, 40 MHz |
| Partition budget | 4 MB layout | v1 flashes with `--flash-size 4MB` on the 16 MB chip |
| OBD units | km/h, rpm, kPa, °C, % | `COBD::normalizeData` converts to exactly these |
| GNSS init order | `gpsBeginExt()` then `gpsBegin()` | measured: "gpsBeginExt() fails; gpsBegin() succeeds" |

The GNSS ordering is safe because **both paths verify**: `gpsBeginExt()` watches
the NMEA sentence counter for ~1.1 s and calls `gpsEnd()` if nothing arrives,
and `gpsBegin()` waits for `$GNGGA` before claiming success. Neither reports a
receiver that is not answering.

The two vendor bugs v1 found are still fixed in the shared library, which v2
inherits through `lib_extra_dirs`:

- `SELF_TEST_CONFIG_REG` is written `0x00` (the vendor shipped `0x07`, leaving
  IMU self-test permanently on)
- `GYRO_ZOUT_L_REG` is `0x2A` (the vendor had `0x30`)

Both verified present. Worth re-checking after any library update, since a
regression there corrupts IMU data silently.

## Defect 1 — coolant temperature wrapped on a hot engine

`COBD::getTemperatureValue` returns `hex2uint8(data) - 40`, so the range is
**−40 … +215 °C**. The specification's field is `i8` with `0x80` reserved for
"unavailable". The firmware did a bare `(int8_t)` cast.

So 128 °C became −128, which *is* the unavailable sentinel, and 130 °C became
−126 °C. An overheating engine — the single reading most worth having — would be
recorded either as a missing sensor or as a plausible cold value. Both are
indistinguishable from real observations, which is the failure mode this
project's whole design exists to prevent.

Fixed with a saturating conversion that reserves −128 for genuinely absent
data and clamps real values to ±127. Saturating at +127 is still obviously
extreme; wrapping was not. Applied to `coolant_temp_c`, `intake_temp_c`,
`timing_advance_deg`, `device_temp_c` and `rssi_dbm`.

## Defect 2 — half of `board_config.h` was dead

`board_config.h` defined `CAIRN_PIN_BUZZER`, `CAIRN_PIN_LINK_UART_RX/TX`,
`CAIRN_PIN_GPS_POWER` and `CAIRN_PIN_GPS_UART_RXD/TXD`. **Nothing read any of
them** — everything reached through FreematicsPlus is configured by that
library's own defines.

That is worse than untidy. The file looks authoritative, so correcting a pin
there would appear to work and change nothing. Reduced to the two pins this
firmware actually drives (`SD_CS`, `LED`), with the library's values recorded as
commentary rather than as code.

While doing this, one genuine piece of information surfaced that the official
guide confirms: `PIN_GPS_UART_RXD 34` / `TXD 26` are the *same pins* as
`PIN_MOLEX_2` / `PIN_MOLEX_4`, and `PIN_GPS_POWER 12` is `PIN_MOLEX_VCC`. The
external GNSS and the external I/O header are one connector — which is why
`gpsBeginExt()` fails on a unit with nothing plugged into it.

## Deliberately not changed

**`sys.begin()` keeps `useCellular = true`** even though no modem is fitted.
That branch is the only thing that sets `FLAG_GNSS_SOFT_SERIAL`, which selects
the GNSS transport. The measured profile recorded 38400 soft serial, i.e. it was
captured with the defaults, so passing `false` would move the firmware off the
configuration this board was validated under. Cost of the default: a UART1 init
on pins 35/2 and GPIO27 driven low for a modem that is not there — wasted setup,
not a conflict, since the coprocessor link is UART2 and the GNSS in use reaches
the receiver over that link. Commented at the call site so it is not "optimized"
later.

**`gpsBeginExt()` is still tried first** even though it is known to fail here,
costing roughly 1.5–2 s at boot. Keeping it means a unit with an external
receiver still works, and the failure is verified rather than assumed. Boot time
is not scarce; silently losing GNSS on a different unit would be.

**`ENABLE_OBD` / `ENABLE_MEMS` / `ENABLE_BLE` are not defined.** v1 set these,
but they were application-level flags in v1's own `#if` blocks — the library
never reads them (`grep` over `lib/FreematicsPlus/` finds nothing). v2 has no
such conditionals, so defining them would be cargo-culting.

## Not verified without hardware

- The GNSS date/time decode. The civil-from-days arithmetic has never seen a
  real fix, and the driver's `date`/`time` encoding is assumed to be `DDMMYY` /
  `HHMMSSss`. Worth checking the first `UTC basis established` log line against
  a known clock.
- `COBD::getVoltage()` units. Assumed volts; the firmware multiplies by 1000 for
  millivolts. The measured ADC map in the profile was taken on USB power, so
  there is no reference value to compare against yet.
- Whether `PID_FUEL_PRESSURE` is supported by the vehicle at all. Unsupported
  PIDs are counted in `pids_requested` vs `pids_answered` rather than guessed,
  so an unsupported PID is recorded as absent — which is the correct outcome,
  but it means a run of zeros there is not necessarily a bug.
