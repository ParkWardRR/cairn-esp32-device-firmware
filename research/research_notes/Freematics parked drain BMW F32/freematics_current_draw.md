# Freematics ONE+ (Model B, classic ESP32) — measured vs claimed current draw, and official-firmware sleep behaviour

Scope note: this covers the ESP32-based **Freematics ONE+** (Model A = no cellular; **Model B** = ESP32 + u-blox M9 GNSS + SIM7670 LTE Cat-1), not the older ATmega328p-based Freematics ONE, and not the ESP32-S3 Model H. The official repo's own README maps the split: `firmware_v4` = ATmega328p Freematics ONE, `firmware_v5` = ESP32 Freematics ONE+, `libraries/` = ESP32 ONE+/Esprit — [stanleyhuangyc/Freematics README](https://github.com/stanleyhuangyc/Freematics).

**Headline:** I found **no independent measurement of any kind** for this device. Every current figure that exists is a vendor claim on a product page. The strongest evidence available is the firmware source itself, and it shows the official standby path never puts the ESP32 into any hardware sleep mode — so the real parked draw is plausibly *higher*, not lower, than the vendor's ~10 mA claim.

## Q1: Current in active logging, WiFi upload, firmware standby, and ESP32 deep sleep

### Takeaway
Only three numbers are published anywhere, all **vendor claims** on Freematics' own product pages: ~20 mA typical (WiFi inactive), ~50 mA (WiFi active), ~10 mA in "low power mode" with GPS/cellular/WiFi off. There is **no published figure at all** for active logging with GNSS+IMU+SD running, and **no figure for ESP32 deep sleep on this board**, because the official firmware never enters ESP32 deep sleep.

### Cited Findings
- **VENDOR CLAIM** (ONE+ Model A product page): "Typical power rating @80Mhz: 20mA" (WiFi inactive) / "50mA (WiFi active)" — [Freematics ONE+ product page](https://freematics.com/products/freematics-one-plus/)
- **VENDOR CLAIM** (ONE+ Model A product page): "In low power mode with all peripherals (GPS, GSM, WiFi) powered off, the power consumption is around 10mA", and "Freematics ONE+ enters and leaves low power mode programmatically" — [Freematics ONE+ product page](https://freematics.com/products/freematics-one-plus/)
- **VENDOR CLAIM** (ONE+ **Model B** product page, the classic-ESP32 variant in question): "When GPS, cellular, and Wi-Fi are powered off, power consumption is around 10mA" — [Freematics ONE+ Model B product page](https://freematics.com/products/freematics-one-plus-model-b/)
- Model B hardware per vendor: "Espressif ESP32 with 16MB Flash, 8MB PSRAM, 32K RTC", ICM-42627 IMU, u-blox M9 GNSS module + antenna, SIM7670 LTE Cat-1 module, buzzer, WiFi b/g/n, Classic BT + BLE — [Freematics ONE+ Model B product page](https://freematics.com/products/freematics-one-plus-model-b/)
- The Model B product page publishes **no input voltage range, no active/logging current, and no CAN transceiver or regulator part numbers** — [Freematics ONE+ Model B product page](https://freematics.com/products/freematics-one-plus-model-b/)
- **ESP32 silicon comparison (vendor datasheet, for scale):** Table 5-4 "Current Consumption Depending on RF Modes": 802.11b TX @ +19.5 dBm = 240 mA typ; 802.11g TX @ +16 dBm = 190 mA; 802.11n TX @ +14 dBm = 180 mA; RX 802.11b/g/n = 95~100 mA; BT/BLE TX @ 0 dBm = 130 mA; BT/BLE RX = 95~100 mA — [ESP32 Series Datasheet v5.3, §5.4](https://documentation.espressif.com/esp32_datasheet_en.pdf)
- Module-level figures are similar: ESP32-WROOM-32E Table 16, 802.11b TX @19.5 dBm average 239 mA / peak 379 mA; RX 802.11b/g/n 112 mA — [ESP32-WROOM-32E/32UE Datasheet v2.1, §6.4](https://documentation.espressif.com/esp32-wroom-32e_esp32-wroom-32ue_datasheet_en.pdf)

### Inferences
- The two vendor numbers are mutually informative: 20 mA is claimed for the ESP32 running at 80 MHz with WiFi off, and 10 mA for "all peripherals off". Since the ESP32 core alone accounts for roughly the 20 mA figure, the 10 mA "low power" claim can only be met if the ESP32 itself is in a reduced-power state — which, per Q4 below, the shipped firmware never does. So the ~10 mA claim does not describe the behaviour of the stock `standby()` path.
- Because active logging (GNSS fix + IMU + SD writes) is not characterised at all by the vendor, any figure for it in a report must be presented as an estimate, not a source.

### Gaps
- No vendor or third-party figure for current while actively logging GNSS + IMU + SD.
- No vendor or third-party figure for WiFi-upload current for the assembled dongle (only ESP32 silicon/module figures exist, which exclude the GNSS module, the co-processor, the CAN transceiver and the two regulators).
- No figure for the board in ESP32 deep sleep — and no firmware path that would produce one.

## Q2: Is the ~10 mA sleep claim independently confirmed or refuted?

### Takeaway
**No.** I could not find a single independent current measurement of a Freematics ONE+ of any variant — no blog post, teardown, hackster/hackaday project, GitHub issue, or forum post with a meter reading. The ~10 mA figure is an unverified vendor claim, repeated verbatim on both the Model A and Model B pages.

### Cited Findings
- The ~10 mA figure appears only as vendor marketing copy on the two product pages cited above; both state it as a flat assertion with no test conditions, no supply voltage, and no firmware version — [ONE+](https://freematics.com/products/freematics-one-plus/); [ONE+ Model B](https://freematics.com/products/freematics-one-plus-model-b/)
- **The Freematics user forum is currently offline**, which forecloses the most likely source of user measurements. `freematics.com/forum` 301-redirects to `forum.freematics.com`, which serves a WordPress "Briefly unavailable for scheduled maintenance" page and additionally presents a TLS certificate for an unrelated domain (`elliehuang.com`) — verified directly, 2026-10-02. Historic threads are also not surfacing in web search indexes.
- **Vendor support is reportedly unresponsive:** a Traccar forum user trying to understand ONE+ standby behaviour reported that attempts to contact Freematics directly went unanswered — [Traccar forum: "freematics one stops transmitting after 4 hours or so"](https://www.traccar.org/forums/topic/freematics-one-stops-transmitting-after-4-hours-or-so/)
- **For scale:** ~10 mA is three orders of magnitude above ESP32 deep-sleep. Note a caveat on the usual "10 µA" citation: **current Espressif datasheets no longer publish a sleep-mode current table at all.** I checked ESP32 Series Datasheet v5.3 (§4 Functional Description and §5 Electrical Characteristics), ESP32-WROVER-E/IE v2.4 (§6), and ESP32-WROOM-32E/32UE v2.1 (§6) — all three contain only *active-mode* RF current tables; the "Power Consumption by Power Modes" table present in older revisions is absent. The ~10 µA deep-sleep number therefore comes from older datasheet revisions and should be labelled as such.
- ESP-IDF does confirm qualitatively what deep sleep powers down: "the CPUs, most of the RAM, and all digital peripherals that are clocked from APB_CLK are powered off. The only parts of the chip that remain powered on are: RTC controller, ULP coprocessor, RTC FAST memory, RTC SLOW memory" — [ESP-IDF Sleep Modes](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/sleep_modes.html)

### Inferences
- The absence of any independent measurement is itself a finding: anyone quoting "10 mA sleep" for a ONE+ is quoting Freematics' marketing, not data.
- The gap between ESP32 deep-sleep (tens of µA) and the claimed 10 mA board figure is ~1000x, which is only explicable by non-ESP32 components (co-processor, transceiver, regulators, LED, GNSS/modem leakage) plus, critically, by the ESP32 itself not actually sleeping.

### Gaps
- Could not retrieve any Freematics forum thread (site down), so I cannot say whether users ever posted meter readings there.
- No GitHub issue in the official repo surfaced with a current measurement.

## Q3: What else on the board draws current and cannot be switched off?

### Takeaway
I could **not** confirm part numbers for the co-processor, the CAN/OBD transceiver, or the regulators from any accessible primary source — the product pages simply do not list them, and the schematic is only referenced, not inlined. What the firmware *does* prove is that GNSS power is switchable under GPIO control, while the co-processor is only ever asked to sleep over a serial AT command and is never hard-powered-off.

### Cited Findings
- GNSS power **is** switchable: the ONE+ library defines `#define PIN_GPS_POWER 12` and `#define PIN_GPS_POWER2 15`, and `FreematicsESP32::gpsEnd(bool powerOff)` does `if (powerOff && m_pinGPSPower) digitalWrite(m_pinGPSPower, LOW);` — [libraries/FreematicsPlus/FreematicsPlus.h](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsPlus.h) and [FreematicsPlus.cpp](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsPlus.cpp)
- On variants where GNSS hangs off the co-processor rather than the ESP32, `gpsEnd(true)` instead sends `"ATGPSOFF\r"` to the co-processor — i.e. GNSS power-down is delegated, not a hard rail cut — [FreematicsPlus.cpp](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsPlus.cpp)
- The cellular/XBee-socket module has a power toggle (`FreematicsESP32::xbTogglePower(unsigned int duration = 200)`), i.e. a pulsed power-key line, not a rail switch — [FreematicsPlus.h](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsPlus.h)
- There is **no API anywhere in the repo to cut power to the co-processor** — a full-repo grep for `esp_deep_sleep|esp_light_sleep|esp_sleep_enable|ATLP|enterLowPowerMode|leaveLowPowerMode|lowPowerMode|esp_pm_config|esp_wifi_stop|btStop` returns only the ATLP/`enterLowPowerMode` family and nothing resembling a co-processor power rail (verified against a fresh clone of master, 2026-10-02) — [stanleyhuangyc/Freematics](https://github.com/stanleyhuangyc/Freematics)
- **LEDs:** the ONE+ page lists the LED set as "GPIO controlled + communication indicator" — i.e. at least one indicator LED exists that is described as a communication indicator rather than as GPIO-controlled — [Freematics ONE+ product page](https://freematics.com/products/freematics-one-plus/)
- A schematic PDF is referenced as available separately from the ONE+ product page but its contents are not on the page — [Freematics ONE+ product page](https://freematics.com/products/freematics-one-plus/)

### Inferences
- Given that the firmware can switch GNSS and the cellular module but has no mechanism at all for the co-processor, the CAN transceiver or the regulators, the idle floor is structurally set by: (a) the co-processor in whatever state ATLP leaves it in, (b) the CAN/OBD transceiver, which on a typical OBD dongle sits in recessive/listen state drawing several mA, (c) quiescent current of the 12V→5V and 5V→3.3V regulators, (d) any always-on LED (a single LED at even 2 mA is a fifth of the claimed 10 mA budget), and (e) the ESP32, which — per Q4 — is not asleep.
- Because the ESP32 is in Active mode during the stock standby, the ESP32 is plausibly the *largest* single contributor in the shipped configuration, which inverts the usual assumption. The "other components dominate" framing is correct for the vendor's hypothetical 10 mA number but likely wrong for the firmware as shipped.

### Gaps
- **Unconfirmed:** the co-processor part (Freematics calls it "the link"/"co-processor" in code comments; the repo never names the silicon). The README confirms the *older* Freematics ONE used an ATmega328p, but that is the predecessor product, not the ONE+ co-processor. I found no primary source naming an STM32 part for the ONE+ — treat "STM32 co-processor" as unverified.
- **Unconfirmed:** CAN transceiver part number and its standby/listen current.
- **Unconfirmed:** regulator topology and quiescent currents. A linear 12V→5V regulator vs a switcher makes a large difference here and I have no source either way.
- **Unconfirmed:** whether the "communication indicator" LED is permanently lit, blinks, or is software-controlled.
- I did not retrieve the schematic PDF; it is the obvious next step and would settle all four items above.

## Q4: What the official firmware's standby()/sleep path actually does

### Takeaway
The official `firmware_v5` standby path **never uses ESP32 deep sleep or light sleep**. It powers down GNSS, sends one AT command to put the co-processor to sleep, and then **busy-polls the accelerometer in a tight loop with the ESP32 CPU fully active and the BLE/WiFi stacks still being serviced**. This is the single most important finding for parked-drain analysis.

### Cited Findings
- **Zero occurrences of ESP32 sleep APIs in the entire repository.** Grepping master for `esp_deep_sleep`, `esp_light_sleep`, `esp_sleep_enable`, `esp_pm_config`, `esp_wifi_stop` and `btStop` returns no hits in any sketch or library; the only power-related hits are `enterLowPowerMode`/`leaveLowPowerMode`/`ATLP` (verified against a fresh clone, 2026-10-02) — [stanleyhuangyc/Freematics](https://github.com/stanleyhuangyc/Freematics)
- **`telelogger` standby(), verbatim** — [firmware_v5/telelogger/telelogger.ino](https://github.com/stanleyhuangyc/Freematics/blob/master/firmware_v5/telelogger/telelogger.ino):
  ```c
  void standby()
  {
    state.set(STATE_STANDBY);
  #if STORAGE != STORAGE_NONE
    if (state.check(STATE_STORAGE_READY)) { logger.end(); }
  #endif
  #if !GNSS_ALWAYS_ON && GNSS == GNSS_STANDALONE
    if (state.check(STATE_GPS_READY)) {
      Serial.println("[GNSS] OFF");
      sys.gpsEnd(true);
      state.clear(STATE_GPS_READY | STATE_GPS_ONLINE);
      gd = 0;
    }
  #endif
    state.clear(STATE_WORKING | STATE_OBD_READY | STATE_STORAGE_READY);
    // this will put co-processor into sleep mode
    Serial.println("STANDBY");
    obd.enterLowPowerMode();
  #if ENABLE_MEMS
    calibrateMEMS();
    waitMotion(-1);
  #elif ENABLE_OBD
    do { delay(5000); } while (obd.getVoltage() < JUMPSTART_VOLTAGE);
  #else
    delay(5000);
  #endif
    Serial.println("WAKEUP");
    sys.resetLink();
  #if RESET_AFTER_WAKEUP
  #if ENABLE_MEMS
    if (mems) mems->end();
  #endif
    ESP.restart();
  #endif
    state.clear(STATE_STANDBY);
  }
  ```
- **`waitMotion(-1)` is an unthrottled polling loop, not a sleep.** Verbatim from the same file: it loops `do { ... if (!mems->read(acc)) continue; ... processBLE(100); ... } while (state.check(STATE_STANDBY) && ((long)(millis() - t) < timeout || timeout == -1));`, with `#if ENABLE_HTTPD serverProcess(100); #endif` inside the loop body. There is no `delay()`, no `vTaskDelay()`, and no sleep call in the loop — [firmware_v5/telelogger/telelogger.ino](https://github.com/stanleyhuangyc/Freematics/blob/master/firmware_v5/telelogger/telelogger.ino)
- **The OBD-only fallback path polls too:** `do { delay(5000); } while (obd.getVoltage() < JUMPSTART_VOLTAGE);` — it wakes the co-processor link every 5 s to read battery voltage, with `#define JUMPSTART_VOLTAGE 14 /* V */` — [telelogger.ino](https://github.com/stanleyhuangyc/Freematics/blob/master/firmware_v5/telelogger/telelogger.ino), [config.h](https://github.com/stanleyhuangyc/Freematics/blob/master/firmware_v5/telelogger/config.h)
- **The AT command is `ATLP`.** `COBD::enterLowPowerMode()` verbatim: `{ char buf[32]; if (link) { reset(); delay(1000); link->sendCommand("ATLP\r", buf, sizeof(buf), 1000); } }` — [libraries/FreematicsPlus/FreematicsOBD.cpp](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsOBD.cpp)
- **Wake-up of the co-processor is by brute force:** `COBD::leaveLowPowerMode()` is `// send any command to wake up` then `for (byte n = 0; n < 30 && !link->sendCommand("ATI\r", buf, sizeof(buf), 1000); n++);` — [FreematicsOBD.cpp](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsOBD.cpp)
- **`datalogger` uses a different, weaker mechanism than `telelogger`.** Its `standby()` does not call `enterLowPowerMode()`/ATLP at all; it calls `sys.resetLink()` with the comment `// this will put co-processor into a delayed sleep`, and wakes with `sys.reactivateLink()` (`// this will wake up co-processor`). `FreematicsESP32::resetLink()` is simply `if (link) link->sendCommand("ATR\r", buf, sizeof(buf), 100);` — [firmware_v5/datalogger/datalogger.ino](https://github.com/stanleyhuangyc/Freematics/blob/master/firmware_v5/datalogger/datalogger.ino), [FreematicsPlus.cpp](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsPlus.cpp)
- `datalogger`'s standby loop likewise polls the IMU continuously and calls `serverCheckup(WIFI_JOIN_TIMEOUT * 4); serverProcess(100);` (WiFi builds) or `processBLE(100)` (otherwise) inside the inner 1-second accumulation loop — so **WiFi can remain joined and serviced throughout "standby"** — [datalogger.ino](https://github.com/stanleyhuangyc/Freematics/blob/master/firmware_v5/datalogger/datalogger.ino)
- Note on `telelogger`: after `waitMotion()` returns it calls `sys.resetLink()` (ATR) rather than `leaveLowPowerMode()` (repeated ATI), then `ESP.restart()` if `RESET_AFTER_WAKEUP` is set — [telelogger.ino](https://github.com/stanleyhuangyc/Freematics/blob/master/firmware_v5/telelogger/telelogger.ino)
- **Relevant stock config defaults** (`firmware_v5/telelogger/config.h`): `MOTION_THRESHOLD 0.4f /* vehicle motion threshold in G */`, `JUMPSTART_VOLTAGE 14 /* V */`, `RESET_AFTER_WAKEUP 1`, `GNSS_ALWAYS_ON 0`, `GNSS_RESET_TIMEOUT 300 /* seconds */`, `SERVER_SYNC_INTERVAL 120 /* seconds, 0 to disable */`, `PING_BACK_INTERVAL 900 /* seconds */`, `SIGNAL_CHECK_INTERVAL 10 /* seconds */`, `COOLING_DOWN_TEMP 75 /* celsius degrees */`, `STATIONARY_TIME_TABLE {10, 60, 180}`, `DATA_INTERVAL_TABLE {1000, 2000, 5000} /* ms */`, and defaults `ENABLE_OBD 1`, `ENABLE_WIFI 1`, `ENABLE_MEMS 1`, `STORAGE STORAGE_SD`, `GNSS GNSS_STANDALONE` — [config.h](https://github.com/stanleyhuangyc/Freematics/blob/master/firmware_v5/telelogger/config.h)
- The library sets a very long watchdog (`esp_task_wdt_init(600, 0)` — 600 s) in `FreematicsESP32::begin()`, consistent with long blocking standby loops being expected — [FreematicsPlus.cpp](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsPlus.cpp)

### Inferences
- In stock `telelogger` standby, expected ESP32 state is: both/one CPU running at full clock, flash/PSRAM active, I2C transactions to the IMU back-to-back, BLE stack serviced every iteration, and (in WiFi builds, or `datalogger`) the WiFi stack still associated. That is Active mode, not Modem-sleep, not Light-sleep. On ESP32 silicon figures alone this is in the tens of mA, before adding the co-processor, CAN transceiver, regulators and LED.
- Therefore the vendor's "around 10 mA" is best read as a *hardware capability* claim (what the board could do if everything including the ESP32 were put to sleep), not a description of the shipped firmware's standby. Any report should not equate the two.
- `ATLP` is sent *after* a `reset()` plus a 1 s delay, so the co-processor reboots and then sleeps — meaning any CAN/OBD state is dropped at standby entry, and wake requires a full re-init.
- The savings attributable to ATLP are **not quantified anywhere** — no source states how much current the co-processor drops by.

### Gaps
- No documentation of what `ATLP` does at the hardware level inside the co-processor (does it stop the CAN transceiver? enter STM32 STOP mode? just halt polling?). Freematics' AT-command reference page for the OBD adapter returns HTTP 404 at `freematics.com/pages/products/freematics-obd-adapter-at-commands/`.
- No stated residual co-processor current after ATLP.
- No `telelogger_idf` variant exists in current master (the `firmware_v5` directory contains `can_sniffer`, `datalogger`, `j1939_monitor`, `mpu9250test`, `sim5360test`, `sim7600test`, `simple_gps_test`, `simple_obd_test`, `telelogger`, `uart_forward`), so the ESP-IDF telelogger referenced in the brief could not be examined — it may have been removed or never existed in this repo.

## Q5: Can the co-processor be fully powered down, and what is the residual draw?

### Takeaway
There is **no documented way to power down the co-processor** — only `ATLP` (sleep request over the serial link) and `ATR` (reset, described in `datalogger` as a "delayed sleep"). No source quantifies the residual draw afterwards.

### Cited Findings
- The only two mechanisms in the entire codebase are `ATLP` via `COBD::enterLowPowerMode()` and `ATR` via `FreematicsESP32::resetLink()`; there is no GPIO, no enable pin, and no API for cutting co-processor power — verified by full-repo grep (see Q4) — [stanleyhuangyc/Freematics](https://github.com/stanleyhuangyc/Freematics)
- Wake is performed over the same serial link (`ATI` repeated up to 30 times, or `ATR`), which implies the co-processor's UART/SPI receiver must stay alive in its low-power state — [FreematicsOBD.cpp](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsOBD.cpp)
- Freematics' cellular/network code recognises a modem state string `",Low Power Mode"` alongside `"NO SERVICE"`, `",Online"`, `",Offline"` — indicating the *modem* has its own distinct low-power state reporting, separate from the co-processor — [libraries/FreematicsPlus/FreematicsNetwork.cpp](https://github.com/stanleyhuangyc/Freematics/blob/master/libraries/FreematicsPlus/FreematicsNetwork.cpp)

### Inferences
- Since the ESP32 must be able to wake the co-processor by sending bytes down the link, the co-processor cannot be in a state with its serial peripheral unpowered. That sets a hard floor on co-processor residual current that no amount of firmware work on the ESP32 side can remove.
- Likewise, if the CAN transceiver sits on the co-processor's always-live rail, it cannot be shut off from the ESP32 side either.

### Gaps
- Residual co-processor current after ATLP: **unknown, no source**.
- Whether the co-processor firmware is user-replaceable (and could therefore be modified to cut the transceiver): not established from sources I could access.

## Q6: Reports of Freematics devices draining vehicle batteries

### Takeaway
Battery drain is treated as a real and expected risk by the user community and is the stated reason the power-saving standby exists — but I found **no report with a measured drain rate or a time-to-flat-battery figure**, and no incident report of an actual dead battery attributable to a ONE+.

### Cited Findings
- Traccar forum thread on a Freematics ONE+ that "stops transmitting after 4 hours or so": users attribute this to the device entering standby, with one commenter stating such power-saving functions are "really necessary in order not to drain the battery of your car", and another noting "there is lot of power saving functions that set device to standby" — [Traccar forum](https://www.traccar.org/forums/topic/freematics-one-stops-transmitting-after-4-hours-or-so/)
- The timescale reported in that thread is **3–5 hours from last motion to the device going quiet** — that is the standby *entry* timescale, not a battery-drain timescale — [Traccar forum](https://www.traccar.org/forums/topic/freematics-one-stops-transmitting-after-4-hours-or-so/)
- A documented user workaround was to trigger sleep on voltage instead: configuring sleep to engage only "when battery voltage is lower than 11,5V", plus more frequent ping-backs — [Traccar forum](https://www.traccar.org/forums/topic/freematics-one-stops-transmitting-after-4-hours-or-so/)
- That thread contains **no current consumption measurements in mA or any other units** — [Traccar forum](https://www.traccar.org/forums/topic/freematics-one-stops-transmitting-after-4-hours-or-so/)

### Inferences
- Users reconfiguring away from motion-triggered standby toward a voltage threshold of 11.5 V are effectively disabling the only parked-drain mitigation the firmware has — a configuration that would meaningfully worsen parked drain in a car left for days.
- With the forum offline, the Traccar thread is the only user-community evidence I could reach; the community signal is qualitative only.

### Gaps
- No measured parked-drain rate, no reported battery-flat timescale, no Freematics-attributed dead-battery incident report found.
- The Freematics forum (the natural home for such reports) is offline, so absence of evidence here is weak evidence of absence.

## Cross-cutting notes for the report writer

- **Label discipline:** the only three numbers (20 mA / 50 mA / ~10 mA) are **vendor claims**, published without test conditions. There are **zero measured figures** in any source I could reach.
- **The strongest claim this research supports** is a source-code claim, not a current claim: the official `firmware_v5` standby path does not use ESP32 deep or light sleep, and busy-polls the IMU while servicing BLE (and, in `datalogger`/WiFi builds, WiFi). This is verifiable by anyone from the repo and is the right anchor for a parked-drain argument.
- **The central question — "is the idle floor set by the ESP32 or by the other components?" — has a two-part answer.** For the vendor's hypothetical ~10 mA figure, the floor must be set by non-ESP32 components (co-processor, CAN transceiver, regulators, LED), because an idle ESP32 can be driven to µA. But for the firmware as actually shipped, the ESP32 is never put to sleep, so it is itself a major contributor and quite possibly the dominant one. Both halves should be stated.
- **Highest-value unresolved item:** the ONE+ schematic PDF (referenced from the product page) would settle the co-processor part number, CAN transceiver part, regulator topology and LED wiring in one go. I was not able to retrieve it within this research pass.
- Repo state used for all code claims: fresh `--depth 1` clone of `https://github.com/stanleyhuangyc/Freematics.git` default branch, 2026-10-02.
