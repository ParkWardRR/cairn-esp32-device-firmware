//! Segment header and writer.

use std::collections::BTreeMap;

use super::{
    FORMAT_VERSION, FormatError, Result,
    crc::crc32,
    frame::{RecordType, append_frame},
    scan::ScanState,
};

/// Fixed at 64 bytes. The encoded `header_len` lets a reader skip a longer
/// header from a future version without misparsing frames; it does not make a
/// future format readable.
pub const SEGMENT_HEADER_SIZE: usize = 64;

/// ASCII `CRN2`.
pub const SEGMENT_MAGIC: [u8; 4] = *b"CRN2";

/// The fixed header at the start of every segment file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SegmentHeader {
    pub format_version: u16,
    pub device_id: [u8; 16],
    pub boot_id: [u8; 16],
    pub segment_index: u32,
    pub first_seq: u32,
    pub opened_monotonic_us: u64,
}

impl Default for SegmentHeader {
    fn default() -> Self {
        Self {
            format_version: FORMAT_VERSION,
            device_id: [0; 16],
            boot_id: [0; 16],
            segment_index: 0,
            first_seq: 0,
            opened_monotonic_us: 0,
        }
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
    dst.extend_from_slice(&h.segment_index.to_le_bytes());
    dst.extend_from_slice(&h.first_seq.to_le_bytes());
    dst.extend_from_slice(&h.opened_monotonic_us.to_le_bytes());
    dst.extend_from_slice(&0u32.to_le_bytes()); // reserved

    let computed = crc32(&dst[start..start + 60]);
    dst.extend_from_slice(&computed.to_le_bytes());
}

/// Decode and verify a segment header, returning it and the encoded
/// `header_len` — the offset at which frame scanning begins.
pub fn parse_segment_header(b: &[u8]) -> Result<(SegmentHeader, usize)> {
    if b.len() < SEGMENT_HEADER_SIZE {
        return Err(FormatError::ShortHeader);
    }
    if b[0..4] != SEGMENT_MAGIC {
        return Err(FormatError::BadMagic);
    }

    let stored = u32::from_le_bytes([b[60], b[61], b[62], b[63]]);
    let computed = crc32(&b[0..60]);
    if computed != stored {
        return Err(FormatError::BadHeaderCrc { computed, stored });
    }

    let format_version = u16::from_le_bytes([b[4], b[5]]);
    let header_len = u16::from_le_bytes([b[6], b[7]]) as usize;

    let mut device_id = [0u8; 16];
    device_id.copy_from_slice(&b[8..24]);
    let mut boot_id = [0u8; 16];
    boot_id.copy_from_slice(&b[24..40]);

    let header = SegmentHeader {
        format_version,
        device_id,
        boot_id,
        segment_index: u32::from_le_bytes([b[40], b[41], b[42], b[43]]),
        first_seq: u32::from_le_bytes([b[44], b[45], b[46], b[47]]),
        opened_monotonic_us: u64::from_le_bytes([
            b[48], b[49], b[50], b[51], b[52], b[53], b[54], b[55],
        ]),
    };

    if format_version != FORMAT_VERSION {
        return Err(FormatError::UnsupportedFormatVersion(format_version));
    }
    if header_len < SEGMENT_HEADER_SIZE {
        return Err(FormatError::BadHeaderLen(header_len));
    }

    Ok((header, header_len))
}

/// Builds a segment, maintaining the two pieces of state that make the format
/// recoverable: the chain-wide sequence number and the CRC chain.
///
/// Both run across a whole chain rather than per segment, so a writer for
/// segment *N+1* must be seeded from segment *N*'s final state via
/// [`SegmentWriter::next_state`]. Capture segments form one chain; the journal
/// is a separate chain with its own sequence space (spec §3.2.1).
pub struct SegmentWriter {
    buf: Vec<u8>,
    seq: u32,
    prev_crc: u32,
    counts: BTreeMap<RecordType, u32>,
    frames: usize,
    first_seq: u32,
}

impl SegmentWriter {
    /// Start a segment. Pass [`ScanState::default`] for a chain's first
    /// segment; for later segments pass the preceding writer's `next_state`.
    pub fn new(mut header: SegmentHeader, state: ScanState) -> Self {
        header.format_version = FORMAT_VERSION;
        header.first_seq = state.expected_seq;

        let mut buf = Vec::with_capacity(SEGMENT_HEADER_SIZE + 4096);
        append_segment_header(&mut buf, &header);

        Self {
            buf,
            seq: state.expected_seq,
            prev_crc: state.expected_prev,
            counts: BTreeMap::new(),
            frames: 0,
            first_seq: state.expected_seq,
        }
    }

    /// Write one record. The sequence number and chain CRC are assigned
    /// automatically, so a caller cannot set them inconsistently.
    pub fn append(
        &mut self,
        record_type: RecordType,
        schema_version: u8,
        flags: u16,
        monotonic_ms: u32,
        payload: &[u8],
    ) -> Result<()> {
        let crc = append_frame(
            &mut self.buf,
            record_type,
            schema_version,
            flags,
            self.seq,
            monotonic_ms,
            self.prev_crc,
            payload,
        )?;

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
}
