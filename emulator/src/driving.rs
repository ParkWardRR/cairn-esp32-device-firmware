use rand::Rng;
use rand::rngs::StdRng;

use crate::math::{bearing_deg, haversine_m, lerp};
use crate::types::{WAYPOINTS, Waypoint};

pub struct RouteSegment {
    pub start: Waypoint,
    pub end: Waypoint,
    pub cruise_kmh: f64,
    pub stop_at_end_s: f64,
    pub profile: String,
    pub gnss_override: Option<String>,
}

pub struct RouteProfile {
    pub name: String,
    pub segments: Vec<RouteSegment>,
    pub home_at_end: bool,
}

pub struct DrivingSimulator {
    route_segments: Vec<RouteSegmentData>,
    seg_idx: usize,
    seg_dist: f64,
    seg_total_dist: f64,
    current_speed_mps: f64,
    finished: bool,
    stopped_timer_s: f64,
    stop_duration_s: f64,
    is_stopped_at_waypoint: bool,
    rng: StdRng,
}

struct RouteSegmentData {
    start_lat: f64,
    start_lon: f64,
    end_lat: f64,
    end_lon: f64,
    cruise_kmh: f64,
    stop_at_end_s: f64,
    profile: String,
    total_dist: f64,
}

impl DrivingSimulator {
    pub fn new(rng: &mut StdRng, profile: RouteProfile) -> Self {
        use rand::SeedableRng;
        let child_rng = StdRng::seed_from_u64(rng.r#gen());

        let route_segments: Vec<RouteSegmentData> = profile
            .segments
            .iter()
            .map(|seg| {
                let dist = haversine_m(seg.start.1, seg.start.2, seg.end.1, seg.end.2);
                RouteSegmentData {
                    start_lat: seg.start.1,
                    start_lon: seg.start.2,
                    end_lat: seg.end.1,
                    end_lon: seg.end.2,
                    cruise_kmh: seg.cruise_kmh,
                    stop_at_end_s: seg.stop_at_end_s,
                    profile: seg.profile.clone(),
                    total_dist: dist,
                }
            })
            .collect();

        let first_dist = route_segments.first().map(|s| s.total_dist).unwrap_or(0.0);
        let first_stop = route_segments
            .first()
            .map(|s| s.stop_at_end_s)
            .unwrap_or(0.0);

        Self {
            route_segments,
            seg_idx: 0,
            seg_dist: 0.0,
            seg_total_dist: first_dist,
            current_speed_mps: 0.0,
            finished: false,
            stopped_timer_s: 0.0,
            stop_duration_s: first_stop,
            is_stopped_at_waypoint: false,
            rng: child_rng,
        }
    }

    pub fn finished(&self) -> bool {
        self.finished
    }

    fn precompute_segment(&mut self) {
        if self.seg_idx >= self.route_segments.len() {
            self.finished = true;
            return;
        }
        let seg = &self.route_segments[self.seg_idx];
        self.seg_total_dist = seg.total_dist;
        self.seg_dist = 0.0;
        self.is_stopped_at_waypoint = false;
        self.stopped_timer_s = 0.0;
        self.stop_duration_s = seg.stop_at_end_s;
    }

    pub fn step(&mut self, dt_s: f64) -> (f64, f64, f64, f64, String) {
        if self.finished || self.route_segments.is_empty() {
            let seg = self.route_segments.last();
            let (lat, lon) = seg.map(|s| (s.end_lat, s.end_lon)).unwrap_or((0.0, 0.0));
            return (lat, lon, 0.0, 0.0, "city".to_string());
        }

        let seg_idx = self.seg_idx;

        let end_lat = self.route_segments[seg_idx].end_lat;
        let end_lon = self.route_segments[seg_idx].end_lon;
        let profile = self.route_segments[seg_idx].profile.clone();
        let cruise_kmh = self.route_segments[seg_idx].cruise_kmh;
        let total_dist = self.route_segments[seg_idx].total_dist;

        if self.is_stopped_at_waypoint {
            self.stopped_timer_s += dt_s;
            if self.stopped_timer_s >= self.stop_duration_s {
                self.seg_idx += 1;
                self.precompute_segment();
                if self.finished {
                    return (end_lat, end_lon, 0.0, 0.0, profile);
                }
            }
            let lat = end_lat + self.gauss(0.0, 0.000001);
            let lon = end_lon + self.gauss(0.0, 0.000001);
            return (lat, lon, 0.0, 0.0, profile);
        }

        let start_lat = self.route_segments[seg_idx].start_lat;
        let start_lon = self.route_segments[seg_idx].start_lon;
        let cruise_mps_orig = cruise_kmh / 3.6;
        let total = total_dist.max(1.0);

        let accel_rate = self.rng.gen_range(1.8..=2.5);
        let decel_rate = self.rng.gen_range(2.5..=3.5);
        let t_accel = cruise_mps_orig / accel_rate;
        let d_accel_orig = 0.5 * accel_rate * t_accel * t_accel;
        let t_decel = cruise_mps_orig / decel_rate;
        let d_decel_orig = 0.5 * decel_rate * t_decel * t_decel;

        let (cruise_mps, d_accel, d_decel);
        if d_accel_orig + d_decel_orig > total {
            let peak = (total / (0.5 / accel_rate + 0.5 / decel_rate)).sqrt();
            d_accel = 0.5 * accel_rate * (peak / accel_rate).powi(2);
            d_decel = total - d_accel;
            cruise_mps = peak;
        } else {
            cruise_mps = cruise_mps_orig;
            d_accel = d_accel_orig;
            d_decel = d_decel_orig;
        }

        let d = self.seg_dist;
        let remaining = total - d;

        let target_speed = if d < d_accel {
            (2.0 * accel_rate * d.max(0.1)).sqrt()
        } else if remaining < d_decel {
            (2.0 * decel_rate * remaining.max(0.1)).sqrt()
        } else {
            cruise_mps
        };
        let target_speed = target_speed.clamp(0.0, cruise_mps);

        let speed_diff = target_speed - self.current_speed_mps;
        let max_delta = if speed_diff > 0.0 {
            accel_rate
        } else {
            decel_rate
        } * dt_s;
        let delta = speed_diff.clamp(-max_delta, max_delta);
        self.current_speed_mps = (self.current_speed_mps + delta).max(0.0);

        self.seg_dist += self.current_speed_mps * dt_s;
        let seg_frac = (self.seg_dist / total).min(1.0);

        let lat = lerp(start_lat, end_lat, seg_frac);
        let lon = lerp(start_lon, end_lon, seg_frac);
        let heading = bearing_deg(start_lat, start_lon, end_lat, end_lon);

        if seg_frac >= 1.0 {
            self.current_speed_mps = 0.0;
            if self.stop_duration_s > 0.0 {
                self.is_stopped_at_waypoint = true;
                self.stopped_timer_s = 0.0;
            } else {
                self.seg_idx += 1;
                self.precompute_segment();
            }
        }

        (lat, lon, self.current_speed_mps, heading, profile)
    }

    fn gauss(&mut self, mean: f64, std_dev: f64) -> f64 {
        let u1: f64 = self.rng.r#gen();
        let u2: f64 = self.rng.r#gen();
        mean + std_dev * (-2.0 * u1.ln()).sqrt() * (2.0 * std::f64::consts::PI * u2).cos()
    }
}

pub fn make_city_route(
    rng: &mut StdRng,
    waypoint_indices: &[usize],
    cruise_range: (f64, f64),
    stops: usize,
) -> RouteProfile {
    let mut segments = Vec::new();
    let mut stop_indices = std::collections::HashSet::new();

    let interior: Vec<usize> = (1..waypoint_indices.len().saturating_sub(1)).collect();
    if !interior.is_empty() && stops > 0 {
        let count = stops.min(interior.len());
        let mut pool = interior.clone();
        for _ in 0..count {
            let idx = rng.gen_range(0..pool.len());
            stop_indices.insert(pool.remove(idx));
        }
    }

    for i in 0..waypoint_indices.len().saturating_sub(1) {
        let start_wp = WAYPOINTS[waypoint_indices[i]];
        let end_wp = WAYPOINTS[waypoint_indices[i + 1]];
        let cruise = rng.gen_range(cruise_range.0..=cruise_range.1);
        let stop = if stop_indices.contains(&i) {
            rng.gen_range(15.0..=60.0)
        } else {
            0.0
        };
        segments.push(RouteSegment {
            start: start_wp,
            end: end_wp,
            cruise_kmh: cruise,
            stop_at_end_s: stop,
            profile: "city".to_string(),
            gnss_override: None,
        });
    }

    RouteProfile {
        name: "city_route".to_string(),
        segments,
        home_at_end: true,
    }
}

pub fn make_highway_route(rng: &mut StdRng) -> RouteProfile {
    let segments = vec![
        RouteSegment {
            start: WAYPOINTS[3],
            end: WAYPOINTS[4],
            cruise_kmh: rng.gen_range(30.0..=45.0),
            stop_at_end_s: 0.0,
            profile: "city".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[4],
            end: WAYPOINTS[15],
            cruise_kmh: rng.gen_range(35.0..=50.0),
            stop_at_end_s: 0.0,
            profile: "city".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[15],
            end: WAYPOINTS[16],
            cruise_kmh: rng.gen_range(90.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[16],
            end: WAYPOINTS[17],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[17],
            end: WAYPOINTS[18],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[18],
            end: WAYPOINTS[19],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[19],
            end: WAYPOINTS[20],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[20],
            end: WAYPOINTS[21],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[21],
            end: WAYPOINTS[22],
            cruise_kmh: rng.gen_range(40.0..=60.0),
            stop_at_end_s: rng.gen_range(300.0..=600.0),
            profile: "city".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[22],
            end: WAYPOINTS[21],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[21],
            end: WAYPOINTS[20],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[20],
            end: WAYPOINTS[19],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[19],
            end: WAYPOINTS[18],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[18],
            end: WAYPOINTS[17],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[17],
            end: WAYPOINTS[16],
            cruise_kmh: rng.gen_range(95.0..=115.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[16],
            end: WAYPOINTS[15],
            cruise_kmh: rng.gen_range(80.0..=100.0),
            stop_at_end_s: 0.0,
            profile: "highway".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[15],
            end: WAYPOINTS[4],
            cruise_kmh: rng.gen_range(30.0..=45.0),
            stop_at_end_s: 0.0,
            profile: "city".to_string(),
            gnss_override: None,
        },
        RouteSegment {
            start: WAYPOINTS[4],
            end: WAYPOINTS[3],
            cruise_kmh: rng.gen_range(30.0..=40.0),
            stop_at_end_s: 0.0,
            profile: "city".to_string(),
            gnss_override: None,
        },
    ];
    RouteProfile {
        name: "highway_trip".to_string(),
        segments,
        home_at_end: true,
    }
}
