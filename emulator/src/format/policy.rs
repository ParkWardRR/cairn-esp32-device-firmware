//! `POLICY_SNAPSHOT` payload (spec §4.9).
//!
//! The active capture policy a bundle was produced under, as a deterministic
//! CBOR map. Recorded because a version number identifies a policy without
//! describing one: interpreting an old bundle from `policy_version` alone would
//! mean finding the firmware build that defined that version, whereas the
//! values make a trip captured under thresholds nobody remembers explainable
//! from the trip itself.

use serde::Deserialize;

use super::{
    FormatError, Result,
    cbor::{Decoder, Encoder},
};

/// Field names mirror the specification (and the vectors' JSON), so the
/// conformance expectation deserialises straight into this struct rather than
/// through a parallel definition somebody has to keep in step.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct PolicySnapshot {
    pub policy_version: u8,
    pub gnss_period_ms: u16,
    pub imu_window_ms: u16,
    pub obd_period_ms: u16,
    pub health_period_ms: u32,
    pub start_score_threshold_e2: u16,
    pub stop_score_threshold_e2: u16,
    pub start_dwell_ms: u32,
    pub stop_dwell_ms: u32,
    pub motion_accel_rms_mg: u16,
    pub motion_speed_cmps: u16,
    pub preroll_window_ms: u32,
    pub preroll_ring_samples: u16,
    pub segment_max_bytes: u32,
    /// The periods above are upper bounds during a trip rather than fixed
    /// rates (§4.9.1). Adaptation may only add detail, so a reader's floor of
    /// one record per nominal period still holds.
    pub adaptive_sampling: bool,
}

const POLICY_FIELD_COUNT: usize = 15;

impl PolicySnapshot {
    /// Encode deterministically. Keys ascend 1..=15 and every value is an
    /// unsigned integer, so identical policy yields identical bytes — which is
    /// what lets a reader group bundles by policy without trusting the version
    /// number.
    pub fn to_cbor(&self) -> Vec<u8> {
        let values: [u64; POLICY_FIELD_COUNT] = [
            self.policy_version as u64,
            self.gnss_period_ms as u64,
            self.imu_window_ms as u64,
            self.obd_period_ms as u64,
            self.health_period_ms as u64,
            self.start_score_threshold_e2 as u64,
            self.stop_score_threshold_e2 as u64,
            self.start_dwell_ms as u64,
            self.stop_dwell_ms as u64,
            self.motion_accel_rms_mg as u64,
            self.motion_speed_cmps as u64,
            self.preroll_window_ms as u64,
            self.preroll_ring_samples as u64,
            self.segment_max_bytes as u64,
            self.adaptive_sampling as u64,
        ];

        let mut e = Encoder::new();
        e.map_header(POLICY_FIELD_COUNT);
        for (i, v) in values.iter().enumerate() {
            e.key(i as u64 + 1);
            e.uint(*v);
        }
        e.into_bytes()
    }

    /// Decode a `POLICY_SNAPSHOT` payload, rejecting anything this
    /// implementation would not itself have produced.
    ///
    /// An unknown key is refused rather than ignored: the field count is fixed,
    /// so an unexpected key means this decoder does not understand the policy it
    /// is being asked to describe, and reporting a partial policy as complete
    /// would be worse than reporting none.
    pub fn from_cbor(b: &[u8]) -> Result<Self> {
        let mut d = Decoder::new(b);
        let n = d.map_header()?;
        if n != POLICY_FIELD_COUNT {
            return Err(FormatError::Malformed(format!(
                "policy snapshot has {n} fields, want {POLICY_FIELD_COUNT}"
            )));
        }

        let mut values = [None::<u64>; POLICY_FIELD_COUNT];
        for _ in 0..n {
            let key = d.uint()?;
            let v = d.uint()?;
            let slot = key
                .checked_sub(1)
                .and_then(|i| values.get_mut(i as usize))
                .ok_or_else(|| {
                    FormatError::Malformed(format!(
                        "policy snapshot has unknown key {key}; this build does not understand the policy"
                    ))
                })?;
            if slot.replace(v).is_some() {
                return Err(FormatError::Malformed(format!(
                    "policy snapshot key {key} appears twice"
                )));
            }
        }
        if !d.at_end() {
            return Err(FormatError::Malformed(format!(
                "policy snapshot has {} trailing byte(s)",
                d.remaining()
            )));
        }

        let get = |i: usize| values[i].unwrap_or(0);
        let narrow = |i: usize, max: u64| -> Result<u64> {
            let v = get(i);
            if v > max {
                return Err(FormatError::Malformed(format!(
                    "policy snapshot key {} value {v} exceeds its field width",
                    i + 1
                )));
            }
            Ok(v)
        };
        let p = Self {
            policy_version: narrow(0, u8::MAX as u64)? as u8,
            gnss_period_ms: narrow(1, u16::MAX as u64)? as u16,
            imu_window_ms: narrow(2, u16::MAX as u64)? as u16,
            obd_period_ms: narrow(3, u16::MAX as u64)? as u16,
            health_period_ms: narrow(4, u32::MAX as u64)? as u32,
            start_score_threshold_e2: narrow(5, u16::MAX as u64)? as u16,
            stop_score_threshold_e2: narrow(6, u16::MAX as u64)? as u16,
            start_dwell_ms: narrow(7, u32::MAX as u64)? as u32,
            stop_dwell_ms: narrow(8, u32::MAX as u64)? as u32,
            motion_accel_rms_mg: narrow(9, u16::MAX as u64)? as u16,
            motion_speed_cmps: narrow(10, u16::MAX as u64)? as u16,
            preroll_window_ms: narrow(11, u32::MAX as u64)? as u32,
            preroll_ring_samples: narrow(12, u16::MAX as u64)? as u16,
            segment_max_bytes: narrow(13, u32::MAX as u64)? as u32,
            adaptive_sampling: narrow(14, 1)? == 1,
        };

        // Re-encode and compare. Keys out of order, or a boolean written as
        // anything but 0/1, decode to the same values but not the same bytes,
        // and byte identity is the property this record exists to provide.
        if p.to_cbor() != b {
            return Err(FormatError::NonCanonicalCbor(
                "policy snapshot does not round-trip to identical bytes".into(),
            ));
        }
        Ok(p)
    }
}
