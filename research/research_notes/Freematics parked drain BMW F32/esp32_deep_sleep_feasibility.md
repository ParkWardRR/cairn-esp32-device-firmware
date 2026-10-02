# ESP32 Deep Sleep + Wake-on-Motion Feasibility on Freematics ONE+ Model B (classic ESP32-D0WDQ6 / WROVER, ICM-42627 with no routed INT pin)

Scope note enforced throughout: every ESP32 claim below is for the **original/classic ESP32** (ESP32-D0WDQ6, Xtensa dual-core, **FSM ULP**). ESP32-S2/S3 facts are called out explicitly where they appear, because the S2/S3 have a **RISC-V ULP** and a *different* RTC_I2C pin map, and conflating them is the single most common source of bad advice on this topic.

---

## Q1. Which ESP32 GPIOs are RTC-capable, and are any of the Freematics board's free pins usable for ext0/ext1?

### Takeaway
On classic ESP32 the RTC GPIO set is **0, 2, 4, 12–15, 25–27, 32–39**. Cross-referencing the Freematics ONE+ Model B pin map, essentially every RTC-capable pin is already consumed by the vendor design except **GPIO36 (SENSOR_VP)** and **GPIO39 (SENSOR_VN)** — both RTC-capable, both input-only with no internal pull resistors, and both are the natural (only) targets for a hand-wired IMU interrupt. GPIO37/38 are RTC-capable on the die but are **not bonded out on WROOM/WROVER modules**, so they are unavailable.

### Cited Findings
- "Supported RTC GPIO pins for ESP32: **0, 2, 4, 12-15, 25-27, 32-39**" — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
- Search-result corroboration of the same list: "For ESP32 RTC GPIOs: 0, 2, 4, 12-15, 25-27, 32-39; for ESP32-S3: 0-21; and for ESP32-S2: 0-21." — [Random Nerd Tutorials, ESP32 External Wake Up from Deep Sleep](https://randomnerdtutorials.com/esp32-external-wake-up-deep-sleep/)
- ext0: "The RTC IO module contains the logic to trigger wakeup when one of RTC GPIOs is set to a predefined logic level." `esp_sleep_enable_ext0_wakeup(GPIO_NUM_X, level)`; only RTC GPIOs allowed — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
- ext1: `esp_sleep_enable_ext1_wakeup(bitmask, mode)` with only two modes — `ESP_EXT1_WAKEUP_ALL_LOW` (wake when **all** listed GPIOs are low) or `ESP_EXT1_WAKEUP_ANY_HIGH` (wake if **any** goes high). With ext1, "RTC peripherals and RTC memories can be powered off." — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html); [Random Nerd Tutorials](https://randomnerdtutorials.com/esp32-external-wake-up-deep-sleep/)
- Mutual-exclusion constraint that bites if you try to combine approaches: "On ESP32, ULP wakeup source cannot be used when RTC_PERIPH power domain is forced to be powered on (ESP_PD_OPTION_ON) **or when ext0 wakeup source is used**." — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
- RTC_GPIO numbering examples: GPIO0 = RTC_GPIO11, GPIO2 = RTC_GPIO12, GPIO4 = RTC_GPIO10; GPIO1 and GPIO3 have **no** RTC_GPIO designation on classic ESP32 — [ESP-IDF GPIO & RTC GPIO](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/api-reference/peripherals/gpio.html); [wardjm/esp32-ulp-i2c](https://github.com/wardjm/esp32-ulp-i2c)
- The ICM-426xx interrupt electrical behaviour is reconfigurable, which matters for matching ext0/ext1: "By default, INT1 and INT2 interrupts are **pulsed (auto-clearing), open-drain (pullup resistors required), and active low** output. These settings are all reconfigurable in software. See the INT_SOURCE{x} and INT_CONFIG{x} registers." — [TDK AN-000173, ICM-426xx APEX Motion Functions, p.5](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- Any APEX interrupt can be routed to **either** INT1 or INT2 via INT_SOURCE6/INT_SOURCE7 (and INT_SOURCE4 for WoM/SMD) — [TDK AN-000173, pp.5, 21, 23](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)

### Pin-by-pin cross-reference (RTC-capable pins vs. the vendor header)

| GPIO | RTC-capable | Freematics ONE+ Model B use | Free for an IMU INT? |
|---|---|---|---|
| 0 | yes (RTC_GPIO11) | not in header, but **boot strapping pin** (and ULP-I2C SDA sel-0) | hostile — holding it low at reset enters download mode |
| 2 | yes (RTC_GPIO12) | LINK_SPI_CS, BEE_UART(2) | taken (also a strapping pin) |
| 4 | yes (RTC_GPIO10) | LED | taken |
| 12 | yes | GPS_POWER, MOLEX_VCC | taken — **and** a WROVER flash-voltage strapping pin (see Q5) |
| 13 | yes | LINK_SPI_READY, LINK_UART_RX | taken |
| 14 | yes | LINK_UART_TX | taken |
| 15 | yes | LINK_RESET, GPS_POWER2 | taken (also strapping) |
| 25 | yes | BUZZER | taken |
| 26 | yes | GPS_UART, MOLEX_4 | taken |
| 27 | yes | BEE_PWR | taken |
| 32 | yes | GPS_UART | taken |
| 33 | yes | GPS_UART | taken |
| 34 | yes (input-only) | GPS_UART, MOLEX_2 | taken |
| 35 | yes (input-only) | BEE_UART | taken |
| **36** | **yes (input-only, SENSOR_VP)** | **not in header** | **candidate** |
| 37 | yes (input-only) | n/a | **not bonded out on WROOM/WROVER modules** |
| 38 | yes (input-only) | n/a | **not bonded out on WROOM/WROVER modules** |
| **39** | **yes (input-only, SENSOR_VN)** | **not in header** | **candidate** |

### Inferences
- **GPIO36 and GPIO39 are the only realistic hand-wire targets.** They are the only RTC-capable pins absent from the vendor header, and they are the two input-only pins that WROVER actually exposes.
- Because 36/39 are **input-only and have no internal pull-up/pull-down**, you cannot rely on `rtc_gpio_pullup_en()`. The cleanest configuration is: reconfigure the ICM interrupt to **push-pull, active-high, latched** (all three are software-configurable per AN-000173 p.5), route WoM/SMD to that pin, and use **ext1 with `ESP_EXT1_WAKEUP_ANY_HIGH`**. That combination also lets you power down RTC_PERIPH, which ext0 cannot do, and sidesteps the "ULP wakeup cannot coexist with ext0" restriction if you later want a ULP timer alongside it.
- The ICM's **default pulsed/auto-clearing** interrupt is wrong for ext0/ext1, which are **level**-triggered, not edge-triggered. A short pulse that fires while the SoC is mid-sleep-entry can be missed entirely. Latched mode is mandatory, with an I2C status-register read after wake to deassert.
- Any of these options requires **physically modifying the board** (a fly-wire from the ICM-42627 INT1/INT2 ball/pad to the WROVER SENSOR_VP/VN pad). On a production ONE+ this is fine-pitch rework on a module in a potted/enclosed assembly.

### Gaps
- Whether GPIO36/GPIO39 are actually **accessible** on the Freematics ONE+ Model B PCB (exposed pad, test point, or buried under the module) — not determinable without the board schematic, which Freematics does not publish; the vendor header only tells you what is *assigned*, not what is *routed*.
- Whether the ICM-42627's INT1/INT2 pads are physically accessible for rework (package is LGA; INT pins may be on inner balls) — needs the ICM-42627 package drawing, which I could not locate (see Q4 gaps).
- The ESP32 TRM's own RTC GPIO table was not fetched directly; the pin list above comes from ESP-IDF docs (primary, Espressif) plus a tutorial corroboration, and they agree.

---

## Q2. Can the classic ESP32 FSM ULP drive I2C to poll the accelerometer during deep sleep?

### Takeaway
Yes in principle — classic ESP32 has an RTC_I2C peripheral with dedicated `I2C_RD`/`I2C_WR` ULP instructions, and the ULP can run a threshold compare and wake the SoC. But it is **hard-wired to only four possible pins (GPIO0/GPIO4 or GPIO2/GPIO15), all four of which are already used or are strapping pins on this board**, and ESP-IDF provides no supported ULP-I2C driver for ESP32 (the `ulp_riscv_i2c` component is S2/S3-only). This path is a non-starter on the ONE+ without a full I2C re-route.

### Cited Findings
- The ULP FSM instruction set does include I2C: `I2C_RD Sub_addr, High, Low, Slave_sel` reads one byte from an I2C slave into R0; `I2C_WR Sub_addr, Value, High, Low, Slave_sel` writes one byte. Slave addresses "must be set in advance into `SENS_I2C_SLAVE_ADDRx` register field, where `x == Slave_sel`" in 7-bit format. — [ESP-IDF ULP FSM instruction set (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ulp_instruction_set.html)
- **RTC_I2C pin options on classic ESP32 are fixed to two selections**: Selection 0 → **SCL = GPIO4 (RTC_GPIO10), SDA = GPIO0 (RTC_GPIO11)**; Selection 1 → **SCL = GPIO2 (RTC_GPIO12), SDA = GPIO15 (RTC_GPIO13)**. Selected via `RTC_IO_SAR_I2C_SDA_SEL` / `RTC_IO_SAR_I2C_SCL_SEL` in `RTC_IO_SAR_I2C_IO_REG`; slave address via `SENS_SAR_SLAVE_ADDR1_REG` (`SENS_I2C_SLAVE_ADDR1_S/_M`). — [wardjm/esp32-ulp-i2c](https://github.com/wardjm/esp32-ulp-i2c)
- Configuration state "is lost when migrating down to low power," so the RTC_I2C setup should be written redundantly from both the main CPU and inside the ULP program. — [wardjm/esp32-ulp-i2c](https://github.com/wardjm/esp32-ulp-i2c)
- "During Deep Sleep, when the ULP coprocessor is powered on, peripherals such as GPIO and **RTC I2C are able to operate**." — ESP32 datasheet power-mode description, as reproduced in [Lucidar: Power consumption of ESP32](https://lucidar.me/en/esp32/power-consumption-of-esp32/) search summary
- ULP wakeup is an official sleep wake source: "ULP coprocessor can run while the chip is in sleep mode, and may be used to poll sensors, monitor ADC or GPIO states, and wake up the chip when a specific event is detected." — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
- **Contrast with S2/S3 (do not conflate):** Espressif's Rust HAL documents RTC_I2C SDA/SCL on **GPIO1 and GPIO3** — but that page is the **esp32s3** target, not esp32. — [esp-hal 1.1.1 esp32s3 i2c::rtc::Sda](https://docs.espressif.com/projects/rust/esp-hal/1.1.1/esp32s3/esp_hal/i2c/rtc/trait.Sda.html)

### Inferences
- **Pin conflict is fatal on this board.** The four RTC_I2C-capable pins map to: GPIO4 = LED, GPIO0 = boot strap, GPIO2 = LINK_SPI_CS, GPIO15 = LINK_RESET / GPS_POWER2. To use ULP I2C you would have to cut the ICM-42627 off the main ESP32 I2C bus and re-route SDA/SCL onto one of these pairs, simultaneously giving up either the LED + bootloader entry, or the STM32 link SPI chip-select + link reset. Giving up LINK_RESET/LINK_SPI_CS breaks the board's core OBD function.
- Even if you re-routed, the ICM would then be reachable **only** via RTC_I2C — the ULP I2C pins are RTC-domain-muxed, and sharing them with the main-CPU I2C controller through the GPIO matrix while alternating masters is possible but is bespoke, fragile plumbing.
- ULP FSM can absolutely do the threshold compare and wake (`I2C_RD` into R0, `JUMPR`/`JUMPS` compare, `WAKE` + `HALT`) — the compute side is not the problem; the I/O side is.

### Gaps
- **ULP program / RTC SLOW memory size limit was not confirmed from a fetched source.** The ESP-IDF ULP FSM page I retrieved did not state it. Do not quote a number without re-checking `CONFIG_ULP_COPROC_RESERVE_MEM` and the RTC_SLOW_MEM size in the ESP32 TRM.
- Maximum RTC_I2C SCL clock rate and whether external pull-ups are needed on the RTC_I2C pins — not covered by the sources fetched.
- Whether ESP-IDF ships *any* ESP32-target ULP I2C example — I found none; the only official I2C-from-ULP component (`ulp_riscv_i2c`) is RISC-V ULP, i.e. S2/S3 only. Treating absence of evidence carefully: I did not find an explicit Espressif statement "ULP I2C is unsupported on ESP32," only the absence of a driver/example.

---

## Q3. Is RTC_I2C on the classic ESP32 usable in practice? Known bugs and community consensus.

### Takeaway
Community consensus is **no, not reliably**: the ESP32 RTC_I2C hardware is reported to sample SDA on the wrong clock edge, and the peripheral is described as over-simplified to the point of limited usefulness. The community workaround is to **abandon the hardware RTC_I2C and bit-bang software I2C from ULP FSM code using RTC GPIOs** — which conveniently removes the GPIO0/2/4/15 constraint but costs ULP instruction space and speed.

### Cited Findings
- "The I2C hardware in the ULP **samples at the wrong edge**. Either using a delay element or software I2C is a possible solution." — esp32.com thread *RTC_I2C SDA sampling*, summarized via search: [esp32.com p=58795](https://esp32.com/viewtopic.php?p=58795) / [p=20080](https://esp32.com/viewtopic.php?p=20080)
- "The RTC-I2C peripheral has a hardware bug with required subregisters, and **the ULP I2C module has been simplified just a tad too much to make it really useful**." — same thread: [esp32.com p=58795](https://esp32.com/viewtopic.php?p=58795)
- "The community has created alternative implementations, such as [tomtor/ulp-i2c](https://github.com/tomtor/ulp-i2c), which provides a working solution using **software I2C** instead of the hardware I2C implementation." — search synthesis over [esp32.com p=58795](https://esp32.com/viewtopic.php?p=58795)
- A second, independent working-example repo exists for the hardware path: [wardjm/esp32-ulp-i2c](https://github.com/wardjm/esp32-ulp-i2c), which documents the register setup but notes "much has to be set up ahead of time" and that low-power transitions lose state.
- Separate but adjacent ULP-I2C defect, **on S2/S3 only**: the ULP RISC-V I2C example caused "a spurious wakeup of the main CPU because of a Trap signal when the ULP core does not meet the wakeup threshold values, due to the `RTC_CNTL_COCPU_DONE` signal being set before `RTC_CNTL_COCPU_SHUT_RESET_EN`," preventing proper ULP core reset each cycle. — [espressif/esp-idf issue #10301](https://github.com/espressif/esp-idf/issues/10301). Issue is closed with "Resolution: Done." Affects ESP32-S2/S3; **not** evidence about classic ESP32.

### Inferences
- The single highest-confidence conclusion of this whole investigation: **hardware RTC_I2C on classic ESP32 is a research project, not an engineering option.** Two independent community repos exist precisely because the vendor path doesn't just work, and the documented failure mode (wrong sampling edge) is a silicon-level issue you cannot fix in software except by slowing/padding the bus.
- **Software bit-banged I2C in the ULP FSM is the only credible ULP route**, and it changes the pin analysis: a bit-banged master can use *any* RTC GPIO pair. But on this board the free RTC GPIOs are **36 and 39, which are input-only** — you cannot drive SDA/SCL from them. So the bit-bang escape hatch is also closed on the ONE+ Model B unless you steal pins from LED/GPS/link functions.
- Net: **ULP-polled I2C wake-on-motion is not achievable on this board without sacrificing a vendor function.** If you're willing to do board rework anyway, a single fly-wire to GPIO36/39 for a real INT line (Q1) is strictly simpler and strictly more reliable than any ULP I2C scheme.

### Gaps
- I could not retrieve the full text of the esp32.com *RTC_I2C SDA sampling* thread (the forum serves a JS bot-challenge to the fetcher), so I cannot confirm **whether the "wrong edge" statement came from an Espressif employee** or from a community member. Both quoted statements are reported via search-engine summarization of that thread, not verbatim-verified by me. Treat as strong community signal, not as vendor errata.
- No official Espressif **errata document** entry for RTC_I2C on ESP32 was located. The published "ESP32 ECO and Workarounds for Bugs" document was not checked in this pass.

---

## Q4. Does the ICM-42627 support autonomous wake-on-motion, and can it be read without a GPIO?

### Takeaway
Yes — the ICM-426xx **APEX** block runs Wake-on-Motion (WoM) and Significant Motion Detection (SMD) **entirely on-chip with zero host involvement**, latching results into interrupt status registers in addition to asserting INT1/INT2. Because the result lands in a readable register, **an unrouted INT pin does not prevent you from using WoM — you can poll the status register over I2C.** What it prevents is the *sensor waking the ESP32*; something must still be awake to poll.

### Cited Findings
- "APEX processes accelerometer data, extracts measurements, and asserts motion-specific interrupts. APEX features use accelerometer data, not gyroscope data." — [TDK AN-000173, p.3](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- "**Wake-On-Motion (WoM)** issues an interrupt when the accelerometer change is greater than a programmable threshold, designed to alert the user upon transition from a stationary state to a moving state. **Significant Motion Detection (SMD)** asserts an interrupt when two sequential WoM events occur within a 1s or 3s window." — [TDK AN-000173, p.3](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- **No initialization/DMP bring-up needed for WoM/SMD:** "Tap/WoM/SMD can be arbitrarily enabled/disabled as they **do not require any initialization commands**." (Tilt/Pedometer/R2W, by contrast, require DMP_INIT_EN, DMP_MEM_RESET_EN, etc.) — [TDK AN-000173, p.5](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- WoM feeds the **Interrupt Registers** block directly in the APEX block diagram, and its outputs are register fields: "`WOM_X_INT`: Wake on Motion Interrupt on x-axis; `WOM_Y_INT` … ; `WOM_Z_INT` …", and for SMD "`SMD_INT`: Significant Motion Detection Interrupt." — [TDK AN-000173, pp.19–20, 22](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- WoM tuning: `WOM_X_TH` / `WOM_Y_TH` / `WOM_Z_TH` "range from 0g~1g in increments of 1/256 g, so WOM_X_TH=10 represents 39.1 mg." `WOM_INT_MODE` selects logical OR (0) vs AND (1) of enabled axes. `WOM_MODE` selects differential (0) vs absolute (1). `SMD_MODE`: 0 = off, 1 = WoM only (no SMD), 2 = SMD with 1 s window, 3 = SMD with 3 s window. — [TDK AN-000173, pp.20, 22](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- Complete WoM bring-up is **8 I2C writes** (AN-000173 Table 6): reset (`0x76←0x00`, `0x11←0x01`); ODR=50 Hz FSR=4g (`0x50←0x49`); accel **low-power** mode (`0x4E←0x02`); bank 4 `ACCEL_WOM_X/Y/Z_THR` at `0x4A/0x4B/0x4C ← 0x66` (≈0.4 g); `INT_SOURCE4` (`0x69←0x07`) to tie WoM x/y/z to INT2; `SMD_CONFIG` (`0x57←0x05`) for differential WoM, interrupt on any axis. — [TDK AN-000173, p.21](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- SMD variant differs only in `INT_SOURCE4 ← 0x08` and `SMD_CONFIG ← 0x06` (SMD short/1 s window). — [TDK AN-000173, p.23](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- WoM/SMD/Tap bypass APEX downsampling: "This does not apply to Tap, WoM, and SMD, as these features receive **raw accelerometer data without any downsampling**." — [TDK AN-000173, p.5](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- Power-saving mode within APEX itself uses WoM as the gate: "To reduce APEX power consumption when the part is motionless, the user can enable Power Save Mode (`DMP_POWER_SAVE=0` and `DMP_POWER_SAVE_TIME_SEL > 0b000`) and WoM (`SMD_MODE=0b01`). Power Save Mode uses the WoM thresholds … to enable/disable the other APEX routines during stationary periods." — [TDK AN-000173, p.5](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- Accel LP vs LN: "LP Mode consumes less power because the ADC is duty cycled. LN Mode consumes constant power… Accelerometer LP Mode is optional for ODR<=500 Hz." WoM is supported at ODRs from <25 Hz up to 1000 Hz. — [TDK AN-000173, p.4](https://www.cdiweb.com/datasheets/invensense/AN-000173-ICM-426xx-Motion-Functions-Description-and-Usage-v1.pdf)
- Related part-family app note exists for the 4260x/4267x generation: [TDK AN-000271, ICM-42607x / ICM-42670x APEX Motion Functions](https://invensense.tdk.com/wp-content/uploads/2023/05/an-000271-icm-42607x-icm-42670x-apex-motion-functions-description-and-usage.pdf) (URL returned 404 at fetch time; listed here as a pointer, not as a verified source).
- The ICM-42670-P family markets "2 programmable interrupts with **ultra-low-power wake-on-motion** support" and 2 kB FIFO — [DigiKey ICM-42670-P product page](https://www.digikey.co.uk/en/products/detail/tdk-invensense/ICM-42670-P/14319531)

### Inferences
- **The unrouted INT pin is not a blocker for *detecting* motion, only for *being woken by* it.** The architecture it forces is: something must periodically read the ICM's interrupt-status register over I2C. The candidates are (a) the ESP32 main CPU on a timer wake, (b) the ESP32 ULP (blocked — see Q2/Q3), or (c) **the on-board STM32 "link" coprocessor**.
- Because WoM/SMD are latched in a status register and run continuously in the accelerometer's own low-power domain, **polling at a slow rate loses no events** (subject to the clear-on-read caveat below). You do not need to poll at the accelerometer ODR; you poll at your wake cadence and ask "did anything happen since last time?" This makes a **timer-wake + I2C-poll** design functionally equivalent to interrupt-driven wake, at the cost of detection latency equal to your wake period.
- SMD (two WoM events within 1 s or 3 s) is the right primitive for a parked-vehicle application — it rejects single bumps, door slams, and wind buffeting far better than raw WoM, and costs nothing extra.
- Since the ICM INT is unrouted, you must **route WoM/SMD to the interrupt-status registers only** and not care about INT_SOURCE4 — but note AN-000173's recipe *does* set INT_SOURCE4, which controls routing to the physical pin. Whether the status bits set independently of pin routing needs confirmation on real silicon (see gaps).

### Gaps
- **I could not locate an ICM-42627 datasheet.** Searches returned only ICM-42670-P, ICM-42605, ICM-42688-P, ICM-40627. All register addresses quoted above are from **AN-000173, which covers the "ICM-426xx" family generically**; the ICM-4260x/4267x generation has a *different* register map (hence TDK publishing a separate AN-000271 for it). **Every register address above must be verified against the actual ICM-42627 datasheet before use.** Which generation the ICM-42627 belongs to is unresolved.
- **Clear-on-read behaviour of the WoM/SMD status bits is not documented in AN-000173.** It states the *pin* is "pulsed (auto-clearing)" by default, but says nothing about the status register. If the status register is read-to-clear (typical for InvenSense), polling works cleanly; if the bits track the pin's auto-clearing pulse, a slow poller could miss events. This is the **single most important thing to verify empirically** before committing to a poll-only design.
- **No current-consumption figure for the ICM in WoM/LP mode** appears in AN-000173. The datasheet is needed for this (typical ICM-426xx accel LP figures are in the tens of µA, but I will not quote a number I did not source).
- Whether the ICM-42627 on the ONE+ shares an I2C bus with the STM32 link coprocessor, or is ESP32-only — not determinable from the pin header.

---

## Q5. What is the actual power benefit, given PSRAM and a ~10 mA board floor?

### Takeaway
ESP32 deep sleep takes the SoC to ~**10 µA** (RTC timer + RTC memory) or ~**5 µA** (RTC timer only), versus ~**0.8 mA** light sleep and 20–68 mA modem-sleep/active. **PSRAM is a non-issue** — it sits on VDD_SDIO and is powered down before deep sleep, so WROVER and WROOM deep-sleep currents are the same. But against a **~10 mA board floor set by other components, the ESP32's entire contribution in deep sleep is ~0.1% of the budget** — deep sleep is essentially a rounding error and does not solve a parked-drain problem on its own.

### Cited Findings
- ESP32 datasheet power-mode table (as reproduced in search over [Lucidar](https://lucidar.me/en/esp32/power-consumption-of-esp32/) and [DroneBotWorkshop](https://dronebotworkshop.com/esp32-low-power/)):
  - Modem-sleep, CPU active @ 240 MHz: **30–68 mA**
  - Modem-sleep, CPU active @ 160 MHz: **27–44 mA**
  - Modem-sleep, CPU active @ 80 MHz: **20–31 mA**
  - Light-sleep: **0.8 mA** *(see conflict note below)*
  - Deep-sleep, ULP coprocessor powered on: **150 µA**
  - Deep-sleep, ULP sensor-monitored pattern (ULP periodically active, ~1% duty): **100 µA**
  - Deep-sleep, RTC timer + RTC memory: **10 µA**
  - Deep-sleep, RTC timer only: **5 µA**
- **Source conflict, flagged:** one search aggregation rendered Light-sleep as "0.8 µA" and then self-corrected mid-answer. The ESP32 datasheet value is **0.8 mA**; 0.8 µA would be below the deep-sleep figure and is physically implausible. — [search summary over Lucidar / DroneBotWorkshop](https://lucidar.me/en/esp32/power-consumption-of-esp32/). **Verify against the ESP32 datasheet "Power consumption by power modes" table before quoting.** (Direct PDF fetch of the datasheet failed — the file is image/compressed-stream based and would not extract.)
- **PSRAM does not add deep-sleep current:** "The maximal standby current of ESP-PSRAM32 is 50 µA, but it is powered by VDD_SDIO and is **powered down before entering deep sleep** (same as Flash chip). As a result, **there is no difference in current consumption in deep sleep between ESP32 WROVER and ESP32 WROOM** Module." — Espressif staff reply on [esp32.com, "ESP32 WROVER pSRAM Quiescent current"](https://esp32.com/viewtopic.php?p=16700)
- **WROVER does have a specific deep-sleep current trap — and it is GPIO12:** "on ESP32-WROVER module, **GPIO12 is pulled up externally**, and it also has an internal pulldown in the ESP32 chip. This means that in Deep-sleep, some current flows through these external and internal resistors, increasing Deep-sleep current." Mitigation: call `rtc_gpio_isolate(GPIO_NUM_12)` before entering deep sleep. — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
- Measured module-level deep sleep on an ALB32-WROVER (8 MB flash + 4 MB PSRAM): **5.5 µA** with RTC-timer + GPIO wake only; **32 µA** with ULP wake also enabled; **300 µA** with touch wake enabled. — [esp32.com WROVER pSRAM quiescent current thread](https://esp32.com/viewtopic.php?p=18147)
- Real-world board-level deep sleep varies by **two to three orders of magnitude** depending on board peripherals: ESP32 FireBeetle DFR0478 at **10 µA**, DFR0654 low-power mode at **11.6 µA**, ESP32-S3-DevKitM-1 at **31.7 µA**, and **ESP32-DevKitC V4 at 3.8 mA** — [Lucidar: real current consumption of ESP32 in deep sleep](https://lucidar.me/en/esp32/power-consumption-of-esp32/)
- For light sleep, VDD_SDIO must be explicitly kept on if you drive pins in that domain: `esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON)` — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)

### Inferences
- **The GPIO12 warning collides head-on with the Freematics pin map.** On this board GPIO12 is **GPS_POWER / MOLEX_VCC**. The recommended mitigation, `rtc_gpio_isolate(GPIO_NUM_12)`, will release GPIO12's drive state — which on this board may mean losing control of GPS power rail state across sleep. Worse, GPIO12 is the WROVER flash/PSRAM voltage strapping pin (MTDI); whatever external pull-up exists on the module interacts with whatever the Freematics design does to GPIO12. **This is a concrete, board-specific deep-sleep current and boot-reliability risk that must be measured, not assumed.**
- **Arithmetic on the 10 mA floor (inference, using the user-supplied floor):**
  - Board floor alone: 10 mA × 24 h = **240 mAh/day ≈ 7.3 Ah/month**.
  - ESP32 in deep sleep adds ~10 µA = **0.24 mAh/day** — i.e. **0.1%** of the floor. Completely negligible.
  - Even ESP32 in *light* sleep at 0.8 mA adds only 19.2 mAh/day — **8%** on top of the floor.
  - So: **deep sleep vs light sleep is a ~0.8 mA argument against a 10 mA problem.** It is not where the parked drain lives.
- **Where deep sleep *does* pay is duty cycling, not the sleep floor.** If the ESP32 is otherwise running at ~40–70 mA with the modem up, sleeping it 95% of the time takes its average contribution from ~50 mA to ~2.5 mA. That is a real 5× reduction *of the ESP32's share* — but it's a change to the *awake/asleep ratio*, which **light sleep achieves almost as well** (0.8 mA floor vs 0.01 mA floor is irrelevant next to a 10 mA board floor), at a fraction of the complexity.
- **Therefore the honest engineering conclusion: deep sleep is not worth it on this board until the ~10 mA floor is attacked first.** Identify and gate the floor (GPS module standby, the STM32 link, the buzzer driver, BEE/cellular module, LDO quiescent, LED, CAN transceiver). Until then, deep sleep changes the total from ~10.05 mA to ~10.01 mA.
- **Light sleep is the better fit for this board anyway**, for a reason that has nothing to do with current: it preserves CPU state and peripheral configuration, so the ESP32 can hold the STM32 link session, keep RAM-resident buffers, and poll the ICM over I2C every few seconds without a full reboot (see Q6).

### Gaps
- I could not fetch the ESP32 datasheet PDF directly (both the legacy `espressif.com` URL and the redirected `documentation.espressif.com` URL returned compressed/image-based PDFs that would not text-extract). **All power-mode numbers above are second-hand** via two independent aggregators that agree with each other; they should be confirmed against the official datasheet table before being published.
- No rev-1-specific power figures for the **ESP32-D0WDQ6 rev 1** were found. Rev-1 silicon has documented ECO workarounds (e.g. the "Deep-sleep current leakage / brownout" and GPIO36/39 ADC-related errata) that were **not** investigated in this pass — notably, there is a known rev-1 erratum affecting **GPIO36 and GPIO39 glitching when ADC2/Hall sensor or certain RTC functions are in use**, which is directly relevant to the Q1 recommendation. **This must be checked in "ESP32 ECO and Workarounds for Bugs" before committing to GPIO36/39 as the wake pin.**
- The actual composition of the ~10 mA floor on the ONE+ Model B is user-supplied and was not independently verified.

---

## Q6. Costs of timer-wake deep sleep specifically (reset semantics, boot time, state loss)

### Takeaway
On ESP32, deep sleep exit is a **full CPU restart from the reset vector** — RTC FAST/SLOW memory survives, all normal RAM (including PSRAM) does not. For an OBD dongle that maintains a link session with an STM32, a cellular/BEE modem, a GPS fix, and in-RAM buffers, that reset cost is substantial and recurs on every wake.

### Cited Findings
- "In Deep-sleep mode, the CPUs, **most of the RAM**, and all digital peripherals that are clocked from APB_CLK are **powered off**." Only the RTC controller, ULP coprocessor, RTC FAST memory and RTC SLOW memory remain powered. — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
- "Variables in RTC SLOW memory (marked with `RTC_DATA_ATTR`) are preserved by default." — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
- Contrast — light sleep: "the digital peripherals, most of the RAM, and CPUs are clock-gated and their supply voltage is reduced. Upon exit… **internal states are preserved**." — [ESP-IDF Sleep Modes (esp32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)
- PSRAM is powered down before deep sleep along with flash (VDD_SDIO domain) — [esp32.com, Espressif reply](https://esp32.com/viewtopic.php?p=16700). Consequence: **all PSRAM contents are lost across deep sleep.**

### Inferences
- **Deep sleep is a reboot.** `esp_deep_sleep_start()` does not return; execution resumes at `app_main`/`setup()`. The *only* carry-over is RTC SLOW memory (via `RTC_DATA_ATTR`) and RTC FAST memory. For this application that means on every wake you must re-run: SPI/UART bring-up to the STM32 link, OBD/CAN session re-establishment, GPS re-acquisition (cold or hot depending on whether GPS was kept powered via GPIO12/15), and modem re-attach if the BEE module was powered down via GPIO27.
- **GPS is the expensive one.** If GPS_POWER (GPIO12) is cut during sleep, each wake incurs a cold/warm TTFF of tens of seconds at GPS-module-level current — which can easily exceed the energy saved by sleeping. If GPS is kept powered, it is probably a major contributor to the 10 mA floor and the ESP32's sleep state barely matters.
- **PSRAM loss is the specific WROVER cost.** Any design that buffers trip data, log records, or a telemetry queue in PSRAM must flush to flash/SD before each deep sleep, or move the hot state into the small RTC SLOW memory. This turns "just add deep sleep" into a data-architecture change.
- **Light sleep avoids every one of these costs** — peripherals, RAM, PSRAM and CPU state all survive — at a floor of ~0.8 mA, which is 8% of the board's existing 10 mA. Given the floor, **light sleep is strictly the better engineering trade for this board.** Deep sleep only becomes worth the state-loss pain once the board floor is driven down toward the tens-of-µA range, at which point the 0.8 mA light-sleep floor would dominate.
- Recommended architecture if motion-gated sleep is pursued without board rework: **timer-wake light sleep every 2–10 s → read ICM-42627 WoM/SMD status over I2C → if no motion, go straight back to sleep; if motion, bring up the full stack.** This needs no INT pin, no ULP, no RTC_I2C, and no rework — and its only cost is detection latency equal to the poll period.

### Gaps
- **ESP32 deep-sleep wake-to-`app_main` boot time was not sourced.** It depends heavily on bootloader configuration, flash frequency/mode, and whether the RTC fast-memory deep-sleep wake stub is used. Do not quote a figure without measuring it on the actual firmware image.
- Whether the ESP-IDF docs use the word "reset" for deep-sleep exit was **not** confirmed in the fetched page; the reset semantics above are inferred from "CPUs… powered off" plus "RTC_DATA_ATTR preserved" plus the well-known `esp_deep_sleep_start()` no-return contract.
- RTC SLOW memory size available for `RTC_DATA_ATTR` state (and how much the ULP would consume if used) — not sourced; see Q2 gaps.
