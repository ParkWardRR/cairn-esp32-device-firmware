# BMW F3x (F30/F32/F33/F36) OBD-II Port Power, Bus Sleep, and Dongle-Induced Drain

**Scope note:** The single best primary-quality source found is a BMW trade-press technical feature, "Energy Management" (*the bimmer pub*, July 2016), which explicitly covers "Boardnet 2020" vehicles — BMW's internal name for the F-series electrical architecture, i.e. the F30/F32 generation — and names FEM and BDC as the F-series gateway/body modules. That document is the backbone of the sleep-mode findings below. Forum sources (bimmerpost/bimmerfest) repeatedly 403'd or paywalled (tollbit) to automated fetch, so several forum claims below are only available via search-engine snippets and are flagged as lower confidence. Where a finding is from a different BMW generation (E53/E6X/E9X/i3) or another manufacturer, it is labelled.

---

## Q1: Is OBD-II pin 16 permanently live on BMW F3x? Does it ever switch off?

### Takeaway
Pin 16 on the F3x OBD connector is fed from terminal 30 (direct battery positive) and is, for practical purposes, permanently live with the ignition off and the car locked — it is not a terminal 30B/30F switched circuit under normal conditions. The one documented way it can lose power is the fault-driven terminal 30F (30g_f) bistable-relay shutdown, which BMW's energy management triggers after detecting a closed-circuit-current violation, unauthorized bus wake-ups, or a sleep-mode preventer. I did **not** find an authoritative F3x wiring-diagram confirmation of the exact fuse, so treat the "always live" claim as high-confidence-but-not-primary-sourced.

### Cited Findings
- Pin 16 constant (never switched) 12 VDC is required by the OBD-II specification; for BMW specifically, pin 16 carries 12 V and this supply is not switched off when the vehicle enters sleep mode — [search-result synthesis across OBD power threads](https://nyc1.lr.ggtyler.dev/r/CarHacking/comments/1fyjhry/power_and_data_via_obd) *(aggregated snippet, not a primary BMW document — flagged as medium confidence)*
- BMW terminal 30 is "a line from the battery positive terminal direct" and connects control units and components that must continue to function up to the point when the battery is completely discharged — ["Energy Management", the bimmer pub, July 2016, p.7](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- Terminal 30g (designated **30B in F series**) is a *time-controlled* terminal shutdown: modules on 30g receive power when the vehicle is on and remain powered for a specified time after shutdown. Terminal 30B is what the cigarette-lighter socket is connected to, and on some BMWs is powered for only about 3 minutes after locking — ["Energy Management", p.7](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf); [spoolstreet BMW terminal explanation thread (snippet only; page returns 403)](https://www.spoolstreet.com/threads/bmw-battery-terminals-explanation-15-15n-15wup-30-30g-30g_f-30b-30f-50.5630)
- The Car Access System (CAS) opens the 30g/30B relays after a specified period from terminal 0; general rule of thumb is **one hour after ignition off**, and "this time can be shorter if the vehicle does not have a TCU or combox **or if it is an F series that has been double locked**" — ["Energy Management", p.7](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- Terminal 30g_f, known as **30F in F series**, uses *bistable* relays that "are normally on and are only switched off in the event of a fault such as unauthorized bus wake up, the start capability limit being reached, closed circuit current violation, or sleep mode preventers ... identified by the vehicle gateway (SGM, KGM, JBE, ZGM, FEM or BDC)" — ["Energy Management", p.8](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- The 30F shutdown sequence: "starts with a 10 second reset. After the reset the IBS and/or vehicle gateway will continue to monitor for continued activity. If the condition that caused the reset continues, the 30g_f/30F relays will be opened until the next authorized vehicle wake up and a fault/information flag will be stored." — ["Energy Management", p.8](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- On a BMW i3 (different generation, EV), an owner reported "My car's OBD currently goes off after a while whenever it's not READY (to drive)" — [Steve Davies, OVMS developer mailing list](https://lists.openvehicles.com/archives/list/ovmsdev@lists.openvehicles.com/thread/YC3BF6GHJUARTKLKLZCA6FH56J2KZIOX/)
- A forum poster states "for a lot of recent BMW models it is necessary to go direct to the battery for the permanent feed as the computer can disable any circuit which has a load that is not meant to be there" — [DashcamTalk, hardwiring Viofo A139 to 2015 F36 4 Series Gran Coupe](https://dashcamtalk.com/forum/threads/hardwiring-viofo-a139-to-2015-bmw-4-series-gran-coupe-f36.44395/post-577595)

### Inferences
- The F3x OBD connector's pin 16 is almost certainly on terminal 30 (permanent), not 30B — otherwise the dongle-drain phenomenon widely reported on F-series cars could not occur past the ~1 hour / double-lock 30B shutdown.
- BMW's 30F mechanism means the *car itself* has a defence: a dongle that provokes a closed-circuit-current violation or repeated unauthorized wake-ups can cause the 30F relays to drop and a fault flag to be stored. This is the mechanism by which an F32 would log an "energy diagnosis"-visible fault rather than silently draining.
- The i3 "OBD goes off" behaviour should **not** be assumed to transfer to the F32 — the i3 is a different (I01) platform with EV-specific power management.

### Gaps
- No F3x-specific wiring diagram or fuse designation for the OBD socket supply was found. The exact fuse number and whether it is terminal 30 or 30B on F30/F32 remains unverified from a primary source.
- Whether the 30F shutdown actually de-powers the OBD socket itself (vs only the other 30F-fed loads) is not stated in any source found.

---

## Q2: Which buses are on the F3x OBD connector, and which ECU gates diagnostic access?

### Takeaway
On F3x the OBD connector carries **D-CAN on pins 6 and 14** (500 kbit/s), with the FEM/BDC containing the ZGM central gateway that translates between D-CAN and the internal PT-CAN, K-CAN and FlexRay buses. Pins 3 and 11 are the Ethernet (ENET) pair used for programming. The dongle therefore does **not** sit directly on the body or powertrain bus — it sits on a stub that only the gateway answers, which is precisely why polling it forces the gateway (and everything behind it) awake.

### Cited Findings
- D-CAN is the diagnostic CAN at 500 kbit/s and is the bus present in the OBD2 port; PT-CAN (powertrain, 500 kbit/s) and K-CAN (body, 100 kbit/s) are separate buses not directly routed to the OBD connector on all models; the gateway module filters and routes messages between them — [search synthesis over BMW CAN bus references](https://github.com/dzid26/opendbc-BMW-E8x-E9x)
- CAN-High is pin 6, CAN-Low pin 14; measuring pins 6 and 14 at the OBD port should read 60 ohms (two 120-ohm terminators in parallel) confirming D-CAN continuity on cars like the F30 — [f30.bimmerpost D-CAN diagnosis thread (snippet; page 403s to fetch)](https://f30.bimmerpost.com/forums/showthread.php?p=30748289)
- On the F30, the two D-CAN wires land at the **FEM** (connector 8B pins 45 and 46); "ZGM (inside the FEM) is the conductor that 'translates' between the various buses, including ENET" — [f30.bimmerpost thread (snippet; page 403s to fetch)](https://f30.bimmerpost.com/forums/showthread.php?p=30748289)
- BMW vehicle diagnostics rely on networking of control units via PT-CAN, K-CAN, FlexRay and, in newer series, Ethernet; ISTA knows the topology per VIN and can distinguish whether a communication error is at a control unit, on the bus, or at the power supply — [kfz-dietrich, BMW ISTA Diagnose](https://kfz-dietrich.com/blog/bmw-ista-diagnose-ablauf-was-geprueft-wird/)
- BMW's vehicle gateway on Boardnet 2020 (F-series) cars is the **FEM or BDC**; earlier gateways were SGM, KGM, JBE, ZGM — ["Energy Management", p.8](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- ISO 13400-4 (DoIP) addresses manufacturer-specific use of the OBD connector's discretionary pins including pins 3 and 11 — [ISO 13400-4](https://www.iso.org/standard/57317.html)

### Inferences
- An OBD dongle on an F32 is electrically on the D-CAN stub, whose only mandatory participant is the FEM/BDC gateway. A passive dongle therefore sees **nothing** on D-CAN when the car is asleep (no traffic exists on a diagnostic bus unless a tester is present) — this is important: a "passive listener" on an F32 gets no data at all while parked, which is a strong argument that any dongle logging data while parked *must* be polling.
- To get any data at all while parked, a dongle must transmit a request, which must wake the FEM/BDC gateway, which is the single module that gates access to K-CAN/PT-CAN/FlexRay — so there is no way to poll "a little bit" without waking the body domain controller.

### Gaps
- I could not confirm from a primary BMW wiring source whether F3x routes any additional raw bus (e.g. K-CAN2) to spare OBD pins. Several aftermarket vendors imply K-CAN is reachable on 6/14 on some BMWs, which conflicts with the D-CAN-only picture; this conflict is unresolved.

---

## Q3: Do F3x buses sleep after a timeout, and what is the mechanism/timescale?

### Takeaway
Yes, and the F-series timings are explicitly documented. On "Boardnet 2020" (F-series) cars the **first phase of sleep mode is 8 minutes** after terminal 0 (vs 16 minutes on older Boardnet 2000 cars). The gateway polls the bus at **5, 10, 15 and 20 minutes** after terminal 0, and any module still logged on at the 20-minute check is formally designated a **"sleep mode preventer."** Sleep mode proper — the point at which the IBS begins measuring closed-circuit current — starts when CAS opens the 30g/30B relays, nominally one hour after ignition off, sooner if double-locked.

### Cited Findings
- "The majority of the control modules in the vehicle should assume sleep mode in the first 16 minutes (**8 minutes for Boardnet 2020 vehicles**) after terminal 0. This time frame, 16 or 8 minutes, is considered the first phase of sleep mode, and added conclusion nonessential consumer cut out occurs." — ["Energy Management", p.10](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- "The vehicle gateway module checks the vehicle bus system for modules that are communicating/still awake at **5, 10, 15 and 20 minutes** after terminal 0. Any module that is still logged on at the 20 minute interval is designated a sleep mode preventer since it should have logged off by the 16 minute time frame." — ["Energy Management", p.10](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- "That strategy is shortened up a little bit for the Boardnet 2020 vehicles since it uses an **8 minute first phase of sleep mode**." — ["Energy Management", p.10](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- Definition: "A sleep mode preventer is the control module that fails to enter sleep mode or set the ready to assume sleep mode bit more than one time after the ignition is switched off (terminal 0). After terminal 0, control modules for relevant data are required to restart and log off of their respective bus systems. The vehicle gateway monitors bus activity and logs which control modules have signed off and assume sleep mode." — ["Energy Management", p.9](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- "When the CAS opens the 30g/30B relays, that marks the start of 'Sleep Mode' and the IBS starts measuring closed circuit current." — ["Energy Management", p.7](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- Older-generation (E53 X5) field practice: "Let the car go to sleep. (16 min) minimum" before measuring parasitic draw; draw is ~300 mA before sleep and "should not be more than 50 mA" after — [xoutpost E53 parasitic draw post](https://xoutpost.com/1156468-post22.html) *(E53, not F3x — the 16-minute figure is the Boardnet 2000 number)*
- BMW scan tools expose a functional test "Enter Sleep Mode" under Service Resets and Relearns → Body Controls, which forces the vehicle network to shut down so parasitic draw can be measured without waiting for full system shutdown — [Snap-on, Parasitic Draw Diagnosis](https://www.snapon.com/EN/US/Diagnostics/News-Center/Technical-Focus-Archive/Parasitic-Draw-Diagnosis)

### Inferences
- The F32's "is it asleep yet?" clock is short — 8 minutes for the bus, up to ~1 hour for the 30B relay drop. A dongle that polls on any interval shorter than ~8 minutes will reset the bus-sleep timer indefinitely and the car will never reach the first phase of sleep, let alone 30B shutdown.
- Because the gateway logs sleep-mode preventers at the 20-minute mark, a persistently polling dongle on an F32 is likely to get *some* module recorded as a preventer in the gateway's energy history — it is observable in diagnostics, not invisible.

### Gaps
- No source states explicitly how the FEM/BDC classifies a *tester* (external D-CAN requester) in its sleep-preventer log — whether it names itself, names "unknown control unit," or logs a diagnostic-session flag. The E92 ISTA screenshot in the bimmer pub article shows entries like "Unknown control unit - 3 h 13 min 4 s", which suggests unattributable wake sources do appear as "unknown control unit."

---

## Q4 (CRITICAL): Passive listening vs active polling — does polling hold the bus awake, and what does it cost in current?

### Takeaway
This is the dominant risk and the sources support it clearly in principle, though the headline current figure is weakly sourced. A dongle that merely listens cannot keep an F3x awake — and on F3x will see nothing at all, because D-CAN is silent without a tester. A dongle that sends OBD requests or tester-present frames must wake the FEM/BDC gateway, which wakes the modules behind it. The one explicit quantification found claims this costs **roughly 5 A on the 12 V bus**, i.e. roughly **100x** a typical dongle's own ~40 mA awake draw and ~2500x a good dongle's ~2 mA sleep draw. That 5 A figure comes from a developer mailing-list quotation of unnamed BMW documentation and should be treated as indicative, not verified.

### Cited Findings
- On BMW, "the OBD port may be kept awake by using the 'tester present' message to the gateway ECU. **This keeps a lot of systems awake and draws roughly 5A on the 12V bus**", described as "not a good idea to do" — [documentation quoted by Steve Davies, OVMS developer list](https://lists.openvehicles.com/archives/list/ovmsdev@lists.openvehicles.com/thread/YC3BF6GHJUARTKLKLZCA6FH56J2KZIOX/) *(secondary quotation of an unnamed source; the vehicle context is a BMW i3 — magnitude may differ on F3x)*
- Same source on how to force the gateway awake: "If the car is unlocked, the gateway can be woken ... by pinging the 'tester present' message continuously every 100 ms until it responds"; "If the car is locked, the gateway may be woken by sending a session 2 command every 250 ms to the gateway." — [OVMS developer list](https://lists.openvehicles.com/archives/list/ovmsdev@lists.openvehicles.com/thread/YC3BF6GHJUARTKLKLZCA6FH56J2KZIOX/)
- BMW's own list of discharge causes includes not only current draw but "**excessive vehicle wake ups due to unauthorized bus activity**" and "control modules that prevent the vehicle from entering sleep mode, i.e. sleep mode preventers" — ["Energy Management", p.5](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- "With regard to authorized vehicle wake ups, **only two modules are authorized to wake up the vehicle**, the CAS and the vehicle gateway ... For Boardnet 2020 vehicles using FEM or BDC, those would be the only authorized wake up modules. **All other modules that wake of the vehicle are considered unauthorized wake ups.**" — ["Energy Management", p.9](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- A dongle's *own* consumption, for comparison: OBDLink adapters draw about **37–40 mA awake** and **1–2 mA in sleep**, entering sleep after 10 minutes with no Bluetooth or CAN activity (green LED blinks every 3 s) — [OBDLink documentation/support guidance](https://www.obdlink.com/mxwf/troubleshooting-guide/)
- Other dongles measured at **31.9 mA in use, 19.7 mA asleep**; optimized designs reach ~1 mA asleep. Hardware design around the ELM327 chip, and the firmware, dominate sleep behaviour — [OBD Auto technical write-up (scithings.id.au)](https://scithings.id.au/OBD_Auto.pdf)
- "Low-power 'sleep' mode is an essential feature for any OBD device advertised as 'safe to leave plugged in'. An intelligent scan tool should recognize this condition, quickly power down and consume the least possible amount of energy in sleep mode." — [OBDLink](https://www.obdlink.com/mxwf/troubleshooting-guide/)
- Guidance that the OBD app "must be set up for manual connection to prevent background 'polling' that may keep the reader awake" — [OBD Auto write-up](https://scithings.id.au/OBD_Auto.pdf)
- BMW's healthy-asleep spec: closed circuit current draw **in excess of 80 mA** is a listed cause of a discharged battery — ["Energy Management", p.5](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf). Independent shop guidance puts the practical threshold at **under 50 mA** after sleep — [xoutpost](https://xoutpost.com/1156468-post22.html); [Snap-on](https://www.snapon.com/EN/US/Diagnostics/News-Center/Technical-Focus-Archive/Parasitic-Draw-Diagnosis). Note the two BMW-adjacent numbers (80 mA as "violation", ~50 mA as practical ceiling) differ; the 80 mA figure is the one from BMW-derived documentation.
- Older-generation data point for how much an awake bus costs: on 2005 MY BMW ASSIST cars, current fluctuations "as high as **500 mA** that last for approximately 2 minutes, occurring every 15 minutes for up to 14 hours after key off" are normal — [BMW SI B61 08 00 / E60 discussion attachment](https://5series.net/forums/attachments/e60-discussion-2/107177d1276207762-no-audio-sos-call-system-failure-sbt_micro_power_module.pdf) *(E60-era, telematics-specific — shows order of magnitude for partial wake, not full bus wake)*

### Inferences
- **Ratio argument (the key finding):** a well-behaved dongle asleep costs ~2 mA; the same dongle polling costs ~40 mA for itself but forces the vehicle from ~<80 mA asleep to a woken-bus state. Even if the 5 A figure is high by a factor of five, a 1 A woken-vehicle draw is still 12x BMW's own 80 mA violation threshold and >100x the dongle's own current. The dongle's self-consumption is therefore essentially a rounding error against the vehicle-wake effect.
- **Timescale arithmetic (my calculation, from the cited figures):** an F32 with a nominal ~70 Ah AGM battery, from full, reaching ~50% state of charge (the practical no-start point for a BMW with start/stop and energy management):
  - asleep at 30 mA → ~1,170 h ≈ **49 days**
  - dongle-only extra 20 mA on top (50 mA total) → ~700 h ≈ **29 days**
  - bus held awake at 1 A → ~35 h ≈ **1.5 days**
  - bus held awake at 5 A → ~7 h ≈ **overnight**
  These are arithmetic from the cited current figures, not measured values.
- A passive-only dongle on an F32 is harmless to sleep but also useless for parked monitoring, because D-CAN carries no traffic with the car asleep. Any "parked tracking via OBD" product on an F3x is necessarily in the harmful category unless it sources data some other way.
- BMW's own framing in the energy-management document — that the vehicle gateway's job is to detect sleep-mode preventers and unauthorized wake-ups — is effectively a description of what an actively polling OBD dongle looks like to an F32.

### Gaps
- **No measured, F3x-specific current figure for "buses awake while parked" was found.** The 5 A claim is a single, second-hand quotation on a mailing list, attributed to unnamed documentation, in an i3 context. This is the single biggest evidentiary weakness in these notes and should be presented as indicative only. An actual clamp-meter measurement on an F32 with a polling dongle would settle it.
- No source quantifies how long an F3x stays awake after a *single* diagnostic request before re-entering the sleep sequence (i.e. whether the 8-minute clock simply restarts, which is the obvious assumption but is not stated).

---

## Q5: Documented reports of OBD dongles causing flat batteries / no-sleep conditions on BMW

### Takeaway
Reports exist and are consistent, but the BMW-specific forum evidence is anecdotal and I was largely blocked from reading the threads directly. The strongest *documented* evidence is the Progressive Snapshot class action, which is not BMW-specific. Reported timescales cluster at "a few days to 2–3 weeks" for self-consumption-only drain, and "overnight to a couple of days" where the device holds the vehicle awake.

### Cited Findings
- Dedicated F-series threads exist and the question is a recurring one: "Leaving OBD Bluetooth Dongle Plugged In – Battery Discharge?" on the F30 and F22 forums; "Battery drain w/engine off, OBD2 scanner plugged in" (F22); "Leave OBDLink CX adapter plugged in?" (G05); "Help! Dead battery after Diagnostics" (Bimmerfest) — [f30.bimmerpost 1722237](https://f30.bimmerpost.com/forums/showthread.php?t=1722237); [f22.bimmerpost 1133448](https://f22.bimmerpost.com/forums/showthread/1133448/battery-drain-w-engine-off-obd2-scanner-plugged-in); [bimmerfest 1380207](https://www.bimmerfest.com/threads/help-dead-battery-after-diagnostics.1380207/) *(all returned HTTP 403/402 to automated fetch; titles and search snippets only)*
- Forum consensus snippets from those threads: dongles draw so little that daily-driven cars are fine; "most cheap dongles would take **2–3 weeks** to drain a new, good battery in summer"; "the OBD circuit is supposed to shut down shortly after the car shuts down but something plugged in can cause the circuit to stay open" — [bimmerpost search snippets](https://f30.bimmerpost.com/forums/showthread.php?t=1722237) *(snippet-level only; the "circuit stays open" claim is a forum user's mental model, not verified)*
- A healthy modern vehicle should draw no more than ~50 mA off; aftermarket accessories can push draw "well past 100 milliamps, which can flatten a fully charged battery in just a couple of days" — [Testing for Parasitic Draws, automotivetechinfo](https://automotivetechinfo.com/wp-content/uploads/2018/02/Testing-for-Parasitic-Drass-and-Intermittent-Electrical-Problems.pdf)
- **Progressive Snapshot class action:** Alex Morales filed in U.S. Federal Court, Fort Lauderdale (article dated 30 Jan 2013), alleging the OBD-port Snapshot device "always drains a vehicle's battery, and many times the battery is drained to the point of becoming nonfunctional." He enrolled Aug 2011, suffered repeated starting failures requiring jump-starts, and paid $91.10 for a replacement battery in Jan 2012. Counsel: Adam Balkan, Balkan and Patterson. Progressive responded that the claims are untrue and Snapshot does not drain batteries or harm vehicles. Outcome not stated in the source. — [Courthouse News](https://www.courthousenews.com/class-claims-insurers-gizmo-kills-batteries/)
- Broader telematics-dongle complaints: customers reported battery failure and electronic malfunctions, including premature alternator failures, attributed to Snapshot; Progressive reimbursed some battery replacements — [KIRO 7 investigation](https://www.kiro7.com/news/insurance-tracking-device-blamed-car-damage/82088181/)
- The cross-brand mechanism, as stated in secondary reporting: "the sleep-mode logic on cheaper insurance-issued dongles conflicts with ECU stay-awake times on modern vehicles, effectively keeping the car awake to ping the server" — [search-result synthesis over telematics complaint coverage](https://www.kiro7.com/news/concerns-insurance-devices-monitor-safe-driver-dis/81766693) *(plausible and consistent, but I could not trace it to a named engineer or test report)*
- Same phenomenon on VAG: "OBD dongles like OBDEleven can provide power to the device while keeping the whole car computer on and preventing the car from going to sleep properly" — [Audizine OBDEleven thread](https://www.audizine.com/threads/obdeleven-owner-problems.767211/) *(VAG, not BMW)*
- Teltonika's FMC003 OBD tracker has a documented "quiescent current violation" support topic, confirming OBD trackers are a known source of quiescent-current faults — [Teltonika community](https://community.teltonika.lt/t/fmc003-quiescent-current-violation/5010)
- Separate but related F3x-relevant report: Thinkware dashcam causing battery drain on an F30 — [DashcamTalk](https://dashcamtalk.com/forum/threads/thinkware-f800-pro-battery-drain-on-bmw-f30.41876/latest) *(dashcam, not OBD dongle)*

### Inferences
- The absence of a BMW TSB specifically about OBD dongles, combined with the existence of the 30F fault-shutdown mechanism and the gateway's sleep-preventer logging, suggests BMW's engineering response was to build detection into energy management rather than to publish a warning.
- The two failure modes produce distinguishable timescales: self-consumption-only → weeks; bus-held-awake → overnight to ~2 days. A user reporting "flat in a weekend" on an F32 is almost certainly in the second category.

### Gaps
- I could not read any BMW forum thread in full; bimmerpost returns 403 and bimmerfest redirects to a paywall proxy (tollbit, HTTP 402). All bimmerpost/bimmerfest claims above are search-snippet level and should be re-verified by a human with browser access before being stated as fact.
- No outcome (settlement/dismissal) found for the Progressive class action.
- Found **no** report specifically naming a Freematics device on an F32.

---

## Q6: Does BMW/TIS/dealer documentation warn against aftermarket devices in the OBD port? Logged fault codes?

### Takeaway
No explicit BMW TIS warning against OBD-port accessories was found. However, BMW's energy-diagnosis framework describes precisely the conditions an active dongle creates, and the F-series 30F logic stores a fault/information flag when it trips — so the condition is logged, even if the dongle is not named.

### Cited Findings
- BMW's "energy diagnosis test module assists service technicians in determining a cause or causes of a discharged battery," with causes listed as a bad battery, closed circuit current draw in excess of 80 mA, excessive vehicle wake-ups due to unauthorized bus activity, or sleep mode preventers — ["Energy Management", p.5](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- "All control modules that wake up the vehicle are logged by the vehicle gateway. This function reliably started with the 9/06 model update for E6X, and continued with more fidelity on all models that were capable of an energy diagnosis test module." — ["Energy Management", pp.8–9](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- When 30F trips, "a fault/information flag will be stored"; typical information flags named in the article are **"30g/30B relay"** and **"30g_f/30F relay"** — ["Energy Management", pp.7–8](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- The Energy Diagnosis test plan can be run using **ISTA 3.39 or later** — [NHTSA-hosted BMW TSB](https://static.nhtsa.gov/odi/tsbs/2017/MC-10146765-9999.pdf)
- The ISTA energy-diagnosis screen logs wake events against odometer readings with per-module durations, e.g. "CAS - 13 h 9 min 37 s", "FRM - 2 min 9 s - Terminal R was activated at least once", "**Unknown control unit** - 3 h 13 min 4 s" — [ISTA screenshot in "Energy Management", p.10](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf) *(screenshot is from an E92, but the test module is the same family)*
- BMW notes a limitation in the wake registration: "no distinction can be made between authorized and unauthorized wakings. Note: If the test module is unable to interpret the waking CAN message, 'unknown control module' is displayed as waking." — [ISTA screenshot in "Energy Management", p.10](https://automotivetechinfo.com/wp-content/uploads/2021/03/BMW-energy-management-systems.pdf)
- Security/behavioural warning from the BimmerCode ecosystem: OBD dongles left plugged in are "wide open all the time," meaning someone could code the car while it is powered; and a left-connected adapter "may block or prevent the vehicle from 'phoning home', causing status update issues" — [search synthesis over BimmerCode/OBDLink discussion](https://scantool.net/obdlink-cx/)

### Inferences
- On an F32 with a polling dongle, the expected diagnostic signature is: energy diagnosis reporting unauthorized wake-ups and/or a sleep-mode preventer, possibly attributed to "unknown control unit" because the waking message is a tester request the module can't map to an ECU; plus a 30g_f/30F relay information flag if the shutdown tripped.
- The explicit BMW caveat that "unknown control module" is displayed when the test module can't interpret the waking CAN message makes an external tester the archetypal "unknown" waker — a useful diagnostic tell for the user.

### Gaps
- No BMW TIS/SIB text explicitly naming aftermarket OBD devices was located. The NHTSA-hosted TSBs found were adjacent (ISTA procedures, programming) rather than on-point.
- No specific DTC number (e.g. an Axxxxx code) tied to a tester-induced wake was found.

---

## Q7: Best practice

### Takeaway
The consistent recommendation across vendor and forum sources is: leave a dongle plugged in only if it has a verified low-power sleep mode *and* the paired app is set to manual connection so it doesn't background-poll; unplug for extended parking. For an F32 specifically, the 8-minute first sleep phase means any polling interval under ~8 minutes is disqualifying.

### Cited Findings
- "The OBD reader should have a 'sleep' mode that draws very little power when there is no UART activity, and the OBD app must be set up for manual connection to prevent background 'polling' that may keep the reader awake." — [OBD Auto write-up](https://scithings.id.au/OBD_Auto.pdf)
- "Removing the OBD connector if the car will be sitting for some time prevents 12V battery problems." — [OBD Auto write-up](https://scithings.id.au/OBD_Auto.pdf)
- OBDLink CX: "advanced sleep mode and overvoltage protection let you keep the adapter plugged in safely without worrying about battery drain or ECU damage"; it "goes into low-power mode when the car is off" — [ScanTool/OBDLink CX product page](https://scantool.net/obdlink-cx/)
- OBDLink sleep can be configured via STSL* commands from a terminal; default behaviour is sleep after 10 minutes with no Bluetooth or CAN activity, drawing 1–2 mA — [OBDLink troubleshooting guide](https://www.obdlink.com/mxwf/troubleshooting-guide/)
- Some users fit an OBD extension cable with an inline on/off switch rather than repeatedly removing the adapter, to address both drain and security — [search synthesis over BimmerCode discussion](https://scantool.net/obdlink-cx/)
- Forum rule of thumb: safe if the car is driven at least ~3x per week; unplug otherwise — [bimmerpost snippets](https://f30.bimmerpost.com/forums/showthread.php?t=1722237)
- Note that remote locking/unlocking re-wakes things: "if you use the remote key it will wake things up; if you use BMW Connected drive app to lock/unlock then that also wakes things up" — [Steve Davies, OVMS list](https://lists.openvehicles.com/archives/list/ovmsdev@lists.openvehicles.com/thread/YC3BF6GHJUARTKLKLZCA6FH56J2KZIOX/) *(i3 context, but CAS/gateway wake authorisation is generic across BMW per the energy-management article)*

### Inferences
- Practical F32 rule derived from the cited 8-minute figure: a dongle must be silent on D-CAN for well over 8 minutes at a time, ideally indefinitely once terminal 0 is detected, or the car never completes phase one of sleep. "Poll every 5 minutes while parked" is the worst possible configuration — frequent enough to reset the sleep clock every single time.
- Double-locking the F32 (pressing lock twice) is cited as shortening the 30B shutdown time, so it is a free mitigation that gets the car to a lower-power state faster — though it does not stop a dongle on terminal 30 from waking the gateway.

### Gaps
- No vendor publishes an explicit "this adapter will not wake a BMW FEM/BDC gateway" claim. Vendor sleep specs describe the *adapter's* current, not its effect on the vehicle — which is exactly the distinction that matters here and is systematically absent from marketing material.
- No source specifies a recommended "unplug after N days" threshold from BMW itself.
