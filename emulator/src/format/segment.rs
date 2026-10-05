//! Segment header and writer (spec §3.1).
//!
//! | Offset | Size | Field |
//! |---:|---:|---|
//! | 0 | 4 | magic `CRN3` |
//! | 4 | 2 | format_version = 3 |
//! | 6 | 2 | header_len = 128 |
//! | 8 | 16 | device_id |
//! | 24 | 16 | boot_id |
//! | 40 | 16 | vehicle_id |
//! | 56 | 16 | assignment_id |
//! | 72 | 4 | segment_index (`0xFFFFFFFF` = journal) |
//! | 76 | 4 | first_seq |
//! | 80 | 8 | opened_monotonic_us |
//! | 88 | 4 | storage_key_version |
//! | 92 | 8 | device_counter |
//! | 100 | 24 | reserved, zero |
//! | 124 | 4 | header_crc32 over `[0, 124)` |

use std::collections::BTreeMap;

use rand::RngCore;

use super::{
    FORMAT_VERSION, FormatError, Result,
    aead::SegmentCipher,
    crc::crc32,
    frame::{FrameFields, RecordType, append_frame},
    keys::{KeyProvider, NONCE_SIZE},
    scan::ScanState,
};

/// Fixed at 128 bytes. The encoded `header_len` lets a reader skip a longer
/// header from a future version without misparsing frames; it does not make a
/// future format readable.
pub const SEGMENT_HEADER_SIZE: usize = 128;

/// ASCII `CRN3`.
pub const SEGMENT_MAGIC: [u8; 4] = *b"CRN3";

/// The reserved `segment_index` of `journal.seg`.
///
/// The journal is a separate chain from the capture segments (§3.2.1), and
/// `segment_index` is an input to the segment key, so giving the journal a
/// value no capture segment can have also gives it a key no capture segment
/// can share — even though every other header field is identical.
pub const JOURNAL_SEGMENT_INDEX: u32 = 0xFFFF_FFFF;

// Byte offsets within the header. Spelled out, as in the Go reference and the
// C port, because an off-by-one here is invisible until two implementations
// disagree about a card.
const OFF_MAGIC: usize = 0;
const OFF_FORMAT_VERSION: usize = 4;
const OFF_HEADER_LEN: usize = 6;
const OFF_DEVICE_ID: usize = 8;
const OFF_BOOT_ID: usize = 24;
const OFF_VEHICLE_ID: usize = 40;
const OFF_ASSIGNMENT_ID: usize = 56;
const OFF_SEGMENT_INDEX: usize = 72;
const OFF_FIRST_SEQ: usize = 76;
const OFF_OPENED_MONOTONIC_US: usize = 80;
const OFF_STORAGE_KEY_VERSION: usize = 88;
const OFF_DEVICE_COUNTER: usize = 92;
const OFF_RESERVED: usize = 100; // 24 bytes, zero
const OFF_CRC: usize = SEGMENT_HEADER_SIZE - 4;

/// The fixed header at the start of every segment file.
///
/// Everything in it except the CRC is authenticated as AAD on every frame of
/// the segment, so the identity fields are not merely labels: altering any of
/// them without the key makes every frame fail its tag. They are nonetheless
/// readable without a key, deliberately — intake must be able to decide
/// whether a bundle is admissible (does this assignment exist, is this counter
/// spent?) before it decrypts anything.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SegmentHeader {
    pub format_version: u16,
    pub device_id: [u8; 16],
    pub boot_id: [u8; 16],
    /// The vehicle the device was assigned to when the bundle was captured.
    pub vehicle_id: [u8; 16],
    /// The specific device→vehicle assignment; a device moved between cars
    /// gets a new one.
    pub assignment_id: [u8; 16],
    /// 0-based within a bundle, or [`JOURNAL_SEGMENT_INDEX`].
    pub segment_index: u32,
    pub first_seq: u32,
    pub opened_monotonic_us: u64,
    /// Selects which escrowed `K_root` the segment key derives from.
    pub storage_key_version: u32,
    /// The device's monotonic bundle counter for the bundle this segment
    /// belongs to. Identical in every segment of a bundle, journal included,
    /// and repeated in the manifest. A card restored to an older image presents
    /// counters the device and the server have already passed.
    pub device_counter: u64,
}

impl Default for SegmentHeader {
    fn default() -> Self {
        Self {
            format_version: FORMAT_VERSION,
            device_id: [0; 16],
            boot_id: [0; 16],
            vehicle_id: [0; 16],
            assignment_id: [0; 16],
            segment_index: 0,
            first_seq: 0,
            opened_monotonic_us: 0,
            storage_key_version: 0,
            device_counter: 0,
        }
    }
}

impl SegmentHeader {
    /// Whether this is the lifecycle journal rather than a capture segment.
    pub fn is_journal(&self) -> bool {
        self.segment_index == JOURNAL_SEGMENT_INDEX
    }
}

/// Encode a segment header, appending it to `dst`.
pub fn append_segment_header(dst: &mut Vec<u8>, h: &SegmentHeader) {
    let start = dst.len();

    dst.extend_from_slice(&SEGMENT_MAGIC);
    dst.extend_from_slice(&h.format_version.to_le_bytes());
    dst.extend_from_slice(&(SEGMENT_HEADER_SIZE as u16).to_le_bytes());
    dst.extend_from_slice(&h.device_id);
    dst.extend_from_slice(&h.boot_id);
    dst.extend_from_slice(&h.vehicle_id);
    dst.extend_from_slice(&h.assignment_id);
    dst.extend_from_slice(&h.segment_index.to_le_bytes());
    dst.extend_from_slice(&h.first_seq.to_le_bytes());
    dst.extend_from_slice(&h.opened_monotonic_us.to_le_bytes());
    dst.extend_from_slice(&h.storage_key_version.to_le_bytes());
    dst.extend_from_slice(&h.device_counter.to_le_bytes());
    dst.extend_from_slice(&[0u8; OFF_CRC - OFF_RESERVED]); // reserved, zero

    debug_assert_eq!(dst.len() - start, OFF_CRC);
    let computed = crc32(&dst[start..start + OFF_CRC]);
    dst.extend_from_slice(&computed.to_le_bytes());
}

fn u16_at(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}

fn u32_at(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(b[off..off + 4].try_into().unwrap())
}

fn u64_at(b: &[u8], off: usize) -> u64 {
    u64::from_le_bytes(b[off..off + 8].try_into().unwrap())
}

fn id_at(b: &[u8], off: usize) -> [u8; 16] {
    b[off..off + 16].try_into().unwrap()
}

/// Decode and verify a segment header, returning it and the encoded
/// `header_len` — the offset at which frame scanning begins.
pub fn parse_segment_header(b: &[u8]) -> Result<(SegmentHeader, usize)> {
    if b.len() < SEGMENT_HEADER_SIZE {
        return Err(FormatError::ShortHeader);
    }
    if b[OFF_MAGIC..OFF_MAGIC + 4] != SEGMENT_MAGIC {
        return Err(FormatError::BadMagic);
    }

    let stored = u32_at(b, OFF_CRC);
    let computed = crc32(&b[..OFF_CRC]);
    if computed != stored {
        return Err(FormatError::BadHeaderCrc { computed, stored });
    }

    let format_version = u16_at(b, OFF_FORMAT_VERSION);
    let header_len = u16_at(b, OFF_HEADER_LEN) as usize;

    let header = SegmentHeader {
        format_version,
        device_id: id_at(b, OFF_DEVICE_ID),
        boot_id: id_at(b, OFF_BOOT_ID),
        vehicle_id: id_at(b, OFF_VEHICLE_ID),
        assignment_id: id_at(b, OFF_ASSIGNMENT_ID),
        segment_index: u32_at(b, OFF_SEGMENT_INDEX),
        first_seq: u32_at(b, OFF_FIRST_SEQ),
        opened_monotonic_us: u64_at(b, OFF_OPENED_MONOTONIC_US),
        storage_key_version: u32_at(b, OFF_STORAGE_KEY_VERSION),
        device_counter: u64_at(b, OFF_DEVICE_COUNTER),
    };

    if format_version != FORMAT_VERSION {
        return Err(FormatError::UnsupportedFormatVersion(format_version));
    }
    if header_len < SEGMENT_HEADER_SIZE {
        return Err(FormatError::BadHeaderLen(header_len));
    }
    if header_len > b.len() {
        return Err(FormatError::HeaderLenPastEnd {
            header_len,
            size: b.len(),
        });
    }

    Ok((header, header_len))
}

/// Builds a segment, maintaining the two pieces of state that make the format
/// recoverable — the chain-wide sequence number and the CRC chain — and
/// sealing every frame on the way in.
///
/// Both pieces of state run across a whole chain rather than per segment, so a
/// writer for segment *N+1* must be seeded from segment *N*'s final state via
/// [`SegmentWriter::next_state`]. Capture segments form one chain; the journal
/// is a separate chain with its own sequence space (spec §3.2.1).
pub struct SegmentWriter {
    buf: Vec<u8>,
    cipher: SegmentCipher,
    nonces: Box<dyn RngCore>,
    seq: u32,
    prev_crc: u32,
    counts: BTreeMap<RecordType, u32>,
    frames: usize,
    first_seq: u32,
}

impl SegmentWriter {
    /// Start a segment. Pass [`ScanState::default`] for a chain's first
    /// segment; for later segments pass the preceding writer's `next_state`.
    ///
    /// `keys` is required: there is no way to write a plaintext frame, by
    /// design. The segment key is derived from `header` exactly as a reader
    /// will derive it, so `header` must carry the final identity fields —
    /// vehicle, assignment, counter, key version — before the writer is built.
    /// They cannot be amended once frames are sealed against them.
    ///
    /// `nonces` supplies the 24-byte frame nonces. On a device it is the
    /// hardware RNG. Tests and the emulator pass a seeded generator so runs
    /// reproduce, which is safe only because each boot has a distinct
    /// `boot_id` and therefore a distinct key: never feed two writers under the
    /// *same* key the same stream. See [`SegmentWriter::append`] for why nonces
    /// must be random at all.
    pub fn new(
        mut header: SegmentHeader,
        state: ScanState,
        keys: &dyn KeyProvider,
        nonces: Box<dyn RngCore>,
    ) -> Result<Self> {
        header.format_version = FORMAT_VERSION;
        header.first_seq = state.expected_seq;

        let key = keys.segment_key(&header)?;

        let mut buf = Vec::with_capacity(SEGMENT_HEADER_SIZE + 4096);
        append_segment_header(&mut buf, &header);
        let cipher = SegmentCipher::new(&buf, &key)?;

        Ok(Self {
            buf,
            cipher,
            nonces,
            seq: state.expected_seq,
            prev_crc: state.expected_prev,
            counts: BTreeMap::new(),
            frames: 0,
            first_seq: state.expected_seq,
        })
    }

    /// Write one record. `payload` is the plaintext; it is sealed before it
    /// touches the buffer. The sequence number and chain CRC are assigned
    /// automatically, so a caller cannot set them inconsistently.
    ///
    /// **The nonce is random, never derived from `seq`.** After a torn-tail
    /// truncation the same `seq` is legitimately written again with different
    /// plaintext; a seq-derived nonce would then encrypt two plaintexts under
    /// one `(key, nonce)` pair, which leaks their XOR and, with Poly1305, the
    /// authenticator key. A 192-bit random nonce makes a collision negligible
    /// with no state for a power cut to lose.
    pub fn append(
        &mut self,
        record_type: RecordType,
        schema_version: u8,
        flags: u16,
        monotonic_ms: u32,
        payload: &[u8],
    ) -> Result<()> {
        let mut nonce = [0u8; NONCE_SIZE];
        self.nonces
            .try_fill_bytes(&mut nonce)
            .map_err(|e| FormatError::NonceUnavailable(e.to_string()))?;

        let fields = FrameFields {
            record_type,
            schema_version,
            flags,
            seq: self.seq,
            monotonic_ms,
            prev_crc32: self.prev_crc,
        };
        let crc = append_frame(&mut self.buf, &fields, payload, &self.cipher, &nonce)?;

        self.prev_crc = crc;
        self.seq += 1;
        self.frames += 1;
        *self.counts.entry(record_type).or_insert(0) += 1;
        Ok(())
    }

    pub fn bytes(&self) -> &[u8] {
        &self.buf
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.buf
    }

    /// Continuity state for the following segment in the same chain.
    pub fn next_state(&self) -> ScanState {
        ScanState {
            expected_seq: self.seq,
            expected_prev: self.prev_crc,
        }
    }

    /// Accepted record counts by type, suitable for a manifest's
    /// `record_counts`.
    pub fn counts(&self) -> BTreeMap<RecordType, u32> {
        self.counts.clone()
    }

    pub fn frames(&self) -> usize {
        self.frames
    }

    pub fn first_seq(&self) -> u32 {
        self.first_seq
    }

    /// Only meaningful once at least one frame has been written.
    pub fn last_seq(&self) -> u32 {
        self.seq.saturating_sub(1)
    }

    /// The segment's cipher. Exists for tests that must craft deliberately
    /// invalid frames and still seal them correctly, so that exactly one
    /// property of the frame is wrong.
    pub fn cipher(&self) -> &SegmentCipher {
        &self.cipher
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_round_trips_at_the_spec_offsets() {
        let h = SegmentHeader {
            device_id: [0x11; 16],
            boot_id: [0x22; 16],
            vehicle_id: [0x33; 16],
            assignment_id: [0x44; 16],
            segment_index: 0x0102_0304,
            first_seq: 0x0506_0708,
            opened_monotonic_us: 0x090a_0b0c_0d0e_0f10,
            storage_key_version: 0x1112_1314,
            device_counter: 0x1516_1718_191a_1b1c,
            ..SegmentHeader::default()
        };
        let mut b = Vec::new();
        append_segment_header(&mut b, &h);

        assert_eq!(b.len(), 128);
        assert_eq!(&b[0..4], b"CRN3");
        assert_eq!(&b[4..6], &[3, 0]);
        assert_eq!(&b[6..8], &[128, 0]);
        assert_eq!(&b[40..56], &[0x33; 16], "vehicle_id at 40");
        assert_eq!(&b[56..72], &[0x44; 16], "assignment_id at 56");
        assert_eq!(
            &b[88..92],
            &0x1112_1314u32.to_le_bytes(),
            "key version at 88"
        );
        assert_eq!(
            &b[92..100],
            &0x1516_1718_191a_1b1cu64.to_le_bytes(),
            "device_counter at 92"
        );
        assert!(b[100..124].iter().all(|&x| x == 0), "reserved is zero");

        let (parsed, len) = parse_segment_header(&b).unwrap();
        assert_eq!(parsed, h);
        assert_eq!(len, 128);
    }
}
