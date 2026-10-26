# Flashing and Testing — Freematics ONE+ Model B

## Hardware Profile

### ESP32

| Property | Value |
|----------|-------|
| Chip | ESP32-D0WDQ6 rev 1 |
| Cores | 2 (Dual Core + LP Core) |
| CPU | 160 MHz (rated for 240 MHz) |
| Crystal | 40 MHz |
| Flash | Winbond W25Q128 (manufacturer 0xEF, device 0x4018), 16 MB, DIO mode, 40 MHz |
| PSRAM | 4 MB usable (chip is 8 MB; ESP32 D0WDQ6 rev 1 addresses 4 MB max) |
| Heap | 314 KB total, 289 KB free at boot, 107 KB max single alloc |
| SDK | ESP-IDF v4.4.7 |
| MAC | 7C:9E:BD:FA:7F:F8 |
| ADC Vref | 1100 mV (eFuse calibrated) |
| Flash encryption | Not enabled (FLASH_CRYPT_CNT = 0) |
| Secure boot | Not enabled (V1 and V2 both off) |
| JTAG | Not disabled (available for debugging) |
| UART download | Not disabled (always reflashable) |

### OBD Coprocessor (STM32)

| Property | Value |
|----------|-------|
| Firmware | OBD2USART V1.5 (ELM327 v1.5 compatible) |
| Device type | 15 (newer board revision) |
| Link | UART (GPIO 13 RX, GPIO 14 TX, 115200 baud) |
| ATRV on USB | 5.42V (USB 5V rail through regulator) |
| OBD protocol | Auto-detect (ATSP0), no ECU without car |
| STN commands | Not supported (basic ELM327 clone, not STN-based) |

### IMU (ICM-42627)

| Property | Value |
|----------|-------|
| I2C address | 0x68 |
| WHO_AM_I | 0x20 (confirmed) |
| Accel config | FS=2g, ODR=200Hz (driver sets 1kHz but bus limits) |
| Gyro config | FS=250dps, ODR=200Hz |
| Zero-rate offset | X=0.0, Y=-0.4, Z=-0.1 dps (within spec after self-test fix) |
| Die temperature | 39°C on bench (USB power) |
| Gravity reading | X=-0.910, Y=-0.410, Z=0.016 g (device lying on side) |
| Accel noise | <0.002 g RMS (10 consecutive readings) |

### I2C Bus

| Address | Device |
|---------|--------|
| 0x68 | ICM-42627 (IMU) |
| 0x7E | ICM-42627 device ID response (I2C reserved address, not a separate chip) |

### GNSS

| Property | Value |
|----------|-------|
| Module | Internal (gpsBeginExt() fails; gpsBegin() succeeds) |
| Power pin | GPIO 12 |
| UART RX | GPIO 34 |
| UART TX | GPIO 26 |
| Baud | 38400 (soft serial) |
| Read paths | `ATGPS`, a parsed ~1 Hz summary; `ATGRR`, the raw NMEA buffer at 5 Hz. Both go through the OBD co-processor link |

Three things about these paths cost real drives and are worth knowing before touching them
(all fixed in `2d262f9`, 2026-10-15):

- **`gpsData.ts` is not a fix time.** FreematicsPlus sets it to `millis()` on every link
  read, so "unchanged ts" never meant "no new fix". Staleness is judged from the receiver's
  own date and time of day.
- **HDOP from `ATGPS` is already ×100.** It was multiplied by 10 again, so stored HDOP read
  ten times too high. It is passed through as the format's HDOP ×100.
- **An `ATGRR` buffer holds several fixes and can repeat across polls.** Each fix is a
  checksummed GGA paired with the RMC of the same time of day (GGA alone has no speed or
  course), aged from its own time of day. The receiver says nothing about solution
  quality, so `lib/cairn_gnss` vets every fix against OBD speed and the IMU before it is
  written (see the README's Status table).

The co-processor link is shared by OBD, GNSS and the supply-voltage read. It has no
protocol for interleaving, so since `6e62b44` (2026-10-23) every exchange holds
`sensors_link_lock`, and only the sensing task reads the voltage. Before that, the
controller's `getVoltage()` exchanges collided with GNSS and OBD traffic and about half
the rail readings while driving came back as 9000 mV, 16000 mV or unreadable.

### ADC Channel Map

Measured on USB power (no car battery):

| Channel | GPIO | Reading | Notes |
|---------|------|---------|-------|
| CH0 | 36 | 425 mV | Battery voltage sense (devType 15 path) |
| CH3 | 39 | 213 mV | Input-only pin, faint signal (floating) |
| CH4 | 32 | 625 mV | GPS UART RXD2 line — serial residual |
| CH5 | 33 | 319 mV | GPS UART TXD2 line — serial residual |
| CH7 | 35 | 3134 mV | Input-only, pulled high (~3.3V reference or status line) |

### GPIO Pin States (at boot)

| GPIO | Function | State | Notes |
|------|----------|-------|-------|
| 4 | LED | LOW | Off at boot |
| 5 | SD_CS | HIGH | Chip select deasserted (idle) |
| 12 | GPS_POWER | LOW | GPS off until gpsBegin() drives it high |
| 13 | LINK_UART_RX | HIGH | UART idle |
| 14 | LINK_UART_TX | HIGH | UART idle |
| 15 | GPS_POWER2 | HIGH | Alternate GPS power, held high |
| 25 | BUZZER | LOW | Silent |
| 26 | GPS_UART_TXD | LOW | |
| 27 | BEE_PWR | LOW | Cellular modem power OFF (confirmed) |
| 32 | GPS_UART_RXD2 | LOW | |
| 33 | GPS_UART_TXD2 | LOW | |
| 34 | GPS_UART_RXD | LOW | |

### WiFi

| Property | Value |
|----------|-------|
| MAC | 7C:9E:BD:FA:7F:F8 |
| Networks detected | 26-37 (varies by scan) |
| Home AP | -62 dBm on CH11 |
| WPA3 support | Yes (WPA3-PSK networks detected and parsed) |

### Peripherals

| Device | Status |
|--------|--------|
| LED (GPIO 4) | Working — blinked on command |
| Buzzer (GPIO 25) | Working — 2kHz tone confirmed |
| SD card slot (SPI, CS=GPIO 5) | Working — FAT32 16 GB mounted |
| Cellular modem (GPIO 27) | Power OFF, confirmed not initializing |

---

## Bugs Found and Fixed

### IMU self-test left permanently enabled (FIXED)

The vendored FreematicsPlus ICM-42627 driver wrote `0x07` to `SELF_TEST_CONFIG` 
(register 0x70) during initialization and never cleared it. This left gyro 
self-test actuation running continuously.

**Before fix:** Gyro read 108/111/118 dps while stationary (internal actuation).
**After fix:** Gyro reads 0.0/-0.4/-0.1 dps while stationary (true zero-rate).

Fix: Changed `writeByte(SELF_TEST_CONFIG_REG, 0x07)` to
`writeByte(SELF_TEST_CONFIG_REG, 0x00)` in
`firmware/cairn-v2/third_party/freematics-base/lib/FreematicsPlus/FreematicsMEMS.cpp:742` (vendored; still used by `firmware/cairn-v2` through `lib_extra_dirs`).

The bias calibration step masked this bug (it subtracted the ~108 dps offset),
but running in self-test mode wastes power and reduces gyro dynamic range.

### GYRO_ZOUT_L register address wrong (FIXED)

`utility/ICM_42627.h` defined `GYRO_ZOUT_L_REG` as `0x30` — should be `0x2A`.
Latent bug (the driver reads 6 contiguous bytes from `GYRO_XOUT_H`, not individual
registers), but wrong in the header.

---

## Flash Station

- **Host:** Raspberry Pi (flash-station.example.lan)
- **OS:** Debian 13 (trixie), kernel 6.18, aarch64
- **Serial port:** `/dev/ttyUSB0` (CH340 — the Freematics; was ttyUSB1 when FT232 adapter was also connected)
- **Tool:** esptool v5.4.0 in `~/cairn-flash/venv/`

### Setup (one-time)

```bash
ssh alfa@flash-station.example.lan
mkdir -p ~/cairn-flash
python3 -m venv ~/cairn-flash/venv
~/cairn-flash/venv/bin/pip install esptool
```

### Build (on dev machine)

The firmware is `firmware/cairn-v2` (the v1 firmware that used to live in
`firmware/cairn-v2/third_party/freematics-base` is gone; only its vendored `lib/` drivers remain and
are pulled in by `lib_extra_dirs`).

```bash
cd firmware/cairn-v2
pio run -e cairn            # production capture image
pio run -e cairn-selftest   # bench self-test image (see v2-firmware-testing.md)
```

Other environments in `platformio.ini`: `cairn-ble`, `cairn-pidtest`,
`cairn-mtprobe`, `cairn-mtprobe-sniff`.

### Flash

Use PlatformIO's own upload, from the Mac or from the flash station; the exact
procedure, baud-rate trap (use 460800) and image order are in
[v2-firmware-testing.md](v2-firmware-testing.md#flashing).

```bash
pio run -e cairn-selftest -t upload --upload-port /dev/ttyUSB0          # Linux / Pi
pio run -e cairn-selftest -t upload --upload-port /dev/cu.usbserial-<n> # macOS
```

### Serial monitor

```bash
screen /dev/ttyUSB0 115200
```

Exit: `Ctrl-A` then `\` then `y`.

### Flashing the car's dongle

There is one dongle and no spare. A channel build goes over USB with
`scripts/release-firmware.sh <beta|stable> --flash`, which **backs up NVS before
uploading** — a lost storage root makes every sealed bundle on the card unreadable. The
latest is a `beta` build of `6e62b44`, flashed this way on 2026-10-23.

**Opening the USB serial port resets the dongle**, monitor included: the adapter's control
lines drive the ESP32's reset (the snippet below relies on it). So attaching a monitor
ends the running boot. That is harmless now — the next boot seals a capture left open by
the earlier one rather than extending it (`f940c84`) — but do not attach to a dongle in
the middle of a drive you want recorded whole.

**When no radio delivers**, as on 2026-10-23 (Wi-Fi refused at the router, LTE
registration denied, BLE too weak from a Mac), the card can be read on a computer and its
sealed bundles uploaded with the server's `cmd/cairn-push`. The bundles are ciphertext, and
the dongle still prunes nothing until it holds a verified receipt.

### Reset without reflashing

```bash
~/cairn-flash/venv/bin/python3 -c "
import serial, time
ser = serial.Serial('/dev/ttyUSB0', 115200)
ser.dtr = False; ser.rts = True; time.sleep(0.1)
ser.rts = False; time.sleep(0.1); ser.close()
"
```

---

## Test Results

Results below were recorded on the v1 firmware (then "Cairn v0.2") and are kept
as a record of the hardware bring-up. For v2/v3 firmware results see
[v2-firmware-testing.md](v2-firmware-testing.md) and
[hardware-roundtrip.md](hardware-roundtrip.md).

### First boot (2026-09-29)

Firmware: Cairn v0.2.

| Subsystem | Result | Notes |
|-----------|--------|-------|
| ESP32 boot | OK | POWERON_RESET, SPI_FAST_FLASH_BOOT |
| PSRAM | OK | 8 MB (full chip accessible) |
| OBD coprocessor | OK | Device type 15, ELM327 v1.5 firmware |
| ICM-42627 IMU | OK | WHO_AM_I 0x20, bias calibrated, self-test bug fixed |
| GNSS | OK | Internal module, no fix indoors (expected) |
| WiFi | OK | 29-34 networks, home APs at -56 to -71 dBm |
| SD card | OK | FAT32 16 GB, mounted, 15177 MB total, /cairn/trips created |
| Battery | OK | 3.8V on USB, 5.42V via ATRV (expected) |
| LED | OK | Blink confirmed |
| Buzzer | OK | 2kHz tone confirmed |
| Cellular | OFF | GPIO 27 LOW, modem not initializing |
| State machine | OK | Initialized → SLEEP |

### SD card test (2026-09-30)

FAT32 16 GB microSD inserted. Firmware-only reflash via `/dev/ttyUSB0`.
Two consecutive boots both report `[SD] Mounted: 15177 MB total, 1 MB used`.
`SD.mkdir(TRIP_BASE_PATH)` called successfully (creates `/cairn/trips`).

### Credentials

`firmware/cairn-v2/include/secrets.h` is gitignored — this repo is public, so
nothing real belongs in a tracked file. `config.h` includes it when present and
falls back to placeholders otherwise, so a fresh clone builds without it (it just
cannot reach a server).

```bash
cd firmware/cairn-v2
cp include/secrets.h.example include/secrets.h
# then edit secrets.h: server host/port, pinned receipt key, enrolment key, CA
```

Since the uplink went live (2026-10-07) the network credentials — Wi-Fi SSID and
passphrase, the pinned CA, this device's mTLS client certificate and key, the APN — are
compiled in from `secrets.h`, so they sit in plaintext flash; the README's Security
posture states what a flash dump costs. A missing or placeholder `secrets.h` fails
open to an unconfigured device that uploads but never prunes, so check the boot
log after flashing rather than assuming.

### Network self-test

`env:cairn-selftest` is the bench build: it runs the known-answer checks, writes
and seals a bundle, joins Wi-Fi and syncs it, printing each step over serial.
Flash it to validate the card and the network path, then flash `env:cairn`
before driving. What each line means is in
[v2-firmware-testing.md](v2-firmware-testing.md#reading-the-self-test).

Two problems the original (v1) network self-test caught on first run, both still
worth checking when association fails:

| Problem | Symptom | Fix |
|---------|---------|-----|
| SSID misspelled (one letter off) | SSID not visible on 2.4 GHz | Correct it; the ESP32 has no 5 GHz radio, so a 5 GHz-only SSID looks the same |
| Server host was `cairn.local` | mDNS not available to the ESP32 resolver | Use a DNS name (`CAIRN_SERVER_HOST`), not a `.local` name |

The server is reached by DNS name, not a hardcoded IP: it holds a DHCP lease, so
the name is the only stable handle.

### Pending tests

The bring-up list that stood here (GNSS outdoors, a full car test, home sync, power-loss
recovery) has been done on the v3 firmware. What is still open, as of 2026-10-24:

1. A drive on `6e62b44`: that the link lock ends the garbled rail readings, that a trip
   starts and stays open on a smooth road, and that OBD answers once the bus opens
   (#37, #42)
2. A drive with the GNSS gate: refusals near parking, none while really driving, and
   bundles sealed within a boot dated by the carried UTC basis
3. Wi-Fi to the home network, which refused association on 2026-10-23 (#31)
4. Standby current measurement (#10)
5. The receiver's position and time hints (README, "The receiver's position and time
   hints"). This needs the server's `GET /api/v2/wifi-anchors` deployed and some learned
   access points.
   - After the first upload session, expect `[wifi] Wi-Fi anchors downloaded: N access
     points` and, on the next boot, `Wi-Fi anchors on the card: N`.
   - About 14 s into a boot indoors (no fix), expect `Wi-Fi hint sent: +/- ... m` with a
     `from the last saved fix` distance of metres to hundreds of metres. `Wi-Fi hint: not
     from this scan (...)` names the rule that refused the scan.
   - On the first fix, expect the `TTFF start=...` line.
   - The co-processor returns no UBX replies, so neither hint can be confirmed directly.
     One indirect check for the time hint: on the bench with no fix, does a
     `TIME_OBSERVATION` with source 1 (the receiver's own date and time) appear in the
     bundle soon after `time hint sent`? That would mean the receiver took the time,
     including the frame split over two `ATGDS` lines. Then compare `ttff` across drives with and without `wifi=sent`.
