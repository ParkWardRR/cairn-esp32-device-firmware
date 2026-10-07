# BMW B58 (M240i) — Candidate PID Manifest

**Status**: Pre-discovery. Candidates assembled from research sources, NOT verified against the actual DME.

**Vehicle**: BMW M240i (F22/G42), B58 engine
**DME**: Expected DME 8.6 / 8.6.0 (from Bimmerprofs; read actual ECU identity before adopting)
**Source**: BMW B58 training manual (April 2015), CSS Electronics, bmw_pid_data repo, MHD channel list

## Rules

- Every entry is a **candidate**, not a confirmed PID.
- Standard Mode 01 PIDs use SAE J1979 formulas. Do not re-invent.
- Enhanced/UDS PIDs are research leads only. Do not send 0x22 requests without documented DID definitions.
- Mark unknowns explicitly. Verify everything against the actual DME.

## Standard Mode 01 PIDs (expected to work on any modern BMW)

| PID | Signal | Bytes | Formula | Unit | Stored field | Confidence | Notes |
|-----|--------|-------|---------|------|-------------|------------|-------|
| 0x04 | Calculated engine load | 1 | A*100/255 | % | engine_load_pct | High | Standard |
| 0x05 | Coolant temperature | 1 | A-40 | degC | coolant_temp_c | High | Standard |
| 0x06 | Short-term fuel trim (B1) | 1 | (A-128)*100/128 | % | fuel_trim_short_pct | High | Standard |
| 0x07 | Long-term fuel trim (B1) | 1 | (A-128)*100/128 | % | fuel_trim_long_pct | High | Standard |
| 0x0B | Intake manifold absolute pressure | 1 | A | kPa | map_kpa | High | Standard, max 255 kPa absolute |
| 0x0C | Engine speed | 2 | (A*256+B)/4 | rpm | rpm | High | Standard |
| 0x0D | Vehicle speed | 1 | A | km/h | speed_kph | High | Standard |
| 0x0E | Timing advance | 1 | A/2-64 | deg | timing_advance_deg | High | Standard, range -64 to 63.5 deg |
| 0x0F | Intake air temperature | 1 | A-40 | degC | intake_temp_c | High | Standard |
| 0x10 | MAF air flow rate | 2 | (A*256+B)/100 | g/s | maf_cgps | High | Standard, store as centigrams/s |
| 0x11 | Throttle position | 1 | A*100/255 | % | throttle_pct | High | Standard, BUT: B58 Valvetronic means idle throttle is NOT near zero |
| 0x2F | Fuel tank level | 1 | A*100/255 | % | fuel_level_pct | High | Standard |
| 0x33 | Absolute barometric pressure | 1 | A | kPa | baro_kpa | High | Standard |
| 0x43 | Absolute load value | 2 | A*256+B | raw | abs_load_raw | High | Standard, capture raw (library mishandles scaling). Can exceed 100% under boost. |
| 0x44 | **Commanded** air-fuel equivalence ratio | 2 | clamp((A*256+B)*10000/32768, 0, 65534) | lambda x 10^4 | lambda_e4 | High | Standard. **This is COMMANDED, not measured.** Do not use for detecting actual lean/rich. |
| 0x46 | Ambient air temperature | 1 | A-40 | degC | ambient_temp_c | High | Standard |

## Standard Mode 01 PIDs (candidate for B58, not currently in Cairn capture)

| PID | Signal | Bytes | Formula | Unit | Confidence | Notes |
|-----|--------|-------|---------|------|------------|-------|
| 0x5C | Engine oil temperature | 1 | A-40 | degC | High | Standard. NOT 0x5D (that is injection timing). |
| 0x52 | Ethanol fuel percentage | 1 | A*100/255 | % | Medium | Standard. B58 may support flex-fuel; verify. |
| 0x51 | Fuel type | 1 | enum | - | Medium | Standard. Identifies fuel system type. |

## Standard Mode 01 PIDs (candidate for B58, requires new capture fields)

| PID | Signal | Bytes | Formula | Unit | Confidence | Notes |
|-----|--------|-------|---------|------|------------|-------|
| 0xA4 | Transmission actual gear | 4 | Complex (support bit + ratio) | ratio | Medium | Standard but NOT a simple gear integer. Response has 4 data bytes. Needs format extension. |
| 0x5E | Engine fuel rate | 2 | (A*256+B)/20 | L/h | Medium | Standard. Needs format extension. |
| 0x49 | Accelerator pedal position D | 1 | A*100/255 | % | Medium | Standard. Distinct from throttle position. |
| 0x4A | Accelerator pedal position E | 1 | A*100/255 | % | Medium | Standard. |
| 0x4B | Accelerator pedal position F | 1 | A*100/255 | % | Medium | Standard. |
| 0x4C | Commanded throttle actuator | 1 | A*100/255 | % | Medium | Standard. |

## Enhanced / Manufacturer-specific PIDs (research leads only)

These come from community sources (bmw_pid_data repo, MHD channel list, Bimmerpost). They are NOT standard Mode 01 PIDs and may require UDS service 0x22 or proprietary DIDs. Do NOT send these without verified request/response documentation.

| Signal | Source | Request format | Confidence | Notes |
|--------|--------|---------------|------------|-------|
| Boost pressure (actual) | MHD, Bimmerpost | Unknown — may be UDS 0x22 DID | Low | Separate from MAP (0x0B). MAP maxes at 255 kPa. |
| Boost pressure (target) | MHD | Unknown | Low | |
| Lambda (actual, wideband) | MHD | May use PID 0x24 or 0x34 (standard wide-range O2 PIDs) | Medium | 0x24/0x34 are standard but 4-byte; verify B58 support |
| Oil pressure | MHD, BMW training manual | Unknown — sensor documented in B58 manual | Low | Map-controlled oil pump; pressure varies with operating state |
| Transmission temperature | MHD | Unknown | Low | |
| HPFP pressure | MHD | Unknown | Low | |
| Per-cylinder timing correction | MHD | Unknown | Low | |
| Wastegate duty cycle (WGDC) | MHD | Unknown | Low | |

## ECU identification (Mode 09, to read on first contact)

| Service | What | Use |
|---------|------|-----|
| Mode 09 PID 0x02 | VIN | Vehicle identification, VIN pattern discovery |
| Mode 09 PID 0x04 | Calibration ID | DME software version |
| Mode 09 PID 0x06 | CVN | Calibration verification number |
| Mode 09 PID 0x0A | ECU name | DME identification |

## Open questions

- [ ] Does the B58 DME honour multi-PID Mode 01 batch requests? (probe will answer)
- [ ] What is the single-request round-trip latency? (probe will measure)
- [ ] Does the B58 use D-CAN (pins 6/14)? (probe will verify)
- [ ] Does the body controller gate the OBD stub like the F32? (sleep observation will determine)
- [ ] What alternator voltage does the B58 run? (health records will measure)
- [ ] Does the B58 report ethanol percentage (PID 0x52)? (probe will check)
- [ ] Are PIDs 0x24/0x34 (wide-range O2) supported? (probe will check)
- [ ] What is the actual DME software version on this car? (Mode 09 will read)

## Sources

1. [BMW B58 training PDF](https://www.bmwz3club.fr/site/faq/z4g29/BMW_B58_Engine.pdf) — April 2015, covers B58B30M0
2. [BMW PID Data repo](https://github.com/Shooooooooo/bmw_pid_data) — B58 table exists; verify contents, check licensing
3. [CSS Electronics PID reference](https://www.csselectronics.com/pages/obd2-pid-table-on-board-diagnostics-j1979)
4. [MHD B58 Monitor License](https://mhdtuning.com/products/b58-monitor-license) — channel list, software dependencies
5. [B58 Log Review Thread](https://f30.bimmerpost.com/forums/showthread.php?t=1648722) — terminology, example values
6. [Bimmerprofs B58 intro](https://bimmerprofs.com/b58-introduction/) — DME 8.6.0, specialist diagnostics
7. [Wikipedia OBD-II PIDs](https://en.wikipedia.org/wiki/OBD-II_PIDs) — standard PID formulas