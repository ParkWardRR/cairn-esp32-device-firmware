# Flashing and Testing — Freematics ONE+ Model B

## Hardware

- **Device:** Freematics ONE+ Model B (ESP32-D0WDQ6 rev v1.0)
- **MAC:** 7c:9e:bd:fa:7f:f8
- **Device type:** 15 (detected by OBD coprocessor `ATI` command)
- **Flash:** 16 MB, **PSRAM:** 8 MB
- **USB chip:** CH340 (QinHeng Electronics)
- **IMU:** ICM-42627 (I2C, address 0x68)

## Flash Station

- **Host:** Raspberry Pi (flash-station.example.lan)
- **OS:** Debian 13 (trixie), kernel 6.18, aarch64
- **Serial port:** `/dev/ttyUSB1` (CH340 — the Freematics)
- **Tool:** esptool v5.4.0 in `~/cairn-flash/venv/`

### Setup (one-time)

```bash
ssh alfa@flash-station.example.lan
mkdir -p ~/cairn-flash
python3 -m venv ~/cairn-flash/venv
~/cairn-flash/venv/bin/pip install esptool
```

### Build (on dev machine)

```bash
cd firmware/freematics-base
pio run -e freematics
```

Build artifacts:
- `.pio/build/freematics/bootloader.bin` (19 KB)
- `.pio/build/freematics/partitions.bin` (3 KB)
- `.pio/build/freematics/firmware.bin` (845 KB)
- `boot_app0.bin` from `~/.platformio/packages/framework-arduinoespressif32/tools/partitions/`

### Transfer

```bash
scp firmware/freematics-base/.pio/build/freematics/{bootloader,partitions,firmware}.bin \
    ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
    alfa@flash-station.example.lan:~/cairn-flash/
```

### Flash

```bash
cd ~/cairn-flash
~/cairn-flash/venv/bin/esptool \
  --chip esp32 --port /dev/ttyUSB1 --baud 460800 \
  --before default-reset --after hard-reset \
  write-flash -z --flash-mode dio --flash-freq 40m --flash-size 4MB \
  0x1000 bootloader.bin \
  0x8000 partitions.bin \
  0xe000 boot_app0.bin \
  0x10000 firmware.bin
```

Flash takes ~15 seconds at 460800 baud.

### Serial Monitor

```bash
screen /dev/ttyUSB1 115200
```

Exit screen: `Ctrl-A` then `\` then `y`.

### Reset device (without reflashing)

Toggle DTR/RTS via pyserial:
```bash
~/cairn-flash/venv/bin/python3 -c "
import serial, time
ser = serial.Serial('/dev/ttyUSB1', 115200)
ser.dtr = False; ser.rts = True; time.sleep(0.1)
ser.rts = False; time.sleep(0.1); ser.close()
"
```

---

## First Boot Results (2026-09-29)

Firmware: Cairn v0.2, flashed from commit `d5bf96d`.

### Serial Output

```
========================================
  Cairn v0.2 -- Offline Car Journal
  Freematics ONE+ Model B (ESP32)
========================================

CPU: 160 MHz  Flash: 16 MB
Heap: 304 KB
PSRAM: 8 MB

[SYS] Device type: 15
[OBD] ECU not responding (ignition off?)
[IMU] ICM-42627
[IMU] Bias: -0.91/-0.41/0.02 (91 samples)
[GNSS] OK (internal)
[SD] Card mount failed
[PWR] Battery: 3.8 V
[STATE] Initialized -> SLEEP
```

### Subsystem Status

| Subsystem | Result | Notes |
|-----------|--------|-------|
| ESP32 boot | OK | POWERON_RESET, SPI_FAST_FLASH_BOOT, DIO mode |
| PSRAM | OK | 8 MB detected and initialized |
| OBD coprocessor | OK | Device type 15 detected. ECU error expected — no car connected |
| ICM-42627 IMU | OK | WHO_AM_I verified, bias calibrated in 1s (91 samples) |
| GNSS | OK | Internal module initialized |
| SD card | FAIL | `f_mount failed: (3)` — no card inserted or unsupported format |
| Battery voltage | OK | 3.8V via analogRead(A0) (devType 15 > 12). Expected low on USB power |
| State machine | OK | Transitioned to SLEEP, entered standby (silent = waiting for motion) |

### Known Issues

1. **SD card not mounted** — insert a FAT32-formatted microSD card. The device
   cannot record trips without storage.
2. **OBD ECU not responding** — expected on bench. Will work when plugged into a
   car's OBD-II port.
3. **Battery reads 3.8V** — the analogRead(A0) path for devType 15 reads OBD
   port voltage. On USB power this is whatever the USB 5V regulates down to.
   In a car with ignition on, expect 13.5–14.5V.

### Next Steps

1. Insert a FAT32 microSD card and reflash/reset to verify SD init
2. Test GNSS fix acquisition (place near a window or outdoors)
3. Plug into a car's OBD-II port for full-system test (OBD + battery + trip recording)
4. Verify trip bundle files written to SD after a short drive
