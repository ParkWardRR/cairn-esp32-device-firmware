//! Decoders for the plaintext payloads the conformance vectors exercise (§4).
//!
//! These only ever see a payload from a keyed scan: a structural scan exposes no
//! payload at all (see [`super::frame::Frame`]), because ciphertext is exactly
//! as long as plaintext and would decode into a plausible record of random
//! numbers.
//!
//! Sentinels are decoded to `None`, never to a number. "The ECU reported 0" and
//! "the ECU did not answer" are different facts, and collapsing them fabricates
//! data.

use super::{FormatError, Result};

// ── TRIP_EVENT (§4.6) ────────────────────────────────────────────────────────

/// Event types. An unknown type is an event this decoder does not understand,
/// not one to discard: it keeps its position and timing.
pub mod event {
    pub const TRIP_START: u8 = 1;
    pub const TRIP_END: u8 = 2;
    pub const HARSH_BRAKE: u8 = 3;
    pub const HARSH_ACCELERATION: u8 = 4;
    pub const HARSH_CORNERING: u8 = 5;
    pub const IMPACT: u8 = 6;
    /// Decisive dynamics whose cause could not be attributed. Carries no guess
    /// on purpose: a guessed label is indistinguishable from a measured one.
    pub const HARSH_MOTION: u8 = 7;
    pub const CAPTURE_RECOVERED: u8 = 8;
}

/// Matches the firmware's detail field.
pub const MAX_EVENT_DETAIL: usize = 48;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TripEvent {
    pub event_type: u8,
    /// Zero when no valid fix was available; position validity travels with
    /// the nearest `GNSS_SAMPLE`, never assumed from these.
    pub lat_e7: i32,
    pub lon_e7: i32,
    pub detail: String,
}

/// The spec's name for an event type; unknown types are named, not dropped.
pub fn event_type_name(t: u8) -> String {
    match t {
        event::TRIP_START => "TRIP_START".into(),
        event::TRIP_END => "TRIP_END".into(),
        event::HARSH_BRAKE => "HARSH_BRAKE".into(),
        event::HARSH_ACCELERATION => "HARSH_ACCELERATION".into(),
        event::HARSH_CORNERING => "HARSH_CORNERING".into(),
        event::IMPACT => "IMPACT".into(),
        event::HARSH_MOTION => "HARSH_MOTION".into(),
        event::CAPTURE_RECOVERED => "CAPTURE_RECOVERED".into(),
        other => format!("UNKNOWN_EVENT({other})"),
    }
}

/// Build a `TRIP_EVENT` payload. A detail longer than [`MAX_EVENT_DETAIL`] is
/// truncated rather than rejected: the event matters more than its
/// annotation, and refusing to record a hard stop because its description was
/// long would be the wrong trade.
pub fn encode_trip_event(event_type: u8, lat_e7: i32, lon_e7: i32, detail: &str) -> Vec<u8> {
    let mut d = detail.as_bytes();
    if d.len() > MAX_EVENT_DETAIL {
        d = &d[..MAX_EVENT_DETAIL];
    }
    let mut out = Vec::with_capacity(12 + d.len());
    out.push(event_type);
    out.push(d.len() as u8);
    out.extend_from_slice(&[0, 0]); // reserved
    out.extend_from_slice(&lat_e7.to_le_bytes());
    out.extend_from_slice(&lon_e7.to_le_bytes());
    out.extend_from_slice(d);
    out
}

pub fn parse_trip_event(p: &[u8]) -> Result<TripEvent> {
    const MIN: usize = 12;
    if p.len() < MIN {
        return Err(FormatError::Malformed(format!(
            "TRIP_EVENT payload is {} bytes, want at least {MIN}",
            p.len()
        )));
    }
    let detail_len = p[1] as usize;
    if MIN + detail_len > p.len() {
        return Err(FormatError::Malformed(format!(
            "TRIP_EVENT detail_len {detail_len} exceeds the {} bytes available",
            p.len() - MIN
        )));
    }
    Ok(TripEvent {
        event_type: p[0],
        lat_e7: i32::from_le_bytes(p[4..8].try_into().unwrap()),
        lon_e7: i32::from_le_bytes(p[8..12].try_into().unwrap()),
        // Lossy rather than an error: an event with a mangled annotation is
        // still an event, and dropping it would lose its position and time.
        detail: String::from_utf8_lossy(&p[MIN..MIN + detail_len]).into_owned(),
    })
}

// ── DEVICE_HEALTH (§4.5, §4.10) ─────────────────────────────────────────────

const HEALTH_BITS: [(u8, &str); 7] = [
    (0x01, "DEGRADED_GNSS"),
    (0x02, "DEGRADED_STORAGE"),
    (0x04, "DEGRADED_TIME"),
    (0x08, "DEGRADED_NETWORK"),
    (0x10, "LOW_POWER"),
    (0x20, "RECOVERY_REQUIRED"),
    (0x40, "DEGRADED_SENSING"),
];

/// `health_state` from a 16-byte `DEVICE_HEALTH` payload.
pub fn parse_health_state(p: &[u8]) -> Result<u8> {
    if p.len() != 16 {
        return Err(FormatError::Malformed(format!(
            "DEVICE_HEALTH payload is {} bytes, want 16",
            p.len()
        )));
    }
    Ok(p[12])
}

/// Render the degraded-state bitmap as names, one per active condition.
///
/// A bitmap, not a severity: a low battery must not hide an unavailable fix.
/// Unknown bits are rendered as hex rather than masked away, so a bundle from
/// newer firmware stays interpretable for the conditions this build does
/// understand.
pub fn health_state_names(state: u8) -> Vec<String> {
    let mut out = Vec::new();
    let mut seen = 0u8;
    for (bit, name) in HEALTH_BITS {
        if state & bit != 0 {
            out.push(name.to_string());
            seen |= bit;
        }
    }
    let unknown = state & !seen;
    if unknown != 0 {
        out.push(format!("0x{unknown:02x}"));
    }
    out
}

// ── OBD_EXTENDED (§4.11) ────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
pub struct ObdExtended {
    /// Intake manifold **absolute** pressure, PID 0x0B.
    pub map_kpa: Option<u16>,
    /// Mass air flow in centigrams/s, PID 0x10.
    pub maf_cgps: Option<u16>,
    /// Equivalence ratio × 10000, PID 0x44.
    pub lambda_e4: Option<u16>,
    /// Raw `(A*256)+B` from PID 0x43; percent is `raw × 100 ÷ 255`.
    pub abs_load_raw: Option<u16>,
    pub baro_kpa: Option<u8>,
    pub ambient_temp_c: Option<i8>,
    pub fuel_trim_short_pct: Option<i8>,
    pub fuel_trim_long_pct: Option<i8>,
    pub pids_requested: u32,
    pub pids_answered: u32,
    pub poll_cadence_ms: u16,
    /// Byte 22, PID 0x2F, `0xFF` = absent.
    ///
    /// The spec's §4.11 table still lists bytes 22–23 as reserved, but the Go
    /// reference decodes byte 22 as fuel level and the firmware writes it
    /// there. Decoded here to match both writers; flagged as a spec gap in the
    /// port report rather than silently papered over.
    pub fuel_level_pct: Option<u8>,
}

fn opt_u16(v: u16) -> Option<u16> {
    (v != 0xFFFF).then_some(v)
}

fn opt_u8(v: u8) -> Option<u8> {
    (v != 0xFF).then_some(v)
}

fn opt_i8(v: u8) -> Option<i8> {
    (v != 0x80).then_some(v as i8)
}

pub fn parse_obd_extended(p: &[u8]) -> Result<ObdExtended> {
    if p.len() != 24 {
        return Err(FormatError::Malformed(format!(
            "OBD_EXTENDED payload is {} bytes, want 24",
            p.len()
        )));
    }
    let u16_at = |o: usize| u16::from_le_bytes([p[o], p[o + 1]]);
    let u32_at = |o: usize| u32::from_le_bytes(p[o..o + 4].try_into().unwrap());
    Ok(ObdExtended {
        map_kpa: opt_u16(u16_at(0)),
        maf_cgps: opt_u16(u16_at(2)),
        lambda_e4: opt_u16(u16_at(4)),
        abs_load_raw: opt_u16(u16_at(6)),
        baro_kpa: opt_u8(p[8]),
        ambient_temp_c: opt_i8(p[9]),
        fuel_trim_short_pct: opt_i8(p[10]),
        fuel_trim_long_pct: opt_i8(p[11]),
        pids_requested: u32_at(12),
        pids_answered: u32_at(16),
        poll_cadence_ms: u16_at(20),
        fuel_level_pct: opt_u8(p[22]),
    })
}

impl ObdExtended {
    /// Gauge boost in psi: `(map − baro) × 0.1450377`. Needs both readings; with
    /// either missing it is unknown rather than guessed from a sea-level
    /// assumption, because a plausible wrong number is worse than an admitted
    /// absence.
    pub fn boost_psi(&self) -> Option<f64> {
        Some((self.map_kpa? as f64 - self.baro_kpa? as f64) * 0.1450377)
    }

    pub fn lambda(&self) -> Option<f64> {
        Some(self.lambda_e4? as f64 / 10000.0)
    }

    /// Values above 100% are normal on a turbocharged engine.
    pub fn abs_load_pct(&self) -> Option<f64> {
        Some(self.abs_load_raw? as f64 * 100.0 / 255.0)
    }

    /// PID 0x0B is one byte, so MAP hard-stops at 255 kPa absolute and a log
    /// past it shows a flat plateau that reads like a boost controller holding
    /// steady. Saying so is the difference between "the tune is flat-lining"
    /// and "the instrument is".
    pub fn map_saturated(&self) -> bool {
        self.map_kpa.is_some_and(|m| m >= 255)
    }
}

// ── TIME_OBSERVATION (§4.12) ────────────────────────────────────────────────

/// Where a `TIME_OBSERVATION` came from (§4.12.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TimeSource(pub u8);

impl TimeSource {
    pub const GNSS: Self = Self(1);
    pub const PHONE: Self = Self(2);
    pub const MODEM_NETWORK: Self = Self(3);
    pub const RTC: Self = Self(4);

    /// Whether this build understands the source. An unknown source is **kept**
    /// rather than rejected: the observation is still evidence, and a newer
    /// device naming a source this build has not heard of is exactly the case
    /// the field exists to survive.
    pub fn known(self) -> bool {
        (Self::GNSS.0..=Self::RTC.0).contains(&self.0)
    }

    pub fn name(self) -> String {
        match self {
            Self::GNSS => "gnss".into(),
            Self::PHONE => "phone".into(),
            Self::MODEM_NETWORK => "modem_network".into(),
            Self::RTC => "rtc".into(),
            Self(other) => format!("unknown({other})"),
        }
    }
}

/// Bit 0 of the flags: this observation is the one the device adopted as the
/// manifest's `utc_basis_ms`.
pub const TIME_OBSERVATION_ADOPTED: u8 = 1 << 0;

/// One wall-clock reading from one source, recorded as evidence rather than as a
/// decision (§4.12).
///
/// The monotonic reading of the same instant is the **frame's** `monotonic_ms`,
/// not a field here, and that pairing is the point: `utc_ms` minus the frame's
/// monotonic is the UTC of monotonic zero this source implies, so sources can be
/// compared directly and drift within one source is visible across a trip.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TimeObservation {
    pub utc_ms: u64,
    /// The source's own uncertainty, or `None` when it stated none. Absent is
    /// not zero: a source that does not report accuracy is not a source claiming
    /// perfect accuracy.
    pub accuracy_ms: Option<u32>,
    pub source: TimeSource,
    pub adopted: bool,
}

impl TimeObservation {
    /// The UTC of monotonic zero this observation implies, given the monotonic
    /// reading of the frame that carried it.
    ///
    /// `None` when the monotonic reading is later than the wall clock, which
    /// cannot happen on a sane device and would otherwise wrap the subtraction.
    pub fn implied_basis_ms(&self, frame_monotonic_ms: u32) -> Option<u64> {
        self.utc_ms.checked_sub(u64::from(frame_monotonic_ms))
    }
}

pub fn parse_time_observation(p: &[u8]) -> Result<TimeObservation> {
    if p.len() != 16 {
        return Err(FormatError::Malformed(format!(
            "TIME_OBSERVATION payload is {} bytes, want 16",
            p.len()
        )));
    }
    let accuracy = u32::from_le_bytes([p[8], p[9], p[10], p[11]]);
    Ok(TimeObservation {
        utc_ms: u64::from_le_bytes(p[0..8].try_into().unwrap()),
        accuracy_ms: (accuracy != u32::MAX).then_some(accuracy),
        source: TimeSource(p[12]),
        adopted: p[13] & TIME_OBSERVATION_ADOPTED != 0,
    })
}

// ── WIFI_SCAN (§4.13) ───────────────────────────────────────────────────────

/// Flag bit 0: an accepted GNSS fix was no older than 2 s at the scan, so the
/// access points can be learned from it.
pub const WIFI_SCAN_LEARNING: u8 = 1 << 0;
/// Flag bit 1: an active scan (probe requests were sent).
pub const WIFI_SCAN_ACTIVE: u8 = 1 << 1;
/// The device keeps the 16 strongest.
pub const WIFI_SCAN_MAX_APS: usize = 16;

/// One access point a scan heard. No SSID: the record has nowhere to carry one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WifiAp {
    /// In transmission order.
    pub bssid: [u8; 6],
    pub rssi_dbm: i8,
    pub channel: u8,
}

impl WifiAp {
    /// `aa:bb:cc:dd:ee:ff`, the form the conformance vectors use.
    pub fn bssid_string(&self) -> String {
        self.bssid
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<Vec<_>>()
            .join(":")
    }
}

/// One Wi-Fi scan (§4.13). The frame's `monotonic_ms` is when it completed; a
/// scan that heard nothing is a valid record with no entries.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WifiScan {
    /// The raw flag byte. Bits this build does not know are kept, not refused,
    /// so a scan from newer firmware still decodes.
    pub flags: u8,
    pub scan_ms: u16,
    /// Strongest first, as the device writes them.
    pub aps: Vec<WifiAp>,
}

impl WifiScan {
    pub fn learning(&self) -> bool {
        self.flags & WIFI_SCAN_LEARNING != 0
    }

    pub fn active(&self) -> bool {
        self.flags & WIFI_SCAN_ACTIVE != 0
    }
}

pub fn encode_wifi_scan(s: &WifiScan) -> Result<Vec<u8>> {
    if s.aps.len() > WIFI_SCAN_MAX_APS {
        return Err(FormatError::Malformed(format!(
            "WIFI_SCAN carries at most {WIFI_SCAN_MAX_APS} entries, not {}",
            s.aps.len()
        )));
    }
    let mut out = Vec::with_capacity(4 + 8 * s.aps.len());
    out.push(s.aps.len() as u8);
    out.push(s.flags);
    out.extend_from_slice(&s.scan_ms.to_le_bytes());
    for ap in &s.aps {
        out.extend_from_slice(&ap.bssid);
        out.push(ap.rssi_dbm as u8);
        out.push(ap.channel);
    }
    Ok(out)
}

pub fn parse_wifi_scan(p: &[u8]) -> Result<WifiScan> {
    if p.len() < 4 {
        return Err(FormatError::Malformed(format!(
            "WIFI_SCAN payload is {} bytes, want at least 4",
            p.len()
        )));
    }
    let count = p[0] as usize;
    if count > WIFI_SCAN_MAX_APS {
        return Err(FormatError::Malformed(format!(
            "WIFI_SCAN count {count} exceeds {WIFI_SCAN_MAX_APS}"
        )));
    }
    // Exact, not at-least: a length that disagrees with the count means writer
    // and reader disagree about the layout, and guessing which is right is how a
    // plausible wrong position gets made.
    if p.len() != 4 + 8 * count {
        return Err(FormatError::Malformed(format!(
            "WIFI_SCAN payload is {} bytes, want {} for {count} entries",
            p.len(),
            4 + 8 * count
        )));
    }
    let aps = p[4..]
        .chunks_exact(8)
        .map(|e| WifiAp {
            bssid: e[0..6].try_into().unwrap(),
            rssi_dbm: e[6] as i8,
            channel: e[7],
        })
        .collect();
    Ok(WifiScan {
        flags: p[1],
        scan_ms: u16::from_le_bytes([p[2], p[3]]),
        aps,
    })
}

#[cfg(test)]
mod wifi_scan_tests {
    use super::*;

    fn scan(n: usize, flags: u8, scan_ms: u16) -> WifiScan {
        WifiScan {
            flags,
            scan_ms,
            aps: (0..n)
                .map(|i| WifiAp {
                    bssid: [0x88, 0xdc, 0x97, 0x07, 0x10 + i as u8, 0xdb],
                    rssi_dbm: -40 - 3 * i as i8,
                    channel: 1 + (i % 13) as u8,
                })
                .collect(),
        }
    }

    #[test]
    fn wifi_scan_is_a_known_type() {
        use crate::format::RecordType;
        assert!(RecordType::WIFI_SCAN.known());
        assert_eq!(RecordType::WIFI_SCAN.0, 0x0C);
        assert_eq!(RecordType::WIFI_SCAN.name(), "WIFI_SCAN");
        assert!(!RecordType(0x0D).known());
    }

    #[test]
    fn count_zero_is_a_four_byte_record() {
        let s = scan(0, WIFI_SCAN_ACTIVE, 2140);
        let b = encode_wifi_scan(&s).unwrap();
        assert_eq!(b, vec![0, 0x02, 0x5c, 0x08]);
        let d = parse_wifi_scan(&b).unwrap();
        assert_eq!(d, s);
        assert!(d.active() && !d.learning());
    }

    #[test]
    fn three_and_sixteen_round_trip() {
        for n in [3usize, 16] {
            let s = scan(n, WIFI_SCAN_LEARNING | WIFI_SCAN_ACTIVE, 2310);
            let b = encode_wifi_scan(&s).unwrap();
            assert_eq!(b.len(), 4 + 8 * n);
            assert_eq!(parse_wifi_scan(&b).unwrap(), s);
        }
        let b = encode_wifi_scan(&scan(16, 0, 1)).unwrap();
        assert_eq!(b.len(), 132);
        // Entry layout: bssid in transmission order, then i8 dBm, then channel.
        assert_eq!(&b[4..10], &[0x88, 0xdc, 0x97, 0x07, 0x10, 0xdb]);
        assert_eq!(b[10] as i8, -40);
        assert_eq!(b[4 + 15 * 8 + 6] as i8, -85);
    }

    #[test]
    fn bssid_renders_as_the_vectors_write_it() {
        let s = scan(1, 0, 0);
        assert_eq!(s.aps[0].bssid_string(), "88:dc:97:07:10:db");
    }

    #[test]
    fn malformed_lengths_and_counts_are_refused() {
        let b = encode_wifi_scan(&scan(3, 0, 1870)).unwrap();
        assert!(parse_wifi_scan(&b[..27]).is_err());
        let mut long = b.clone();
        long.push(0);
        assert!(parse_wifi_scan(&long).is_err());
        assert!(parse_wifi_scan(&b[..3]).is_err());
        let mut over = vec![17u8, 0, 0, 0];
        over.extend(std::iter::repeat_n(0u8, 17 * 8));
        assert!(parse_wifi_scan(&over).is_err());
        assert!(encode_wifi_scan(&scan(17, 0, 0)).is_err());
    }

    #[test]
    fn unknown_flag_bits_are_kept() {
        let mut b = encode_wifi_scan(&scan(3, 0, 1870)).unwrap();
        b[1] = 0xFD;
        let d = parse_wifi_scan(&b).unwrap();
        assert_eq!(d.flags, 0xFD);
        assert!(d.learning() && !d.active());
    }
}
