//! The engine profile model (cairn.engine/v1-draft), strict parsing and validation.
//!
//! Strictness is the point: an unknown field, a bad formula, a duplicate PID or an
//! impossible range is an error that stops the build, never a warning. The draft
//! JSON Schema in engines/engine.schema.draft.json states the same structure for
//! other consumers; a test keeps the two in step on the shipped profiles and on the
//! negative vectors.

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

use serde::de::{DeserializeOwned, Error as _};
use serde::{Deserialize, Deserializer};
use sha2::{Digest, Sha256};

use crate::expr;

pub const SCHEMA_ID: &str = "cairn.engine/v1-draft";

/// A value the profile may state, or explicitly say is not known.
///
/// `unknown` is a deliberate answer ("nobody has established this"), distinct from a
/// missing key, which is an error. The firmware treats unknown as "no data" and falls
/// back to its own device default, logging that it did so.
#[derive(Debug, Clone, PartialEq)]
pub enum Maybe<T> {
    Unknown,
    Known(T),
}

impl<T> Maybe<T> {
    pub fn known(&self) -> Option<&T> {
        match self {
            Maybe::Known(v) => Some(v),
            Maybe::Unknown => None,
        }
    }
}

impl<'de, T: DeserializeOwned> Deserialize<'de> for Maybe<T> {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let v = serde_yaml::Value::deserialize(d)?;
        if v.as_str() == Some("unknown") {
            return Ok(Maybe::Unknown);
        }
        T::deserialize(v).map(Maybe::Known).map_err(D::Error::custom)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Status {
    /// Identity only. Claims nothing else, and the generator enforces that.
    Stub,
    /// Extracted from behaviour the firmware already has; not independently re-measured.
    Derived,
    /// Checked against raw ECU replies from the real car.
    Verified,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Tier {
    Hot,
    Cold,
}

/// The capture-record fields a PID can feed. The firmware enumerates the same list in
/// lib/cairn_engine/cairn_engine.h (cairn_field_t); the order is the enum's value.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Field {
    Rpm,
    SpeedKph,
    ThrottlePct,
    TimingAdvanceDeg,
    MapKpa,
    LambdaE4,
    EngineLoadPct,
    CoolantTempC,
    IntakeTempC,
    MafCgps,
    AmbientTempC,
    FuelTrimShortPct,
    FuelTrimLongPct,
    BaroKpa,
    AbsLoadRaw,
    FuelLevelPct,
}

impl Field {
    pub const ALL: [Field; 16] = [
        Field::Rpm,
        Field::SpeedKph,
        Field::ThrottlePct,
        Field::TimingAdvanceDeg,
        Field::MapKpa,
        Field::LambdaE4,
        Field::EngineLoadPct,
        Field::CoolantTempC,
        Field::IntakeTempC,
        Field::MafCgps,
        Field::AmbientTempC,
        Field::FuelTrimShortPct,
        Field::FuelTrimLongPct,
        Field::BaroKpa,
        Field::AbsLoadRaw,
        Field::FuelLevelPct,
    ];

    /// Index into ALL, plus one: 0 is reserved for "no field" in the C enum.
    pub fn c_value(self) -> usize {
        Field::ALL.iter().position(|f| *f == self).unwrap() + 1
    }

    pub fn c_name(self) -> String {
        format!("CAIRN_FIELD_{}", self.snake().to_uppercase())
    }

    pub fn snake(self) -> &'static str {
        match self {
            Field::Rpm => "rpm",
            Field::SpeedKph => "speed_kph",
            Field::ThrottlePct => "throttle_pct",
            Field::TimingAdvanceDeg => "timing_advance_deg",
            Field::MapKpa => "map_kpa",
            Field::LambdaE4 => "lambda_e4",
            Field::EngineLoadPct => "engine_load_pct",
            Field::CoolantTempC => "coolant_temp_c",
            Field::IntakeTempC => "intake_temp_c",
            Field::MafCgps => "maf_cgps",
            Field::AmbientTempC => "ambient_temp_c",
            Field::FuelTrimShortPct => "fuel_trim_short_pct",
            Field::FuelTrimLongPct => "fuel_trim_long_pct",
            Field::BaroKpa => "baro_kpa",
            Field::AbsLoadRaw => "abs_load_raw",
            Field::FuelLevelPct => "fuel_level_pct",
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Years {
    pub from: u16,
    pub to: u16,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AppliesTo {
    pub make: String,
    pub models: Vec<String>,
    pub years: Maybe<Years>,
    /// 17 characters, `?` matches any character. How a VIN identifies this engine.
    pub vin_patterns: Maybe<Vec<String>>,
    /// VIN position 4-8 substring (e.g. "A5C5" for N20, "2J7C" for B58).
    /// Shorter and simpler than full VIN patterns; matches the engine variant
    /// encoded in BMW's VDS section.
    #[serde(default)]
    pub engine_codes: Option<Vec<String>>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum FuelType {
    Gasoline,
    Diesel,
    Hybrid,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Fuel {
    #[serde(rename = "type")]
    pub fuel_type: FuelType,
    pub blend: Maybe<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Obd {
    pub bus: String,
    pub bitrate_kbps: u32,
    pub addressing: Maybe<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Pid {
    pub id: u32,
    #[serde(default = "default_service")]
    pub service: u8,
    pub name: String,
    pub field: Field,
    pub unit: String,
    #[serde(default)]
    pub scale: i8,
    pub bytes: u8,
    pub tier: Tier,
    #[serde(default)]
    pub cold_slot: Option<u8>,
    pub formula: String,
    pub range: [i64; 2],
    pub log: bool,
}

fn default_service() -> u8 {
    0x01
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Measured {
    pub single_request_ms: [u16; 2],
    pub batch_6pid_ms: u16,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Cadence {
    pub obd_period_ms: Maybe<u16>,
    pub obd_batch_period_ms: Maybe<u16>,
    pub cold_slots: Maybe<u8>,
    #[serde(default)]
    pub measured: Option<Measured>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct WakeSleep {
    pub engine_on_mv: Maybe<u16>,
    pub standby_idle_ms: Maybe<u32>,
    pub standby_heartbeat_ms: Maybe<u32>,
    pub drive_voltage_dwell_ms: Maybe<u32>,
    pub drive_motion_dwell_ms: Maybe<u32>,
    pub drive_both_dwell_ms: Maybe<u32>,
    pub bus_first_sleep_phase_ms: Maybe<u32>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Supply {
    pub plausible_mv: Maybe<[u16; 2]>,
    #[serde(default)]
    pub alternator_measured_mv: Option<[u16; 2]>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Probe {
    pub id: u32,
    pub name: String,
    pub std_bytes: u8,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub schema: String,
    pub engine_id: String,
    pub version: u16,
    pub name: String,
    pub status: Status,
    pub sources: Vec<String>,
    pub applies_to: AppliesTo,
    pub fuel: Maybe<Fuel>,
    pub obd: Maybe<Obd>,
    pub pids: Maybe<Vec<Pid>>,
    pub cadence: Maybe<Cadence>,
    pub wake_sleep: Maybe<WakeSleep>,
    pub supply: Maybe<Supply>,
    #[serde(default)]
    pub probes: Vec<Probe>,
}

/// A profile that passed validation, with its formulas compiled and its identity hash.
#[derive(Debug, Clone)]
pub struct Checked {
    pub profile: Profile,
    /// SHA-256 of the file's bytes, after normalising CRLF to LF.
    pub sha256: [u8; 32],
    /// One compiled formula per PID, in the same order as `ordered_pids`.
    pub code: Vec<Vec<u8>>,
}

impl Checked {
    /// Hot PIDs first, in file order (that is the batch request order), then cold PIDs
    /// by slot. This is the order of the C table.
    pub fn ordered_pids(&self) -> Vec<&Pid> {
        match &self.profile.pids {
            Maybe::Known(v) => {
                let mut hot: Vec<&Pid> = v.iter().filter(|p| p.tier == Tier::Hot).collect();
                let mut cold: Vec<&Pid> = v.iter().filter(|p| p.tier == Tier::Cold).collect();
                cold.sort_by_key(|p| p.cold_slot);
                hot.extend(cold);
                hot
            }
            Maybe::Unknown => Vec::new(),
        }
    }
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

pub fn content_hash(bytes: &[u8]) -> [u8; 32] {
    let text: Vec<u8> = {
        let mut out = Vec::with_capacity(bytes.len());
        let mut i = 0;
        while i < bytes.len() {
            if bytes[i] == b'\r' && bytes.get(i + 1) == Some(&b'\n') {
                i += 1;
                continue;
            }
            out.push(bytes[i]);
            i += 1;
        }
        out
    };
    Sha256::digest(&text).into()
}

pub fn parse_yaml(text: &str) -> Result<Profile, String> {
    serde_yaml::from_str::<Profile>(text).map_err(|e| e.to_string())
}

fn valid_id(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 31
        && s.split('-').all(|p| !p.is_empty() && p.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit()))
}

fn valid_vin_pattern(s: &str) -> bool {
    s.len() == 17 && s.bytes().all(|c| c == b'?' || (c.is_ascii_uppercase() && !b"IOQ".contains(&c)) || c.is_ascii_digit())
}

/// Validate parsed YAML. `stem` is the file name without extension.
pub fn check(profile: Profile, stem: &str, file_bytes: &[u8]) -> Result<Checked, String> {
    let mut errs: Vec<String> = Vec::new();
    macro_rules! bad {
        ($($a:tt)*) => { errs.push(format!($($a)*)) };
    }

    if profile.schema != SCHEMA_ID {
        bad!("schema is `{}`, this generator reads `{}`", profile.schema, SCHEMA_ID);
    }
    if !valid_id(&profile.engine_id) {
        bad!("engine_id `{}` must be lowercase words joined by `-`, at most 31 characters", profile.engine_id);
    }
    if profile.engine_id != stem {
        bad!("engine_id `{}` does not match the file name `{}.yaml`", profile.engine_id, stem);
    }
    if profile.version == 0 {
        bad!("version must be at least 1");
    }
    if profile.name.trim().is_empty() {
        bad!("name is empty");
    }
    if profile.sources.is_empty() || profile.sources.iter().any(|s| s.trim().is_empty()) {
        bad!("sources must list where the content came from");
    }
    if profile.applies_to.make.trim().is_empty() || profile.applies_to.models.is_empty() {
        bad!("applies_to needs a make and at least one model");
    }
    if let Maybe::Known(y) = &profile.applies_to.years {
        if y.from > y.to {
            bad!("applies_to.years: from {} is after to {}", y.from, y.to);
        }
    }
    if let Maybe::Known(v) = &profile.applies_to.vin_patterns {
        let mut seen = BTreeSet::new();
        if v.is_empty() {
            bad!("applies_to.vin_patterns is empty; say `unknown` instead");
        }
        for p in v {
            if !valid_vin_pattern(p) {
                bad!("vin pattern `{p}` must be 17 characters of A-Z (no I, O, Q), 0-9 or `?`");
            }
            if !seen.insert(p) {
                bad!("duplicate vin pattern `{p}`");
            }
        }
    }
    if let Some(codes) = &profile.applies_to.engine_codes {
        let mut seen = BTreeSet::new();
        if codes.is_empty() {
            bad!("applies_to.engine_codes is empty; omit it instead");
        }
        for c in codes {
            if c.is_empty() || c.len() > 8 || !c.bytes().all(|b| b.is_ascii_alphanumeric()) {
                bad!("engine code `{c}` must be 1-8 alphanumeric characters");
            }
            if !seen.insert(c.as_str()) {
                bad!("duplicate engine code `{c}`");
            }
        }
    }
    if let Maybe::Known(o) = &profile.obd {
        if o.bus.trim().is_empty() || o.bitrate_kbps == 0 {
            bad!("obd needs a bus and a non-zero bitrate_kbps");
        }
    }

    // A stub claims nothing. This is what stops an unverified engine borrowing the
    // numbers of another one by copy and paste.
    if profile.status == Status::Stub {
        let claims = [
            ("fuel", matches!(profile.fuel, Maybe::Known(_))),
            ("obd", matches!(profile.obd, Maybe::Known(_))),
            ("pids", matches!(profile.pids, Maybe::Known(_))),
            ("cadence", matches!(profile.cadence, Maybe::Known(_))),
            ("wake_sleep", matches!(profile.wake_sleep, Maybe::Known(_))),
            ("supply", matches!(profile.supply, Maybe::Known(_))),
            ("years", matches!(profile.applies_to.years, Maybe::Known(_))),
            ("vin_patterns", matches!(profile.applies_to.vin_patterns, Maybe::Known(_))),
            ("probes", !profile.probes.is_empty()),
        ];
        for (name, claimed) in claims {
            if claimed {
                bad!("status is `stub`, which claims nothing, but `{name}` states a value; use `unknown` or raise the status");
            }
        }
    }

    // ── cadence ──
    let mut cold_slots_known: Option<u8> = None;
    let mut batch_known = false;
    let mut period_known = false;
    if let Maybe::Known(c) = &profile.cadence {
        if let Maybe::Known(p) = &c.obd_period_ms {
            period_known = true;
            if *p == 0 {
                bad!("cadence.obd_period_ms must be positive");
            }
        }
        if let Maybe::Known(p) = &c.obd_batch_period_ms {
            batch_known = true;
            if *p == 0 {
                bad!("cadence.obd_batch_period_ms must be positive");
            }
        }
        if let Maybe::Known(s) = &c.cold_slots {
            cold_slots_known = Some(*s);
            if *s == 0 || *s > 32 {
                bad!("cadence.cold_slots must be 1..=32, got {s}");
            }
        }
        if let Some(m) = &c.measured {
            if m.single_request_ms[0] > m.single_request_ms[1] {
                bad!("cadence.measured.single_request_ms is a [min, max] pair but min > max");
            }
        }
    }

    // ── wake / sleep ──
    if let Maybe::Known(w) = &profile.wake_sleep {
        if let Maybe::Known(mv) = &w.engine_on_mv {
            if !(1000..=65000).contains(mv) {
                bad!("wake_sleep.engine_on_mv {mv} is not a plausible supply voltage");
            }
            if let Maybe::Known(s) = &profile.supply {
                if let Maybe::Known(r) = &s.plausible_mv {
                    if *mv < r[0] || *mv > r[1] {
                        bad!("wake_sleep.engine_on_mv {mv} lies outside supply.plausible_mv {}..{}: it could never trigger", r[0], r[1]);
                    }
                }
            }
        }
        let positives: [(&str, &Maybe<u32>); 6] = [
            ("standby_idle_ms", &w.standby_idle_ms),
            ("standby_heartbeat_ms", &w.standby_heartbeat_ms),
            ("drive_voltage_dwell_ms", &w.drive_voltage_dwell_ms),
            ("drive_motion_dwell_ms", &w.drive_motion_dwell_ms),
            ("drive_both_dwell_ms", &w.drive_both_dwell_ms),
            ("bus_first_sleep_phase_ms", &w.bus_first_sleep_phase_ms),
        ];
        for (n, v) in positives {
            if let Maybe::Known(0) = v {
                bad!("wake_sleep.{n} must be positive");
            }
        }
        if let (Maybe::Known(b), Maybe::Known(v)) = (&w.drive_both_dwell_ms, &w.drive_voltage_dwell_ms) {
            if b > v {
                bad!("wake_sleep.drive_both_dwell_ms {b} exceeds drive_voltage_dwell_ms {v}: the both-signals shortcut must not be slower than either signal alone");
            }
        }
        if let (Maybe::Known(b), Maybe::Known(m)) = (&w.drive_both_dwell_ms, &w.drive_motion_dwell_ms) {
            if b > m {
                bad!("wake_sleep.drive_both_dwell_ms {b} exceeds drive_motion_dwell_ms {m}: the both-signals shortcut must not be slower than either signal alone");
            }
        }
    }

    // ── supply ──
    if let Maybe::Known(s) = &profile.supply {
        if let Maybe::Known(r) = &s.plausible_mv {
            if r[0] >= r[1] {
                bad!("supply.plausible_mv {}..{} is empty", r[0], r[1]);
            }
        }
        if let Some(a) = &s.alternator_measured_mv {
            if a[0] > a[1] {
                bad!("supply.alternator_measured_mv min exceeds max");
            }
            if let Maybe::Known(r) = &s.plausible_mv {
                if a[0] < r[0] || a[1] > r[1] {
                    bad!("supply.alternator_measured_mv lies outside supply.plausible_mv");
                }
            }
        }
    }

    // ── probes ──
    {
        let mut seen = BTreeSet::new();
        for p in &profile.probes {
            if p.id > 0xFF || p.std_bytes == 0 || p.std_bytes > 7 {
                bad!("probe `{}`: id must be a Mode 01 PID (0..=0xFF) and std_bytes 1..=7", p.name);
            }
            if !seen.insert(p.id) {
                bad!("duplicate probe PID 0x{:02X}", p.id);
            }
        }
    }

    // ── pids ──
    let mut code: Vec<Vec<u8>> = Vec::new();
    if let Maybe::Known(pids) = &profile.pids {
        if pids.is_empty() {
            bad!("pids is empty; say `unknown` instead");
        }
        if !period_known {
            bad!("pids are listed but cadence.obd_period_ms is unknown");
        }
        let mut ids = BTreeSet::new();
        let mut names = BTreeSet::new();
        let mut fields = BTreeSet::new();
        let mut slots = BTreeMap::new();
        let (mut hot, mut cold) = (0, 0);
        for p in pids {
            let tag = format!("pid {} (0x{:02X})", p.name, p.id);
            if !(p.service == 0x01 || p.service == 0x22) {
                bad!("{tag}: service 0x{:02X} is not supported (0x01 or 0x22)", p.service);
            }
            let max_id = if p.service == 0x01 { 0xFF } else { 0xFFFF };
            if p.id > max_id {
                bad!("{tag}: id is out of range for service 0x{:02X}", p.service);
            }
            if !ids.insert((p.service, p.id)) {
                bad!("{tag}: duplicate PID (service 0x{:02X}, id 0x{:02X})", p.service, p.id);
            }
            if !valid_ident(&p.name) || !names.insert(p.name.clone()) {
                bad!("{tag}: name must be unique lowercase_snake_case");
            }
            if !fields.insert(p.field) {
                bad!("{tag}: field `{}` is fed by more than one PID", p.field.snake());
            }
            if p.bytes == 0 || p.bytes > 4 {
                bad!("{tag}: bytes must be 1..=4");
            }
            if p.unit.trim().is_empty() {
                bad!("{tag}: unit is empty");
            }
            if !(-9..=9).contains(&p.scale) {
                bad!("{tag}: scale must be a decimal exponent within -9..=9");
            }
            match p.tier {
                Tier::Hot => {
                    hot += 1;
                    if p.service != 0x01 {
                        bad!("{tag}: only Mode 01 PIDs can be in the multi-PID batch");
                    }
                    if p.cold_slot.is_some() {
                        bad!("{tag}: a hot PID has no cold_slot");
                    }
                    if !batch_known {
                        bad!("{tag}: hot PIDs need cadence.obd_batch_period_ms");
                    }
                }
                Tier::Cold => {
                    cold += 1;
                    match (p.cold_slot, cold_slots_known) {
                        (None, _) => bad!("{tag}: a cold PID needs a cold_slot"),
                        (Some(s), Some(n)) if s >= n => bad!("{tag}: cold_slot {s} is not below cadence.cold_slots {n}"),
                        (Some(_), None) => bad!("{tag}: cold PIDs need cadence.cold_slots"),
                        (Some(s), Some(_)) => {
                            if let Some(prev) = slots.insert(s, p.name.clone()) {
                                bad!("{tag}: cold_slot {s} is already used by {prev}");
                            }
                        }
                    }
                }
            }
            if p.range[0] > p.range[1] {
                bad!("{tag}: range {}..{} is empty", p.range[0], p.range[1]);
            }
            match expr::compile(&p.formula, p.bytes as usize) {
                Err(e) => {
                    bad!("{tag}: formula `{}`: {e}", p.formula);
                    code.push(Vec::new());
                }
                Ok(c) => {
                    if c.range.0 < p.range[0] || c.range.1 > p.range[1] {
                        bad!(
                            "{tag}: formula `{}` can produce {}..{} but the declared range is {}..{}",
                            p.formula,
                            c.range.0,
                            c.range.1,
                            p.range[0],
                            p.range[1]
                        );
                    }
                    code.push(c.code);
                }
            }
        }
        if hot > 6 {
            bad!("{hot} hot PIDs, but one Mode 01 request carries at most six");
        }
        let _ = cold;
        // Re-order `code` to match ordered_pids() once everything else passed.
        if errs.is_empty() {
            let mut idx: Vec<usize> = (0..pids.len()).collect();
            idx.sort_by_key(|i| match pids[*i].tier {
                Tier::Hot => (0, *i as u8),
                Tier::Cold => (1, pids[*i].cold_slot.unwrap_or(0)),
            });
            code = idx.into_iter().map(|i| code[i].clone()).collect();
        }
    } else if cold_slots_known.is_some() || batch_known || period_known {
        // Cadence without PIDs is allowed (it is a statement about the car), nothing to check.
    }

    if errs.is_empty() {
        Ok(Checked { profile, sha256: content_hash(file_bytes), code })
    } else {
        Err(errs.join("\n  "))
    }
}

fn valid_ident(s: &str) -> bool {
    !s.is_empty() && s.len() <= 40 && s.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'_') && !s.starts_with(|c: char| c.is_ascii_digit())
}

/// Read, parse and validate one file.
pub fn load(path: &Path) -> Result<Checked, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let text = std::str::from_utf8(&bytes).map_err(|_| format!("{}: not UTF-8", path.display()))?;
    let stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
    let profile = parse_yaml(text).map_err(|e| format!("{}: {e}", path.display()))?;
    check(profile, stem, &bytes).map_err(|e| format!("{}:\n  {e}", path.display()))
}

/// Load every `*.yaml` in `dir`, sorted by engine id. Any invalid file fails the lot.
pub fn load_dir(dir: &Path) -> Result<Vec<Checked>, String> {
    let rd = std::fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let mut paths: Vec<_> = rd
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().and_then(|s| s.to_str()) == Some("yaml"))
        .collect();
    paths.sort();
    if paths.is_empty() {
        return Err(format!("{}: no engine profiles (*.yaml) found", dir.display()));
    }
    let mut out = Vec::new();
    let mut errs = Vec::new();
    for p in paths {
        match load(&p) {
            Ok(c) => out.push(c),
            Err(e) => errs.push(e),
        }
    }
    if !errs.is_empty() {
        return Err(errs.join("\n"));
    }
    out.sort_by(|a, b| a.profile.engine_id.cmp(&b.profile.engine_id));
    Ok(out)
}

/// Resolve a selection (`all` or a comma list) against the loaded profiles.
pub fn select<'a>(all: &'a [Checked], spec: &str) -> Result<Vec<&'a Checked>, String> {
    let spec = spec.trim();
    if spec.is_empty() {
        return Err("no engines selected (use ENGINES=all or ENGINES=bmw-n20,bmw-b58)".into());
    }
    if spec == "all" {
        return Ok(all.iter().collect());
    }
    let mut chosen: BTreeMap<&str, &Checked> = BTreeMap::new();
    for want in spec.split(',').map(str::trim) {
        if want == "all" {
            return Err("`all` cannot be combined with other engine names".into());
        }
        match all.iter().find(|c| c.profile.engine_id == want) {
            Some(c) => {
                if chosen.insert(c.profile.engine_id.as_str(), c).is_some() {
                    return Err(format!("engine `{want}` is selected twice"));
                }
            }
            None => {
                let have: Vec<_> = all.iter().map(|c| c.profile.engine_id.as_str()).collect();
                return Err(format!("unknown engine `{want}`; engines/ has: {}", have.join(", ")));
            }
        }
    }
    Ok(chosen.into_values().collect())
}
