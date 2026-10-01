//! Framed record envelope.

use super::{FormatError, Result, crc::crc32};

/// Frame envelope geometry. `frame_len` counts the whole frame, including the
/// `frame_len` field itself and the trailing CRC.
pub const FRAME_HEADER_SIZE: usize = 24;
pub const FRAME_TRAILER_SIZE: usize = 4;
pub const FRAME_OVERHEAD: usize = FRAME_HEADER_SIZE + FRAME_TRAILER_SIZE; // 28
pub const MIN_FRAME_LEN: usize = FRAME_OVERHEAD;
pub const MAX_FRAME_LEN: usize = 4096;
pub const MAX_PAYLOAD_SIZE: usize = MAX_FRAME_LEN - FRAME_OVERHEAD; // 4068

/// Payload schema identifier.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RecordType(pub u8);

impl RecordType {
    pub const GNSS_SAMPLE: Self = Self(0x01);
    pub const IMU_SUMMARY: Self = Self(0x02);
    pub const IMU_RAW_WINDOW: Self = Self(0x03);
    pub const OBD_SNAPSHOT: Self = Self(0x04);
    pub const DEVICE_HEALTH: Self = Self(0x05);
    pub const TRIP_EVENT: Self = Self(0x06);
    pub const STATE_TRANSITION: Self = Self(0x07);
    pub const GNSS_GAP: Self = Self(0x08);
    pub const POLICY_SNAPSHOT: Self = Self(0x09);

    /// Whether this implementation understands the type.
    ///
    /// An unknown type is not an error: it is skipped via `frame_len`, counted
    /// and reported, which is how a newer device stays partially readable by an
    /// older decoder. The frame CRC still applies, so a skipped record remains
    /// integrity-checked.
    pub fn known(self) -> bool {
        (Self::GNSS_SAMPLE.0..=Self::POLICY_SNAPSHOT.0).contains(&self.0)
    }

    /// The spec's name for this type, used in conformance expectations.
    pub fn name(self) -> String {
        match self {
            Self::GNSS_SAMPLE => "GNSS_SAMPLE".into(),
            Self::IMU_SUMMARY => "IMU_SUMMARY".into(),
            Self::IMU_RAW_WINDOW => "IMU_RAW_WINDOW".into(),
            Self::OBD_SNAPSHOT => "OBD_SNAPSHOT".into(),
            Self::DEVICE_HEALTH => "DEVICE_HEALTH".into(),
            Self::TRIP_EVENT => "TRIP_EVENT".into(),
            Self::STATE_TRANSITION => "STATE_TRANSITION".into(),
            Self::GNSS_GAP => "GNSS_GAP".into(),
            Self::POLICY_SNAPSHOT => "POLICY_SNAPSHOT".into(),
            Self(other) => format!("UNKNOWN(0x{other:02x})"),
        }
    }
}

/// Frame flags.
pub mod flags {
    /// From the pre-roll buffer; precedes trip confirmation.
    pub const PRETRIP: u16 = 1 << 0;
    /// Captured while a degraded health state was active.
    pub const DEGRADED: u16 = 1 << 1;
    /// The UTC basis was an estimate; no valid fix at write time.
    pub const ESTIMATED_UTC: u16 = 1 << 2;
    /// First record written after a boot recovery.
    pub const POST_RECOVERY: u16 = 1 << 3;
}

/// One framed record.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub record_type: RecordType,
    pub schema_version: u8,
    pub flags: u16,
    pub seq: u32,
    /// Milliseconds since the segment header's `opened_monotonic_us`.
    pub monotonic_ms: u32,
    /// The `crc32` of the preceding frame in this chain; 0 for the first.
    pub prev_crc32: u32,
    pub payload: Vec<u8>,
    /// The frame's own checksum, populated on encode and on scan.
    pub crc32: u32,
}

impl Frame {
    /// The encoded `frame_len`.
    pub fn len(&self) -> usize {
        FRAME_OVERHEAD + self.payload.len()
    }

    pub fn is_empty(&self) -> bool {
        self.payload.is_empty()
    }
}

/// Encode a frame and append it to `dst`, returning the frame's CRC — which is
/// the next frame's `prev_crc32`.
///
/// The argument list mirrors the frame's fields one-for-one. A struct would read
/// worse here, because `SegmentWriter` is the API callers actually use — it
/// assigns `seq` and `prev_crc32` itself, so they cannot be set inconsistently.
#[allow(clippy::too_many_arguments)]
pub fn append_frame(
    dst: &mut Vec<u8>,
    record_type: RecordType,
    schema_version: u8,
    flags: u16,
    seq: u32,
    monotonic_ms: u32,
    prev_crc32: u32,
    payload: &[u8],
) -> Result<u32> {
    if payload.len() > MAX_PAYLOAD_SIZE {
        return Err(FormatError::PayloadTooLarge {
            size: payload.len(),
            max: MAX_PAYLOAD_SIZE,
        });
    }

    let start = dst.len();
    let frame_len = (FRAME_OVERHEAD + payload.len()) as u16;

    dst.extend_from_slice(&frame_len.to_le_bytes());
    dst.push(record_type.0);
    dst.push(schema_version);
    dst.extend_from_slice(&flags.to_le_bytes());
    dst.extend_from_slice(&0u16.to_le_bytes()); // reserved
    dst.extend_from_slice(&seq.to_le_bytes());
    dst.extend_from_slice(&monotonic_ms.to_le_bytes());
    dst.extend_from_slice(&prev_crc32.to_le_bytes());
    dst.extend_from_slice(&0u32.to_le_bytes()); // reserved2, aligns payload to 4 bytes
    dst.extend_from_slice(payload);

    let crc = crc32(&dst[start..start + FRAME_HEADER_SIZE + payload.len()]);
    dst.extend_from_slice(&crc.to_le_bytes());

    Ok(crc)
}

/// Decode the 24-byte envelope. `b` must be at least `FRAME_HEADER_SIZE` long.
pub(crate) fn decode_frame_header(b: &[u8]) -> Frame {
    Frame {
        record_type: RecordType(b[2]),
        schema_version: b[3],
        flags: u16::from_le_bytes([b[4], b[5]]),
        seq: u32::from_le_bytes([b[8], b[9], b[10], b[11]]),
        monotonic_ms: u32::from_le_bytes([b[12], b[13], b[14], b[15]]),
        prev_crc32: u32::from_le_bytes([b[16], b[17], b[18], b[19]]),
        payload: Vec::new(),
        crc32: 0,
    }
}
