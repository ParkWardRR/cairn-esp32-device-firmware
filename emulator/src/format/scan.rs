//! The recovery scan (spec §3.3).
//!
//! This is the algorithm that decides what survives a power cut, so it is
//! specified normatively and implemented identically in all three languages.
//! Its behaviour is stop-at-first-invalid: every frame before the failure point
//! is valid and retained, everything from there to end-of-segment is discarded
//! as an incomplete tail, and both the byte count and the reason are always
//! reported rather than silently swallowed.
//!
//! It runs in two modes. Without a key it is *structural*: torn tails, frame
//! CRCs, the `prev_crc32` chain and the sequence are all computed over the
//! stored ciphertext, so anyone holding the bytes gets the same verdict — the
//! device at boot, intake before it has fetched a root, or a thief. With a key
//! it additionally authenticates every frame, and adds exactly one stop reason:
//! `AUTH_FAILED`. Nothing structural depends on the key.

use std::collections::BTreeMap;

use super::{
    Result,
    aead::SegmentCipher,
    crc::crc32,
    frame::{
        FRAME_HEADER_SIZE, FRAME_TRAILER_SIZE, Frame, MAX_FRAME_LEN, MIN_FRAME_LEN, RecordType,
        decode_frame_header,
    },
    keys::KeyProvider,
    segment::{SegmentHeader, parse_segment_header},
};

/// Why a scan stopped. Every scan ends with exactly one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StopReason {
    /// Clean: the whole segment was consumed and every frame validated.
    Eof,
    /// The segment ends mid-frame — the expected outcome of a power cut during
    /// a write.
    TornTail,
    /// A frame's CRC did not match. Corruption is isolated to that frame.
    CorruptFrame,
    /// A frame's `prev_crc32` did not match its predecessor's CRC, indicating a
    /// record was removed, reordered or spliced.
    ChainBreak,
    /// The sequence number skipped a value.
    SeqGap,
    /// Keyed scans only. CRC, chain and sequence all held but the
    /// authentication tag did not: tampering with a repaired CRC, a frame moved
    /// from another segment, or the wrong key. Never the result of a power cut,
    /// which is why it is distinct from `CorruptFrame` and always worth an
    /// operator's attention. Like every other stop it is never skipped past: a
    /// frame whose authenticity is in doubt makes everything after it equally
    /// doubtful.
    AuthFailed,
}

impl StopReason {
    /// The spec's name, used in conformance expectations.
    pub fn name(self) -> &'static str {
        match self {
            Self::Eof => "EOF",
            Self::TornTail => "TORN_TAIL",
            Self::CorruptFrame => "CORRUPT_FRAME",
            Self::ChainBreak => "CHAIN_BREAK",
            Self::SeqGap => "SEQ_GAP",
            Self::AuthFailed => "AUTH_FAILED",
        }
    }

    /// Whether the scan found no damage.
    pub fn clean(self) -> bool {
        matches!(self, Self::Eof)
    }
}

impl std::fmt::Display for StopReason {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.name())
    }
}

/// Continuity across a segment boundary.
///
/// Sequence numbers and the CRC chain both run across a whole chain, not per
/// segment, so scanning segment *N+1* requires the result of segment *N*.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ScanState {
    pub expected_seq: u32,
    pub expected_prev: u32,
}

/// The outcome of scanning one segment.
#[derive(Debug, Clone)]
pub struct ScanResult {
    pub header: SegmentHeader,

    /// The valid frames, in file order. Every frame here passed its CRC, chain
    /// and sequence checks, and — when `decrypted` — its authentication tag.
    pub frames: Vec<Frame>,

    /// Whether the scan was keyed: every retained frame authenticated and
    /// carries its plaintext. `false` means a structural scan, whose verdicts
    /// are real but say nothing about authenticity.
    pub decrypted: bool,

    pub stop: StopReason,

    /// A human-readable explanation, suitable for an operator or a ledger
    /// reason code.
    pub stop_detail: String,

    /// The byte offset at which scanning stopped.
    pub stop_offset: usize,

    /// How many bytes from `stop_offset` to end-of-segment were discarded.
    /// Reported exactly, never rounded.
    pub discarded_tail_bytes: u32,

    /// Continuity for the following segment.
    pub next: ScanState,

    /// Frames whose record type this implementation does not understand. These
    /// were integrity-checked and skipped, not rejected.
    pub unknown_type_count: usize,

    /// Accepted frames tallied by record type.
    pub record_counts: BTreeMap<RecordType, usize>,
}

/// Walk a segment's frames per the specification's recovery algorithm.
///
/// Pass [`ScanState::default`] for a chain's first segment; for later segments
/// pass the preceding segment's `next`.
///
/// `keys` selects the mode: `None` for the structural scan, a provider for the
/// keyed one.
///
/// A header error is returned as `Err`: the segment is unusable. So is a key
/// error ([`super::FormatError::NoKey`],
/// [`super::FormatError::KeyVersionMismatch`]): the segment is intact but cannot
/// be read with what the caller holds. The caller must still delete neither —
/// unreadable is not the same as worthless, and the offline salvage tool, or a
/// caller with the right key, may yet recover records from it.
pub fn scan_segment(
    b: &[u8],
    state: ScanState,
    keys: Option<&dyn KeyProvider>,
) -> Result<ScanResult> {
    let (header, header_len) = parse_segment_header(b)?;

    // The key comes from the header, before any frame is read, so a version
    // mismatch is a refusal of the whole segment rather than a tag failure on
    // its first frame.
    let cipher = match keys {
        None => None,
        Some(k) => {
            let key = k.segment_key(&header)?;
            Some(SegmentCipher::new(&b[..header_len], &key)?)
        }
    };

    let mut res = ScanResult {
        header: header.clone(),
        frames: Vec::new(),
        decrypted: cipher.is_some(),
        stop: StopReason::Eof,
        stop_detail: String::new(),
        stop_offset: 0,
        discarded_tail_bytes: 0,
        next: ScanState::default(),
        unknown_type_count: 0,
        record_counts: BTreeMap::new(),
    };

    // For a chain's first segment the caller has no prior state, so the
    // header's own first_seq establishes the expectation. The journal is the
    // first and only segment of its own chain.
    let mut expected_seq = state.expected_seq;
    if (header.segment_index == 0 || header.is_journal())
        && state.expected_seq == 0
        && state.expected_prev == 0
    {
        expected_seq = header.first_seq;
    }
    let mut expected_prev = state.expected_prev;

    let mut offset = header_len;

    macro_rules! stop {
        ($reason:expr, $detail:expr) => {{
            res.stop = $reason;
            res.stop_detail = $detail;
            res.stop_offset = offset;
            res.discarded_tail_bytes = (b.len() - offset) as u32;
            res.next = ScanState {
                expected_seq,
                expected_prev,
            };
            return Ok(res);
        }};
    }

    loop {
        let remaining = b.len() - offset;
        if remaining == 0 {
            stop!(StopReason::Eof, "segment fully consumed".to_string());
        }
        if remaining < MIN_FRAME_LEN {
            stop!(
                StopReason::TornTail,
                format!(
                    "{remaining} trailing bytes cannot hold a frame envelope of {MIN_FRAME_LEN}"
                )
            );
        }

        let frame_len = u16::from_le_bytes([b[offset], b[offset + 1]]) as usize;
        // Written as two explicit comparisons rather than a range check,
        // because that is how the specification states the rule and this
        // function is meant to be read side by side with it.
        #[allow(clippy::manual_range_contains)]
        if frame_len < MIN_FRAME_LEN || frame_len > MAX_FRAME_LEN {
            stop!(
                StopReason::TornTail,
                format!(
                    "frame_len {frame_len} outside the valid range [{MIN_FRAME_LEN}, {MAX_FRAME_LEN}]"
                )
            );
        }
        if offset + frame_len > b.len() {
            stop!(
                StopReason::TornTail,
                format!(
                    "frame_len {} at offset {} extends {} bytes past end of segment",
                    frame_len,
                    offset,
                    offset + frame_len - b.len()
                )
            );
        }

        let body = &b[offset..offset + frame_len];
        let stored_crc = u32::from_le_bytes(body[frame_len - 4..].try_into().unwrap());
        let computed = crc32(&body[..frame_len - FRAME_TRAILER_SIZE]);
        if computed != stored_crc {
            stop!(
                StopReason::CorruptFrame,
                format!(
                    "frame at offset {offset}: computed CRC {computed:08x}, stored {stored_crc:08x}"
                )
            );
        }

        let mut f = decode_frame_header(body);
        f.crc32 = stored_crc;
        f.sealed = body[FRAME_HEADER_SIZE..frame_len - FRAME_TRAILER_SIZE].to_vec();

        if f.prev_crc32 != expected_prev {
            stop!(
                StopReason::ChainBreak,
                format!(
                    "frame at offset {} (seq {}): prev_crc32 {:08x}, expected {:08x} \
                     — a record was removed, reordered or spliced",
                    offset, f.seq, f.prev_crc32, expected_prev
                )
            );
        }
        if f.seq != expected_seq {
            stop!(
                StopReason::SeqGap,
                format!(
                    "frame at offset {}: seq {}, expected {}",
                    offset, f.seq, expected_seq
                )
            );
        }

        // Authentication comes after the structural checks, not before: those
        // need no key and must give the same verdict whether or not one is
        // held, so a damaged frame reports as damage rather than as an auth
        // failure.
        if let Some(c) = &cipher {
            match c.open(&body[..FRAME_HEADER_SIZE], &f.sealed) {
                Ok(pt) => f.payload = Some(pt),
                Err(e) => stop!(
                    StopReason::AuthFailed,
                    format!(
                        "frame at offset {} (seq {}): {e} — the ciphertext, frame header or \
                         segment binding was altered, or the key is wrong",
                        offset, f.seq
                    )
                ),
            }
        }

        if !f.record_type.known() {
            res.unknown_type_count += 1;
        }
        *res.record_counts.entry(f.record_type).or_insert(0) += 1;

        expected_seq = f.seq.wrapping_add(1);
        expected_prev = stored_crc;
        offset += frame_len;

        res.frames.push(f);
    }
}

/// Scan an ordered list of segments in one chain, threading continuity between
/// them. Segments must be supplied in ascending `segment_index` order. `keys`
/// is as for [`scan_segment`].
///
/// Scanning stops at the first segment that does not end cleanly: a damaged
/// segment makes every later segment's chain expectation unknowable, so
/// continuing would produce misleading verdicts rather than more data.
pub fn scan_bundle(
    segments: &[Vec<u8>],
    keys: Option<&dyn KeyProvider>,
) -> Result<Vec<ScanResult>> {
    let mut results = Vec::with_capacity(segments.len());
    let mut state = ScanState::default();

    for seg in segments {
        let res = scan_segment(seg, state, keys)?;
        let clean = res.stop.clean();
        state = res.next;
        results.push(res);
        if !clean {
            break;
        }
    }

    Ok(results)
}
