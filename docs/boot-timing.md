# Boot timing

Boot speed and speed to upload are owner priorities (issue #19, umbrella
[#25](https://github.com/ParkWardRR/cairn-driving-log-selfhosted/issues/25)). This page
defines what is measured, what is instrumented, and what is **not yet measured**.

## Status

| Part of #19 | State |
|---|---|
| 1. Instrument: a monotonic boot-timing record, printed on the console | **Done** (`lib/cairn_boottime`, `src/boot_timing.*`, 29 host rows) |
| 1. ...exposed over BLE (device info) | **Not done.** The device-info contract is not released (upstream issue 21). The record already has a fixed, provisional wire layout so this is a small step once it is. |
| 2. Measure on the real unit | **Not done. Needs the dongle.** No numbers exist yet, and none are claimed here. |
| 3. Optimise the biggest terms | **Not started**, by design: it follows measurement. |
| Regression visible without hardware | **Mechanism done** (`cairn_boottime_check` against a budget); no budget is committed because there are no measurements to set one from. |

## Definitions

T0 is the first line of `setup()`. `pre_app_us` is the time the ROM and bootloader took
before that (`esp_timer` starts at reset).

| Stage | Meaning | Marked in |
|---|---|---|
| `app_start` | T0 | `setup()` |
| `log_ready` | UART + RAM log ring up | `setup()` |
| `sd_mounted` | the card mounted (first success, including a deferred retry) | `try_mount_sd()` |
| `store_ready` | NVS identity and counters loaded, interrupted seals and prunes recovered | `lifecycle_begin()` |
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
