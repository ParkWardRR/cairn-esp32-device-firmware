const EARTH_RADIUS_M: f64 = 6_371_000.0;

pub fn haversine_m(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    let r1 = lat1.to_radians();
    let r2 = lat2.to_radians();
    let dl = (lon2 - lon1).to_radians();
    let dlat = r2 - r1;
    let a = (dlat / 2.0).sin().powi(2) + r1.cos() * r2.cos() * (dl / 2.0).sin().powi(2);
    EARTH_RADIUS_M * 2.0 * a.sqrt().atan2((1.0 - a).sqrt())
}

pub fn bearing_deg(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    let r1 = lat1.to_radians();
    let r2 = lat2.to_radians();
    let dl = (lon2 - lon1).to_radians();
    let x = dl.sin() * r2.cos();
    let y = r1.cos() * r2.sin() - r1.sin() * r2.cos() * dl.cos();
    x.atan2(y).to_degrees().rem_euclid(360.0)
}

pub fn lerp(a: f64, b: f64, t: f64) -> f64 {
    a + (b - a) * t
}

const CROCKFORD_BASE32: &[u8] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

pub fn generate_ulid(rng: &mut impl rand::Rng, timestamp_ms: u64) -> String {
    let ts = timestamp_ms & 0xFFFF_FFFF_FFFF;
    let mut chars = Vec::with_capacity(26);
    for i in (0..10).rev() {
        chars.push(CROCKFORD_BASE32[((ts >> (i * 5)) & 0x1F) as usize] as char);
    }
    for _ in 0..16 {
        chars.push(CROCKFORD_BASE32[rng.gen_range(0..32)] as char);
    }
    chars.into_iter().collect()
}
