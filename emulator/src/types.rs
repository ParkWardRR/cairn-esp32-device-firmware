use serde::Serialize;
use std::fmt;

/// 32-byte little-endian packed GNSS record, matching firmware GNSSSample.
#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
pub struct GnssSample {
    pub timestamp_ms: u64,
    pub latitude: i32,
    pub longitude: i32,
    pub altitude_cm: i32,
    pub speed_cmps: u16,
    pub heading_cdeg: u16,
    pub fix_quality: u8,
    pub satellites: u8,
    pub hdop_tenths: u16,
    pub accuracy_cm: u16,
    pub _reserved: [u8; 2],
}

const _: () = assert!(std::mem::size_of::<GnssSample>() == 32);

impl GnssSample {
    pub fn speed_kmh(&self) -> f64 {
        self.speed_cmps as f64 * 0.036
    }

    pub fn lat_deg(&self) -> f64 {
        self.latitude as f64 / 1e7
    }

    pub fn lon_deg(&self) -> f64 {
        self.longitude as f64 / 1e7
    }

    pub fn to_bytes(&self) -> [u8; 32] {
        unsafe { std::mem::transmute(*self) }
    }
}

/// 24-byte little-endian packed IMU summary, matching firmware IMUSummary.
#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
pub struct ImuSummary {
    pub window_start_ms: u64,
    pub window_duration_ms: u16,
    pub accel_peak_x_mg: i16,
    pub accel_peak_y_mg: i16,
    pub accel_peak_z_mg: i16,
    pub accel_rms_mg: u16,
    pub gyro_peak_dps: i16,
    pub variance: u16,
    pub flags: u8,
    pub _reserved: u8,
}

const _: () = assert!(std::mem::size_of::<ImuSummary>() == 24);

impl ImuSummary {
    pub fn to_bytes(&self) -> [u8; 24] {
        unsafe { std::mem::transmute(*self) }
    }
}

/// Device state enum matching firmware DeviceState.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum DeviceState {
    Sleep,
    Arming,
    Recording,
    StopCandidate,
    Finalizing,
    QueuedForHomeSync,
    Syncing,
    Retained,
    Prunable,
    LowBatteryProtection,
    Fault,
}

impl fmt::Display for DeviceState {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Sleep => write!(f, "SLEEP"),
            Self::Arming => write!(f, "ARMING"),
            Self::Recording => write!(f, "RECORDING"),
            Self::StopCandidate => write!(f, "STOP_CANDIDATE"),
            Self::Finalizing => write!(f, "FINALIZING"),
            Self::QueuedForHomeSync => write!(f, "QUEUED_FOR_HOME_SYNC"),
            Self::Syncing => write!(f, "SYNCING"),
            Self::Retained => write!(f, "RETAINED"),
            Self::Prunable => write!(f, "PRUNABLE"),
            Self::LowBatteryProtection => write!(f, "LOW_BATTERY_PROTECTION"),
            Self::Fault => write!(f, "FAULT"),
        }
    }
}

/// Waypoint: (name, lat, lon).
pub type Waypoint = (&'static str, f64, f64);

/// Santa Monica / West LA waypoints.
pub const WAYPOINTS: &[Waypoint] = &[
    ("Santa Monica Pier", 34.0094, -118.4973),
    ("Ocean & Colorado", 34.0137, -118.4976),
    ("Ocean & Wilshire", 34.0195, -118.4912),
    ("Montana & 7th", 34.0302, -118.4928),
    ("Lincoln & Wilshire", 34.0262, -118.4710),
    ("Wilshire & Bundy", 34.0371, -118.4531),
    ("Venice & Lincoln", 33.9970, -118.4592),
    ("Main & Rose", 33.9941, -118.4744),
    ("Olympic & 26th", 34.0225, -118.4770),
    ("PCH & Temescal", 34.0450, -118.5250),
    ("Lincoln & Montana", 34.0305, -118.4750),
    ("Pico & Lincoln", 34.0135, -118.4600),
    ("26th & San Vicente", 34.0345, -118.4720),
    ("Clover Park", 34.0165, -118.4640),
    ("Bergamot Station", 34.0255, -118.4665),
    ("I-10 & Lincoln On-Ramp", 34.0200, -118.4580),
    ("I-10 & Centinela", 34.0170, -118.4370),
    ("I-10 & Robertson", 34.0290, -118.3870),
    ("I-10 & La Cienega", 34.0330, -118.3700),
    ("I-10 & La Brea", 34.0380, -118.3440),
    ("I-10 & Crenshaw", 34.0280, -118.3270),
    ("I-10 & Western", 34.0300, -118.3090),
    ("Rest Stop (fictional)", 34.0350, -118.2950),
    ("I-10 & Vermont", 34.0330, -118.2920),
];

pub const HOME_LOCATION: Waypoint = ("Home (Montana & 7th)", 34.0302, -118.4928);
pub const GARAGE_LOCATION: Waypoint = ("Underground Garage", 34.0200, -118.4580);

/// Trip event recorded during emulation.
#[derive(Clone, Serialize)]
pub struct TripEvent {
    #[serde(rename = "type")]
    pub event_type: String,
    pub timestamp: String,
    pub data: serde_json::Value,
}

/// Summary of a completed trip.
#[derive(Clone, Serialize)]
pub struct TripSummary {
    pub trip_id: String,
    pub gnss_samples: usize,
    pub imu_summaries: usize,
    pub events: usize,
    pub duration_s: f64,
    pub samples_bytes: usize,
    pub imu_bytes: usize,
}

/// Summary of a scenario run.
#[derive(Clone, Serialize)]
pub struct ScenarioResult {
    pub scenario: String,
    pub device_id: String,
    pub trips_generated: usize,
    pub trips_uploaded: usize,
    pub trips_queued: usize,
    pub final_state: String,
    pub trips: Vec<TripSummary>,
    pub errors: Vec<String>,
    pub expects_errors: bool,
    pub total_gnss_samples: usize,
    pub total_imu_summaries: usize,
}

pub fn clamp_i16(v: f64) -> i16 {
    v.clamp(-32768.0, 32767.0) as i16
}

pub fn clamp_u16(v: f64) -> u16 {
    v.clamp(0.0, 65535.0) as u16
}
