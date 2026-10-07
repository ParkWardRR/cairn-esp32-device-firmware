# Engine Data Gathering — Process & Implementation Plan

This document serves two purposes:

1. **Repeatable process**: How to add a new engine to Cairn, from stub to verified profile. Refer to this section the next time a new engine needs profiling.
2. **B58-specific plan**: The concrete execution of that process for the BMW B58 (M240i).

---

## Part 1: How to Add an Engine (Repeatable Process)

### Overview

Adding an engine moves through six gates. Each gate produces evidence the next one needs. No gate is skipped.

```
Stub → Research → Discovery → Provisional Capture → Validation → Verified
         (offline)   (car)        (car)              (offline)     (car)
```

### Gate 0: Stub (starting state)

Every engine begins as a stub in `engines/<engine_id>.yaml` with `status: stub`. A stub claims identity only — no PIDs, no cadence, no thresholds. The engine code generator (`tools/enginegen`) enforces that a stub claims nothing. The firmware correctly refuses to poll OBD on a stub engine.

**Deliverable**: An `engines/<id>.yaml` with `engine_id`, `name`, `applies_to.make`, `applies_to.models`, and everything else `unknown`.

### Gate 1: Research (no car needed)

Before touching the car, gather every existing source of PID definitions, sensor architecture, and ECU behaviour for this engine.

#### 1.1 Source categories

| Category | What to gather | Example sources |
|----------|---------------|-----------------|
| **Standard PIDs** | SAE J1979 Mode 01 definitions, formulas, byte counts | CSS Electronics PID table, SAE standard |
| **Manufacturer training** | Sensor architecture, DME identity, load control, cooling, oil system | BMW technical training PDFs |
| **Community PID data** | Enhanced PID definitions, request formats, provenance | `github.com/Shooooooooo/bmw_pid_data` (B58, N55, B48, S55, S63, N63) |
| **Vendor logging tools** | Channel lists, confirmed signals, software dependencies | MHD, bootmod3, ISTA |
| **Community logs** | Channel terminology, example values, operating-state context | Bimmerpost, Spoolstreet |
| **Existing Cairn profiles** | Template for shared standard PIDs, formula patterns | `engines/bmw-n20.yaml` |
| **NHTSA bulletins** | Engine variant identification, known concerns | NHTSA TSB database |

#### 1.2 Source rules

- **Community PID data is a candidate, not a fact.** Verify every entry against the actual ECU. The `bmw_pid_data` repository credits [The Secret Ingredient](https://thesecretingredient.neocities.org/bmw/) as its original source — inspect the request format before assuming entries are ordinary Mode 01 PIDs or plain UDS DIDs.
- **Manufacturer training covers the original engine revision.** BMW's B58 training manual covers B58B30M0; do not extrapolate to every B58 revision. Record the DME identity from the actual car.
- **Vendor tools may use manufacturer-specific services.** MHD and bootmod3 channels may use UDS service 0x22 or proprietary DIDs, not standard Mode 01. "Available in MHD" does not mean "available via standard OBD."
- **Standard PIDs are authoritative.** Use the SAE definition for standard Mode 01 PIDs. Do not re-derive standard formulas from drive data — validate your implementation against them.
- **Check licensing** before copying data from community sources into the project.

#### 1.3 Known PID corrections (apply to all engines)

| PID | Common mistake | Correct definition |
|-----|---------------|-------------------|
| 0x44 | Called "measured lambda" | **Commanded** air-fuel equivalence ratio. Not measured. Keep separate from actual wideband lambda. |
| 0x5C | Confused with 0x5D | 0x5C = engine oil temperature. 0x5D = fuel injection timing. |
| 0x0B | Assumed unlimited range | Standard MAP reports 0–255 kPa absolute. With ~101 kPa baro, max gauge boost is ~22.3 psi. Detect saturation. |
| 0xA4 | Interpreted as gear number | Standard response has 4 data bytes including a support bit and transmission-ratio value, not a simple gear integer. |
| 0x11 | Assumed 0 at idle | B58 uses Valvetronic (largely throttle-free load control). Idle throttle position is NOT near zero. Do not treat throttle, pedal, and engine load as interchangeable. |

#### 1.4 Producing a candidate manifest

Before the first drive, produce a source-backed candidate manifest:

```
For each candidate PID:
  - Signal name (unambiguous physical quantity)
  - Standard/enhanced classification (Mode 01 vs UDS 0x22 vs proprietary)
  - Request bytes
  - ECU addressing (who responds)
  - Response layout (byte count, field positions)
  - Formula (or "unknown — needs measurement")
  - Unit and stored scaling
  - Provenance (which source, with URL)
  - Applicability (which engine variants)
  - Confidence (confirmed / probable / unknown)
  - Licensing status
```

Mark unknowns explicitly. The manifest drives discovery — it is not a PID table.

#### 1.5 Deliverable

A research notes directory under `research/research_notes/<Engine> OBD PID support/` containing:
- The candidate manifest
- Source documents and their provenance
- DME identity and engine variant notes
- Known limitations and open questions

### Gate 2: Discovery (first car contact)

**Goal**: Determine which PIDs the ECU actually answers, and verify the ECU's identity.

#### 2.1 Build the discovery firmware

The MTPROBE build (`-DCAIRN_MTPROBE=1`) probes candidate PIDs and logs the ECU's support bitmaps. The probe must run in a **restricted, read-only mode**:

- The override grants permission to execute an explicit read-only probe allowlist — it does NOT globally make an unsupported engine "accepted."
- Reject the override in production builds.
- Display discovery mode in every session header.
- The probe sends only standard Mode 01 requests. Nothing is written to the car, no diagnostic session is opened, no manufacturer-specific service is touched.

#### 2.2 Stub gate bypass

The firmware's `engine_gate()` refuses engines with no PID table. For discovery, add a `CAIRN_MTPROBE_ALLOW_STUB` build flag that:
- Skips the PID-table check in `engine_gate()`
- Allows the probe to run while the profile remains a stub
- Does NOT enable production OBD capture (there are no PIDs to poll)
- Is rejected in non-MTPROBE builds

#### 2.3 Stationary discovery first

Before driving, with the ignition on and engine off:
1. Read the ECU identification (Mode 09: VIN, calibration ID, CVN, ECU name)
2. Walk the Mode 01 support bitmaps (PIDs 0x00..0xBF)
3. Log the responder CAN IDs (which ECU answers functional OBD requests)
4. Verify the transport/framing contract (adapter text format, header/padding, timeouts)

Functional OBD requests can receive replies from multiple ECUs. Record and separate per-ECU support maps. Never merge replies from different ECUs into one support table.

#### 2.4 Driven discovery

After stationary discovery succeeds:
1. Drive with the MTPROBE build (15-30 min, mix of city and highway)
2. The probe runs alongside normal capture, using the candidate manifest from Gate 1
3. Multi-PID requests: the probe tests whether the ECU honours multi-PID Mode 01 batches. After 3 failures without anchor PIDs returning, it falls back to one PID per request.

#### 2.5 Evidence preservation

Retain full original logs, not just `grep MTP`. Each probe transaction must record:
- Monotonic timestamp
- Transaction ID
- TX bytes (request)
- RX bytes (raw response, unmodified)
- Responder CAN ID
- Transport errors (timeout, malformed, truncated)
- Firmware hash and profile hash
- Adapter configuration

#### 2.6 Discovery state model

Track three distinct states per PID — do not collapse them:
1. **Bitmap claims support**: the ECU's support bitmap says this PID is available
2. **Valid reply observed**: a probe request got a correctly-formatted response echoing this PID
3. **Usable value observed**: the decoded value is plausible for the signal

A timeout is NOT proof of unsupported status. An unanswered batch does not identify which member failed.

#### 2.7 Deliverable

A verified PID support table:

| PID | Signal | Standard/Enhanced | Bitmap | Answered | Raw example | Responder | Notes |
|-----|--------|-------------------|--------|----------|-------------|-----------|-------|

Plus: ECU identity (DME version, software), VIN, responder CAN IDs, transport verification, and any anomalies.

### Gate 3: Provisional Capture (standard PIDs only)

**Goal**: Capture data using only standard Mode 01 PIDs with known formulas. No enhanced channels yet.

#### 3.1 Build a provisional profile

Using the standard PIDs confirmed in Gate 2, write `engines/<id>.yaml` with:
- Only PIDs that have a validated standard formula AND a defined storage mapping
- `status: derived`
- Standard formulas from SAE J1979 (not re-invented from drive data)
- Cadence values carried from the existing profile template (N20) until measured

#### 3.2 What NOT to include yet

- Enhanced/manufacturer-specific PIDs (UDS 0x22, proprietary DIDs)
- PIDs whose response semantics are unclear
- PIDs that map to new capture fields (those need format changes first)
- Any PID whose formula was guessed rather than validated

#### 3.3 Validate with enginegen

```bash
cd tools/enginegen
cargo run -- ../../engines/ --engines <engine-id>
```

The path from `tools/enginegen/` to `engines/` is `../../engines/`, not `../engines/`.

#### 3.4 Independent formula validation

Do NOT compare the library against itself. Build an independent reference decoder:

| Test | Purpose |
|------|---------|
| Offline reference decoder | Decode fixtures using independently implemented standard formulas |
| Boundary vectors | Minimum, maximum, midpoint, negative timing/trim values, fixed-point rounding |
| Invalid-response vectors | Short replies, wrong PID/service, unexpected responder, duplicate PID, malformed batches |
| Integer semantics | Truncation, rounding, signed division, intermediate overflow, destination-field width |
| Clamp visibility | Preserve or flag out-of-range values rather than letting a clamp hide a defect |
| Recorded-log replay | Reproduce firmware decoding deterministically without another drive |

For example, timing advance (0x0E) has half-degree precision and can be negative (standardized range: -64 to 63.5 degrees). The profile must state whether the stored field preserves half degrees or intentionally quantizes them.

#### 3.5 Sample validity

Represent missing, unsupported, stale, malformed, saturated, and valid samples distinctly. Never silently substitute zero or reuse a previous value as though it were fresh. The current sentinel system (`CAIRN_U8_UNKNOWN`, `CAIRN_I16_UNKNOWN`, etc.) handles this — verify it works for every new PID.

#### 3.6 End-to-end decoding test

Test the full chain: raw response -> expression VM -> stored field -> capture record -> server decoder -> displayed value. Formula tests alone miss unit, scaling, signedness, and serialization errors.

#### 3.7 N20 regression

Run the existing N20 host tests to prove the new engine does not break the existing one:
```bash
make -C test/host engine
```

#### 3.8 Deliverable

A firmware build with the new engine that:
- Captures standard OBD data correctly
- Passes enginegen validation
- Passes host tests (new engine + N20 regression)
- Has a verified end-to-end decoding chain

### Gate 4: Validation (offline analysis)

**Goal**: Prove the provisional profile is correct through systematic offline analysis.

#### 4.1 Cadence measurement

From the discovery and capture logs:
- Single-request round trip latency: measure p50/p95/p99, not just "110-140ms"
- Multi-PID batch response time and success rate
- Per-channel delivery rate and freshness under mixed hot/cold traffic
- Timeouts, retries, and adapter overhead

#### 4.2 Sleep and power behaviour

Compare active-polling shutdown with a no-transmit baseline. The measurement method must not contaminate the sleep result. Record:
- When the ECU stops answering after ignition off
- Whether the body controller gates the OBD stub
- Alternator voltage behaviour (BMW alternator management is NOT a fixed-voltage system — do not assume a single `engine_on_mv` cutoff from one drive)
- Resting battery voltage across conditions

#### 4.3 Vehicle fingerprint

Record from the actual vehicle:
- Full VIN (Mode 09)
- DME identity and software version (Mode 09: calibration ID, CVN)
- Engine variant (e.g., N20 vs N26, PWG vs EWG)
- Transmission type
- Market
- Tune status (stock vs tuned — affects which channels are available)
- Production date

#### 4.4 Deliverable

- Measured cadence values (replacing template values)
- Measured sleep/wake thresholds
- Vehicle fingerprint documented
- Known limitations and open questions

### Gate 5: Verified (production confidence)

**Goal**: The profile is correct enough for production use.

#### 5.1 Verification matrix

"Three clean drives" measures repetition, not coverage. Use a verification matrix:

| Test condition | Required evidence |
|----------------|-------------------|
| Ignition on, engine off | Correct connection, no false engine-running classification |
| Cold start through warm-up | Valid temperatures, RPM, state transitions, sample freshness |
| Warm idle and steady cruise | Stable decoding, documented per-channel delivery/latency |
| Safe load variation | Correct signal semantics, no parser failures, saturation checks |
| Shutdown and locked parking | Device sleep, no unintended wakeups, log completion |
| Wake and restart | Recovery without stale data or permanent refusal flags |
| Transport failure | Replay/injected tests for timeout, partial batch, malformed payload |
| Existing engine vehicle | Unchanged profile selection, decoding, refusal, capture compatibility |

#### 5.2 Acceptance criteria

Choose acceptance numbers before testing. Example:
- Minimum valid-response rate per channel (e.g., >95% for hot PIDs)
- Maximum sample age per channel
- Zero silent substitution of stale/zero values
- N20 regression tests pass

#### 5.3 Upgrade to verified

After the verification matrix passes with chosen acceptance criteria:
```yaml
status: verified
```

#### 5.4 Separate verification levels

Do not conflate these — each needs its own evidence:
- **Verified decoding**: formulas are correct for this ECU
- **Verified transport behaviour**: bus timing, batch support, sleep/wake work
- **Validated analysis thresholds**: health warning limits (separate, later work)

### Gate 6: Analysis Profile (server-side, separate work)

**Goal**: Define what readings mean for this engine. This is explicitly NOT part of capture verification.

#### Key distinctions

- **Display bounds** are NOT alert thresholds. A plotting range is not a diagnosis.
- **Commanded vs measured lambda** must be separate fields. Do not issue measured-mixture warnings from 0x44.
- **Operating state** gates checks. PID 0x03 exposes fuel-system operating states. Gate checks by warm-up, fuel-system state, load, RPM, and signal freshness.
- **Temporal behaviour**: add persistence, hysteresis, minimum sample counts, suppression during unsuitable states.
- **Evidence level**: mark thresholds as disabled, provisional, or validated individually — not merely the whole profile as `derived`.

**Recommendation**: Make the first server profile descriptive, with health warnings disabled. Successful capture should not depend on inventing warning limits.

---

## Part 2: BMW B58 (M240i) — Specific Plan

### Current state

| Component | Status | File |
|---|---|---|
| B58 firmware profile | **Stub** — identity only | `engines/bmw-b58.yaml` |
| B58 analysis profile | **Stub** — no limits | `cairn-vehicle-server/internal/engine/profiles/bmw-b58.json` |
| PID discovery probe | **Working** | `src/mtprobe.cpp` |
| PID validation probe | **Working** | `src/pidtest.cpp` |
| Engine profile schema | **Draft** | `engines/SPEC.draft.md` |
| Engine code generator | **Working** | `tools/enginegen/` |
| Multi-PID batch | **Working** (confirmed N20) | `sensors.cpp:sensors_read_obd_batch()` |
| Cold-channel rotation | **Working** (confirmed N20) | `sensors.cpp:sensors_read_obd_extended()` |
| Formula language | **Working** | `lib/cairn_engine/cairn_expr.h` |
| Engine gate | **Working** — refuses stubs | `sensors.cpp:engine_gate()` |

### B58-specific research sources

#### Primary sources (use before first drive)

| Source | What it saves | Reliability |
|--------|--------------|-------------|
| [BMW B58 training PDF](https://www.bmwz3club.fr/site/faq/z4g29/BMW_B58_Engine.pdf) (April 2015) | Sensor architecture, Valvetronic, cooling, oil system, charge-air, DME 8.6 | BMW-authored, covers B58B30M0. Free. |
| [BMW PID Data repo](https://github.com/Shooooooooo/bmw_pid_data) | B58 PID table, shared entry structure, scraper, CSV-to-header converter | Community-derived, free. **Verify contents against actual DME.** |
| [CSS Electronics PID reference](https://www.csselectronics.com/pages/obd2-pid-table-on-board-diagnostics-j1979) | Standard PID formulas, signal names | Good reference; not the normative SAE standard. Free. |
| [MHD B58 Monitor License](https://mhdtuning.com/products/b58-monitor-license) | Confirmed B58 channels: boost/target, lambda, oil temp/pressure, trans temp, HPFP, per-cylinder timing, WGDC | Vendor docs, ~$130 USD. Some channels depend on MHD flash. |

#### Secondary sources

| Source | Use for |
|--------|---------|
| [B58 Log Review Thread](https://f30.bimmerpost.com/forums/showthread.php?t=1648722) | Channel checklist, terminology, example logs |
| [Bimmerprofs B58 intro](https://bimmerprofs.com/b58-introduction/) | DME 8.6.0 identification, specialist diagnostics |
| [MHD logging channels](https://spoolstreet.com/threads/mhd-logging-channels-for-b58.9597/) | Custom/less-common channel leads |

#### Known B58 facts from research

| Fact | Implication |
|------|-------------|
| DME 8.6 / 8.6.0 | Use as research identifier; read actual car's ECU identity |
| Valvetronic load control | Idle throttle is NOT near zero. Pedal, throttle, and engine load are separate signals. |
| Automatic start-stop | Include stopped-engine/awake-vehicle states. Voltage-only engine-running classifier needs more than one drive. |
| Map-controlled oil supply | Oil pressure thresholds need operating-state context. |
| Heat-management system | Coolant behaviour needs operating-state context. |
| MHD exposes enhanced channels | Oil pressure, trans temp, HPFP exist in established tooling — but exact requests need evidence. |
| Some MHD monitors are software-dependent | A channel working on a tuned car does not establish availability on stock DME. Record tune status. |

### B58-specific PID corrections

| Item | Correction |
|------|-----------|
| Oil temperature PID | Use 0x5C (oil temp), NOT 0x5D (injection timing) |
| 0x44 semantics | Commanded equivalence ratio, NOT measured lambda. Label accordingly. |
| 0xA4 interpretation | Standard response has 4 data bytes with support bit and ratio value, not a simple gear integer |
| Boost range | Standard 0x0B tops at 255 kPa absolute (~22.3 psi gauge with baro). Detect saturation. |
| Throttle at idle | B58 Valvetronic means throttle is NOT 0 at idle. Do not use as acceptance criterion. |

### B58 execution phases

#### Phase 0: Preparation

**Build MTPROBE firmware with restricted discovery mode:**

1. Add `CAIRN_MTPROBE_ALLOW_STUB` to `include/config.h`
2. Modify `engine_gate()` in `src/sensors.cpp` to respect it:
   ```cpp
   #if CAIRN_MTPROBE && CAIRN_MTPROBE_ALLOW_STUB
   // Discovery mode: allow stub engines past the gate for probe-only operation.
   // Production OBD capture is still blocked (no PID table to poll).
   #endif
   ```
3. Build: `make firmware DEFINES="-DCAIRN_MTPROBE=1 -DCAIRN_MTPROBE_SNIFF=1 -DCAIRN_MTPROBE_ALLOW_STUB=1 -DCAIRN_VEHICLE_ENGINE_ID='\"bmw-b58\"'"`

**Produce candidate manifest** from research sources above.

**Prepare offline tooling**: log parser for MTPROBE output, independent reference decoder.

#### Phase 1: Stationary discovery

With ignition on, engine off, M240i parked:
1. Read ECU identification (Mode 09): VIN, calibration ID, CVN, ECU name
2. Walk Mode 01 support bitmaps
3. Record responder CAN IDs
4. Verify transport/framing

#### Phase 2: Driven discovery

1. Drive M240i (15-30 min, city + highway)
2. MTPROBE probes candidate PIDs from manifest
3. Full logs preserved (not just grep)

#### Phase 3: Provisional capture profile

Write `engines/bmw-b58.yaml` with:
- Only standard Mode 01 PIDs confirmed in Phase 2
- Standard SAE formulas (not re-invented)
- `status: derived`
- Cadence carried from N20 template until measured

Validate with enginegen (path: `../../engines/` from `tools/enginegen/`).

#### Phase 4: Validation

- Independent formula decoder with boundary/error vectors
- End-to-end decode chain test
- Cadence measurement (latency distribution, not just average)
- Sleep/wake measurement (with no-transmit baseline)
- Vehicle fingerprint (VIN, DME, variant, tune status)
- N20 regression tests

#### Phase 5: Verification drives

Execute the verification matrix from Part 1, Gate 5. Choose acceptance criteria before testing.

#### Phase 6: Analysis profile (separate, later)

Make the first server profile descriptive only — display bounds, no health warnings. Warnings require operating-state gating, temporal behaviour, and validated thresholds.

### B58 files to create/modify

#### New files
- `engines/bmw-b58.yaml` — replace stub with derived profile
- `research/research_notes/BMW B58 OBD PID support/` — research notes, candidate manifest
- `test/host/b58_engine_test.c` — B58 formula and decoding tests

#### Modified files
- `include/config.h` — add `CAIRN_MTPROBE_ALLOW_STUB`
- `src/sensors.cpp` — respect new flag in `engine_gate()`
- `lib/cairn_engine/gen/cairn_engines_gen.h` — regenerated
- `tools/enginegen/tests/profiles.rs` — add B58 tests

#### Files NOT modified until separate work
- `cairn-vehicle-server/internal/engine/profiles/bmw-b58.json` — analysis thresholds are later
- `lib/cairn_engine/cairn_engine.h` — no new fields until format change
- `src/facts.h` — no new fact types yet
- `lib/cairn_format/cairn_format.h` — no new record types yet

---

## Part 3: N20 Profile Audit

The existing N20 profile is the reference implementation. Audit it against the research findings below before using it as a template for the B58.

### N20-specific sources

| Source | What it provides |
|--------|-----------------|
| [BMW N20 training PDF](https://www.bimmerpost.com/goodiesforyou/N20engine-techguide-BIMMERPOST.pdf) | Sensor architecture, DME MEVD17.2.4, Valvetronic 3rd gen, oil system, cooling |
| [Readable mirror](https://www.scribd.com/document/434971769/N20-Engine-BMW) | Searchable text with chapter/page references |
| [bootmod3 N20/N26](https://www.bootmod3.com/collections/bootmod3-menu/products/bootmod3-n20-n26-bmw-220i-228i-320i-328i-420i-428i-520i-528i) | Confirms N20/N26 logging, 200+ channels, PWG/EWG distinction. $595 USD license. |
| [N20/N26 bulletin SI B11 03 17](https://static.nhtsa.gov/odi/tsbs/2017/MC-10142923-9999.pdf) | Timing-chain concerns, N20 vs N26 identification |
| [BMW PID Data repo](https://github.com/Shooooooooo/bmw_pid_data) | **No N20-specific table.** N55 definitions are research candidates only. |

### N20 audit targets

| Target | What to check |
|--------|--------------|
| `0x44` field naming | It is commanded equivalence ratio, not measured lambda. Correct the firmware field name and server interpretation. |
| Oil-temperature mapping | Confirm 0x5C is used (not 0x5D). The N20 profile does not currently include oil temp — verify whether the N20 DME supports it. |
| MAP-derived boost | Standard 0x0B maxes at 255 kPa absolute. Verify saturation is detected, not silently capped. |
| Existing measurements | Attach raw-log evidence to batch support, latency, sleep/wake, voltage settings. Label as observations from this vehicle/configuration. |
| Decoding verification | Replay raw responses through old hard-coded path, generated profile, AND independent reference decoder. Agreement between the first two alone can preserve an old bug. |
| Missing data handling | Prove unsupported replies and timeouts do not become zeros or false server alerts. |
| Enhanced channels | Investigate oil pressure, rail pressure, actual boost, measured lambda as candidates. |

### N20 known facts from research

| Fact | Source | Implication |
|------|--------|-------------|
| DME: Bosch MEVD17.2.4 | Training manual p.121+ | Use for research; read actual car's DME identity |
| Closely related to N55 | Manual p.1 | N55 docs are a research lead, not permission to reuse request bytes |
| 3rd-gen Valvetronic | Manual pp.49-61 | Throttle, pedal, and engine load are separate signals |
| Map-controlled oil pump | Manual pp.64-90 | Oil pressure thresholds need operating-state context |
| Combined oil-pressure/temperature sensor | Manual p.85 | Documented sensor to research for enhanced channels |
| Electric coolant pump + map-controlled thermostat | Manual pp.92-97 | Coolant behaviour needs operating-state context |
| No fuel low-pressure sensor (early manual) | Manual p.7 | Do not assume low-side pressure channel exists on actual car |
| PWG vs EWG variants | bootmod3 product page | Record wastegate type; do not apply one boost policy to all N20s |
| N20 and N26 in F32 428i | SI B11 03 17 | "428i" alone is insufficient engine identification |
| No N20-specific PID table in community repo | GitHub bmw_pid_data | Do NOT fabricate one or silently substitute N55 definitions |

### N20 chain bulletin context

BMW's SI B11 03 17 covers timing-chain/oil-pump-chain concerns. The bulletin's diagnostic procedure includes identifying a characteristic lower-engine whining noise and following a service process. It does NOT provide a rule that normal RPM, temperature, or fuel-trim logs prove the chain is healthy. Do not create chain-health telemetry claims.

---

## Part 4: Risks and mitigations

### `engine_gate()` refuses stubs

**Mitigation**: Add `CAIRN_MTPROBE_ALLOW_STUB` build flag. The override grants permission to execute an explicit read-only probe allowlist, NOT globally make an unsupported engine "accepted." Reject the override in production builds. Display discovery mode in every session header.

### ECU identity mismatch

The training manual identifies one DME version; the actual car may differ. Always read the car's ECU identification (Mode 09) before adopting manufacturer-specific definitions.

### Multi-ECU responses

Functional OBD requests can receive replies on multiple ECU response IDs. Discover responders, preserve separate support maps per ECU, select the intended DME responder, and record request/response CAN IDs. Never merge replies from different ECUs into one support table.

### Enhanced PIDs require UDS

UDS service 0x22 DIDs are NOT the same namespace as Mode 01 PIDs. If UDS becomes necessary, give it its own plan with documented ECU/DID definitions and an allowlisted read-only implementation. Do not brute-force identifiers.

### New capture fields

If the B58 exposes PIDs that map to new fields (transmission gear, oil temp, ethanol %), the capture format needs changes. This is a separate effort. For the initial B58 profile, use only the existing 16 fields.

### BMW alternator management

BMW's intelligent generator control is NOT a fixed-voltage system. A single `engine_on_mv` cutoff needs validation across conditions — charging voltage varies with battery state, temperature, and load management. Do not adopt from one drive.

### Transport/framing contract

Specify whether logs contain CAN frames, ISO-TP payloads, adapter text, or normalized replies. Define header/padding removal, multi-frame reassembly, length checks, timeouts, and truncated-message behaviour. The current ELM-style adapter text format needs explicit documentation.

### Shared transport ownership

Probe, regular polling, and sniffing share one OBD link. Define whether sniffing suspends polling, how late replies are handled, and whether changing adapter modes requires restoration.

---

## Timeline (B58-specific)

| Phase | Effort | Dependencies |
|-------|--------|-------------|
| 0: Preparation | 2-3 days | None |
| 1: Stationary discovery | 1 session | Phase 0 |
| 2: Driven discovery | 1-2 drives | Phase 1 |
| 3: Provisional profile | 2-3 days | Phase 2 |
| 4: Validation (offline) | 3-5 days | Phase 3 |
| 5: Verification drives | 3+ drives, verification matrix | Phase 4 |
| 6: Analysis profile | Separate, later | Phase 5 |

**Total**: ~3-4 weeks from preparation to verified capture profile. Analysis thresholds are a separate, later effort.

---

## Appendix A: Copyable brief for next engine

```text
Before implementing <ENGINE> discovery, research these sources:

1. Standard Mode 01 PID definitions:
   https://www.csselectronics.com/pages/obd2-pid-table-on-board-diagnostics-j1979
   Use for standard formula validation. Do not re-invent standard conversions.

2. Manufacturer training manual (if available):
   <URL>
   Focus on sensor architecture, DME identity, load control, cooling, oil system.

3. Community PID data (if available):
   https://github.com/Shooooooooo/bmw_pid_data
   Treat entries as candidates, not verified facts. Check licensing.
   Verify request format (Mode 01 vs UDS 0x22 vs proprietary).

4. Vendor logging tools (if available):
   <MHD/bootmod3 URL>
   Use for channel identification. Do not infer raw requests from names.
   Record software/tune dependencies.

5. Community logging threads:
   <Bimmerpost/Spoolstreet URL>
   Use for terminology and example values, not health limits.

Produce a source-backed candidate manifest containing:
signal name, standard/enhanced classification, request bytes,
ECU addressing, response layout, formula, unit, provenance,
applicability, licensing status, and confidence.

Mark unknowns explicitly. Verify candidates against the actual DME.
Keep initial discovery read-only and allowlisted.
Do not brute-force identifiers or alter diagnostic sessions.

Prioritise validated standard-PID capture for existing fields.
Treat enhanced diagnostics and health thresholds as separate work.
```