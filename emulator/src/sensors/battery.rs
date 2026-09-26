use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};

pub struct BatteryEmulator {
    rng: StdRng,
    voltage: i32,
    engine_running: bool,
    cranking: bool,
    forced_low: bool,
}

impl BatteryEmulator {
    pub fn new(rng_seed: u64) -> Self {
        Self {
            rng: StdRng::seed_from_u64(rng_seed),
            voltage: 12_600,
            engine_running: false,
            cranking: false,
            forced_low: false,
        }
    }

    pub fn start_engine(&mut self) {
        self.cranking = true;
        self.voltage = self.rng.gen_range(10_800..=11_200);
    }

    pub fn engine_running(&mut self) {
        self.cranking = false;
        self.engine_running = true;
        self.voltage = self.rng.gen_range(14_000..=14_400);
    }

    pub fn stop_engine(&mut self) {
        self.engine_running = false;
        self.cranking = false;
        self.voltage = 12_600;
    }

    pub fn force_low_battery(&mut self) {
        self.forced_low = true;
    }

    pub fn force_power_loss(&mut self) {
        self.voltage = 0;
    }

    pub fn update(&mut self, _dt_ms: u64) -> i32 {
        if self.forced_low {
            let drop = self.rng.gen_range(5..=15);
            self.voltage = (self.voltage - drop).max(10_500);
            return self.voltage;
        }

        if self.cranking {
            self.voltage = self.rng.gen_range(10_500..=11_200);
        } else if self.engine_running {
            self.voltage = 14_200 + self.rng.gen_range(-200..=200);
        } else {
            let drift = self.rng.gen_range(-3..=1);
            self.voltage = (self.voltage + drift).max(11_000);
        }

        self.voltage
    }

    pub fn voltage_mv(&self) -> i32 {
        self.voltage
    }
}
