use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};

use crate::types::{ImuSummary, clamp_i16, clamp_u16};

pub struct ImuEmulator {
    rng: StdRng,
    prev_speed_mps: f64,
    prev_heading: f64,
}

impl ImuEmulator {
    pub fn new(rng_seed: u64) -> Self {
        Self {
            rng: StdRng::seed_from_u64(rng_seed),
            prev_speed_mps: 0.0,
            prev_heading: 0.0,
        }
    }

    pub fn update(
        &mut self,
        sim_time_ms: u64,
        speed_mps: f64,
        heading_deg: f64,
        dt_ms: u64,
        profile: &str,
    ) -> ImuSummary {
        let mut summary = ImuSummary::default();
        summary.window_start_ms = sim_time_ms;
        summary.window_duration_ms = 20;

        let dt_s = (dt_ms as f64 / 1000.0).max(0.001);

        let accel_mps2 = (speed_mps - self.prev_speed_mps) / dt_s;
        let mut accel_x_mg = (accel_mps2 / 9.81 * 1000.0) as i32;

        let mut heading_delta = heading_deg - self.prev_heading;
        if heading_delta > 180.0 {
            heading_delta -= 360.0;
        } else if heading_delta < -180.0 {
            heading_delta += 360.0;
        }
        let yaw_rate_dps = heading_delta / dt_s;
        let lat_accel_mps2 = speed_mps * yaw_rate_dps.to_radians();
        let mut accel_y_mg = (lat_accel_mps2 / 9.81 * 1000.0) as i32;

        let road_noise_mg: i32 = match profile {
            "city" => 30,
            "highway" => 20,
            "parking" => 10,
            "mountain" => 50,
            _ => 25,
        };

        let vibration = if speed_mps > 0.5 {
            self.gauss(0.0, road_noise_mg as f64) as i32
        } else {
            0
        };
        let mut accel_z_mg = 1000 + vibration;

        let mut is_impact = false;
        if (profile == "city" || profile == "mountain") && self.rng.r#gen::<f64>() < 0.002 {
            accel_z_mg += self.rng.gen_range(500..=1500);
            is_impact = true;
        }

        let rms = ((accel_x_mg as f64).powi(2)
            + (accel_y_mg as f64).powi(2)
            + (accel_z_mg as f64).powi(2))
        .sqrt() as i32;

        let gyro_dps10 = (yaw_rate_dps * 10.0) as i32;

        let variance = (rms - 1000 + accel_x_mg.abs() + accel_y_mg.abs()).max(0);

        accel_x_mg += self.gauss(0.0, road_noise_mg as f64 * 0.5) as i32;
        accel_y_mg += self.gauss(0.0, road_noise_mg as f64 * 0.5) as i32;

        let mut flags: u8 = 0;
        if is_impact || rms > 3000 {
            flags |= 0x01;
        }
        if accel_x_mg < -800 {
            flags |= 0x02;
        }
        if accel_y_mg.abs() > 600 {
            flags |= 0x04;
        }

        summary.accel_peak_x_mg = clamp_i16(accel_x_mg as f64);
        summary.accel_peak_y_mg = clamp_i16(accel_y_mg as f64);
        summary.accel_peak_z_mg = clamp_i16(accel_z_mg as f64);
        summary.accel_rms_mg = clamp_u16(rms as f64);
        summary.gyro_peak_dps = clamp_i16(gyro_dps10 as f64);
        summary.variance = clamp_u16(variance as f64);
        summary.flags = flags;

        self.prev_speed_mps = speed_mps;
        self.prev_heading = heading_deg;

        summary
    }

    fn gauss(&mut self, mean: f64, std_dev: f64) -> f64 {
        let u1: f64 = self.rng.r#gen();
        let u2: f64 = self.rng.r#gen();
        mean + std_dev * (-2.0 * u1.ln()).sqrt() * (2.0 * std::f64::consts::PI * u2).cos()
    }
}
