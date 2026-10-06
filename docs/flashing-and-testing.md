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

Production Wi-Fi credentials and the mTLS client key are not compiled in: they
are provisioned into NVS over USB with `cairn-provision`
(see [device-provisioning.md](https://github.com/ParkWardRR/cairn-vehicle-server/blob/main/docs/device-provisioning.md)). The `CAIRN_WIFI_*`
defines in `secrets.h` are only a development fallback, active with
`-DCAIRN_COMPILED_WIFI_FALLBACK=1`. A missing or placeholder `secrets.h` fails
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

1. GNSS fix acquisition outdoors
2. Full car test: OBD + battery + trip recording
3. WiFi home sync — see [hardware-roundtrip.md](hardware-roundtrip.md)
4. Standby current measurement
5. Power-loss recovery
