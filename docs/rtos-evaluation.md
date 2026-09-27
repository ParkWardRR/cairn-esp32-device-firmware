# RTOS Evaluation — Zephyr and Apache NuttX vs. Arduino/ESP-IDF

> Status: **research only**, no action taken. Written in response to a question about
> replacing the Arduino/PlatformIO firmware stack (`firmware/freematics-base`,
> `firmware/hal`) with Zephyr RTOS or Apache NuttX. Conclusion: **not viable today** —
> see [Bottom line](#bottom-line). Revisit if upstream support changes or if a specific
> pain point with the current stack (not raised here) forces the question.

## Why this exists

Cairn's firmware targets the **Freematics ONE+ Model B**, built on a plain **ESP32**
(original dual-core Xtensa LX6 — not S2/S3/C3/C6). The current stack is the Arduino
framework on PlatformIO (`firmware/freematics-base/platformio.ini`), with a custom HAL
(`firmware/hal/`) that calls ESP-IDF APIs fairly directly for low-level control. Someone
asked whether switching to Zephyr or NuttX made sense. The answer depends entirely on
whether those RTOSes support the *specific* ESP32 peripherals the HAL relies on — this
doc is that peripheral-by-peripheral check.

## What the current HAL depends on

| HAL module | Capability | Why it matters |
| --- | --- | --- |
| `hal_ulp.h` | ULP coprocessor: custom assembly program in RTC slow memory, RTC I2C reads of the IMU during deep sleep, wake-on-motion via `esp_sleep_enable_ulp_wakeup()` | Drops parked-state current from ~10 mA to ~150 µA |
| `hal_sdmmc.h` | Native SDMMC host controller, 4-bit parallel DMA mode (not SPI-mode SD) | 4–8x faster trip bundle writes/reads than SPI |
| `hal_dual_core.h` | FreeRTOS tasks pinned to Core 0 (Wi-Fi/sync/NVS/TLS) vs Core 1 (sensor DMA/recording), with inter-core queues and mutexes | Wi-Fi latency never steals sensor sample timing, and vice versa |
| `hal_crypto.h` | Hardware SHA-256 for bundle hashing, hardware RNG for Ed25519 keypair generation | Bundle integrity and device identity |
| `hal_nvs.h` | Encrypted NVS for Wi-Fi credentials and the Ed25519 private key | Secrets at rest |
| `hal_power.h` | Dynamic CPU frequency scaling (240/160/80 MHz), brownout detection, calibrated ADC battery reads | Power budget per device state |
| `hal_wifi.h` | BSSID-locked (not just SSID) station association to the home AP | Rejects rogue APs spoofing the home SSID |

## Findings

### ULP coprocessor — hard blocker on both

**Zephyr**: Espressif's own [Zephyr support-status page](https://developer.espressif.com/software/zephyr-support-status/)
states plainly that the ULP "is not a full CPU and will not be supported in Zephyr for
ESP32 and ESP32-S2." This is a permanent design exclusion, confirmed by the ESP32
tracking issue [zephyrproject-rtos/zephyr#29394](https://github.com/zephyrproject-rtos/zephyr/issues/29394).
Zephyr's `samples/boards/espressif/ulp/lp_core` samples target the newer RISC-V LP-core
on S3/C6/C5 — an unrelated coprocessor design from plain ESP32's FSM-based ULP.

**NuttX**: Docs and source (`arch/xtensa/src/esp32/`) show LP-core support only for
ESP32-S2/S3/C6. No trace of the original ESP32's FSM ULP anywhere in NuttX — this looks
unattempted rather than merely undocumented.

**Impact**: the ~150 µA parked-state design has no path on either RTOS without a custom
ESP-IDF interop shim that bypasses the RTOS driver model — which mostly defeats the
point of migrating.

### Native SDMMC 4-bit DMA host

**Zephyr**: a real, merged driver — [`drivers/sdhc/sdhc_esp32.c`](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/sdhc/sdhc_esp32.c) —
supports 1- to 4-bit SDIO and currently targets **only the original ESP32**. FAT access
runs through Zephyr's standard `fs` subsystem on top. Effectively drop-in.

**NuttX**: native SDMMC exists only for **ESP32-S3** (`CONFIG_ESP32S3_SDMMC`). NuttX's
plain-ESP32 platform docs describe SD support as SPI-mode only; there's no sdmmc/sdio
source under the esp32 (non-S3) tree. Getting this on plain ESP32 under NuttX means
porting the S3 driver.

**Impact**: Zephyr is meaningfully ahead here for this exact chip.

### Dual-core pinning (Core 0 = Wi-Fi/sync, Core 1 = sensors)

**Zephyr**: `k_thread_cpu_pin()` is the pinning primitive, and hardware-validated
`CONFIG_SMP` for plain ESP32 landed via [PR #114570](https://github.com/zephyrproject-rtos/zephyr/pull/114570)
— but explicitly scoped to the **no-Wi-Fi case**; `WIFI_ESP32` still hard-depends on
`!SMP`. Espressif's status table (last checked 2025-09-20) lists SMP as not
supported/in progress.

**NuttX**: generic SMP APIs exist (`sched_setaffinity()`, `pthread_setaffinity_np()`),
and ESP32 board support exists for SMP testing, but the [platform docs](https://nuttx.apache.org/docs/latest/platforms/xtensa/esp32/index.html)
say it is "still not yet ready for usage."

**Impact**: neither RTOS can currently do SMP + Wi-Fi at once on plain ESP32 — which is
exactly Cairn's Core0/Core1 split.

### Crypto/RNG, dynamic frequency scaling, deep sleep

Both expose basic HW SHA/RNG primitives, but PSA/mbedTLS hardware-accelerated
integration is immature on both: Zephyr has an open, unresolved build issue for PSA
hash on ESP32 ([discussion #60738](https://github.com/zephyrproject-rtos/zephyr/discussions/60738));
NuttX has an open architectural issue on managing HW crypto modules generally
([apache/nuttx#9314](https://github.com/apache/nuttx/issues/9314)). Espressif's table
marks **cryptography and dynamic frequency scaling as unsupported** for ESP32 on Zephyr.
Deep sleep has official Zephyr samples but is flagged only "in progress"; no confirmed
API on either RTOS for arbitrary RTC-memory retention across deep sleep equivalent to
IDF's `RTC_DATA_ATTR` — unconfirmed either way, flagged rather than guessed.

### BSSID-locked Wi-Fi association

**NuttX** has this merged directly: [apache/nuttx#3793](https://github.com/apache/nuttx/pull/3793),
"esp32&esp32c3/wifi: Support specific channel and bssid scan." No Zephyr-specific
equivalent was found. NuttX has the edge here.

### Is either port a thin wrapper over ESP-IDF?

No, on either. Zephyr's `hal_espressif` is a modified/forked slice of the IDF HAL, not
the full IDF — non-portable features like ULP are explicitly excluded from porting.
NuttX reimplements drivers per chip; the same feature (e.g. SDMMC) existing on S3 but not
plain ESP32 shows piecemeal reimplementation, not a wrapper that grants free access to
IDF APIs from application code.

## Bottom line

Neither Zephyr nor NuttX is close to feature parity with Cairn's custom HAL for the
original ESP32. Zephyr is ahead on native SDMMC; NuttX is ahead on POSIX-native
philosophy and BSSID-locked Wi-Fi; both explicitly lack or have unready SMP+Wi-Fi, ULP,
and hardware-accelerated crypto for this specific chip. **ULP support is the hard wall
on both** — losing it means either a real regression in parked-state power draw, or
maintaining a custom ESP-IDF interop shim that undercuts the reason to migrate at all.

**Recommendation**: stay on Arduino/ESP-IDF via PlatformIO. Revisit this doc if:
- Espressif or the Zephyr/NuttX communities land ULP (FSM variant) or SMP+Wi-Fi support
  for plain ESP32, or
- A concrete problem with the current Arduino/PlatformIO stack (build reproducibility,
  library rot, etc.) makes the migration cost worth paying regardless of the gaps above.

*Research compiled 2026-09-26 from public docs, GitHub issues/PRs, and vendor status
pages current as of that date — recheck links before treating any "not yet supported"
finding as still true, since both projects move quickly.*
