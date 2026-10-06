# Boot timing

Boot speed and speed to upload are owner priorities (issue #19, umbrella
[#25](https://github.com/ParkWardRR/cairn-driving-log-selfhosted/issues/25)). This page
defines what is measured, what is instrumented, and what is **not yet measured**.

## Status

| Part of #19 | State |
|---|---|
| 1. Instrument: a monotonic boot-timing record, printed on the console | **Done** (`lib/cairn_boottime`, `src/boot_timing.*`, 29 host rows) |
| 1. ...exposed over BLE (device info) | **Not done.** The device-info contract is not released (upstream issue 21). The record already has a fixed, provisional wire layout so this is a small step once it is. |
| 2. Measure on the real unit | **First bench measurements done** (below): cold start from power-on, production image. Not yet done: wake from sleep, with no card, small and large card, the self-test image, anything on the car |
| 3. Optimise the biggest terms | **The biggest term is fixed** (below): 55 s to 8.9 s. The next terms are listed |
| Regression visible without hardware | **Mechanism done** (`cairn_boottime_check` against a budget). No budget is committed yet: one bench run on one unit is not enough to set one from |

## Definitions

T0 is the first line of `setup()`. `pre_app_us` is the time the ROM and bootloader took
before that (`esp_timer` starts at reset).

| Stage | Meaning | Marked in |
|---|---|---|
| `app_start` | T0 | `setup()` |
| `log_ready` | UART + RAM log ring up | `setup()` |
| `sd_mounted` | the card mounted (first success, including a deferred retry) | `try_mount_sd()` |
| `sensors_ready` | the coprocessor, GNSS and IMU brought up (`sensors_begin` returned) | `lifecycle_begin()` |
| `store_ready` | interrupted seals and prunes recovered (after `sensors_ready`) | `lifecycle_begin()` |
| `obd_first_answer` | first OBD snapshot decoded | `on_obd_snapshot()` |
| `gnss_first_fix` | first internal GNSS sample with a fix | `on_gnss_sample()` |
| `first_sample` | **T_capture**: a sample entered the pre-roll or the capture | `emit_capture_record()` |
| `ble_advertising` | **T_ble**: the phone can find the dongle | `lifecycle_begin()` |
| `first_chunk` | **T_uplink** (BLE path): first offload data notification sent | `io_notify()` |

The first mark of a stage wins, so "BLE advertising resumed" after a drive does not
overwrite the boot value. The record is logged once, when both `first_sample` and
`ble_advertising` are known, as `BOOTTIME` lines on the console and the SD log:

```
boot: reset=power_on pre_app=310000 us sd=yes
boot: app_start                0 us  (+0)
boot: log_ready             4000 us  (+4000)
...
```

The `(+n)` column is the delta from the previous stage, which is what shows the biggest
term.

Per the schedule in [#17](https://github.com/ParkWardRR/cairn-esp32-device-firmware/issues/17),
Wi-Fi never sits on the boot path: boot reaches capture and BLE first. Measure T_capture
and T_ble with the Wi-Fi code present but idle, so adding it cannot regress them. Time to
upload is measured from two starting points: the car stops, and the slot starts.

## To measure on hardware

For each of: cold start from ignition, wake from sleep, with and without the SD card, a
small and a large card, the self-test and production images:

1. Flash, open the monitor, power-cycle, copy the `BOOTTIME` lines.
2. Record the table here (before), optimise the largest term, record again (after).
3. Set `cairn_boot_budget_t` from the numbers and add a host row that feeds a recorded
   boot to `cairn_boottime_check`.

Safety properties are not negotiable for speed: the counter commit, torn-tail recovery
and the receipt gate stay, and the fault matrix must stay green.

## Measurements

Bench, 2026-10-06: one unit (Freematics ONE+ Model B, MAC `7c:9e:bd:fa:7f:f8`), powered from
USB, no car and no OBD, GNSS without a fix, a 15 GB card holding about 270 old log files and
20 sealed bundles, production image with the bench log level (TRACE to the card). Cold start
(power-on reset). The unit's ROM and bootloader take about 0.53 s before the application starts
(`pre_app`).

| Stage (time after app start) | Before | After |
|---|---|---|
| SD mounted | 0.54 s | 0.15 s |
| sensors ready | not recorded (about 5.7 s) | 7.8 s |
| capture open | 54.0 s | 8.1 s |
| BLE advertising (**T_ble**) | 54.9 s | 8.9 s |
| first sample (**T_capture**) | 54.9 s | 8.9 s |

"After" was the same on two consecutive boots (8.90 s and 8.92 s). "Before" is one boot of the
image built from the commit that was on `main`; the first sample was 54.9 s in that run and the
SD log attach alone took about 46.8 s. Add 0.53 s for the ROM and bootloader to get time from
power-on.

### What the 46 s was

Two defects, both found only by measuring on the unit:

1. **`File::size()` on a newly created file returns uninitialised memory** in the Arduino core
   (the stat buffer is only filled in once the file has been written). `open_next_file()` used
   it to start the rotation counter. On this unit it returned 1767990063, the ASCII bytes
   `/cai` of the path string, which is past the 2 MiB rotation limit, so the first log line
   rotated the file, the next file did the same, and so on. The value depends on the heap
   layout, so the image built earlier happened to get a small number and did not show it.
   Now the length is read with `seek(end)` and `position()`.
2. **The log budget scan was O(N squared).** `enforce_log_budget()` walked the directory with
   `openNextFile()`, which opens and stats every entry, and a FAT stat is itself a directory
   search: 267 files took 23 s per scan, once at boot and once per rotation, with the log lock
   held. It now makes one pass with FatFs `f_readdir`, which returns each size as it reads the
   directory (about 1 s for the same directory, most of it the card). The old walk is kept as a
   fallback if FatFs cannot open the directory.

The storage layer is not affected by the first one: its only `size()` call is on a file opened
for reading, which has a valid stat.

### The next terms

After the fix the time to first sample is about 8.9 s. Of that: sensors bring-up 7.7 s
(coprocessor about 2 s, GNSS about 2 s, IMU settling and bias about 2 s), SD log attach about
1.6 s (a one-pass directory read of the log directory), BLE start about 0.8 s. These are the
terms to look at next. Whether BLE and capture can start before the sensors finish is a
design question, not a measurement.
