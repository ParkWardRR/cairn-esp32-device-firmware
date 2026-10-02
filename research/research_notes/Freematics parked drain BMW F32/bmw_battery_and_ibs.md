# BMW F32 428i battery system, closed-circuit current spec, and IBS / energy-management reaction to added parasitic load

> Scope note up front: BMW's publicly reachable closed-circuit-current bulletin (SI B61 08 00) stops at F01/F02/F07. I could not find a published BMW figure that is *specifically* for F3x (F30/F32/F33/F36). Everything F3x-specific below is either (a) extrapolated from the F01 generation figure, (b) from the BMW-technician trade press, or (c) owner/technician forum measurement — each is labelled.

## Q1. What battery does an F32 428i use from the factory (Ah, chemistry, size, part number)? Base vs start-stop?

### Takeaway
The F3x/F32 platform ships an **AGM** battery in the **DIN H8 / BCI Group 49** case, nominally **90–92 Ah**, mounted in the trunk (right-hand side) with the IBS on the negative post. The genuine BMW part most commonly listed for the F32 4 Series is the **92 Ah AGM, p/n 61216806755**. A 90 Ah AGM (61216924023) also circulates for this family. I did **not** find an authoritative source distinguishing a base-vs-start-stop battery for the 428i specifically.

### Cited Findings
- Genuine BMW **AGM battery, 92 Ah, part number 61216806755** is the complete battery listed under BMW 428i battery parts — [FCP Euro, BMW 428i Battery](https://www.fcpeuro.com/BMW-parts/428i/Battery/)
- The same 61216806755 92 Ah genuine BMW AGM is listed as fitting a wide spread of BMWs (328d, X3, 840i xDrive, Z4, M6 and others), i.e. it is the generic F-chassis H8-case AGM, not a 428i-unique part — [FCP Euro BMW Car Battery listings](https://www.fcpeuro.com/BMW-parts/328d/Car-Battery/)
- The equivalent aftermarket sizing is **Group 49 / H8 / LN5, 92 Ah, 850 CCA, ~170 min reserve capacity** (Bosch S6 AGM S6588B) — [Bosch S6 AGM Group 49 listing via FCP Euro search results](https://www.fcpeuro.com/BMW-parts/328i/Car-Battery/)
- Group 49 / H8 AGM batteries are generally 12 V, ~80–95 Ah, ~890–950 CCA depending on brand (ACDelco AGM Gold 900 CCA; Odyssey 49-950 at 950 CCA) — [Voltloop Group 49 listing](https://voltloop.ca/collections/group-size-49-batteries)
- F3x-generation siblings (F30 328i, 2012–2018) are commonly described as **Group 94R / H7 AGM, ~80–95 Ah, 800+ CCA**; the 2014 435i is described as Group 94R (H7) AGM ~80–95 Ah / ~800 CCA — [tpautorepair, 328i battery](https://tpautorepair.net/what-type-of-battery-does-a-bmw-328i-use/); [tpautorepair, 435i battery](https://tpautorepair.net/what-size-battery-is-in-the-2014-bmw-435i/). **Conflicts with** the H8/92 Ah figure above — see Inferences.
- BMW's own guidance quoted in the coding/registration trade: "If the car is coded to have a 90 Ah AGM installed, it must be replaced with the same and the battery must be registered" — [SF BMW Coding, Replacing a BMW Battery](https://www.sfbmwcoding.com/replacing-a-bmw-battery/)
- The IBS is physically "a Mechatronic component attached to the battery negative lead … mounted to the battery negative terminal", i.e. the negative cable and the IBS are one assembly (BMW p/n 61 12 7 616 200 shown as "Battery cable, negative, IBS") — [The Bimmer Pub, "Energy Management", July 2016, pp. 5–6](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)

### Inferences
- Both H7/94R (~80 Ah) and H8/49 (~90–92 Ah) AGM appear in F3x sources. The most likely explanation is that BMW fitted the **larger H8/90–92 Ah AGM to higher-content / start-stop cars and the H7/80 Ah to lower-content ones**, with the physical tray accepting both. A 428i (N20/N26, US-market, almost always auto-trans with auto start-stop) is far more likely to be the **90–92 Ah H8 AGM**. This is an inference, not a sourced fact — verify by reading the label on the installed battery or by reading the coded battery capacity in ISTA.
- Because the F32 ships AGM regardless, the "AGM vs flooded" question that matters on older BMWs does not arise here — unless the replacement 1.5 years ago was a cheaper flooded/EFB unit, which would itself be a charging-strategy mismatch (see Q5).

### Gaps
- No primary BMW parts-catalogue extract (RealOEM blocks automated fetch, HTTP 403) confirming the exact 428i/F32 battery p/n and whether it varies by option code. **Action for the user: read the sticker on the battery itself** — it states Ah and AGM, and that is the number that must match what is coded.
- No source found that distinguishes battery spec for a start-stop vs non-start-stop 428i.

## Q2. BMW's permissible closed-circuit / quiescent current (Ruhestrom): the mA figure, the time after lock, and the energy-diagnosis procedure

### Takeaway
BMW's published blanket rule in **SI B61 08 00** is that **closed-circuit current consistently over 50 mA must be investigated**, with a per-chassis table; the newest chassis in that table, **F01/F02/F07, is specified at 7–21 mA after 30 minutes**. The BMW technician trade press uses **>80 mA** as the threshold that energy diagnosis treats as a fault cause. Measurement is part of the **"Energy Diagnosis" test plan** in ISTA, and the IBS itself only begins measuring closed-circuit current once the CAS opens the 30g/30B relays, which marks the start of Sleep Mode.

### Cited Findings
- "In general, closed-circuit current consistently over **50 mA** must be investigated." — [BMW SI B61 08 00, Closed-circuit Current Measurement, January 2010, p. 2](https://5series.net/forums/attachments/e60-discussion-2/107876d1276867275-normal-battery-open-circuit-voltage-si-b-61-08-00-closed-circuit-current-measurement.pdf)
- Per-chassis table from the same bulletin (nominal value **after** the stated settling time):
  - E36, Z3 — 30 mA after 16 min
  - E34, E39, E46, E53, E83, E85 — 40 mA after 16 min
  - E31, E32, E38, E52 — 50 mA after 16 min
  - E60/E61/E63/E64, E65/E66 — 40 mA after 60–70 min
  - E82/E88, E90/E91/E92/E93, E89, E70/E71/E72 — **40 mA after 60–70 min with TCU (30 min without TCU)**
  - **F01, F02, F07 — 7–21 mA after 30 minutes**
  — [BMW SI B61 08 00, p. 2](https://5series.net/forums/attachments/e60-discussion-2/107876d1276867275-normal-battery-open-circuit-voltage-si-b-61-08-00-closed-circuit-current-measurement.pdf)
- BMW technician trade press, describing what energy diagnosis flags: causes of a discharged battery "may be a bad battery, **a closed circuit current draw in excess of 80 mA**, or excessive vehicle wake ups due to unauthorized bus activity" — [The Bimmer Pub, "Energy Management", July 2016, p. 5](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- SI B61 08 00 measurement procedure, verbatim essentials:
  1. Diagnose/correct any stored "Power management" faults and **run the "Energy Diagnosis" test plan first** — this is mandatory before measuring current.
  2. Test the battery with the BMW Battery Tester (SI B04 25 02); recharge or replace if needed.
  3. Trunk-mounted battery: open the trunk and turn the lock to the locked position with a screwdriver to simulate a closed lid; **hood must be closed**. (This is the F32 case — battery is in the trunk.)
  4. All other doors/lids closed.
  5. Simulate normal closed-circuit conditions: ignition on, activate all consumers including accessories, ignition off; in some cases a drive cycle is needed to reproduce the fault; open and close the driver's door (simulates someone getting out); lock the car, arming the DWA alarm if fitted.
  6. Compare against the per-chassis table after the stated settling time.
  — [BMW SI B61 08 00, pp. 1–2](https://5series.net/forums/attachments/e60-discussion-2/107876d1276867275-normal-battery-open-circuit-voltage-si-b-61-08-00-closed-circuit-current-measurement.pdf)
- Important diagnostic caveat from the same bulletin: on a car that has broken down with a flat battery, **do not disconnect the battery** — a disconnect resets control modules and a faulty module may then behave correctly, making diagnosis impossible. — [BMW SI B61 08 00, p. 1](https://5series.net/forums/attachments/e60-discussion-2/107876d1276867275-normal-battery-open-circuit-voltage-si-b-61-08-00-closed-circuit-current-measurement.pdf)
- For extended logging BMW uses the **50-amp clip-on probe into IMIB measurement input 3 (green socket)**, run as an oscilloscope channel with Record enabled, time/div up to 200 s — i.e. BMW's own method for a drain that only appears intermittently is a long recording, not a spot multimeter reading. — [BMW SI B61 08 00, pp. 2–3](https://5series.net/forums/attachments/e60-discussion-2/107876d1276867275-normal-battery-open-circuit-voltage-si-b-61-08-00-closed-circuit-current-measurement.pdf)
- Sleep-mode timing and what starts the measurement: "The CAS will open the 30g/30B relays after a specified period of time… the general rule of thumb is the terminal 30g/30B relays will be opened by the CAS **one hour after ignition off**. This time can be shorter if the vehicle does not have a TCU or combox **or if it is an F series that has been double locked**. When the CAS opens the 30g/30B relays, that marks the start of 'Sleep Mode' **and the IBS starts measuring closed circuit current**." — [The Bimmer Pub, "Energy Management", July 2016, p. 7](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- First phase of sleep mode: "The majority of the control modules in the vehicle should assume sleep mode in the first **16 minutes (8 minutes for Boardnet 2020 vehicles)** after terminal 0… added conclusion nonessential consumer cut out occurs." The vehicle gateway re-checks for modules still awake at **5, 10, 15 and 20 minutes**; anything still logged on at 20 minutes is designated a sleep-mode preventer. **F series are Boardnet 2020**, i.e. the 8-minute first phase applies. — [The Bimmer Pub, "Energy Management", July 2016, p. 10](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- Historical false-positive worth knowing about: 2005 MY BMW ASSIST cars show fluctuations as high as **500 mA lasting ~2 minutes, every 15 minutes, for up to 14 hours after key-off** — normal TCU operation, not a fault. — [BMW SI B61 08 00, p. 2](https://5series.net/forums/attachments/e60-discussion-2/107876d1276867275-normal-battery-open-circuit-voltage-si-b-61-08-00-closed-circuit-current-measurement.pdf)
- Independent (non-BMW) rule of thumb: acceptable parasitic draw is typically 20–50 mA, with newer heavily-electronic vehicles acceptable closer to 85 mA; sleep can take **15–45 minutes** depending on vehicle — [EngineerFix, What Is Considered a Parasitic Draw?](https://engineerfix.com/what-is-considered-a-parasitic-draw/)
- CTEK's figures for modern cars: "50-milliamps to 85-milliamps is considered typical" on newer vehicles; "less than 50-milliamps is normal" on older ones — [CTEK, How parasitic drain affects your car battery](https://ctek.com/en-uk/blogs/ctek-magazine/how-parasitic-drain-affects-your-car-battery)

### Inferences
- **The 50 mA "investigate" line and the 80 mA "fault" line are not the same thing.** 50 mA is SI B61 08 00's trigger for *investigation* across all chassis; 80 mA is the trade-press description of what energy diagnosis treats as a *cause of discharge*. Treating 80 mA as "the BMW spec" overstates the allowance for an F-chassis car; treating 7–21 mA as the F3x spec is the conservative reading.
- Because the only F-chassis entry in BMW's table (F01/F02/F07) is **7–21 mA after 30 minutes**, an F32 almost certainly has a factory sleep current in the **same single-to-low-double-digit mA band**, not 50–85 mA. This matters a lot: a 20–30 mA added load is not "a small fraction of the allowance" — it is likely **1–4× the entire factory sleep current**, and would on its own push the measured closed-circuit current past the F01-generation nominal band. It would still be below the 50 mA blanket investigate line if the car's baseline is ~10 mA.
- Practical measurement implication for anyone adding a device: after fitting, re-measure closed-circuit current using the SI B61 08 00 trunk-lock/hood-closed/lock-the-car ritual, wait at least 30–70 minutes, and compare against the pre-fit baseline — the delta is the meaningful number, not the absolute.

### Gaps
- **No published BMW closed-circuit-current figure specifically for F30/F32/F33/F36.** SI B61 08 00's last revision (January 2010) predates the F3x launch. ISTA's F3x energy-diagnosis test plan presumably carries a model-specific target, but I found no reachable copy of it. Bimmerpost and Bimmerfest threads that would contain technician quotes returned HTTP 403 / paywall redirects to automated fetching.
- I found no source for a BMW figure expressed as "permissible Ruhestrom in mA" in the German-language TIS wording; the English SI is the primary document I could verify.

## Q3. How much charge can be drawn before the battery fails to crank? DoD rule of thumb, resting voltage vs SoC, and where a modern BMW refuses to start or sheds loads

### Takeaway
Rule of thumb (not a manufacturer spec): **treat ~50% depth of discharge as the practical floor for a starting battery**, which for a 12 V AGM corresponds to roughly **12.2–12.3 V open-circuit after a long rest**. BMW does not expose a simple "won't start below X volts" number — instead the DME computes a **start capability limit** from SoH, and the IBS wakes the DME when the battery reaches it; load shedding of comfort functions happens before that point.

### Cited Findings
- AGM open-circuit voltage vs state of charge (consolidated from battery-reference tables): **100% ≈ >12.8 V; 75% ≈ 12.6 V; 50% ≈ 12.3 V; 25% ≈ 12.0 V; 0% ≈ <11.8 V**; an alternative table gives 100% = 12.8–13.2 V, 75–100% = 12.6–12.8 V, 50–75% = 12.3–12.6 V, 25–50% = 12.0–12.3 V, 0–25% = ≤12.0 V — [Discover Battery, How can you tell if a battery is fully charged](https://discoverbattery.com/support/learning-center/battery-101/how-can-you-tell-if-a-battery-is-fully-charged)
- True open-circuit voltage can only be measured **after the battery has been off charge and off load for ~24 hours**; a surface charge will read high — [Discover Battery](https://discoverbattery.com/support/learning-center/battery-101/how-can-you-tell-if-a-battery-is-fully-charged); see also [EngineerFix, What should a 12 V battery read when fully charged](https://engineerfix.com/what-should-a-12-volt-battery-read-when-fully-charged/)
- **12.4 V is the commonly cited sulfation onset threshold**: "if a battery drops below 12.4 volts, sulfation begins to set in" — [CTEK, How parasitic drain affects your car battery](https://ctek.com/en-uk/blogs/ctek-magazine/how-parasitic-drain-affects-your-car-battery)
- AGM cycle life vs DoD, from a manufacturer datasheet (deep-cycle AGM, not an automotive starting AGM): **≥300 cycles at 100% DoD, ≥700 cycles at 60% DoD, ≥1000 cycles at 40% DoD** — [Victron Energy AGM Super Cycle datasheet](https://www.victronenergy.com.au/batteries/agm-super-cycle-battery)
- BMW's actual crank-threshold mechanism: "The DME uses the SOH factor to set the **start capability limit** and after-start recovery charge strategy… The start capability limit is the minimum voltage required to re-start the vehicle based on the state of health of the battery. There are typically two start capability limits, an upper and lower… These limits are transmitted to the IBS from the DME **prior to the DME entering sleep mode. The IBS will monitor the battery voltage level and wake the DME if the limit has been reached.**" — [The Bimmer Pub, "Energy Management", July 2016, pp. 6–7](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- Load shedding is layered. First, in the first 8–16 minutes of sleep, "added conclusion nonessential consumer cut out occurs… any interior lights, map lights, convenience lighting and convenience features are turned off." Second, the 30g_f/30F bistable relays are "switched off in the event of a fault such as unauthorized bus wake up, **the start capability limit being reached, closed circuit current violation**, or if sleep mode preventers are identified by the vehicle gateway." The 30g_f/30F shutdown "starts with a 10 second reset." — [The Bimmer Pub, "Energy Management", July 2016, pp. 8, 10](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- The "Increased battery discharge while stationary" message means the DME detected the battery voltage dropping and has shut off non-essential systems — power windows, mirrors, seats, sunroof etc. — to preserve it; the power-management system also reduces seat heating output and disables rear defrost as SoC falls — [YouCanic, BMW Increased Battery Discharge Explained](https://www.youcanic.com/bmw-increased-battery-discharge/); [Euro Premium Parts, BMW Increased Battery Discharge](https://europremiumparts.com/blogs/buying-guides/bmw-increased-battery-discharge-causes-what-it-means-and-how-to-diagnose-it-1)
- Forum rule-of-thumb arithmetic in the same spirit: "50 Ah at 0.25 A drain will require approx 9 days before the battery reaches 50% discharge" — [Bimmerfest, Ensuring Sleep mode for Parasitic Battery Drain Testing](https://www.bimmerfest.com/threads/ensuring-sleep-mode-for-parasitic-battery-drain-testing.967579/)

### Inferences
- **The practically usable reservoir on a 92 Ah AGM before cranking is at risk is ~46 Ah (50% DoD), and in reality less** for two reasons: (a) CCA falls with SoC and with cold, so a 50%-charged battery at -10 °C may fail to crank an N20 even though 50% is "fine" at 20 °C; (b) the car does not park at 100% SoC — short trips leave it at 70–85%, so the headroom to the 50% floor is often only ~25–35 Ah.
- **"Won't start" on an F32 is a computed, not a fixed, threshold.** Because the limit is derived from SoH, an older/degraded battery hits its start-capability limit at a *higher* resting voltage than a new one. Translation for the user's case: the risk of an added drain is not "will it cross 11.8 V" — it is "will it cross whatever the DME currently thinks the start-capability limit is", which tightens as the battery ages.
- The ordering is: comfort load shedding → "increased battery discharge" message → 30F relay shutdown → no-start. A user adding a drain would typically see the message well before an actual no-start, which is a useful early-warning signal.

### Gaps
- No manufacturer cycle-life-vs-DoD data for an **automotive starting AGM** (Varta/Exide/Bosch) — the Victron numbers cited are a deep-cycle AGM and will overstate cycle life for a thin-plate starting AGM. Treat them as directionally correct only.
- BMW does not publish the numeric start-capability-limit voltages; they are SoH- and temperature-dependent and are not a constant.

## Q4. How the IBS works, what it measures, and what an unregistered/unknown parasitic load does to its SoC estimate

### Takeaway
The IBS is a mechatronic sensor on the negative battery post measuring **voltage in/out, current in/out, and battery temperature under all operating conditions**; it computes **SoC** locally and feeds the DME, which computes **SoH** from the voltage drop during cranking. The IBS *does* see an added constant load — current through the negative cable is exactly what it measures — so a small added drain is **integrated into the SoC estimate rather than invisible to it**. The failure mode is not SoC drift; it is that the extra coulombs come out of the battery's reserve and that the closed-circuit-current violation can itself trip the 30F shutdown.

### Cited Findings
- "The IBS is a Mechatronic component attached to the battery negative lead. It is mounted to the battery negative terminal and has power supply from a fused connection to the B+ terminal of the battery. It has the function of **measuring battery voltage in, battery voltage out, current in, current out, and battery temperature under all operating conditions.**" — [The Bimmer Pub, "Energy Management", July 2016, pp. 5–6](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- "The IBS performs minor calculations and forwards its data directly to the DME. The IBS determines the **state of charge (SOC)** of the battery and is a critical component in determining the battery **state of health (SOH)**. SOC… is the open circuit voltage value or the amount of energy left in the battery. It has some diagnostic value but doesn't indicate the battery's ability to deliver current." — [ibid., p. 6](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- "The DME calculates the battery SOH based on the voltage drop across the battery, measured by the IBS, **during engine starting**… The software in the DME processes those values to determine the battery's impedance ('effective resistance') and conductance and derives the SOH value. This process should be familiar to anyone who has performed a load test on a battery with a VAT40." — [ibid., p. 6](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- The DME uses the IBS data to "increase or decrease the output from the alternator, increase the idle speed to assist with alternator output if needed, and request reduction or termination of nonessential loads if the demands on the electrical system are too great." — [ibid., p. 5](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- A **closed circuit current violation is an explicit trigger** for the 30g_f/30F relay shutdown, alongside unauthorized bus wake-up and start-capability-limit breach — [ibid., p. 8](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- All control-module wake-ups are logged by the vehicle gateway; only **CAS and the vehicle gateway (SGM/KGM/JBE/ZGM, or FEM/BDC on Boardnet 2020)** are authorized wake-up modules — anything else waking the car is an "unauthorized wake up". The KOMBI appearing intermittently in the wake-up list is normal (it wakes to check ambient temperature for cold-start emissions and energy-management calculations) and can generally be disregarded. — [ibid., pp. 8, 10](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- Common BMW parasitic-draw offenders named in the field: CIC/NBT head unit, comfort access module, TCU/telephone module, panoramic roof module — [Euro Premium Parts](https://europremiumparts.com/blogs/buying-guides/bmw-increased-battery-discharge-causes-what-it-means-and-how-to-diagnose-it-1)

### Inferences
- **Whether the IBS "sees" an added load depends entirely on where it is tapped.** Anything wired downstream of the IBS shunt (i.e. returning through the chassis/negative post the normal way, or fed from a fuse box) is measured and correctly integrated — SoC stays honest. Anything wired **directly to the battery negative post on the battery side of the IBS** bypasses the shunt, and that current is genuinely invisible to the IBS. **That is the one wiring mistake that causes real SoC mis-learn**, because the IBS will believe the battery holds more charge than it does. This is an inference from the sensor's described construction, not a statement I found verbatim in a BMW document, but it follows directly from a shunt-on-the-negative-lead topology.
- A correctly-tapped added load will **not** corrupt SoC. It will, however, change the car's measured closed-circuit current, which is a separately monitored quantity, and a large enough delta is a "closed circuit current violation" the gateway can act on.
- SoH is computed from cranking voltage drop and is therefore **unaffected by a parked drain directly** — but indirectly a chronically lower SoC produces a larger cranking voltage drop, which the DME reads as worse SoH, which tightens the start-capability limit. So a persistent drain can make the car *believe* the battery is older than it is.

### Gaps
- I found no BMW document stating a numeric SoC-estimation error band, nor any statement about IBS re-learning behaviour in the presence of an unknown load. Claims that an added drain "confuses" the IBS are, as far as I can tell, folklore not traceable to BMW documentation.

## Q5. Does BMW require battery registration, and what goes wrong if it wasn't done? Interaction with charge management

### Takeaway
Yes — BMW requires registration (F-series via ISTA/D or Rheingold), and the failure mode is a **charging strategy matched to the old battery**: the DME keeps applying an aged-battery charge profile to a new battery, which sources describe as overcharging an AGM and killing it, sometimes within 6–12 months. Given the user's battery was replaced ~1.5 years ago, **confirming it was registered — and registered as AGM with the correct Ah — is the single highest-value check** before worrying about a 10–30 mA load.

### Cited Findings
- "BMW vehicles have an intelligent battery system (IBS) charging routine that tracks power usage and determines how long and when to run the car's alternator… battery registration is crucial to ensure that the DME recognizes the new battery and optimizes the charging system accordingly." — [YouCanic, BMW Battery Registration](https://www.youcanic.com/wiki/bmw-battery-registration-programing-procedure)
- Consequences of not registering, per an independent repair-advice source: "could shorten the life of an unregistered battery by charging it too aggressively when it's cold; it also could shorten the life of an unregistered **AGM** battery by **overcharging** it"; also inaccurate iDrive warning messages and, rarely, a no-start — [Rick's Free Auto Repair Advice, Why you have to register a new battery in your BMW](https://ricksfreeautorepairadvice.com/register-new-bmw-battery/)
- "Without registration after replacement, the battery may be dead within 6 months… the car continues using outdated charging parameters designed for the old battery." — [SF BMW Coding](https://www.sfbmwcoding.com/replacing-a-bmw-battery/)
- Battery type matters, not just capacity: "If you use a different spec battery than the one that was originally installed… additional coding [is needed] in order to specify the battery **type and capacity** for it to be charged correctly, especially for AGM type batteries." — [SF BMW Coding](https://www.sfbmwcoding.com/replacing-a-bmw-battery/)
- F-series registration path in ISTA/Rheingold: Operations > Vehicle ID > Vehicle Management > Service Function > Body > Voltage Supply > Battery > Register Battery Change > execute ABL register battery — [SF BMW Coding](https://www.sfbmwcoding.com/replacing-a-bmw-battery/)
- Field reports of unregistered batteries not lasting a year — [5series.net, "Another battery bites the dust"](https://5series.net/forums/e60-discussion-2/another-battery-bites-dust-97445/page4)
- "Increased battery discharge" is explicitly a power-management action, not just a warning: the DME has already shut off non-essential consumers when it displays — [YouCanic](https://www.youcanic.com/bmw-increased-battery-discharge/)
- A weak/aging battery is the most frequent cause of the message; beyond 4–5 years internal resistance rises and the battery can no longer hold or accept a full charge — [Euro Premium Parts](https://europremiumparts.com/blogs/buying-guides/bmw-increased-battery-discharge-causes-what-it-means-and-how-to-diagnose-it-1)

### Inferences
- The registration question and the parasitic-load question **compound**. An unregistered battery is charged on a profile intended for a battery the DME believes is 5+ years old, which in practice means a less complete recharge per drive cycle; adding a continuous drain on top of an already-undercharged battery is where sulfation actually bites. Conversely, a correctly registered, healthy 92 Ah AGM has substantial margin.
- Because registration resets the DME's assumed battery age, a battery registered 1.5 years ago will have an accurate age model and should charge correctly. If it was *not* registered, the DME's model is now ~1.5 years further out of date on top of the already-wrong starting point.
- Note the source quality here: the registration claims come from independent shops and repair-advice sites, not from a BMW SI I could obtain. The *requirement* is well attested; the specific "dead in 6 months" figure should be treated as a shop's anecdote, not a spec.

### Gaps
- No BMW Service Information bulletin on battery registration obtained directly. The procedural detail and the ISTA menu path are from an independent coding shop.

## Q6. What does an F32 normally draw once asleep, and how long can it sit?

### Takeaway
Extrapolating from BMW's only published F-chassis figure, expect a healthy F32 to settle to roughly **7–25 mA** 30–70 minutes after locking. On a 92 Ah AGM that is about **0.2–0.6 Ah/day**, giving a theoretical **2.5–7+ months** to 50% DoD from a full charge — but real-world advice converges on **3–6 weeks** as the point at which an F-series should be driven or put on a maintainer, because the car never parks at 100% and self-discharge plus occasional wake-ups add to the budget.

### Cited Findings
- BMW's published figure for the nearest F-chassis: **F01/F02/F07 = 7–21 mA after 30 minutes** — [BMW SI B61 08 00, p. 2](https://5series.net/forums/attachments/e60-discussion-2/107876d1276867275-normal-battery-open-circuit-voltage-si-b-61-08-00-closed-circuit-current-measurement.pdf)
- Owner clamp-meter measurement on a BMW in sleep: **~25 mA consistently**, considered acceptable — [Bimmerfest threads on BMW parasitic drain](https://www.bimmerfest.com/threads/bmw-e90-parasitic-drain.1450125/)
- Forum consensus framing: "A healthy draw on the battery will be between 20–50 mA" — [Bimmerfest, how much battery drain is normal](https://bimmerfest.com/forums/showthread.php?t=384281)
- Forum arithmetic for the sitting-time question: at 0.25 A on a 100 Ah battery, ~9 days to 50% discharge; a good battery with a normal draw "should [not] fail to start a BMW even after a month sitting idle" — [Bimmerfest, Ensuring Sleep mode for Parasitic Battery Drain Testing](https://www.bimmerfest.com/threads/ensuring-sleep-mode-for-parasitic-battery-drain-testing.967579/)
- Sleep is not instantaneous: the 30g/30B relays open roughly **one hour after ignition off**, sooner if the car is double-locked or lacks a TCU/combox; until then the car is drawing far more than its sleep current — [The Bimmer Pub, "Energy Management", July 2016, p. 7](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)

### Inferences (arithmetic — all of this is my calculation, not a sourced figure)
Assume a 92 Ah AGM, 50% DoD floor = 46 Ah usable from full:

| Total sleep current | Ah/day | Days from 100% SoC to 50% DoD | Days from a realistic 80% SoC to 50% DoD (27.6 Ah) |
|---|---|---|---|
| 10 mA (factory, best case) | 0.24 | ~190 | ~115 |
| 20 mA (factory, upper) | 0.48 | ~96 | ~58 |
| 20 mA + 10 mA added | 0.72 | ~64 | ~38 |
| 20 mA + 30 mA added | 1.20 | ~38 | ~23 |
| 50 mA (BMW "investigate" line) | 1.20 | ~38 | ~23 |
| 85 mA (CTEK "typical modern") | 2.04 | ~23 | ~14 |

- The headline: **adding 30 mA to a ~20 mA baseline roughly halves to one-third the safe parking window** — from ~2 months to ~3 weeks on a realistically-charged battery. Adding 10 mA costs maybe a third of the window.
- These numbers ignore self-discharge (an AGM at rest loses roughly 1–3%/month at 20 °C, more when warm) and ignore wake-up events, so treat them as optimistic ceilings.
- For a daily/weekly-driven car, **none of this matters**: a 30 mA load costs 0.72 Ah/day, which the alternator replaces in a few minutes of driving. The risk is entirely concentrated in long parked periods — airport trips, winter layup, illness, travel.

### Gaps
- No published BMW figure for F3x sleep current. No measured F32-specific number from a source I could fetch (Bimmerpost returns 403 to automated retrieval).

## Q7. Does a battery tender/maintainer neutralise a small parasitic draw?

### Takeaway
**Yes, essentially completely** — for the parked-car case, a modern AGM-capable maintainer on float makes a 10–30 mA parasitic draw a non-issue, because a maintainer sources far more current than the draw consumes and holds the battery above the 12.4 V sulfation threshold indefinitely. The caveat is that a maintainer only helps while it is connected; it does nothing for the user who parks at an airport.

### Cited Findings
- "If a battery goes long periods without being recharged by the alternator, those tiny milliamps drawn by parasitic devices will kill it… if a battery drops below 12.4 volts, sulfation begins to set in." — [CTEK, How parasitic drain affects your car battery](https://ctek.com/en-uk/blogs/ctek-magazine/how-parasitic-drain-affects-your-car-battery)
- CTEK chargers "use a series of 4 to 8 patented charging and maintenance stages… reviving, charging, conditioning, **desulfation**, and maintaining"; for a vehicle in storage they "supply a pulse charge when it senses that it is needed" — [CTEK](https://ctek.com/en-uk/blogs/ctek-magazine/how-parasitic-drain-affects-your-car-battery)
- A battery maintainer "will deliver a controlled low current to keep a vehicle's battery in peak condition, making it ideal for vehicles which are parked for extended periods" — [CTEK, Car battery maintainers](https://ctek.com/sv-se/blogs/ctek-magazine-se/car-battery-maintainers-keep-your-battery-healthy-all)
- AGM compatibility is explicit: the CT5 START/STOP is "specifically designed for AGM/EFB Start/Stop batteries"; the CTEK MULTI US 4.3 covers "standard 12V Lead-Acid batteries (including premium AGM batteries)" — [CTEK](https://ctek.com/en-uk/blogs/ctek-magazine/how-parasitic-drain-affects-your-car-battery)
- Recommended AGM storage/float voltage band from a battery manufacturer: **13.2–13.5 V** — [Victron Energy AGM Super Cycle](https://www.victronenergy.com.au/batteries/agm-super-cycle-battery)
- Smart maintainers "switch to a float charge (around 13.2 V for 12 V lead-acid) when full, which prevents thermal cycling stress" — [CTEK magazine / maintainer guidance](https://ctek.com/sv-se/blogs/ctek-magazine-se/car-battery-maintainers-keep-your-battery-healthy-all)
- BMW-specific practical note: FCP Euro lists CTEK and NOCO chargers under BMW 428i battery parts, i.e. these are the mainstream choices for this chassis — [FCP Euro, BMW 428i Battery](https://www.fcpeuro.com/BMW-parts/428i/Battery/)

### Inferences
- Magnitude check: the smallest common maintainers output **0.75–1.5 A**, which is **25–150× a 10–30 mA parasitic draw**. A maintainer is not "offsetting" the draw at the margin — it is overwhelming it, and will spend nearly all its time in float.
- **Connect the maintainer at the jump-start posts under the hood, not at the battery**, on an F3x. Connecting at the battery terminals is the conventional advice, but on a BMW with an IBS, charging through the IBS shunt is what keeps the energy-management model in sync; the engine-bay jump posts are routed through the normal path. (Inference from the IBS topology, not from a fetched BMW document — worth verifying against the owner's manual, which on F-series does specify the engine-bay jump posts for charging.)
- Net judgement: **a tender does not change whether the parasitic load exists, but it removes the only mechanism by which a 10–30 mA load causes harm** (prolonged time below 12.4 V). If the car lives on a tender when parked for more than a week, the added drain is effectively free.

### Gaps
- No BMW SI obtained stating the official charging-connection point or any prohibition on maintainers; the F-series owner's manual would be the authority.

## Q8. Does ~10–30 mA of added constant load meaningfully change AGM life or sulfation risk?

### Takeaway
**Not for a regularly-driven car; potentially yes for a car that sits for a month or more at a time.** The mechanism of harm is not the current itself — it is the extra time spent below ~12.4 V, which is where sulfation starts. 10–30 mA is a rounding error against driving-cycle recharge but can turn a 2-month safe parking window into a 3-week one.

### Cited Findings
- Sulfation onset threshold: below **12.4 V** — [CTEK](https://ctek.com/en-uk/blogs/ctek-magazine/how-parasitic-drain-affects-your-car-battery)
- Sulfation mechanism: "occurs when a lead-acid battery is left in a partially or fully discharged state for an extended period, with lead sulfate hardening into large, stable crystals that are difficult to convert back during recharging" — [Victron Energy AGM documentation](https://www.victronenergy.com.au/batteries/agm-super-cycle-battery)
- "Limiting the depth of discharge is paramount, as frequent deep discharges significantly reduce the battery's lifespan"; AGM cycle life scales steeply with DoD (≥300 cycles @ 100% DoD vs ≥1000 @ 40% DoD) — [Victron Energy AGM Super Cycle datasheet](https://www.victronenergy.com.au/batteries/agm-super-cycle-battery)
- Modern AGM chemistries "use new additives to the electrolyte that reduce sulfation in case of deep discharge" — [Victron Energy](https://www.victronenergy.com.au/batteries/agm-super-cycle-battery)
- Independent framing of what is acceptable: 20–50 mA typical, up to ~85 mA acceptable on heavily-electronic modern cars — [EngineerFix](https://engineerfix.com/what-is-considered-a-parasitic-draw/); [CTEK](https://ctek.com/en-uk/blogs/ctek-magazine/how-parasitic-drain-affects-your-car-battery)
- A healthy battery with normal draw should still start a BMW after a month idle — [Bimmerfest](https://www.bimmerfest.com/threads/ensuring-sleep-mode-for-parasitic-battery-drain-testing.967579/)

### Inferences
- **Energy framing:** 30 mA × 24 h = 0.72 Ah/day ≈ **0.8% of a 92 Ah battery per day**. A 20-minute drive at even a modest 20 A net charge current returns ~6.7 Ah — roughly nine days' worth of a 30 mA drain. For any car driven weekly, the added load is recovered many times over.
- **Where it actually hurts:** the AGM's calendar life is governed by how long it sits below ~12.4 V, not by total Ah throughput at these currents. A 30 mA load does not meaningfully add cycles; it adds *time below threshold* during long parks. On a car driven weekly and/or tended during long parks, the expected effect on AGM life is **negligible and probably unmeasurable**.
- **Risk tiers, in order of severity:** (1) drain tapped on the battery side of the IBS shunt — corrupts the energy model, worst case; (2) battery never registered after the 1.5-year-old replacement — chronic undercharge, worse than any 30 mA load; (3) 30 mA drain + car parked >1 month in winter with no tender — real risk; (4) 10–30 mA drain on a weekly-driven, tended, registered car — negligible.
- **A defensible design budget:** keep any added continuous load at or below **~20 mA**, ideally with a sleep/deep-sleep mode so the device drops to single-digit mA once it detects no bus activity. That keeps the car's total closed-circuit current under BMW's 50 mA "investigate" line even assuming a ~20 mA factory baseline, and under the 80 mA energy-diagnosis fault threshold with large margin. If the device can also drop out entirely below a configured battery voltage (e.g. cut itself off at 12.2 V), the sulfation risk goes to essentially zero.

### Gaps
- I found **no manufacturer or BMW study quantifying battery-life reduction as a function of added parasitic current.** Everything in this section is mechanism-based reasoning from the sulfation-threshold and DoD-vs-cycle-life data, not a measured life-expectancy figure. Anyone wanting a hard number on "how many months of life does 30 mA cost" will not find one — it does not appear to exist publicly.
- No automotive-starting-AGM (Varta/Exide/Bosch) datasheet obtained; the DoD/cycle-life figures are from a deep-cycle AGM and are optimistic for a thin-plate starting battery.
