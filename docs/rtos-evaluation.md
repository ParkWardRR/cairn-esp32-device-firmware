# RTOS Evaluation — Zephyr and Apache NuttX vs. Arduino/ESP-IDF

> Status: **research only**, no action taken. Written in response to a question about
> replacing the Arduino/PlatformIO firmware stack (then `firmware/cairn-v2/third_party/freematics-base`
> and `firmware/hal`, both since removed; the current firmware is
> `firmware/cairn-v2`, still Arduino-on-ESP-IDF via PlatformIO) with Zephyr RTOS or
> Apache NuttX. Conclusion: **not viable today** —
> see [Bottom line](#bottom-line). Revisit if upstream support changes or if a specific
> pain point with the current stack (not raised here) forces the question.
>
> Revised 2026-09-27 after a second-pass fact-check: several claims below were tightened
> from absolute "does not exist" statements to "not documented/supported in current
> platform docs," the NuttX crypto claim was corrected (it does document AES/RNG/SHA
> support), and the deep-sleep/RTC-retention paragraph was narrowed to the actual
> blocker (classic FSM-ULP *execution*, not RTC memory retention in general). The
> strategic recommendation is unchanged.

## Why this exists

Cairn's firmware targets the **Freematics ONE+ Model B**, built on a plain **ESP32**
(original dual-core Xtensa LX6 — not S2/S3/C3/C6). The current stack is the Arduino
framework on PlatformIO (at the time `firmware/cairn-v2/third_party/freematics-base/platformio.ini`;
now `firmware/cairn-v2/platformio.ini`), with a custom HAL (`firmware/hal/`, since removed) that calls ESP-IDF APIs fairly directly for low-level control. Someone
asked whether switching to Zephyr or NuttX made sense. The answer depends entirely on
whether those RTOSes support the *specific* ESP32 peripherals the HAL relies on — this
doc is that peripheral-by-peripheral check.

## What the current HAL depends on

| HAL module | Capability | Why it matters |
| --- | --- | --- |
| `hal_ulp.h` | ULP coprocessor: custom assembly program in RTC slow memory, RTC I2C reads of the IMU during deep sleep, wake-on-motion via `esp_sleep_enable_ulp_wakeup()` | Design intent per the HAL header comment (not a measured Cairn benchmark): drop parked-state current from ~10 mA to ~150 µA |
| `hal_sdmmc.h` | Native SDMMC host controller, 4-bit parallel DMA mode (not SPI-mode SD) | Design intent per the HAL header comment (not a measured Cairn benchmark): materially faster trip bundle writes/reads than SPI-mode SD |
| `hal_dual_core.h` | FreeRTOS tasks pinned to Core 0 (Wi-Fi/sync/NVS/TLS) vs Core 1 (sensor DMA/recording), with inter-core queues and mutexes | Wi-Fi latency never steals sensor sample timing, and vice versa |
| `hal_crypto.h` | Hardware SHA-256 for bundle hashing; hardware RNG supplies entropy for Ed25519 private-key seed generation (Ed25519 key derivation/signing itself is software arithmetic on this chip) | Bundle integrity and device identity |
| `hal_nvs.h` | ESP-IDF NVS with flash-encryption-backed storage for Wi-Fi credentials and the Ed25519 private key | Secrets at rest |
| `hal_power.h` | Dynamic CPU frequency scaling (240/160/80 MHz), brownout detection, calibrated ADC battery reads | Power budget per device state |
| `hal_wifi.h` | BSSID-locked (not just SSID) station association to the home AP | Rejects rogue APs spoofing the home SSID |

## Findings

### ULP coprocessor — hard blocker on both

**Zephyr**: Espressif's own [Zephyr support-status page](https://developer.espressif.com/software/zephyr-support-status/)
states plainly that the ULP "is not a full CPU and will not be supported in Zephyr for
ESP32 and ESP32-S2." This is a permanent design exclusion, confirmed by the ESP32
tracking issue [zephyrproject-rtos/zephyr#29394](https://github.com/zephyrproject-rtos/zephyr/issues/29394).
Zephyr's `samples/boards/espressif/ulp/lp_core` samples target the newer RISC-V LP-core
on S3/C6/C5 — an unrelated coprocessor design from plain ESP32's FSM-based ULP. ESP-IDF
itself documents the original ESP32 ULP as a fixed-width-instruction FSM-style
coprocessor with its own assembly integration — distinct from the RISC-V LP-core on
newer chips ([ESP-IDF ULP docs](https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32/api-reference/system/ulp.html)).

**NuttX**: docs and source (`arch/xtensa/src/esp32/`) show LP-core support only for
ESP32-S2/S3/C6. NuttX's classic-ESP32 platform documentation does not document a
supported classic FSM-ULP programming/load/wake workflow — its documented ULP support
is for the newer RISC-V LP-core devices. (NuttX does reserve RTC slow-memory space that
the ULP would occupy, which is not the same as documenting execution support; treat the
absence here as "not documented for original ESP32," not a verified source-tree audit
against a specific commit.)

**Impact**: the parked-state low-power design (intended ~150 µA, see table above) has
no documented path on either RTOS without a custom ESP-IDF interop shim that bypasses
the RTOS driver model — which mostly defeats the point of migrating.

### Native SDMMC 4-bit DMA host

**Zephyr**: a real, merged driver — [`drivers/sdhc/sdhc_esp32.c`](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/sdhc/sdhc_esp32.c) —
supports 1- to 4-bit SDIO and currently targets **only the original ESP32**, with DMA
descriptors configured internally. FAT access runs through Zephyr's standard `fs`
subsystem on top. This makes the SDMMC requirement *feasible* — the essential native
hardware driver exists — but it is not source-level drop-in relative to the current
ESP-IDF HAL: devicetree/pin wiring, disk/SDMMC integration, filesystem configuration,
mount lifecycle, and buffering/error-retry behavior would all need to be redone and
validated against real hardware.

**NuttX**: its published ESP32 platform support table lists SD/MMC as "SPI based SD
card driver" and SDIO as unsupported — i.e. for the original ESP32, NuttX documents
SPI-mode SD, not native 4-bit SDMMC/SDIO. Native SDMMC is documented for ESP32-S3
(`CONFIG_ESP32S3_SDMMC`) but not for the original chip. Getting native SDMMC on plain
ESP32 under NuttX would mean porting/adapting the S3 driver.

**Impact**: Zephyr is meaningfully ahead here for this exact chip — it has a documented
native driver where NuttX only documents the SPI fallback — but "ahead" means "has a
starting point," not "no integration work required."

### Dual-core pinning (Core 0 = Wi-Fi/sync, Core 1 = sensors)

**Zephyr**: `k_thread_cpu_pin()` is the pinning primitive, and hardware-validated
`CONFIG_SMP` for plain ESP32 landed via [PR #114570](https://github.com/zephyrproject-rtos/zephyr/pull/114570)
— but explicitly scoped to the **no-Wi-Fi case**; `WIFI_ESP32` still hard-depends on
`!SMP` in current Zephyr configuration material ([reference](https://stackoverflow.com/questions/76253779/enable-wifi-drivers-for-esp32-board-in-zephyr)).
Espressif's status page (last checked 2025-09-20) calls SMP "currently non-functional"
for ESP32, with additional Bluetooth limitations noted.

**NuttX**: generic SMP APIs exist (`sched_setaffinity()`, `pthread_setaffinity_np()`),
and ESP32 board support exists for SMP testing, but the [platform docs](https://nuttx.apache.org/docs/latest/platforms/xtensa/esp32/index.html)
say it is "still not yet ready for usage."

**Impact**: neither RTOS can currently do SMP + Wi-Fi at once on plain ESP32 — which is
exactly Cairn's Core0/Core1 split.

### Crypto/RNG, dynamic frequency scaling, deep sleep

This is not symmetric between the two RTOSes — an earlier pass of this doc overstated
the NuttX side by lumping it in with Zephyr's "unsupported" crypto claim. Corrected below.

**Zephyr**: Espressif's own status material marks cryptography and dynamic frequency
scaling as **unsupported** for ESP32, with crypto operations handled in software (flash
encryption is the one exception). Zephyr does provide an entropy/RNG source and
software crypto stacks, but there's an open, unresolved build issue for PSA hardware
hash on ESP32 ([discussion #60738](https://github.com/zephyrproject-rtos/zephyr/discussions/60738)) —
general hardware crypto acceleration is not classified as supported.

**NuttX**: its ESP32 platform table documents native **AES, RNG, and SHA (including
HMAC-SHA1/256 and PBKDF2) as supported**. That's real platform-level support. It does
*not* by itself prove a production-ready, application-level hardware-acceleration path
for Cairn's exact stack (Ed25519 signing + SHA-256 bundle hashing), and NuttX has a
separate open architectural issue on how HW crypto modules should be managed generally
([apache/nuttx#9314](https://github.com/apache/nuttx/issues/9314)) — but "NuttX lacks
hardware crypto" is not a safely established claim, and an earlier version of this doc
was wrong to imply that.

**Deep sleep / RTC retention — narrower gap than previously stated**: Zephyr has
official deep-sleep samples, and RTC RAM retention across deep sleep is possible via a
maintainer-documented macro per the ESP32 tracking issue
([zephyrproject-rtos/zephyr#29394](https://github.com/zephyrproject-rtos/zephyr/issues/29394)) —
so "no confirmed API for RTC-memory retention" is too pessimistic. The real gap is
narrower: see [the ULP-vs-deep-sleep nuance](#the-important-nuance-zephyrs-blocker-is-ulp-not-deep-sleep-alone)
below. Zephyr's deep sleep is a reboot-style flow rather than a FreeRTOS/IDF-style
continuation, and — separately from RTC retention — it has no route to *execute* the
classic FSM-ULP program that reads the IMU and sets the wake flag while the main cores
are off.

### BSSID-locked Wi-Fi association — unverified, not confirmed

**NuttX** has a merged PR in this area: [apache/nuttx#3793](https://github.com/apache/nuttx/pull/3793),
"esp32&esp32c3/wifi: Support specific channel and bssid scan." No Zephyr-specific
equivalent was found, so NuttX likely has the edge here — but the PR title describes
*scan* filtering/hinting by BSSID and channel, which is not the same claim as "station
association is forcibly pinned to a BSSID and rejects same-SSID APs with a different
BSSID" (Cairn's actual security requirement: reject a rogue AP spoofing the home SSID).
This needs verification against the final station-connect API semantics and a test
against a real same-SSID/different-BSSID AP before being relied on.

### Is either port a thin wrapper over ESP-IDF?

No, on either. Zephyr's `hal_espressif` is a modified/forked slice of the IDF HAL, not
the full IDF — non-portable features like ULP are explicitly excluded from porting.
NuttX reimplements drivers per chip; the same feature (e.g. SDMMC) existing on S3 but not
plain ESP32 shows piecemeal reimplementation, not a wrapper that grants free access to
IDF APIs from application code.

## The important nuance: Zephyr's blocker is ULP, not deep sleep alone

Someone could push back on the ULP blocker by pointing to Zephyr's deep-sleep samples or
RTC retention support. Those don't refute the conclusion — they answer a different
question. Distinguishing the concepts:

| Capability | Zephyr classic-ESP32 status | Cairn relevance |
| --- | --- | --- |
| Enter deep sleep / reboot on wake | Available in some form; support has evolved | Not enough by itself |
| Retain a small amount of RTC memory across deep sleep | Possible per maintainer discussion, but not a full IDF-equivalent (`RTC_DATA_ATTR`-style) model | Useful but insufficient |
| Run classic FSM-ULP code while main cores sleep | Explicitly excluded ("will not be supported") | **Critical blocker** |
| ULP RTC-I2C IMU reads and ULP-driven wake-on-motion | No supported Zephyr route (depends on the above) | **Critical blocker** |
| Replicate the existing IDF ULP firmware via an interop fork/shim | Technically imaginable, not supported by Zephyr's architecture | Likely erases the migration's value |

Cairn's parked-state design needs the third and fourth rows, not just the first two.
NuttX's position is the same in substance: no documented classic-FSM-ULP execution path,
regardless of RTC memory being reserved/addressable.

## Bottom line

Neither Zephyr nor NuttX is close to feature parity with Cairn's custom HAL for the
original ESP32, but the gaps are not symmetric or equally severe:

- **Zephyr** has a real native SDMMC driver for this exact chip (an advantage), but
  explicitly excludes classic FSM-ULP support and currently ships Wi-Fi and SMP as
  mutually exclusive (`WIFI_ESP32` depends on `!SMP`).
- **NuttX** documents native AES/RNG/SHA support (an earlier version of this doc
  incorrectly said both RTOSes lack hardware crypto — corrected above) and has Wi-Fi
  BSSID/channel scan support worth following up on, but documents SD/MMC as SPI-only
  for the original ESP32, and its own docs call ESP32 SMP "not yet ready for usage."
- **Both** lack a documented, supported way to run the classic FSM-ULP program that
  Cairn's parked-state design depends on. This is the decisive issue, not ordinary
  driver availability — see the nuance table above.

**Recommendation: remain on Arduino/ESP-IDF for the original ESP32 target.** A Zephyr or
NuttX migration would require rewriting substantial HAL functionality, but the decisive
blocker is that Cairn relies on the classic FSM-ULP to do IMU-related work and generate
wake events while the main LX6 cores are in deep sleep, and neither RTOS supports that
for this chip. Even where one RTOS is ahead on a given peripheral (Zephyr's SDMMC driver,
NuttX's crypto/Wi-Fi support), neither offers a production-ready path for the intended
dual-core Wi-Fi/sensor partitioning. Switching would either regress the parked-state
power architecture or require retaining a bespoke ESP-IDF-derived ULP subsystem
alongside the new RTOS — a hybrid that keeps the most non-portable part of the firmware
while adding migration risk and maintenance cost.

Revisit this doc if:
- Espressif or the Zephyr/NuttX communities land classic FSM-ULP or SMP+Wi-Fi support
  for plain ESP32, or
- A concrete problem with the current Arduino/PlatformIO stack (build reproducibility,
  library rot, etc.) makes the migration cost worth paying regardless of the gaps above.

*Research compiled 2026-09-26, fact-checked and revised 2026-09-27, from public docs,
GitHub issues/PRs, and vendor status pages current as of those dates — recheck links
before treating any "not yet supported" finding as still true, since both projects move
quickly. Several claims above are marked unverified/needs-testing rather than confirmed;
treat those as open questions, not settled facts.*
