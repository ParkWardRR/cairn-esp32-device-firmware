//! Framed record envelope (spec §3.2).
//!
//! ```text
//! 0   frame_len u16   record_type u8   schema_version u8   flags u16   reserved u16
//! 8   seq u32         monotonic_ms u32  prev_crc32 u32     reserved2 u32
//! 24  nonce[24]
//! 48  ciphertext[N]
//! 48+N tag[16]
//! 64+N crc32 over [0, 64+N)   — over the ciphertext frame
//! ```

use super::{
    FormatError, Result,
    aead::SegmentCipher,
    crc::crc32,
    keys::{AEAD_OVERHEAD, NONCE_SIZE},
};

/// Frame envelope geometry. `frame_len` counts the whole frame: the
/// `frame_len` field itself, the sealed payload (nonce, ciphertext, tag) and
/// the trailing CRC.
///
/// Every frame is encrypted, so even an empty record costs the 40-byte AEAD
/// overhead and the minimum frame is 68 bytes. `MAX_FRAME_LEN` did not change
/// from v2, which is why the largest plaintext payload shrank from 4068 to 4028.
pub const FRAME_HEADER_SIZE: usize = 24;
pub const FRAME_TRAILER_SIZE: usize = 4;
/// Header plus CRC, no payload.
pub const FRAME_ENVELOPE_SIZE: usize = FRAME_HEADER_SIZE + FRAME_TRAILER_SIZE; // 28
/// Envelope plus nonce and tag: everything but the plaintext.
pub const FRAME_OVERHEAD: usize = FRAME_ENVELOPE_SIZE + AEAD_OVERHEAD; // 68
pub const MIN_FRAME_LEN: usize = FRAME_OVERHEAD;
pub const MAX_FRAME_LEN: usize = 4096;
/// The largest plaintext payload a frame can carry.
pub const MAX_PAYLOAD_SIZE: usize = MAX_FRAME_LEN - FRAME_OVERHEAD; // 4028

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
    pub const OBD_EXTENDED: Self = Self(0x0A);

    /// Whether this implementation understands the type.
    ///
    /// An unknown type is not an error: it is skipped via `frame_len`, counted
    /// and reported, which is how a newer device stays partially readable by an
    /// older decoder. The frame CRC and — in a keyed scan — the tag still apply,
    /// so a skipped record remains integrity-checked.
    pub fn known(self) -> bool {
        (Self::GNSS_SAMPLE.0..=Self::OBD_EXTENDED.0).contains(&self.0)
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
            Self::OBD_EXTENDED => "OBD_EXTENDED".into(),
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

/// One framed record, as scanned.
///
/// A scanned frame is in one of two states. After a structural (keyless) scan
/// `payload` is `None` and `sealed` holds the nonce, ciphertext and tag as
/// stored: the CRC, chain and sequence checks passed, nothing more is known.
/// After a keyed scan `payload` is `Some(plaintext)` and the tag verified.
///
/// `payload` is `None` rather than the ciphertext on purpose. Ciphertext is
/// exactly as long as plaintext, so a payload parser handed it would not fail
/// on length — it would return a plausible-looking record of random numbers.
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
    /// `nonce[24] ‖ ciphertext ‖ tag[16]` exactly as stored.
    pub sealed: Vec<u8>,
    /// The authenticated plaintext, present only after a keyed scan.
    pub payload: Option<Vec<u8>>,
    /// The frame's own checksum.
    pub crc32: u32,
}

impl Frame {
    /// The encoded `frame_len`.
    pub fn len(&self) -> usize {
        FRAME_ENVELOPE_SIZE + self.sealed.len()
    }

    /// Whether the plaintext payload is empty (the frame itself never is).
    pub fn is_empty(&self) -> bool {
        self.sealed.len() == AEAD_OVERHEAD
    }

    /// Whether the scan that produced this frame authenticated it.
    pub fn decrypted(&self) -> bool {
        self.payload.is_some()
    }
}

/// The header fields of a frame being written. `frame_len` is derived from the
/// payload, and `reserved`/`reserved2` are always zero, so neither is here.
#[derive(Debug, Clone, Copy)]
pub struct FrameFields {
    pub record_type: RecordType,
    pub schema_version: u8,
    pub flags: u16,
    pub seq: u32,
    pub monotonic_ms: u32,
    pub prev_crc32: u32,
}

/// Encode the 24-byte frame header for a frame whose plaintext payload is
/// `payload_len` bytes.
fn append_frame_header(dst: &mut Vec<u8>, f: &FrameFields, payload_len: usize) {
    let frame_len = (FRAME_OVERHEAD + payload_len) as u16;
    dst.extend_from_slice(&frame_len.to_le_bytes());
    dst.push(f.record_type.0);
    dst.push(f.schema_version);
    dst.extend_from_slice(&f.flags.to_le_bytes());
    dst.extend_from_slice(&0u16.to_le_bytes()); // reserved
    dst.extend_from_slice(&f.seq.to_le_bytes());
    dst.extend_from_slice(&f.monotonic_ms.to_le_bytes());
    dst.extend_from_slice(&f.prev_crc32.to_le_bytes());
    dst.extend_from_slice(&0u32.to_le_bytes()); // reserved2, keeps the sealed payload 4-byte aligned
}

/// Seal `payload` and append the encoded frame to `dst`, returning the frame's
/// CRC — which is the next frame's `prev_crc32`.
///
/// The CRC covers the ciphertext frame, not the plaintext. That is what lets a
/// holder of no key verify torn tails, the chain and the Merkle root. The
/// header is authenticated exactly as written, `frame_len` and `prev_crc32`
/// included, so a frame cannot be resized or re-chained without failing its
/// tag.
///
/// On error `dst` is left exactly as it was: a half-appended frame would be a
/// torn tail manufactured in memory.
pub fn append_frame(
    dst: &mut Vec<u8>,
    fields: &FrameFields,
    payload: &[u8],
    cipher: &SegmentCipher,
    nonce: &[u8; NONCE_SIZE],
) -> Result<u32> {
    if payload.len() > MAX_PAYLOAD_SIZE {
        return Err(FormatError::PayloadTooLarge {
            size: payload.len(),
            max: MAX_PAYLOAD_SIZE,
        });
    }

    let start = dst.len();
    append_frame_header(dst, fields, payload.len());

    let sealed = match cipher.seal(&dst[start..start + FRAME_HEADER_SIZE], nonce, payload) {
        Ok(s) => s,
        Err(e) => {
            dst.truncate(start);
            return Err(e);
        }
    };
    dst.extend_from_slice(&sealed);

    let crc = crc32(&dst[start..]);
    dst.extend_from_slice(&crc.to_le_bytes());

    Ok(crc)
}

/// Decode the 24-byte envelope. `b` must be at least `FRAME_HEADER_SIZE` long.
/// The sealed body, payload and CRC are filled in by the scanner.
pub(crate) fn decode_frame_header(b: &[u8]) -> Frame {
    Frame {
        record_type: RecordType(b[2]),
        schema_version: b[3],
        flags: u16::from_le_bytes([b[4], b[5]]),
        seq: u32::from_le_bytes([b[8], b[9], b[10], b[11]]),
        monotonic_ms: u32::from_le_bytes([b[12], b[13], b[14], b[15]]),
        prev_crc32: u32::from_le_bytes([b[16], b[17], b[18], b[19]]),
        sealed: Vec::new(),
        payload: None,
        crc32: 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The geometry the spec states in prose, pinned as arithmetic.
    #[test]
    fn geometry_matches_the_spec() {
        assert_eq!(FRAME_OVERHEAD, 68);
        assert_eq!(MIN_FRAME_LEN, 68);
        assert_eq!(MAX_PAYLOAD_SIZE, 4028);
        assert_eq!(FRAME_HEADER_SIZE + NONCE_SIZE, 48, "ciphertext offset");
    }
}
