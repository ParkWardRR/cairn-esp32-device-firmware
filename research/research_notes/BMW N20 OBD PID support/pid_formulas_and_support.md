# BMW F32 428i (N20/N26, 2014-2016) — Generic OBD-II Mode 01 PID Support and Standard Formulas

Scope note: formula/byte-count findings below are from SAE J1979 / ISO 15031-5 consolidated reference tables. Vehicle-specific support findings for the N20/N26 in F3x are much more thinly sourced than the formula findings — see the Gaps sections, which are substantial and deliberate.

## Q1: Exact SAE J1979 / ISO 15031-5 formulas, byte counts, units and ranges

### Takeaway
All requested formulas are confirmed from the consolidated J1979/ISO 15031-5 PID table. The two critical ones for the caller's validation work — 0x43 absolute load and 0x44 commanded equivalence ratio — are **both two-byte PIDs**, and 0x0B / 0x33 are **both one-byte, 0-255 kPa absolute**.

### Cited Findings

Byte counts, units, ranges and formulas, all from the consolidated Service 01 PID table — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs):

| PID | Description | Data bytes | Min | Max | Units | Formula (verbatim from source) |
|-----|-------------|-----------|-----|-----|-------|--------------------------------|
| 0x06 | Short term fuel trim (STFT) — Bank 1 | **1** | -100 | 99.2 | % | `100/128 A − 100`, equivalently `A/1.28 − 100` |
| 0x07 | Long term fuel trim (LTFT) — Bank 1 | **1** | -100 | 99.2 | % | same as 0x06 |
| 0x0B | Intake manifold absolute pressure | **1** | 0 | 255 | kPa (absolute) | `A` |
| 0x0E | Timing advance | **1** | -64 | 63.5 | ° before TDC | `A/2 − 64` |
| 0x0F | Intake air temperature | **1** | -40 | 215 | °C | `A − 40` |
| 0x10 | MAF air flow rate | **2** | 0 | 655.35 | g/s | `(256A + B)/100` |
| 0x33 | Absolute barometric pressure | **1** | 0 | 255 | kPa (absolute) | `A` |
| 0x43 | Absolute load value | **2** | 0 | 25,700 | % | `100/255 (256A + B)` |
| 0x44 | Commanded air-fuel equivalence ratio (lambda) | **2** | 0 | <2 | ratio | `2/65536 (256A + B)` |
| 0x46 | Ambient air temperature | **1** | -40 | 215 | °C | `A − 40` |
| 0x04 | Calculated engine load (for contrast with 0x43) | **1** | 0 | 100 | % | `100/255 A`, equivalently `A/2.55` |

- PID 0x00 / 0x20 / 0x40 / 0x60 / 0x80 each return **4 data bytes**, bit-encoded. For 0x00: "Bit encoded [A7..D0] == [PID $01..PID $20]" — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- Related wide-range lambda PIDs use the same 2/65536 scaling on the first two bytes: PID 0x24 (O2 Sensor 1 wide-range) is 4 bytes, AB = air-fuel equivalence ratio `2/65536(256A+B)`, CD = voltage `8/65536(256C+D)`. PID 0x34 is 4 bytes, AB = ratio `2/65536(256A+B)`, CD = current `(256C+D)/256−128` mA — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- PID 0x4F ("Maximum value for Fuel–Air equivalence ratio, oxygen sensor voltage, oxygen sensor current, and intake manifold absolute pressure") is 4 bytes with formula `A, B, C, D×10` — i.e. the vehicle can *declare* its own maximum IMAP via byte D × 10 kPa — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)

### Inferences
- `100/255 (256A + B)` is algebraically identical to the caller's `((A*256)+B)*100/255`. Max = 65535 × 100/255 = 25,700.0%. Confirmed.
- `2/65536 (256A + B)` is algebraically identical to `((A*256)+B)/32768`. Max = 65535/32768 = 1.99997, i.e. "0 to just under 2.0". Confirmed.
- STFT/LTFT max of 99.2% comes from 255/1.28 − 100 = 99.218…; so a reported "+99.2%" is the saturated top of the scale, not a plausible physical trim.
- PID 0x4F is the standard-defined way a tool is *supposed* to learn the vehicle's declared IMAP ceiling. A logger that wants to be correct about scaling limits should read 0x4F byte D (×10 kPa) rather than assume 255 kPa. This is a standard mechanism, not a convention.

### Gaps
- I could not access the SAE J1979 or ISO 15031-5 documents themselves (paywalled). All formula confirmations are from the consolidated Wikipedia table, cross-checked where possible. The cross-check attempts at csselectronics.com returned only prose (it explicitly defers to "ISO 15031-5/SAE J1979" and puts its table behind an interactive tool), and palmerperformance.com lists 229 generic parameter *names* with no units or formulas — [CSS Electronics: OBD2 Explained](https://www.csselectronics.com/pages/obd2-explained-simple-intro); [Palmer Performance: Generic OBD-II Parameters](https://www.palmerperformance.com/support/supported_vehicles/generic_pids.php). So the byte counts/formulas are well-corroborated as community consensus but I have no page-cited primary-standard text.
- One independent corroboration for 0x43 was found: a tuning-forum source states PID 43 is "Absolute load value" with range 0 to 25,700 percent, two bytes, formula `((A*256)+B)*100/255` — [VersaTune forum](https://www.versatune.net/forum/viewtopic.php?p=4401). This matches the Wikipedia table exactly. I found no equivalent independent corroboration page for 0x44's byte count beyond the Wikipedia table and the structurally identical 0x24/0x34 entries.

## Q2 (CRITICAL): 0x43 absolute load — two bytes, 0-25700%?

### Takeaway
**Confirmed. 0x43 is a two-byte PID with formula ((A*256)+B)*100/255, range 0 to 25,700%.** Reading only byte A and capping at 100% is definitively wrong, and is wrong in a way that matters specifically on a boosted engine, because absolute load legitimately exceeds 100% under boost.

### Cited Findings
- "PID 43 is 'Absolute load value' with a range of 0 to 25,700 percent, using two bytes with the formula ((A*256)+B)*100/255" — [VersaTune forum](https://www.versatune.net/forum/viewtopic.php?p=4401)
- Wikipedia's Service 01 table: PID 0x43, 2 data bytes, min 0, max 25,700, units %, formula `100/255 (256A + B)` — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- Semantics: "The absolute load value (PID 43) is the normalized value of air mass per intake stroke displayed as a percent. It is often used to schedule spark and EGR rates." — [VersaTune forum](https://www.versatune.net/forum/viewtopic.php?p=4401)
- Boosted-engine behaviour: absolute load is based on swept volume; a 3.0L engine is 100% volumetrically efficient if it moves 3.0 L of air per engine cycle, and "If a turbo charger is added, the engine can move far more air and may reflect 190% Absolute Load under full boost." — [VersaTune forum](https://www.versatune.net/forum/viewtopic.php?p=4401)
- By contrast, PID 0x04 *calculated* engine load **is** one byte and **is** genuinely capped at 0-100% with formula `100/255 A` — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- Absolute vs calculated load is a recognized distinction in the diagnostic trade literature — [Mastering Diagnostics #18: Calculated Load vs. Absolute Load](https://www.vehicleservicepros.com/service-repair/video-network/video/55235258/mastering-diagnostics-18-calculated-load-vs-absolute-load)

### Inferences
- Likely root cause of the bug in the caller's code: 0x04 (calculated load, 1 byte, 0-100%) and 0x43 (absolute load, 2 bytes, 0-25700%) were conflated. They are different PIDs with different byte counts and different ranges.
- Concrete failure mode for a 1-byte reader on 0x43: a true 150% absolute load corresponds to raw = 150 × 255/100 = 382.5 → 0x017F, so A=0x01, B=0x7F. Reading only A gives 1 × 100/255 = 0.39%. The reading does not merely saturate, it collapses to near zero and wraps every 100.0% of load (A increments once per 255 raw counts = 100%). This produces a sawtooth artifact in logs, which is a useful signature to look for when validating.
- A naive 1-byte reader would also mis-frame the CAN response: 0x43's response payload is `41 43 A B`, so a parser that expects 1 data byte may either mis-index or treat B as padding.

### Gaps
- No primary standard text cited (see Q1 gaps). The 25,700% figure is consistent across the two sources I found and matches the 65535 × 100/255 arithmetic exactly, which is strong internal evidence, but it is not a primary citation.

## Q3 (CRITICAL): 0x44 equivalence ratio — two bytes, 0 to just under 2.0?

### Takeaway
**Confirmed. 0x44 is two bytes, formula ((A*256)+B)/32768 (published as `2/65536 (256A+B)`), range 0 to just under 2.0 (max 1.99997), dimensionless ratio.** 1.0 = stoichiometric is the standard meaning of lambda / air-fuel equivalence ratio.

### Cited Findings
- Wikipedia Service 01 table: PID 0x44 "Commanded Air-Fuel Equivalence Ratio (lambda)", 2 data bytes, min 0, max "<2", units "ratio", formula `2/65536 (256A + B)` — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- The identical 2/65536 scaling appears on the wide-range O2 sensor PIDs 0x24 (AB = air-fuel ratio) and 0x34 (AB = ratio), both 4-byte PIDs — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- PID 0x4F declares the vehicle's maximum for fuel-air equivalence ratio in byte A — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)

### Inferences
- `2/65536 × (256A+B)` = `(256A+B)/32768`. Identical to the caller's stated formula. Confirmed.
- The resolution is 1/32768 ≈ 0.0000305 lambda per LSB, which is far finer than any real sensor; the wide range exists to accommodate both very rich commanded enrichment and lean-burn operation in one scale.
- Standard-defined vs convention: the 1.0 = stoichiometric interpretation is inherent to the definition of lambda (equivalence ratio = actual AFR / stoichiometric AFR), so it is definitional rather than a convention. Converting lambda to a gasoline AFR by multiplying by 14.7 **is** a convention — 14.7:1 is the stoich AFR for pump gasoline specifically, and is wrong for E85 (~9.8:1) or for a vehicle running high ethanol content. A logger should store lambda and convert for display only.
- A 1-byte read of 0x44 would be catastrophically wrong: byte A alone, divided by 32768, yields values in the 0.000-0.008 range and would be read as a near-zero lambda. If the caller's code shows lambda near zero, that is the signature.

### Gaps
- I did not find an independent non-Wikipedia page confirming the byte count for 0x44 specifically. The structural consistency with 0x24/0x34 and the 0x4F max-declaration entry are supporting but indirect.

## Q4: 0x0B is one byte, 0-255 kPa — what about boost above 255 kPa absolute? Is there a higher-range PID?

### Takeaway
0x0B is confirmed one byte, 0-255 kPa absolute, so it hard-saturates at 255 kPa — about 22.5 psi of gauge boost at sea level. There **are** higher-range standard PIDs: PID 0x70 (boost pressure control) is well-sourced at 0-2047.96875 kPa, and PID 0x87 does exist and is "Intake manifold absolute pressure" with **5** data bytes, but I could not source 0x87's formula or range, so treat the 0x87 specifics as unverified.

### Cited Findings
- PID 0x0B: 1 data byte, 0-255, kPa, formula `A` — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- PID 0x70 "Boost pressure control": **10** data bytes, range **0 to 2047.96875 kPa**; the source's formula column for one of its fields reads "Sensor 1: (256D+E)/0.03125" — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- PID 0x87: description "Intake manifold absolute pressure", **5** data bytes. The source's min/max and formula columns were not retrievable for this row — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- PID 0x71 "Variable Geometry turbo (VGT) control": 6 data bytes. PID 0x73 "Exhaust pressure": 5 data bytes — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- PID 0x4F byte D declares the vehicle's maximum intake manifold absolute pressure as `D × 10` kPa — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)

### Inferences
- 255 kPa absolute minus ~101.3 kPa ambient = ~153.7 kPa gauge = **~22.3 psi gauge** at sea level. So the caller's "about 22 psi" figure is right. Anything above that pegs 0x0B at 255 and the log goes flat — the signature is a clipped plateau at exactly 255 kPa / 22.3 psi, not a rollover.
- Note the direction error in the published 0x70 formula string: "(256D+E)/0.03125" cannot be right, because 65535/0.03125 = 2,097,120, not the stated 2047.96875 max. The correct scaling must be **×0.03125** (65535 × 0.03125 = 2047.96875 kPa exactly). This is a transcription error in the reference table, and it is worth flagging to anyone implementing from that table. The 0.03125 kPa/bit (= 1/32 kPa) scaling over 2 bytes is the standard J1979 high-range pressure scaling.
- PID 0x87 having 5 data bytes is consistent with the J1979 multi-sensor pattern used by 0x70/0x73: one bitmap/support byte followed by two 2-byte sensor values (sensor A and sensor B), which is why it is often written "Intake manifold absolute pressure A/B". If it uses the same 1/32 kPa scaling as 0x70, each sensor would span 0-2047.96875 kPa. **I could not confirm this and it should be verified against the standard before relying on it.**
- Practical consequence for the caller: a heavily tuned N20 running above ~22 psi gauge cannot be logged correctly via 0x0B. The correct approach is (a) read 0x4F byte D to learn the declared max, (b) check the 0x60/0x80 support bitmaps for 0x70 and 0x87, and (c) fall back to a manufacturer-specific request if neither is supported.

### Gaps
- **0x87's exact formula, byte layout and min/max are unconfirmed.** Four separate search and fetch attempts failed: searches for the 0x87 formula, for the 0.03125 scaling tied to 0x87, and for "2047.96875" returned only unrelated MAP-sensor pages. One search engine response even incorrectly asserted "PID 87 (0x57) corresponds to short term secondary oxygen trim" — a decimal/hex confusion (decimal 87 = 0x57) that is a real trap and worth noting; 0x87 is decimal 135 — [search result commentary on team-bhp thread](https://www.team-bhp.com/forum/technical-stuff/125767-obd2-parameter-ids-pids-what-they-mean.html). The caller should verify 0x87 against ISO 15031-5 directly.
- I found no evidence either way on whether an N20/N26 F3x supports 0x70 or 0x87. Given these are mostly diesel/heavy-duty-oriented PIDs in the 0x61-0x87 block, support on a 2014-2016 gasoline BMW is doubtful but unverified.

## Q5: Does an F3x N20/N26 answer 0x0B, 0x43, 0x44, 0x10, 0x33 generically, or does BMW restrict these to UDS?

### Takeaway
**This is the weakest-sourced area of my research and I want to be blunt about it: I could not find a single credible first-hand report enumerating which generic Mode 01 PIDs an F3x N20/N26 does or does not answer.** The relevant forum threads (bimmerpost, spoolstreet) returned HTTP 403 to automated fetching. What I can establish is that the F3x is OBD-II compliant and responds to generic requests, that the tuning community routes boost logging through BMW-specific extended PIDs rather than generic ones, and that there is documented precedent on *older* BMW DMEs for a sensor being readable only with BMW-specific software.

### Cited Findings
- A 2014 BMW 328i (F30) "connected instantly and displayed accurate fault codes" with a generic OBD2 scanner, confirming the F30 platform does speak generic OBD-II — [search summary of OBDLink support article](https://support.obdlink.com/en/support/solutions/articles/43000705533)
- The documented approach for boost on BMW N-series in Torque Pro is a **BMW-specific extended PID, not a generic one**: "the BMW specific boost setpoint value (Ladedrucksollwert) is at PID 2C1001F4 for N-Series engines, with the unit in hPa that needs to be multiplied by 91.554" — [f30.bimmerpost thread (via search summary)](https://f30.bimmerpost.com/forums/showthread.php?p=23022421). Note: `2C 10 01 F4` is a BMW extended-service request, not Mode 01; and the stated 91.554 multiplier is inconsistent with a plain hPa reading, so treat the scaling claim with suspicion.
- Custom BMW PID sets circulated in the community "include EGT and boost commanded" and require custom initialization commands in Torque Pro to reach BMW-specific PIDs — [f30.bimmerpost thread (via search summary)](https://f30.bimmerpost.com/forums/showthread.php?p=23022421)
- Precedent on an older BMW generation (**MS43/MS45 DMEs, E46/E39 era — NOT N20**): for barometric pressure, "you need a scanner with BMW software to read the sensor's value (OBD2 scanners don't)" — [zcoupe.net thread](https://zcoupe.net/threads/can-your-scanner-read-the-barometric-pressure-sensor-in-the-ms43-and-ms45-dmes.53333)
- BMW "utilizes proprietary communication protocols beyond generic OBD2 protocols for accessing vehicle data", enabling deeper ECU diagnostics than generic OBD-II — [search summary, OBDeleven BMW fault codes](https://obdeleven.com/it/bmw-fault-codes)
- The aftermarket sells **physical boost taps** for the N20 (P3Cars N20/N55 BMW Boost Tap) and standalone gauge/data-acquisition kits for F30/F32 chassis, rather than relying on an OBD boost channel — [Turner Motorsport: P3 Gauges for BMW F30](https://www.turnermotorsport.com/BMW-F30-Parts/c-302-bmw-gauge-kits-and-data-acquisition/m-716-p3-gauges?Nrpp=50)
- "Absolute Load Value" is listed as a standard *generic* OBD-II parameter available through generic interfaces, where "generic" means the government-required parameters and "enhanced" means factory parameters not required by regulation — [Palmer Performance: Generic OBD-II Parameters](https://www.palmerperformance.com/support/supported_vehicles/generic_pids.php); [AutoMeter OBD2 generic list](https://www.autometer.com/obd2_generic)

### Inferences
- Regulatory reasoning (strong, but inferential rather than directly cited for this vehicle): a 2014-2016 US-market F32 428i must be OBD-II/ISO 15031 compliant for emissions certification. 0x06, 0x07, 0x0B *or* 0x10, 0x0E, 0x0F and 0x33 are the kind of emissions-relevant parameters a spark-ignition vehicle of this era would be expected to report. However, J1979 only requires a PID to be supported if the corresponding parameter is *used by the emissions control system* — which is exactly why support is per-vehicle and why the 0x00/0x20/0x40 bitmap discovery mechanism exists. **Do not assume; query the bitmaps.**
- The N20 is a MAF-and-MAP engine (it uses a TMAP — combined temperature/manifold-absolute-pressure sensor). Both 0x0B and 0x10 are therefore plausibly backed by real sensors. But some manufacturers report only one of MAF/MAP generically.
- 0x43 and 0x44 are in the "later-added" J1979 block (0x41-0x60) and are notably less universally supported than the 0x01-0x20 block. These two are the most likely of the caller's list to return no data on a 2014-2016 vehicle.
- The strongest *indirect* signal that generic boost logging is inadequate on the N20: the entire tuning ecosystem (JB4, MHD, P3, Torque custom PID lists) uses BMW-specific extended requests or physical taps for boost. If generic 0x0B gave usable boost data on the N20, that workaround economy would be smaller. This is suggestive, not proof — the community may prefer BMW PIDs simply because they expose *target* boost and other channels generic OBD-II never has.
- A logger validated against this vehicle family should therefore be built defensively: enumerate support via the bitmaps, and report "unsupported" distinctly from "supported but returned no data" and from "returned data".

### Gaps
- **No first-hand PID-by-PID support report for F3x N20/N26 found.** f30.bimmerpost.com and both spoolstreet.com hostnames returned HTTP 403 to WebFetch, so I could only see search-engine summaries of those threads, which I consider too lossy to cite as fact. The caller should either read those threads manually or, far better, **just query the car**: send `0100`, `0120`, `0140` and decode the bitmaps. That is a five-minute empirical answer that beats any amount of forum archaeology.
- No JB4 or MHD datalogging channel documentation was retrievable stating whether a given channel comes from generic Mode 01 or from BMW-specific requests.
- No Carly, OBDLink or RealDash published N20 PID support matrix was found.
- I found no report of a BMW *refusing* generic Mode 01 PIDs that the standard would require. The MS43/MS45 barometric-pressure finding is a different engine and a ~decade-earlier DME generation and should not be transferred to the N20.

## Q6: How does a tool discover supported PIDs? The 0x00/0x20/0x40/0x60/0x80 bitmap mechanism

### Takeaway
Confirmed and well-sourced. Each support PID returns 4 data bytes read as a 32-bit big-endian bitmap, MSB first; bit N (counting from MSB = 1) indicates support for the PID at base+N. Mode 01 PID 0x00 is mandatory for any ECU that supports any OBD2 service, which makes it a safe entry point.

### Cited Findings
- "if an emissions-related ECU supports *any OBD2 services*, then **it must support mode 0x01 PID 0x00**. In response to this PID, the vehicle ECU informs whether it supports PIDs 0x01-0x20." — [CSS Electronics: OBD2 Explained](https://www.csselectronics.com/pages/obd2-explained-simple-intro)
- "Further, PIDs 0x20, 0x40, ..., 0xC0 can be used to determine the support for the remaining mode 0x01 PIDs." — [CSS Electronics: OBD2 Explained](https://www.csselectronics.com/pages/obd2-explained-simple-intro)
- "A request for this PID returns 4 bytes of data (Big-endian). Each bit, from MSB to LSB, represents one of the next 32 PIDs and specifies whether that PID is supported." — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- Bit mapping notation per PID: 0x00 → "Bit encoded [A7..D0] == [PID $01..PID $20]"; 0x40 → "Bit encoded [A7..D0] == [PID $41..PID $60]"; 0x20, 0x60 and 0x80 follow the identical structure for their respective ranges — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)
- Worked example, response `BE 1F A8 13`: binary `10111110 00011111 10101000 00010011`, and the supported PIDs are "01, 03, 04, 05, 06, 07, 0C, 0D, 0E, 0F, 10, 11, 13, 15, 1C, 1F and 20" — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs)

**Caution on the worked example:** the fetched rendering of Wikipedia's binary breakdown table was garbled — it emitted only 4 binary groups for 8 hex nibbles and one group read "11010100" where `A8` is `10101000`. The *hex value* `BE1FA813` and the *resulting supported-PID list* are consistent with each other and with the stated bit rule; the intermediate binary row in my fetch is unreliable. I have written the binary above as the correct expansion of BE 1F A8 13. The caller can verify: `BE` = 10111110 → PIDs 01,03,04,05,06,07 (bit for 02 and 08 clear); `1F` = 00011111 → PIDs 0C,0D,0E,0F,10; `A8` = 10101000 → PIDs 11,13,15; `13` = 00010011 → PIDs 1C,1F,20. That reproduces the published list exactly.

### Inferences
- Precise bit rule, stated unambiguously for implementation: for support PID with base B (0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0), response data bytes A,B,C,D form a 32-bit big-endian word W = (A<<24)|(B<<16)|(C<<8)|D. PID number `B + n` (for n = 1..32) is supported iff bit `(32 - n)` of W is set. Equivalently: A's MSB is base+1, A's LSB is base+8, B's MSB is base+9, … D's LSB is base+32.
- The last bit of each support PID (base+32) indicates support for the *next* support PID: bit D0 of 0x00's response is PID 0x20, bit D0 of 0x20's response is PID 0x40, and so on. **A tool should stop walking the chain when that bit is clear** — continuing to request 0x40 after 0x20's response says 0x40 is unsupported will typically yield no response and wasted timeouts. In the BE1FA813 example, PID 0x20 is supported, so the walk continues.
- Full CAN response framing for a support PID: `41 00 BE 1F A8 13` (0x41 = 0x01 + 0x40 positive response, 0x00 = echoed PID, then 4 data bytes). This is 6 bytes and fits in a single-frame ISO-TP message; the data PIDs in question (0x43, 0x44, 0x10 at 2 data bytes → 4-byte response) also fit single frames, so no multi-frame handling is needed for any PID in the caller's list. PID 0x70 at 10 data bytes **would** require ISO-TP multi-frame (flow control) handling.
- Practical validation step for the caller's logger: dump the raw bitmaps from the actual F32 and diff against the logger's assumed support table. This answers Q5 definitively for the specific car.

### Gaps
- None material. This mechanism is unambiguous and consistently described across sources.

## Q7: Judging aftermarket tune health on an N20 — boost, lambda, fuel trims, knock/timing retard

### Takeaway
Stock N20 peak boost is reported around 17-18 psi gauge, and Stage 2 tunes around 21-23 psi gauge; fuel trims within roughly ±10% are considered normal with persistent excursions beyond that indicating a fault. **Lambda-under-load targets for the N20 specifically, and whether knock/timing retard is visible generically on BMW, I could not source and am flagging as gaps rather than guessing.**

### Cited Findings
Boost:
- "Stock N20 engines produce approximately 17.4 PSI boost pressure" — [search summary of f30.bimmerpost thread](https://f30.bimmerpost.com/forums/showthread.php?p=13650671)
- "The N20 should run at around 18PSI target boost, and if the target boost is significantly lower with no error codes logged, it could indicate a DME software mismatch rather than limp mode, which won't trigger codes but keeps wastegates open" — [search summary, z4-forum thread](https://z4-forum.com/threads/z4-no-any-code-no-power.127266/page-2)
- "Stage 2+ tuning on the N20 can increase boost pressure to approximately 21-22 PSI at lower RPMs (around 2713-5410 RPM), representing roughly a +4 PSI increase over stock" — [search summary of f30.bimmerpost thread](https://f30.bimmerpost.com/forums/showthread.php?p=13650671)
- On MHD: "The output of the boost target calc is maxed out at 23PSIg with MBoost enabled, and your TMAP sensor scaling will then take effect and bring it up" and "MHD can target a maximum of 220 load" — [search summary, spoolstreet "Tuning w/ N20 TMAP to see 22+ Boost"](https://www.spoolstreet.com/threads/tuning-w-n20-tmap-to-see-22-boost.1847/page-3)
- Stage 2 N20 output figures quoted around 260-310 whp — [search summary of f30.bimmerpost thread](https://f30.bimmerpost.com/forums/showthread.php?p=13650671)

Fuel trims (general, not N20-specific):
- "Normal fuel trim (STFT and LTFT) values usually range between -10% and +10%" — [Rick's Free Auto Repair Advice: Fuel Trim](https://ricksfreeautorepairadvice.com/fuel-trim-what-the-numbers-mean/)
- "An LTFT above or below -10%/+10% is an indication of an air/fuel-related issue"; LTFT should be near zero or in single digits under normal conditions — [Rick's Free Auto Repair Advice: Fuel Trim](https://ricksfreeautorepairadvice.com/fuel-trim-what-the-numbers-mean/)
- Positive trim = ECM adding fuel because the engine is running lean; negative trim = ECM reducing fuel because it is running rich — [Rick's Free Auto Repair Advice: Fuel Trim](https://ricksfreeautorepairadvice.com/fuel-trim-what-the-numbers-mean/)
- LTFT is the channel for chronic issues: persistent high positive LTFT suggests an ongoing lean condition, persistent negative suggests rich — [MEMS FCR: Understanding fuel trim in detail](https://memsfcr.co.uk/2020/07/10/understanding-fuel-trim-in-detail/); [Fuel Trims & The Tuning Process](https://thetuningschool.com/blogs/news/fuel-trims-the-tuning-process)

Load:
- N20 "load" in BMW tuning terms runs to ~220 on MHD; relatedly, absolute load on a turbo engine "may reflect 190% Absolute Load under full boost" — [spoolstreet summary](https://www.spoolstreet.com/threads/tuning-w-n20-tmap-to-see-22-boost.1847/page-3); [VersaTune forum](https://www.versatune.net/forum/viewtopic.php?p=4401)

### Inferences
- Unit conversions for threshold-setting in the logger, assuming 101.3 kPa ambient at sea level:
  - 17.4 psi gauge ≈ 120 kPa gauge ≈ **221 kPa absolute** (stock N20 peak) — comfortably inside 0x0B's 255 kPa ceiling.
  - 18 psi gauge ≈ 124 kPa gauge ≈ **225 kPa absolute** (stock target).
  - 22 psi gauge ≈ 152 kPa gauge ≈ **253 kPa absolute** (Stage 2) — right at the 0x0B ceiling.
  - 23 psi gauge ≈ 159 kPa gauge ≈ **260 kPa absolute** (MHD max with MBoost) — **exceeds 0x0B's range.**
  - This is the crux: a stock or lightly tuned N20 fits in 0x0B; an aggressively tuned one does not. The caller's logger will silently clip exactly in the regime a tuner most cares about. Note also that at altitude the absolute pressure for a given gauge boost is lower, so the clipping is sea-level-worst-case.
- The spoolstreet discussion of swapping/rescaling the N20 TMAP "to see 22+ boost" and of "TMAP sensor scaling" implies the **stock N20 TMAP sensor itself** has a measurement ceiling that tuners hit around 22 psi, independent of the OBD-II 255 kPa limit. If so, even a correct 0x0B read cannot show boost above that sensor's range, and the two ceilings happen to land in a similar place. I could not confirm the stock TMAP's rated range — see Gaps.
- Fuel trim interpretation for tune validation (inferred from the general trim sources, applied to a boosted context): trims are measured in closed loop, which a tuned turbo car largely leaves at full load, so **trims validate part-throttle health, not the full-load tune.** Sustained LTFT beyond +10% on a tuned N20 points at unmetered air (the N20's notorious oil-filter-housing and valve-cover gasket leaks, or boost-side leaks), a failing MAF/TMAP, or a fuel-delivery shortfall (HPFP, injectors). Sustained LTFT below -10% points at over-fuelling, a leaking injector, or wrong MAF scaling. A tune that moved trims materially from their pre-tune baseline is itself the finding — the delta matters more than the absolute number.
- Data-quality check the caller can build in: 0x33 barometric pressure should read roughly 101 kPa at sea level and should track altitude, and 0x0B should read *below* barometric at idle (vacuum, typically 30-40 kPa absolute) and *above* it under boost. If 0x0B never drops below barometric at idle, or if the two never diverge, the parse or the PID mapping is wrong. Likewise 0x44 should sit at ~1.00 in closed-loop cruise and drop below 1.0 (commanded rich) under full load — a 0x44 that reads ~0.000x is the 1-byte-read bug from Q3.

### Gaps
- **No sourced lambda-under-full-load figure for the N20.** I did not find a citable value for commanded lambda at full load on a stock or tuned N20. Factory turbo gasoline engines of this era commonly command rich of stoichiometric under full load for charge cooling and exhaust-temperature management, but I will not put a number on it without a source. The caller should establish this empirically per-car rather than against a published threshold.
- **Whether knock/timing retard is visible via generic OBD-II on a BMW F3x: unresolved.** Generic PID 0x0E gives timing advance for cylinder 1 only, 1 byte, `A/2 − 64`, resolution 0.5° — [Wikipedia: OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs). There is no generic Mode 01 PID for per-cylinder knock counts or knock-based timing correction; those are manufacturer-specific. I found no source confirming whether BMW exposes N20 knock retard through any channel reachable by a generic reader. The tuning community's reliance on JB4/MHD logging for this is suggestive that it is not generically available, but I could not confirm it.
- Stock N20 TMAP sensor rated pressure range: not sourced (spoolstreet 403'd on both hostnames; only a search summary was available).
- The N20 vs N26 distinction (N26 is the SULEV variant) produced no differentiating PID-support or boost information in any source I found. I have no basis to claim they differ for logging purposes, and no basis to claim they are identical.
- All boost figures above come from search-engine summaries of forum threads I could not fetch directly (HTTP 403). They are mutually consistent across three independent threads, which is reassuring, but they are enthusiast-forum claims, not manufacturer specifications, and no BMW factory boost specification was located.
