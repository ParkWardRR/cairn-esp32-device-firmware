use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};

use crate::config::MIN_SATELLITES;
use crate::math::lerp;
use crate::types::{clamp_u16, GnssSample};

pub struct GnssEmulator {
    rng: StdRng,
    fix_acquired: bool,
    fix_start_delay_ms: i64,
    fix_acquired_at_ms: i64,
    current_lat: f64,
    current_lon: f64,
    current_alt_cm: i32,
    current_speed_mps: f64,
    current_heading: f64,
    satellites: i32,
    hdop_tenths: i32,
    accuracy_cm: i32,
    gnss_blocked: bool,
    degraded: bool,
}

impl GnssEmulator {
    pub fn new(rng_seed: u64, cold_start: bool) -> Self {
        let mut rng = StdRng::seed_from_u64(rng_seed);
        let fix_start_delay_ms = if cold_start {
            rng.gen_range(30_000..=45_000)
        } else {
            rng.gen_range(5_000..=10_000)
        };

        Self {
            rng,
            fix_acquired: false,
            fix_start_delay_ms,
            fix_acquired_at_ms: 0,
            current_lat: 0.0,
            current_lon: 0.0,
            current_alt_cm: 3000,
            current_speed_mps: 0.0,
            current_heading: 0.0,
            satellites: 0,
            hdop_tenths: 99,
            accuracy_cm: 9999,
            gnss_blocked: false,
            degraded: false,
        }
    }

    pub fn set_position(&mut self, lat: f64, lon: f64) {
        self.current_lat = lat;
        self.current_lon = lon;
    }

    pub fn set_blocked(&mut self, blocked: bool) {
        self.gnss_blocked = blocked;
        if blocked {
            self.fix_acquired = false;
        }
    }

    pub fn set_degraded(&mut self, degraded: bool) {
        self.degraded = degraded;
    }

    pub fn update(
        &mut self,
        sim_time_ms: u64,
        target_lat: f64,
        target_lon: f64,
        target_speed_mps: f64,
        target_heading: f64,
        dt_ms: u64,
    ) -> GnssSample {
        let mut sample = GnssSample::default();
        sample.timestamp_ms = sim_time_ms;

        if self.gnss_blocked {
            sample.fix_quality = 0;
            sample.satellites = 0;
            sample.hdop_tenths = 99;
            sample.accuracy_cm = 9999;
            sample.latitude = 0;
            sample.longitude = 0;
            return sample;
        }

        let elapsed = sim_time_ms as i64;

        if !self.fix_acquired {
            if elapsed < self.fix_start_delay_ms {
                sample.fix_quality = 0;
                sample.satellites = self.rng.gen_range(0..=3);
                sample.hdop_tenths = 99;
                sample.accuracy_cm = 9999;
                return sample;
            } else {
                self.fix_acquired = true;
                self.fix_acquired_at_ms = elapsed;
                self.satellites = self.rng.gen_range(6..=8);
            }
        }

        let fix_age_ms = elapsed - self.fix_acquired_at_ms;
        let fix_maturity = (fix_age_ms as f64 / 15_000.0).min(1.0);

        let alpha = (dt_ms as f64 / 500.0).min(1.0);
        self.current_lat = lerp(self.current_lat, target_lat, alpha);
        self.current_lon = lerp(self.current_lon, target_lon, alpha);
        self.current_speed_mps = lerp(self.current_speed_mps, target_speed_mps, alpha);
        self.current_heading = target_heading;

        let target_sats = if !self.degraded {
            10
        } else {
            self.rng.gen_range(4..=7)
        };
        self.satellites =
            lerp(self.satellites as f64, target_sats as f64, fix_maturity * 0.3) as i32;
        self.satellites = self.satellites.clamp(0, 14);

        let (base_hdop, base_accuracy, noise_m) = if self.degraded {
            let hdop = self.rng.gen_range(25..=45);
            let acc = self.rng.gen_range(800..=2000);
            (hdop, acc, 0.00003)
        } else {
            let hdop = lerp(30.0, 8.0, fix_maturity) as i32;
            let acc = lerp(1500.0, 200.0, fix_maturity) as i32;
            let noise = 0.000002 * (1.5 - fix_maturity);
            (hdop, acc, noise)
        };

        self.hdop_tenths = base_hdop + self.rng.gen_range(-2..=2);
        self.accuracy_cm = base_accuracy + self.rng.gen_range(-50..=50);

        let noisy_lat = self.current_lat + self.gauss(0.0, noise_m);
        let noisy_lon = self.current_lon + self.gauss(0.0, noise_m);

        sample.latitude = (noisy_lat * 1e7).round() as i32;
        sample.longitude = (noisy_lon * 1e7).round() as i32;
        sample.altitude_cm = self.current_alt_cm + self.rng.gen_range(-50..=50);
        sample.speed_cmps = clamp_u16((self.current_speed_mps * 100.0).round());
        sample.heading_cdeg =
            clamp_u16(((self.current_heading * 100.0) as i64).rem_euclid(36000) as f64);
        sample.fix_quality = if self.satellites >= MIN_SATELLITES as i32 {
            1
        } else {
            0
        };
        sample.satellites = self.satellites as u8;
        sample.hdop_tenths = clamp_u16(self.hdop_tenths as f64);
        sample.accuracy_cm = clamp_u16(self.accuracy_cm as f64);

        sample
    }

    fn gauss(&mut self, mean: f64, std_dev: f64) -> f64 {
        // Box-Muller transform
        let u1: f64 = self.rng.r#gen();
        let u2: f64 = self.rng.r#gen();
        mean + std_dev * (-2.0 * u1.ln()).sqrt() * (2.0 * std::f64::consts::PI * u2).cos()
    }
}
