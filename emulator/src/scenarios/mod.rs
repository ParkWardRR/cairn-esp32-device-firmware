use rand::Rng;

use crate::device::{default_is_complete, DeviceEmulator, Scenario};
use crate::driving::{
    make_city_route, make_highway_route, DrivingSimulator, RouteProfile, RouteSegment,
};
use crate::sensors::gnss::GnssEmulator;
use crate::types::*;

pub const SCENARIO_NAMES: &[&str] = &[
    "normal_commute",
    "highway_trip",
    "short_errand",
    "garage_start",
    "multi_stop_errands",
    "power_loss_recording",
    "power_loss_upload",
    "low_battery",
    "storage_full",
    "long_park",
    "tunnel_drive",
    "multi_device",
    "rapid_trips",
    "degraded_gnss",
    "cold_start",
];

pub fn get_scenario(name: &str) -> Option<Box<dyn Scenario>> {
    match name {
        "normal_commute" => Some(Box::new(NormalCommute)),
        "highway_trip" => Some(Box::new(HighwayTrip)),
        "short_errand" => Some(Box::new(ShortErrand)),
        "garage_start" => Some(Box::new(GarageStart::new())),
        "multi_stop_errands" => Some(Box::new(MultiStopErrands)),
        "power_loss_recording" => Some(Box::new(PowerLossRecording::new())),
        "power_loss_upload" => Some(Box::new(PowerLossUpload::new())),
        "low_battery" => Some(Box::new(LowBattery::new())),
        "storage_full" => Some(Box::new(StorageFull)),
        "long_park" => Some(Box::new(LongPark::new())),
        "tunnel_drive" => Some(Box::new(TunnelDrive::new())),
        "multi_device" => Some(Box::new(MultiDevice)),
        "rapid_trips" => Some(Box::new(RapidTrips::new())),
        "degraded_gnss" => Some(Box::new(DegradedGnss)),
        "cold_start" => Some(Box::new(ColdStart)),
        _ => None,
    }
}

fn setup_driving(device: &mut DeviceEmulator, route: RouteProfile, start_wp: Waypoint) {
    device.driving = Some(DrivingSimulator::new(&mut device.rng, route));
    device.gnss.set_position(start_wp.1, start_wp.2);
    device.battery.start_engine();
    device.battery.engine_running();
}

// ---------------------------------------------------------------------------
// 1. NormalCommute
// ---------------------------------------------------------------------------

struct NormalCommute;

impl Scenario for NormalCommute {
    fn name(&self) -> &str { "normal_commute" }
    fn description(&self) -> &str { "25-min city drive, park at destination, Wi-Fi sync at home" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let route = make_city_route(
            &mut device.rng,
            &[0, 1, 2, 3, 10, 4, 5, 4, 10, 3],
            (25.0, 45.0),
            4,
        );
        setup_driving(device, route, WAYPOINTS[0]);
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 2. HighwayTrip
// ---------------------------------------------------------------------------

struct HighwayTrip;

impl Scenario for HighwayTrip {
    fn name(&self) -> &str { "highway_trip" }
    fn description(&self) -> &str { "2-hour highway drive with rest stop" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let route = make_highway_route(&mut device.rng);
        setup_driving(device, route, WAYPOINTS[3]);
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 3. ShortErrand
// ---------------------------------------------------------------------------

struct ShortErrand;

impl Scenario for ShortErrand {
    fn name(&self) -> &str { "short_errand" }
    fn description(&self) -> &str { "3-min trip to corner store" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let cruise = device.rng.gen_range(20.0..=35.0);
        let segments = vec![RouteSegment {
            start: WAYPOINTS[3],
            end: WAYPOINTS[10],
            cruise_kmh: cruise,
            stop_at_end_s: 0.0,
            profile: "city".to_string(),
            gnss_override: None,
        }];
        let route = RouteProfile {
            name: "short_errand".to_string(),
            segments,
            home_at_end: true,
        };
        setup_driving(device, route, WAYPOINTS[3]);
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 4. GarageStart
// ---------------------------------------------------------------------------

struct GarageStart {
    exit_time_ms: u64,
    setup_done: bool,
}

impl GarageStart {
    fn new() -> Self {
        Self {
            exit_time_ms: 0,
            setup_done: false,
        }
    }
}

impl Scenario for GarageStart {
    fn name(&self) -> &str { "garage_start" }
    fn description(&self) -> &str { "Start in underground garage (no GNSS), drive out, get fix" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let seed: u64 = device.rng.r#gen();
        device.gnss = GnssEmulator::new(seed, true);
        device.gnss.set_blocked(true);

        let c1 = device.rng.gen_range(15.0..=25.0);
        let c2 = device.rng.gen_range(30.0..=45.0);
        let c3 = device.rng.gen_range(30.0..=40.0);

        let segments = vec![
            RouteSegment {
                start: GARAGE_LOCATION,
                end: WAYPOINTS[4],
                cruise_kmh: c1,
                stop_at_end_s: 0.0,
                profile: "parking".to_string(),
                gnss_override: Some("none".to_string()),
            },
            RouteSegment {
                start: WAYPOINTS[4],
                end: WAYPOINTS[5],
                cruise_kmh: c2,
                stop_at_end_s: 0.0,
                profile: "city".to_string(),
                gnss_override: None,
            },
            RouteSegment {
                start: WAYPOINTS[5],
                end: WAYPOINTS[3],
                cruise_kmh: c3,
                stop_at_end_s: 0.0,
                profile: "city".to_string(),
                gnss_override: None,
            },
        ];
        let route = RouteProfile {
            name: "garage_start".to_string(),
            segments,
            home_at_end: true,
        };
        device.driving = Some(DrivingSimulator::new(&mut device.rng, route));
        device.gnss.set_position(GARAGE_LOCATION.1, GARAGE_LOCATION.2);
        device.battery.start_engine();
        device.battery.engine_running();
        self.exit_time_ms = device.sim_time_ms + 30_000;
    }

    fn tick(&mut self, device: &mut DeviceEmulator, sim_time_ms: u64) {
        if sim_time_ms >= self.exit_time_ms && !self.setup_done {
            device.gnss.set_blocked(false);
            device.log("[SCENARIO] Exited garage -- GNSS unblocked");
            self.setup_done = true;
        }
    }

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 5. MultiStopErrands
// ---------------------------------------------------------------------------

struct MultiStopErrands;

impl Scenario for MultiStopErrands {
    fn name(&self) -> &str { "multi_stop_errands" }
    fn description(&self) -> &str { "4 stops in 90 minutes" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let c = |d: &mut DeviceEmulator| d.rng.gen_range(25.0..=40.0);
        let s = |d: &mut DeviceEmulator| d.rng.gen_range(180.0..=300.0);

        let c1 = c(device); let s1 = s(device);
        let c2 = c(device); let s2 = s(device);
        let c3 = c(device); let s3 = s(device);
        let c4 = c(device); let s4 = s(device);
        let c5 = c(device);

        let segments = vec![
            RouteSegment {
                start: WAYPOINTS[3], end: WAYPOINTS[10],
                cruise_kmh: c1, stop_at_end_s: s1,
                profile: "city".to_string(), gnss_override: None,
            },
            RouteSegment {
                start: WAYPOINTS[10], end: WAYPOINTS[4],
                cruise_kmh: c2, stop_at_end_s: s2,
                profile: "city".to_string(), gnss_override: None,
            },
            RouteSegment {
                start: WAYPOINTS[4], end: WAYPOINTS[11],
                cruise_kmh: c3, stop_at_end_s: s3,
                profile: "city".to_string(), gnss_override: None,
            },
            RouteSegment {
                start: WAYPOINTS[11], end: WAYPOINTS[13],
                cruise_kmh: c4, stop_at_end_s: s4,
                profile: "city".to_string(), gnss_override: None,
            },
            RouteSegment {
                start: WAYPOINTS[13], end: WAYPOINTS[3],
                cruise_kmh: c5, stop_at_end_s: 0.0,
                profile: "city".to_string(), gnss_override: None,
            },
        ];
        let route = RouteProfile {
            name: "multi_stop".to_string(),
            segments,
            home_at_end: true,
        };
        setup_driving(device, route, WAYPOINTS[3]);
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 6. PowerLossRecording
// ---------------------------------------------------------------------------

struct PowerLossRecording {
    loss_triggered: bool,
}

impl PowerLossRecording {
    fn new() -> Self {
        Self { loss_triggered: false }
    }
}

impl Scenario for PowerLossRecording {
    fn name(&self) -> &str { "power_loss_recording" }
    fn description(&self) -> &str { "Power yanked during active recording" }
    fn expects_errors(&self) -> bool { true }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let route = make_city_route(
            &mut device.rng,
            &[0, 1, 2, 3, 10, 4],
            (30.0, 50.0),
            2,
        );
        setup_driving(device, route, WAYPOINTS[0]);
    }

    fn tick(&mut self, device: &mut DeviceEmulator, _sim_time_ms: u64) {
        if device.state == DeviceState::Recording
            && device.gnss_sample_count > 50
            && !self.loss_triggered
        {
            self.loss_triggered = true;
            device.force_power_loss = true;
            device.power_loss_at_state = Some(DeviceState::Recording);
            device.log("[SCENARIO] Power loss triggered NOW");
        }
    }

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 7. PowerLossUpload
// ---------------------------------------------------------------------------

struct PowerLossUpload {
    drop_triggered: bool,
    reconnect_time_ms: u64,
}

impl PowerLossUpload {
    fn new() -> Self {
        Self {
            drop_triggered: false,
            reconnect_time_ms: 0,
        }
    }
}

impl Scenario for PowerLossUpload {
    fn name(&self) -> &str { "power_loss_upload" }
    fn description(&self) -> &str { "Wi-Fi drops during sync, resume on reconnect" }
    fn expects_errors(&self) -> bool { true }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let route = make_city_route(
            &mut device.rng,
            &[3, 10, 4, 3],
            (25.0, 40.0),
            1,
        );
        setup_driving(device, route, WAYPOINTS[3]);
        device.network.force_drop_after(2048);
    }

    fn tick(&mut self, device: &mut DeviceEmulator, sim_time_ms: u64) {
        if device.state == DeviceState::QueuedForHomeSync
            && self.drop_triggered
            && sim_time_ms >= self.reconnect_time_ms
        {
            device.network.set_force_drop(false);
            device.network.set_drop_after_bytes(-1);
            device.log("[SCENARIO] Wi-Fi reconnected -- retry upload");
        }

        if device.state == DeviceState::QueuedForHomeSync
            && !self.drop_triggered
            && !device.network.is_connected()
        {
            self.drop_triggered = true;
            self.reconnect_time_ms = sim_time_ms + 10_000;
            device.log("[SCENARIO] Wi-Fi dropped -- will reconnect in 10s");
        }
    }

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 8. LowBattery
// ---------------------------------------------------------------------------

struct LowBattery {
    triggered: bool,
}

impl LowBattery {
    fn new() -> Self {
        Self { triggered: false }
    }
}

impl Scenario for LowBattery {
    fn name(&self) -> &str { "low_battery" }
    fn description(&self) -> &str { "Battery drops below threshold during recording" }
    fn expects_errors(&self) -> bool { true }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let route = make_city_route(
            &mut device.rng,
            &[0, 1, 2, 3, 10, 4],
            (25.0, 40.0),
            2,
        );
        setup_driving(device, route, WAYPOINTS[0]);
    }

    fn tick(&mut self, device: &mut DeviceEmulator, _sim_time_ms: u64) {
        if device.state == DeviceState::Recording
            && device.gnss_sample_count > 30
            && !self.triggered
        {
            device.battery.force_low_battery();
            self.triggered = true;
            device.log("[SCENARIO] Battery dropping below threshold");
        }
    }

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 9. StorageFull
// ---------------------------------------------------------------------------

struct StorageFull;

impl Scenario for StorageFull {
    fn name(&self) -> &str { "storage_full" }
    fn description(&self) -> &str { "SD card nearly full, device must handle pressure" }
    fn expects_errors(&self) -> bool { true }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        device.storage.set_nearly_full(45);
        let route = make_city_route(
            &mut device.rng,
            &[3, 10, 4, 5, 4, 10, 3],
            (25.0, 40.0),
            2,
        );
        setup_driving(device, route, WAYPOINTS[3]);
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 10. LongPark
// ---------------------------------------------------------------------------

struct LongPark {
    park_end_ms: u64,
}

impl LongPark {
    fn new() -> Self {
        Self { park_end_ms: 0 }
    }
}

impl Scenario for LongPark {
    fn name(&self) -> &str { "long_park" }
    fn description(&self) -> &str { "Device parked for 8 hours, no false triggers" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        device.gnss.set_position(WAYPOINTS[5].1, WAYPOINTS[5].2);
        self.park_end_ms = device.sim_time_ms + 8 * 3600 * 1000;
        device.log("[SCENARIO] Parked for 8 hours -- no movement expected");
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        sim_time_ms >= self.park_end_ms || device.shutdown
    }
}

// ---------------------------------------------------------------------------
// 11. TunnelDrive
// ---------------------------------------------------------------------------

struct TunnelDrive {
    tunnel_start_ms: u64,
    tunnel_end_ms: u64,
    in_tunnel: bool,
    exited: bool,
}

impl TunnelDrive {
    fn new() -> Self {
        Self {
            tunnel_start_ms: 0,
            tunnel_end_ms: 0,
            in_tunnel: false,
            exited: false,
        }
    }
}

impl Scenario for TunnelDrive {
    fn name(&self) -> &str { "tunnel_drive" }
    fn description(&self) -> &str { "GNSS loss for 2 minutes mid-highway" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let c1 = device.rng.gen_range(30.0..=45.0);
        let c2 = device.rng.gen_range(40.0..=55.0);
        let c3 = device.rng.gen_range(90.0..=110.0);
        let c4 = device.rng.gen_range(90.0..=110.0);
        let c5 = device.rng.gen_range(90.0..=110.0);
        let c6 = device.rng.gen_range(80.0..=100.0);
        let c7 = device.rng.gen_range(30.0..=45.0);
        let c8 = device.rng.gen_range(25.0..=40.0);

        let segments = vec![
            RouteSegment { start: WAYPOINTS[3], end: WAYPOINTS[4], cruise_kmh: c1, stop_at_end_s: 0.0, profile: "city".to_string(), gnss_override: None },
            RouteSegment { start: WAYPOINTS[4], end: WAYPOINTS[15], cruise_kmh: c2, stop_at_end_s: 0.0, profile: "city".to_string(), gnss_override: None },
            RouteSegment { start: WAYPOINTS[15], end: WAYPOINTS[16], cruise_kmh: c3, stop_at_end_s: 0.0, profile: "highway".to_string(), gnss_override: None },
            RouteSegment { start: WAYPOINTS[16], end: WAYPOINTS[17], cruise_kmh: c4, stop_at_end_s: 0.0, profile: "highway".to_string(), gnss_override: None },
            RouteSegment { start: WAYPOINTS[17], end: WAYPOINTS[16], cruise_kmh: c5, stop_at_end_s: 0.0, profile: "highway".to_string(), gnss_override: None },
            RouteSegment { start: WAYPOINTS[16], end: WAYPOINTS[15], cruise_kmh: c6, stop_at_end_s: 0.0, profile: "highway".to_string(), gnss_override: None },
            RouteSegment { start: WAYPOINTS[15], end: WAYPOINTS[4], cruise_kmh: c7, stop_at_end_s: 0.0, profile: "city".to_string(), gnss_override: None },
            RouteSegment { start: WAYPOINTS[4], end: WAYPOINTS[3], cruise_kmh: c8, stop_at_end_s: 0.0, profile: "city".to_string(), gnss_override: None },
        ];
        let route = RouteProfile {
            name: "tunnel_drive".to_string(),
            segments,
            home_at_end: true,
        };
        setup_driving(device, route, WAYPOINTS[3]);
    }

    fn tick(&mut self, device: &mut DeviceEmulator, sim_time_ms: u64) {
        if device.state == DeviceState::Recording
            && device.gnss_sample_count > 100
            && !self.in_tunnel
            && !self.exited
        {
            device.gnss.set_blocked(true);
            self.in_tunnel = true;
            self.tunnel_start_ms = sim_time_ms;
            self.tunnel_end_ms = sim_time_ms + 120_000;
            device.log("[SCENARIO] Entering tunnel -- GNSS blocked");
        }

        if self.in_tunnel && sim_time_ms >= self.tunnel_end_ms {
            device.gnss.set_blocked(false);
            self.in_tunnel = false;
            self.exited = true;
            device.log("[SCENARIO] Exiting tunnel -- GNSS restored");
        }
    }

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 12. MultiDevice
// ---------------------------------------------------------------------------

struct MultiDevice;

impl Scenario for MultiDevice {
    fn name(&self) -> &str { "multi_device" }
    fn description(&self) -> &str { "3 emulated devices uploading simultaneously" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let route = make_city_route(
            &mut device.rng,
            &[0, 1, 2, 3, 10, 4, 5],
            (25.0, 45.0),
            3,
        );
        setup_driving(device, route, WAYPOINTS[0]);
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 13. RapidTrips
// ---------------------------------------------------------------------------

struct RapidTrips {
    trip_count: usize,
    max_trips: usize,
}

impl RapidTrips {
    fn new() -> Self {
        Self {
            trip_count: 0,
            max_trips: 10,
        }
    }

    fn schedule_next_trip(&mut self, device: &mut DeviceEmulator) {
        if self.trip_count >= self.max_trips {
            return;
        }

        let wp_pool: Vec<usize> = vec![0, 1, 2, 7, 8, 11, 13, 14];
        let start_idx = device.rng.gen_range(0..wp_pool.len());
        let start = wp_pool[start_idx];
        let remaining: Vec<usize> = wp_pool.iter().copied().filter(|&w| w != start).collect();
        let end_idx = device.rng.gen_range(0..remaining.len());
        let end = remaining[end_idx];

        let cruise = device.rng.gen_range(10.0..=25.0);

        let segments = vec![RouteSegment {
            start: WAYPOINTS[start],
            end: WAYPOINTS[end],
            cruise_kmh: cruise,
            stop_at_end_s: 0.0,
            profile: "parking".to_string(),
            gnss_override: None,
        }];
        let route = RouteProfile {
            name: format!("rapid_trip_{}", self.trip_count),
            segments,
            home_at_end: false,
        };
        device.driving = Some(DrivingSimulator::new(&mut device.rng, route));
        device.gnss.set_position(WAYPOINTS[start].1, WAYPOINTS[start].2);
        self.trip_count += 1;
    }
}

impl Scenario for RapidTrips {
    fn name(&self) -> &str { "rapid_trips" }
    fn description(&self) -> &str { "10 trips in quick succession (valet parking scenario)" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        self.schedule_next_trip(device);
        device.battery.start_engine();
        device.battery.engine_running();
    }

    fn tick(&mut self, device: &mut DeviceEmulator, _sim_time_ms: u64) {
        if device.state == DeviceState::QueuedForHomeSync && self.trip_count < self.max_trips {
            device.log(&format!(
                "[SCENARIO] Scheduling rapid trip {}/{}",
                self.trip_count + 1,
                self.max_trips
            ));
            self.schedule_next_trip(device);
        }
    }

    fn is_complete(&self, device: &DeviceEmulator, _sim_time_ms: u64) -> bool {
        device.shutdown
            || (self.trip_count >= self.max_trips
                && matches!(
                    device.state,
                    DeviceState::Sleep | DeviceState::Prunable | DeviceState::Retained
                ))
    }
}

// ---------------------------------------------------------------------------
// 14. DegradedGnss
// ---------------------------------------------------------------------------

struct DegradedGnss;

impl Scenario for DegradedGnss {
    fn name(&self) -> &str { "degraded_gnss" }
    fn description(&self) -> &str { "Driving in urban canyon with intermittent fix quality" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        device.gnss.set_degraded(true);
        let route = make_city_route(
            &mut device.rng,
            &[2, 4, 5, 12, 10, 3],
            (20.0, 40.0),
            3,
        );
        setup_driving(device, route, WAYPOINTS[2]);
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}

// ---------------------------------------------------------------------------
// 15. ColdStart
// ---------------------------------------------------------------------------

struct ColdStart;

impl Scenario for ColdStart {
    fn name(&self) -> &str { "cold_start" }
    fn description(&self) -> &str { "Device first boot, no stored state, cold GNSS fix" }

    fn setup(&mut self, device: &mut DeviceEmulator) {
        let seed: u64 = device.rng.r#gen();
        device.gnss = GnssEmulator::new(seed, true);
        let route = make_city_route(
            &mut device.rng,
            &[3, 10, 4, 5, 4, 3],
            (25.0, 40.0),
            2,
        );
        setup_driving(device, route, WAYPOINTS[3]);
    }

    fn tick(&mut self, _device: &mut DeviceEmulator, _sim_time_ms: u64) {}

    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool {
        default_is_complete(device, sim_time_ms)
    }
}
