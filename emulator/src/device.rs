use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use chrono::DateTime;
use rand::rngs::StdRng;
use rand::Rng;
use sha2::{Digest, Sha256};

use crate::config::*;
use crate::driving::DrivingSimulator;
use crate::math::generate_ulid;
use crate::network::NetworkEmulator;
use crate::sensors::battery::BatteryEmulator;
use crate::sensors::gnss::GnssEmulator;
use crate::sensors::imu::ImuEmulator;
use crate::storage::StorageEmulator;
use crate::types::*;

pub struct EventRecorder {
    pub events: Vec<TripEvent>,
}

impl EventRecorder {
    pub fn new() -> Self {
        Self { events: Vec::new() }
    }

    pub fn add(&mut self, event_type: &str, timestamp_ms: u64, lat: f64, lon: f64, details: &str) {
        let secs = (timestamp_ms / 1000) as i64;
        let nanos = ((timestamp_ms % 1000) * 1_000_000) as u32;
        let dt = DateTime::from_timestamp(secs, nanos).unwrap_or_default();
        self.events.push(TripEvent {
            event_type: event_type.to_string(),
            timestamp: dt.to_rfc3339(),
            data: serde_json::json!({
                "details": details,
                "lat": lat,
                "lon": lon,
            }),
        });
    }
}

pub struct DeviceEmulator {
    pub device_id: String,
    pub rng: StdRng,
    pub speedup: f64,
    pub verbose: bool,

    pub state: DeviceState,
    state_entered_ms: u64,
    pub sim_time_ms: u64,

    pub gnss: GnssEmulator,
    pub imu: ImuEmulator,
    pub battery: BatteryEmulator,
    pub storage: StorageEmulator,
    pub network: NetworkEmulator,

    pub driving: Option<DrivingSimulator>,

    pub trip_id: String,
    trip_start_ms: u64,
    gnss_samples: Vec<GnssSample>,
    imu_summaries: Vec<ImuSummary>,
    events: EventRecorder,
    pub gnss_sample_count: usize,
    imu_summary_count: usize,
    last_gnss_ms: u64,
    last_imu_ms: u64,

    queued_trips: Vec<String>,
    synced_trips: Vec<String>,
    total_uploaded: usize,
    all_trips: Vec<TripSummary>,

    pub force_power_loss: bool,
    pub power_loss_at_state: Option<DeviceState>,
    pub shutdown: bool,
    last_wifi_scan_ms: u64,

    log_lines: Vec<String>,
    expects_errors: bool,
}

impl DeviceEmulator {
    pub fn new(
        device_id: &str,
        rng: StdRng,
        output_dir: &Path,
        server_url: &str,
        speedup: f64,
        verbose: bool,
    ) -> Self {
        let seed_base = {
            let mut r = rng.clone();
            r.r#gen::<u64>()
        };
        let now_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_millis() as u64;

        Self {
            device_id: device_id.to_string(),
            rng,
            speedup,
            verbose,
            state: DeviceState::Sleep,
            state_entered_ms: 0,
            sim_time_ms: now_ms,
            gnss: GnssEmulator::new(seed_base, false),
            imu: ImuEmulator::new(seed_base.wrapping_add(1)),
            battery: BatteryEmulator::new(seed_base.wrapping_add(2)),
            storage: StorageEmulator::new(output_dir, 4096),
            network: NetworkEmulator::new(seed_base.wrapping_add(3), server_url, device_id),
            driving: None,
            trip_id: String::new(),
            trip_start_ms: 0,
            gnss_samples: Vec::new(),
            imu_summaries: Vec::new(),
            events: EventRecorder::new(),
            gnss_sample_count: 0,
            imu_summary_count: 0,
            last_gnss_ms: 0,
            last_imu_ms: 0,
            queued_trips: Vec::new(),
            synced_trips: Vec::new(),
            total_uploaded: 0,
            all_trips: Vec::new(),
            force_power_loss: false,
            power_loss_at_state: None,
            shutdown: false,
            last_wifi_scan_ms: 0,
            log_lines: Vec::new(),
            expects_errors: false,
        }
    }

    pub fn log(&mut self, msg: &str) {
        let secs = (self.sim_time_ms / 1000) as i64;
        let millis = (self.sim_time_ms % 1000) as u32;
        let dt = DateTime::from_timestamp(secs, millis * 1_000_000).unwrap_or_default();
        let ts = dt.format("%H:%M:%S%.3f").to_string();
        let line = format!("[{}] [{}] {}", ts, self.device_id, msg);
        self.log_lines.push(line.clone());
        if self.verbose {
            eprintln!("{}", line);
        }
    }

    fn transition(&mut self, new_state: DeviceState) {
        let old = self.state;
        let msg = format!("[STATE] {} -> {}", old, new_state);
        self.log(&msg);
        self.state = new_state;
        self.state_entered_ms = self.sim_time_ms;
    }

    fn state_elapsed_ms(&self) -> u64 {
        self.sim_time_ms.saturating_sub(self.state_entered_ms)
    }

    fn advance_time(&mut self, real_dt_ms: u64) -> u64 {
        let sim_dt = (real_dt_ms as f64 * self.speedup) as u64;
        self.sim_time_ms += sim_dt;
        sim_dt
    }

    pub fn run_scenario(&mut self, scenario: &mut dyn Scenario) -> ScenarioResult {
        self.log(&format!("=== Starting scenario: {} ===", scenario.name()));
        self.expects_errors = scenario.expects_errors();

        scenario.setup(self);

        let tick_interval_ms: u64 = 200;

        while !self.shutdown {
            let sim_dt = self.advance_time(tick_interval_ms);

            scenario.tick(self, self.sim_time_ms);

            if self.state != DeviceState::Sleep
                && self.state != DeviceState::LowBatteryProtection
                && (self.battery.voltage_mv() as u32) < LOW_BATTERY_THRESHOLD_MV
            {
                self.transition(DeviceState::LowBatteryProtection);
            }

            match self.state {
                DeviceState::Sleep => self.handle_sleep(sim_dt),
                DeviceState::Arming => self.handle_arming(sim_dt),
                DeviceState::Recording => self.handle_recording(sim_dt),
                DeviceState::StopCandidate => self.handle_stop_candidate(sim_dt),
                DeviceState::Finalizing => self.handle_finalizing(),
                DeviceState::QueuedForHomeSync => self.handle_queued(sim_dt),
                DeviceState::Syncing => self.handle_syncing(),
                DeviceState::Retained => self.handle_retained(),
                DeviceState::Prunable => self.handle_prunable(),
                DeviceState::LowBatteryProtection => self.handle_low_battery(),
                DeviceState::Fault => self.handle_fault(),
            }

            self.battery.update(sim_dt as u64);

            if scenario.is_complete(self, self.sim_time_ms) {
                break;
            }
        }

        let name = scenario.name().to_string();
        self.log(&format!("=== Scenario complete: {} ===", name));
        self.build_summary(&name)
    }

    fn handle_sleep(&mut self, _dt_ms: u64) {
        let has_driving = self
            .driving
            .as_ref()
            .map(|d| !d.finished())
            .unwrap_or(false);

        if has_driving {
            self.log("[SLEEP] Motion detected -> ARMING");
            self.battery.start_engine();
            self.battery.engine_running();
            self.transition(DeviceState::Arming);
        }
    }

    fn handle_arming(&mut self, dt_ms: u64) {
        let elapsed = self.state_elapsed_ms();

        let driving_active = self
            .driving
            .as_ref()
            .map(|d| !d.finished())
            .unwrap_or(false);

        if !driving_active {
            if elapsed > ARMING_DURATION_MS {
                self.log("[ARMING] No sustained movement -> SLEEP");
                self.transition(DeviceState::Sleep);
            }
            return;
        }

        // Cap sub-steps to 200ms to avoid overshooting short routes at high speedup
        let sub_step = dt_ms.min(200);
        let mut remaining = dt_ms;
        let mut lat = 0.0;
        let mut lon = 0.0;
        let mut speed_mps = 0.0;
        let mut peak_speed_mps: f64 = 0.0;
        let mut heading = 0.0;
        let mut profile = "city".to_string();

        while remaining > 0 {
            let chunk = remaining.min(sub_step);
            remaining -= chunk;

            if let Some(driving) = &mut self.driving {
                let result = driving.step(chunk as f64 / 1000.0);
                lat = result.0;
                lon = result.1;
                speed_mps = result.2;
                heading = result.3;
                profile = result.4;
                peak_speed_mps = peak_speed_mps.max(speed_mps);

                if driving.finished() {
                    break;
                }
            }
        }

        // Use peak speed for arming check so short routes at high speedup still trigger
        let check_speed = peak_speed_mps;
        let speed_kmh = check_speed * 3.6;
        let sim_time = self.sim_time_ms;

        let gnss_sample = self.gnss.update(sim_time, lat, lon, check_speed, heading, dt_ms);
        let imu_sample = self.imu.update(sim_time, check_speed, heading, dt_ms, &profile);

        let conditions_met = speed_kmh >= MIN_SPEED_KMH
            && (gnss_sample.speed_kmh() >= MIN_SPEED_KMH
                || imu_sample.variance > ULP_MOTION_THRESHOLD_MG);

        if !conditions_met {
            if elapsed > ARMING_DURATION_MS {
                self.log("[ARMING] Conditions not sustained -> SLEEP");
                self.transition(DeviceState::Sleep);
            }
            return;
        }

        if elapsed >= ARMING_DURATION_MS {
            let trip_id = generate_ulid(&mut self.rng, self.sim_time_ms);
            self.trip_id = trip_id.clone();
            self.trip_start_ms = self.sim_time_ms;
            self.gnss_samples.clear();
            self.imu_summaries.clear();
            self.events = EventRecorder::new();
            self.gnss_sample_count = 0;
            self.imu_summary_count = 0;
            self.last_gnss_ms = 0;
            self.last_imu_ms = 0;

            if self.storage.is_full() {
                self.log("[ARMING] Storage full -- cannot record");
                self.events
                    .add("storage_pressure", self.sim_time_ms, 0.0, 0.0, "SD card full");
                self.transition(DeviceState::Fault);
                return;
            }

            self.storage.open_trip(&trip_id);
            self.events
                .add("trip_start", self.sim_time_ms, lat, lon, "recording started");
            self.log(&format!(
                "[ARMING] Trip started: {} (speed={:.1} km/h)",
                trip_id, speed_kmh
            ));
            self.transition(DeviceState::Recording);
        }
    }

    fn handle_recording(&mut self, dt_ms: u64) {
        if self.force_power_loss && self.power_loss_at_state == Some(DeviceState::Recording) {
            self.log("[RECORDING] POWER LOSS -- data may be incomplete");
            self.events.add(
                "power_anomaly",
                self.sim_time_ms,
                0.0,
                0.0,
                "power loss during recording",
            );
            self.handle_finalizing();
            self.shutdown = true;
            return;
        }

        let elapsed = self.state_elapsed_ms();
        if elapsed >= MAX_TRIP_DURATION_MS {
            self.log("[RECORDING] Max trip duration reached");
            self.events
                .add("trip_end", self.sim_time_ms, 0.0, 0.0, "max duration");
            self.transition(DeviceState::Finalizing);
            return;
        }

        if self.driving.is_none() {
            return;
        }

        let gnss_interval = if GNSS_RATE_ACTIVE_HZ > 0 {
            1000 / GNSS_RATE_ACTIVE_HZ as u64
        } else {
            dt_ms
        };
        let imu_per_gnss = if GNSS_RATE_ACTIVE_HZ > 0 {
            IMU_RATE_ACTIVE_HZ / GNSS_RATE_ACTIVE_HZ
        } else {
            1
        };
        let imu_window_ms = if IMU_RATE_ACTIVE_HZ > 0 {
            1000 / IMU_RATE_ACTIVE_HZ as u64
        } else {
            20
        };

        // Cap sub-steps to avoid generating excessive samples at high speedup
        let effective_dt = dt_ms.min(5000);
        let mut remaining = effective_dt;
        let sim_time_base = self.sim_time_ms;

        while remaining > 0 {
            let sub_dt = remaining.min(gnss_interval);
            remaining -= sub_dt;

            let (lat, lon, speed_mps, heading, profile) = if let Some(driving) = &mut self.driving {
                driving.step(sub_dt as f64 / 1000.0)
            } else {
                return;
            };

            let speed_kmh = speed_mps * 3.6;
            let sample_ts = sim_time_base - remaining;

            if sample_ts - self.last_gnss_ms >= gnss_interval {
                let sample = self.gnss.update(sample_ts, lat, lon, speed_mps, heading, sub_dt);
                self.gnss_samples.push(sample);
                self.gnss_sample_count += 1;
                self.last_gnss_ms = sample_ts;

                for k in 0..imu_per_gnss {
                    let imu_ts = sample_ts - gnss_interval + k as u64 * imu_window_ms;
                    let imu = self.imu.update(imu_ts, speed_mps, heading, imu_window_ms, &profile);
                    self.imu_summaries.push(imu);
                    self.imu_summary_count += 1;
                }
                self.last_imu_ms = sample_ts;

                if speed_kmh < MIN_SPEED_KMH {
                    let last_imu = self.imu_summaries.last().unwrap();
                    if last_imu.variance <= ULP_MOTION_THRESHOLD_MG {
                        self.events.add(
                            "stop_candidate",
                            sample_ts,
                            lat,
                            lon,
                            "speed below threshold",
                        );
                        self.transition(DeviceState::StopCandidate);
                        return;
                    }
                }
            }
        }
    }

    fn handle_stop_candidate(&mut self, dt_ms: u64) {
        let elapsed = self.state_elapsed_ms();

        if self.driving.is_none() {
            if elapsed >= STOP_DWELL_MS {
                self.events
                    .add("trip_end", self.sim_time_ms, 0.0, 0.0, "dwell timeout");
                self.transition(DeviceState::Finalizing);
            }
            return;
        }

        let gnss_interval = if GNSS_RATE_SLOW_HZ > 0 {
            1000 / GNSS_RATE_SLOW_HZ as u64
        } else {
            dt_ms
        };

        // Cap sub-steps to avoid spinning at high speedup during dwell
        let effective_dt = dt_ms.min(5000);
        let mut remaining = effective_dt;
        let sim_time_base = self.sim_time_ms;

        while remaining > 0 {
            let sub_dt = remaining.min(gnss_interval);
            remaining -= sub_dt;
            let sample_ts = sim_time_base - remaining;

            let (lat, lon, speed_mps, heading, profile) = if let Some(driving) = &mut self.driving {
                driving.step(sub_dt as f64 / 1000.0)
            } else {
                return;
            };

            let speed_kmh = speed_mps * 3.6;

            if sample_ts - self.last_gnss_ms >= gnss_interval {
                let sample = self.gnss.update(sample_ts, lat, lon, speed_mps, heading, sub_dt);
                self.gnss_samples.push(sample);
                self.gnss_sample_count += 1;
                self.last_gnss_ms = sample_ts;

                let imu = self.imu.update(sample_ts, speed_mps, heading, sub_dt, &profile);
                self.imu_summaries.push(imu);
                self.imu_summary_count += 1;

                if speed_kmh >= MIN_SPEED_KMH || imu.variance > ULP_MOTION_THRESHOLD_MG {
                    self.log("[STOP_CANDIDATE] Motion resumed -> RECORDING");
                    self.events
                        .add("trip_resumed", sample_ts, lat, lon, "motion resumed");
                    self.transition(DeviceState::Recording);
                    return;
                }
            }
        }

        let elapsed = self.state_elapsed_ms();
        if elapsed >= STOP_DWELL_MS {
            self.log("[STOP_CANDIDATE] Dwell timeout -> FINALIZING");
            self.events
                .add("trip_end", self.sim_time_ms, 0.0, 0.0, "dwell timeout");
            self.transition(DeviceState::Finalizing);
        }
    }

    fn handle_finalizing(&mut self) {
        if self.gnss_samples.is_empty() {
            self.log("[FINALIZING] No samples -- nothing to finalize");
            self.transition(DeviceState::QueuedForHomeSync);
            return;
        }

        let ok = self.write_trip_bundle();
        if ok {
            self.log(&format!(
                "[FINALIZING] Bundle written: {} GNSS, {} IMU",
                self.gnss_sample_count, self.imu_summary_count
            ));
            self.queued_trips.push(self.trip_id.clone());
            self.transition(DeviceState::QueuedForHomeSync);
        } else {
            self.log("[FINALIZING] Failed to write bundle");
            self.transition(DeviceState::Fault);
        }
    }

    fn handle_queued(&mut self, _dt_ms: u64) {
        let driving_active = self
            .driving
            .as_ref()
            .map(|d| !d.finished())
            .unwrap_or(false);

        if driving_active {
            self.transition(DeviceState::Arming);
            return;
        }

        if self.sim_time_ms - self.last_wifi_scan_ms >= WIFI_SCAN_INTERVAL_MS {
            self.last_wifi_scan_ms = self.sim_time_ms;

            if self.network.scan_for_home() && self.network.connect() {
                self.log("[QUEUED] Connected to home Wi-Fi -> SYNCING");
                self.transition(DeviceState::Syncing);
                return;
            }
        }

        if self.state_elapsed_ms() > WIFI_SCAN_INTERVAL_MS * 10 {
            self.log("[QUEUED] No network -- going to SLEEP");
            self.transition(DeviceState::Sleep);
        }
    }

    fn handle_syncing(&mut self) {
        if self.force_power_loss && self.power_loss_at_state == Some(DeviceState::Syncing) {
            self.log("[SYNCING] POWER LOSS during sync");
            self.network.disconnect();
            self.shutdown = true;
            return;
        }

        if self.queued_trips.is_empty() {
            self.log("[SYNCING] All trips synced");
            self.network.disconnect();
            self.transition(DeviceState::Retained);
            return;
        }

        let trip_id = self.queued_trips[0].clone();
        let bundle_dir = match self.storage.get_trip_dir(&trip_id) {
            Some(d) => d.clone(),
            None => {
                self.log(&format!("[SYNCING] Trip dir not found for {}", trip_id));
                self.queued_trips.remove(0);
                return;
            }
        };

        let verbose = self.verbose;
        let device_id = self.device_id.clone();
        let sim_time_ms = self.sim_time_ms;

        let log_fn = |msg: &str| {
            let secs = (sim_time_ms / 1000) as i64;
            let millis = (sim_time_ms % 1000) as u32;
            let dt = DateTime::from_timestamp(secs, millis * 1_000_000).unwrap_or_default();
            let ts = dt.format("%H:%M:%S%.3f").to_string();
            let line = format!("[{}] [{}] {}", ts, device_id, msg);
            if verbose {
                eprintln!("{}", line);
            }
        };

        let ok = self.network.upload_bundle(&trip_id, &bundle_dir, &log_fn);
        if ok {
            self.queued_trips.remove(0);
            self.synced_trips.push(trip_id.clone());
            self.total_uploaded += 1;
            self.log(&format!("[SYNCING] Upload complete for {}", trip_id));
            if self.queued_trips.is_empty() {
                self.network.disconnect();
                self.transition(DeviceState::Retained);
            }
        } else {
            self.log(&format!(
                "[SYNCING] Upload failed for {} -- requeue",
                trip_id
            ));
            self.network.disconnect();
            self.transition(DeviceState::QueuedForHomeSync);
        }
    }

    fn handle_retained(&mut self) {
        self.log("[RETAINED] Trip data retained (retention window skipped in emulation)");
        self.transition(DeviceState::Prunable);
    }

    fn handle_prunable(&mut self) {
        for trip_id in &self.synced_trips {
            self.log_lines.push(format!(
                "[PRUNABLE] Trip {} marked prunable (files retained for testing)",
                trip_id
            ));
        }
        self.synced_trips.clear();
        self.battery.stop_engine();
        self.transition(DeviceState::Sleep);
        self.shutdown = true;
    }

    fn handle_low_battery(&mut self) {
        self.log("[LOW_BATTERY] Finalizing trip if active, entering deep sleep");
        self.events.add(
            "power_anomaly",
            self.sim_time_ms,
            0.0,
            0.0,
            "low battery protection",
        );
        if !self.gnss_samples.is_empty() {
            self.write_trip_bundle();
            self.queued_trips.push(self.trip_id.clone());
        }
        self.shutdown = true;
    }

    fn handle_fault(&mut self) {
        self.log("[FAULT] Error state -- device would reboot");
        self.shutdown = true;
    }

    fn write_trip_bundle(&mut self) -> bool {
        let trip_id = self.trip_id.clone();

        let mut samples_buf = Vec::with_capacity(self.gnss_samples.len() * 32);
        for s in &self.gnss_samples {
            samples_buf.extend_from_slice(&s.to_bytes());
        }

        let mut imu_buf = Vec::with_capacity(self.imu_summaries.len() * 24);
        for s in &self.imu_summaries {
            imu_buf.extend_from_slice(&s.to_bytes());
        }

        let events_bytes =
            serde_json::to_string_pretty(&self.events.events).unwrap_or_default().into_bytes();

        let ended_ms = self.sim_time_ms;
        let started_at = DateTime::from_timestamp(
            (self.trip_start_ms / 1000) as i64,
            ((self.trip_start_ms % 1000) * 1_000_000) as u32,
        )
        .unwrap_or_default()
        .to_rfc3339();
        let ended_at = DateTime::from_timestamp(
            (ended_ms / 1000) as i64,
            ((ended_ms % 1000) * 1_000_000) as u32,
        )
        .unwrap_or_default()
        .to_rfc3339();

        let manifest = serde_json::json!({
            "version": 1,
            "trip_id": trip_id,
            "device_id": self.device_id,
            "firmware_version": "0.1.0-emulator",
            "started_at": started_at,
            "ended_at": ended_at,
            "started_at_ms": self.trip_start_ms,
            "ended_at_ms": ended_ms,
            "sample_count": {
                "gnss": self.gnss_sample_count,
                "imu_summary": self.imu_summary_count,
                "imu_raw_windows": 0,
            },
            "gnss_sample_count": self.gnss_sample_count,
            "imu_summary_count": self.imu_summary_count,
            "gnss_rate_hz": GNSS_RATE_ACTIVE_HZ,
            "imu_rate_hz": IMU_RATE_ACTIVE_HZ,
            "schema_version": 1,
            "compression": "none",
        });
        let manifest_bytes = serde_json::to_string_pretty(&manifest)
            .unwrap_or_default()
            .into_bytes();

        let samples_hash = format!("{:x}", Sha256::digest(&samples_buf));
        let imu_hash = format!("{:x}", Sha256::digest(&imu_buf));
        let events_hash = format!("{:x}", Sha256::digest(&events_bytes));
        let manifest_hash = format!("{:x}", Sha256::digest(&manifest_bytes));

        let sums_text = format!(
            "{}  samples.bin\n{}  imu_summary.bin\n{}  events.json\n{}  manifest.json\n",
            samples_hash, imu_hash, events_hash, manifest_hash
        );

        let mut ok = true;
        ok = ok && self.storage.write_file(&trip_id, "samples.bin", &samples_buf);
        ok = ok && self.storage.write_file(&trip_id, "imu_summary.bin", &imu_buf);
        ok = ok && self.storage.write_file(&trip_id, "events.json", &events_bytes);
        ok = ok && self.storage.write_file(&trip_id, "manifest.json", &manifest_bytes);
        ok = ok && self.storage.write_file(&trip_id, "sha256sums.txt", sums_text.as_bytes());

        if ok {
            let duration_s = (ended_ms - self.trip_start_ms) as f64 / 1000.0;
            self.all_trips.push(TripSummary {
                trip_id: trip_id.clone(),
                gnss_samples: self.gnss_sample_count,
                imu_summaries: self.imu_summary_count,
                events: self.events.events.len(),
                duration_s,
                samples_bytes: samples_buf.len(),
                imu_bytes: imu_buf.len(),
            });
        }

        ok
    }

    fn build_summary(&self, scenario_name: &str) -> ScenarioResult {
        let errors: Vec<String> = self
            .log_lines
            .iter()
            .filter(|l| l.contains("FAULT") || l.to_lowercase().contains("failed"))
            .cloned()
            .collect();

        ScenarioResult {
            scenario: scenario_name.to_string(),
            device_id: self.device_id.clone(),
            trips_generated: self.all_trips.len(),
            trips_uploaded: self.total_uploaded,
            trips_queued: self.queued_trips.len(),
            final_state: self.state.to_string(),
            trips: self.all_trips.clone(),
            errors,
            expects_errors: self.expects_errors,
            total_gnss_samples: self.all_trips.iter().map(|t| t.gnss_samples).sum(),
            total_imu_summaries: self.all_trips.iter().map(|t| t.imu_summaries).sum(),
        }
    }
}

pub trait Scenario {
    fn name(&self) -> &str;
    fn description(&self) -> &str;
    fn expects_errors(&self) -> bool {
        false
    }
    fn setup(&mut self, device: &mut DeviceEmulator);
    fn tick(&mut self, device: &mut DeviceEmulator, sim_time_ms: u64);
    fn is_complete(&self, device: &DeviceEmulator, sim_time_ms: u64) -> bool;
}

pub fn default_is_complete(device: &DeviceEmulator, _sim_time_ms: u64) -> bool {
    if device.shutdown {
        return true;
    }
    if device.state == DeviceState::Sleep {
        let driving_done = device
            .driving
            .as_ref()
            .map(|d| d.finished())
            .unwrap_or(true);
        if driving_done && device.queued_trips.is_empty() {
            return true;
        }
    }
    false
}
