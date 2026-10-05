# Freematics ONE+ Model B -- Emulation Reference

> Hardware reference. The Rust emulator no longer simulates drives (its legacy v1
> capture path was removed on 2026-10-05; it now only runs `conformance` and
> `fault-matrix`), so the values below are kept as a description of the real
> device and sensor behaviour, not as a simulator spec.

Sources: Freematics product pages, u-blox datasheets, TDK InvenSense docs,
Freematics firmware (github.com/stanleyhuangyc/Freematics), automotive research.

---

## 1. Hardware Summary

| Component | Part | Notes |
|-----------|------|-------|
| MCU | ESP32 dual-core (Xtensa LX6), 240 MHz | 16 MB Flash, 8 MB PSRAM, 520 KB IRAM |
| GNSS | u-blox M9 module (active ceramic antenna) | Firmware has M10 UBX config packets; both use same NMEA interface |
| IMU | TDK InvenSense ICM-42627 | 6-axis (accel + gyro), ICM-426xx family |
| Cellular | SIM7670 LTE CAT-1 | Deliberately unused in Cairn |
| OBD-II | STM32 coprocessor (ELM327-compatible AT cmds) | CAN 500/250 Kbps, KWP2000; Cairn uses power only |
| Storage | microSD card slot | SPI interface, CS=GPIO5, clock 1 MHz |
| Wi-Fi | 802.11 b/g/n | 2.4 GHz only |
| Bluetooth | Dual-mode (Classic + BLE) | |
| Enclosure | 60 x 48 x 20 mm | OBD-II male connector form factor |

---

## 2. Communication Interfaces

### UART Channels

| Bus | Baud Rate | Pins (RX/TX) | Purpose |
|-----|-----------|-------------|---------|
| GNSS | 38,400 | GPIO34/GPIO26 (alt: GPIO32/33) | NMEA sentences from u-blox module |
| Link (OBD coprocessor) | 115,200 | GPIO13/GPIO14 | AT commands and PID queries |
| Cellular (Bee) | 115,200 | GPIO35/GPIO2 | LTE modem (unused in Cairn) |

All UARTs: 8N1, no flow control. NMEA buffer: 512 bytes.

### I2C

| Device | Address | Clock | Notes |
|--------|---------|-------|-------|
| ICM-42627 IMU | 0x68 | 100 kHz (firmware default) | WHO_AM_I reg 0x75 = 0x20 |

### SPI

| Device | CS Pin | Clock | Notes |
|--------|--------|-------|-------|
| microSD | GPIO5 | 1 MHz | FAT32 filesystem |
| OBD Link SPI | GPIO2 (CS), GPIO13 (READY) | 1 MHz | Frame: `$OBD` header (0x24 0x4F 0x42 0x44) + AT cmd + 0x1B tail |

### Key GPIO

GPS power: GPIO12/GPIO15. Buzzer: GPIO25 (PWM). Cellular power: GPIO27.
LED: GPIO4. Molex connector: GPIO26 (out) + GPIO34 (input-only).

---

## 3. GNSS Module (u-blox M9/M10)

### Core Specifications

| Parameter | Value |
|-----------|-------|
| Constellations | GPS + Galileo + BeiDou B1 (default); GLONASS configurable |
| Channels | 72 (M10) / 92 (M9) concurrent |
| Max nav update rate | 5 Hz (firmware configures GGA at 5 Hz, RMC at 1 Hz) |
| Horizontal accuracy | 1.5 m CEP open sky; 2.5-5 m urban canyon |
| Velocity accuracy | 0.05 m/s |
| NMEA version | 4.11 (default) |

### Time-to-First-Fix

| Start Type | Time | Condition |
|------------|------|-----------|
| Cold start | 24-28 s | No almanac, no ephemeris, no position |
| Warm start | 1-2 s | Valid almanac, approximate position |
| Hot start | ~1 s | Valid ephemeris (<4 hours old), position known |

Backup battery (V_BCKP) must hold charge >4 hours for hot starts.

### Default NMEA Output Sentences

| Sentence | Content | Default Rate |
|----------|---------|-------------|
| GGA | Fix data, position, altitude, satellites, HDOP | Every fix (1-5 Hz) |
| RMC | Recommended minimum: position, velocity, date/time | 1 Hz |
| GSA | DOP and active satellites | 1 Hz |
| GSV | Satellites in view (azimuth, elevation, SNR) | 1 Hz |
| VTG | Course over ground and ground speed | 1 Hz |
| GLL | Geographic position (lat/lon) | 1 Hz |
| TXT | Text messages (firmware info, errors) | On event |

### No-Fix Behavior (Indoors / Garages)

When the module has no satellite fix:

```
$GPGGA,093112.00,,,,,0,00,99.99,,,,,,*64
$GPRMC,093112.00,V,,,,,,,250926,,,N*7A
$GPGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99*30
```

GGA fix quality = `0`, RMC status = `V` (void), lat/lon fields empty,
HDOP = 99.99, sat count = 0. Time field still present if previously acquired.
Module continues outputting at configured rate. Recovery: immediate outdoors
with warm start data; 24-28 s cold start if backup battery lost.

Firmware sends UBX-CFG-VALSET on init: GGA at 5 Hz, RMC at 1 Hz, NAV-PVT disabled.

---

## 4. IMU -- ICM-42627 (TDK InvenSense ICM-426xx Family)

### Sensor Specifications

**Accelerometer:**

| Range | Sensitivity (LSB/g) | Resolution |
|-------|---------------------|------------|
| +/-2 g | 16,384 | 0.061 mg/LSB |
| +/-4 g | 8,192 | 0.122 mg/LSB |
| +/-8 g | 4,096 | 0.244 mg/LSB |
| +/-16 g | 2,048 | 0.488 mg/LSB |

**Gyroscope:**

| Range | Sensitivity (LSB/deg/s) |
|-------|------------------------|
| +/-250 deg/s | 131.0 |
| +/-500 deg/s | 65.5 |
| +/-1000 deg/s | 32.8 |
| +/-2000 deg/s | 16.4 |

**General:**

| Parameter | Value |
|-----------|-------|
| Output format | 16-bit signed integers (two's complement) |
| Max ODR (accel & gyro) | 32 kHz (typical app: 25-100 Hz) |
| FIFO | 2 KB |
| Supply voltage | 1.71 - 3.6 V |
| Current (low-noise mode) | 0.65 mA |
| Accel noise density | ~70 ug/sqrt(Hz) |
| Gyro noise density | ~0.004 deg/s/sqrt(Hz) |
| Operating temp | -40 to +85 C |

### Register Map (from Freematics ICM_42627.h)

| Register | Address | Notes |
|----------|---------|-------|
| TEMP_OUT_H | 0x1D | Temperature high byte |
| ACCEL_XOUT_H | 0x1F | Accel data starts here (6 bytes: XH,XL,YH,YL,ZH,ZL) |
| GYRO_XOUT_H | 0x25 | Gyro data starts here (6 bytes) |
| INT_STATUS | 0x2D | Interrupt status |
| PWR_MGMT0 | 0x4E | Power management (accel/gyro mode select) |
| GYRO_CONFIG0 | 0x4F | Gyro full-scale and ODR |
| ACCEL_CONFIG0 | 0x50 | Accel full-scale and ODR |
| SELF_TEST_CONFIG | 0x70 | Self-test enable |
| WHO_AM_I | 0x75 | Device ID, reads 0x20 |

### Initialization Sequence (from firmware)

```
1. Read WHO_AM_I (0x75), expect 0x20
2. Write PWR_MGMT0 (0x4E) = 0x0F  (accel + gyro low-noise mode)
3. Wait 100 ms
4. Write ACCEL_CONFIG0 (0x50) = 0x66  (FS=+/-2g, ODR=1kHz)
5. Wait 100 ms
6. Write GYRO_CONFIG0 (0x4F) = 0x66  (FS=+/-250dps, ODR=1kHz)
7. Wait 100 ms
8. Write SELF_TEST_CONFIG (0x70) = 0x07
9. Wait 100 ms
```

### Recommended Ranges for Automotive

- Accelerometer: **+/-8 g** (captures speed bumps without clipping; +/-2g clips on bumps)
- Gyroscope: **+/-500 deg/s** (all normal driving within +/-250, +/-500 adds margin)

### Typical Driving Accelerometer Values (g)

| Scenario | X (longitudinal) | Y (lateral) | Z (vertical) |
|----------|:-:|:-:|:-:|
| Stationary / parked | ~0 | ~0 | ~1.0 (gravity) |
| Highway cruising | <0.05 | <0.05 | ~1.0 |
| Normal acceleration | 0.1 - 0.3 | <0.05 | ~1.0 |
| Normal braking | 0.2 - 0.4 | <0.05 | ~1.0 |
| Hard braking | 0.5 - 0.7 | <0.1 | ~1.0 |
| Emergency braking | 0.7 - 1.0 | <0.2 | ~1.0 |
| Normal turn | <0.1 | 0.1 - 0.3 | ~1.0 |
| Sharp turn | <0.2 | 0.3 - 0.5 | ~1.0 |
| Speed bump (30 km/h) | <0.1 | <0.1 | 2.0 - 3.0 spike |
| Pothole | <0.1 | <0.1 | 1.5 - 3.0 spike |

Fleet thresholds: >0.3g harsh braking, >0.25g harsh accel, >0.47g harsh cornering.

### Typical Driving Gyroscope Values (deg/s, yaw axis)

Lane change: <5. City turn: 5-15. Sharp turn: 10-20. U-turn: 20-50. Spin: 50-150+.

### Noise Floor (Emulation)

Add Gaussian noise: sigma ~3-5 mg for accel, ~0.01 deg/s for gyro.
At +/-2g range with 100 Hz bandwidth, RMS noise is ~4 mg (well below
driving signals of interest at ~100 mg).

---

## 5. OBD-II Power

Cairn uses OBD-II for power only (pin 16 = battery positive).
No diagnostic polling.

### Voltage Profile

| State | Voltage Range | Duration |
|-------|:---:|----------|
| Battery resting (100% SOC) | 12.6 V | Steady state, engine off |
| Battery resting (50% SOC) | 12.0 - 12.2 V | Steady state |
| Engine cranking | 9.6 - 10.5 V (min ~7 V worst case) | 1 - 5 seconds |
| Alternator charging (idle) | 13.0 - 13.5 V | Engine running, low RPM |
| Alternator charging (driving) | 13.5 - 14.5 V | Engine running, normal |
| Accessory mode | 12.0 - 12.6 V | Key in ACC, engine off |

### Engine State Detection

Voltage threshold with hysteresis: >13.2V = engine on, <12.8V = engine off
(0.4V band avoids flapping). Cairn uses voltage + IMU motion (no OBD polling).

### Cranking Voltage Profile (for emulation)

12.6V -> 11.0V (0.1s, starter engages) -> 9.8V (0.3s, cranking min) ->
10.5V (1.0s, cranking) -> 13.0V (1.5s, engine catches) -> 14.2V (3.0s, steady).

---

## 6. Wi-Fi (ESP32 802.11 b/g/n)

| Parameter | Value |
|-----------|-------|
| Frequency | 2.4 GHz only |
| Scan time (full 13 channels) | 1.0 - 1.5 s |
| Scan time (single known channel) | 100 - 200 ms |
| Connection time (scan + WPA2 + DHCP) | 3 - 7 s typical |
| Connection time (optimized, saved channel/IP) | 2 - 3 s |
| TCP throughput | 16 - 20 Mbps |
| HTTP upload round-trip (small payload) | 50 - 200 ms per request |
| HTTP upload (10 KB payload) | 100 - 300 ms |

A 30-minute trip at 1 Hz GNSS (~57 KB) uploads in <1 s of sustained
transfer. The bottleneck is connection establishment (3-7 s), not throughput.

---

## 7. Timing Characteristics

### Boot Sequence

| Phase | Duration | Cumulative |
|-------|:---:|:---:|
| ESP32 cold boot to app_main | 300 - 500 ms | ~0.5 s |
| Peripheral init (I2C, SPI, UART) | 50 - 100 ms | ~0.6 s |
| OBD coprocessor init (ATZ + config) | 2 - 5 s | ~4 s |
| GNSS module power-on + UART sync | 1 - 2 s | ~6 s |
| IMU init (I2C config registers) | 50 - 200 ms | ~6.2 s |
| Wi-Fi connect (if at home) | 3 - 7 s | ~12 s |
| **Total boot to data-ready** | | **~6 - 15 s** |

GNSS time-to-first-fix runs in parallel after module power-on
(24-28 s cold, 1-2 s warm/hot). Not a boot blocker.

### OBD Coprocessor Init (STM32, ELM327-compatible AT commands)

```
ATZ       ->  Reset, wait 1-2 s for banner      ATAT1  ->  Adaptive timing     (~50 ms)
ATE0      ->  Echo off              (~50 ms)     ATSTFF ->  Timeout max 1020 ms (~50 ms)
ATL0      ->  Linefeeds off         (~50 ms)     ATSP0  ->  Auto-detect protocol(~50 ms)
ATH1      ->  Headers on            (~50 ms)     ATRV   ->  Read voltage        (~100 ms)
ATS0      ->  Spaces off            (~50 ms)     0100   ->  Supported PIDs      (2-10 s)
```

Serial: 38,400 baud, 8N1, CR (0x0D) terminator, wait for `>` (0x3E)
prompt before next command. Cairn reads voltage only (ATRV), no PID queries.

### Sleep / Wake

Deep sleep wake (IMU motion interrupt): 10-50 ms. Light sleep: ~1 ms.
Full cold boot: 300-500 ms. Deep sleep current: ~10 mA (vendor claim).

---

## 8. Axis Orientation and Typical Trips

**Axis orientation** (device plugged into OBD-II port under dashboard):
X = longitudinal (+ forward), Y = lateral (+ left), Z = vertical (+ up, reads +1g at rest).

**Trip patterns** (Santa Monica / West LA): 10-30 min, 5-15 km, city 30-50 km/h
with stops every 1-3 min, highway 80-110 km/h. GNSS generally good; degrades
near overpasses and tall buildings on Wilshire corridor.

The on-wire encoding is specified in [bundle-format-v3.md](bundle-format-v3.md).
