//! Rust implementation of Cairn bundle format v3.
//!
//! The normative specification is `contracts/format/v3/spec.md`. This is a second,
//! independent implementation of it — the Go one under `server/format/` is the
//! reference. Both are checked against the committed vectors in
//! `contracts/format/v3/vectors/`, which is what makes the spec meaningful rather than
//! merely descriptive: if the two disagree about a single byte, the conformance
//! runner fails.
//!
//! v3 replaces v2 outright. Every frame is sealed with XChaCha20-Poly1305 under
//! a per-segment key derived from the device's storage root (§3.6), and every
//! segment and manifest names its vehicle, its assignment and a monotonic device
//! counter (§3.1, §5.1). There is no v2 read path: v2 recordings were test data.
//!
//! The firmware is the third implementation, and it is the one that matters
//! most. Writing this one establishes that the spec can actually be implemented
//! from the document, by someone who is not reading the Go.

// Parts of this module's surface are not yet exercised by the emulator: the
// frame flags, scan_bundle and several re-exports are here because the
// specification defines them and the firmware implementation will need them.
// Keeping the whole format in one place, tested against the vectors, is how it
// stays a single artifact rather than three partial ones.
#![allow(dead_code, unused_imports)]

pub mod aead;
pub mod bind;
pub mod cbor;
pub mod crc;
pub mod frame;
pub mod keys;
pub mod manifest;
pub mod merkle;
pub mod payload;
pub mod policy;
pub mod receipt;
pub mod scan;
pub mod segment;
pub mod update;

#[cfg(test)]
mod tests;

pub use aead::SegmentCipher;
pub use bind::{verify_members_against_manifest, verify_segment_against_manifest};
pub use frame::{
    FRAME_ENVELOPE_SIZE, FRAME_HEADER_SIZE, FRAME_OVERHEAD, Frame, MAX_FRAME_LEN, MAX_PAYLOAD_SIZE,
    MIN_FRAME_LEN, RecordType,
};
pub use keys::{
    AEAD_OVERHEAD, ENCRYPTION_SUITE_V1, KeyProvider, NONCE_SIZE, ROOT_KEY_SIZE, RootKeyProvider,
    SEGMENT_KEY_SIZE, TAG_SIZE, derive_segment_key,
};
pub use manifest::{ChunkDescriptor, Manifest, Member, RecoveryState};
pub use merkle::{content_root, leaf_hash, merkle_root};
pub use receipt::Receipt;
pub use scan::{ScanResult, ScanState, StopReason, scan_bundle, scan_segment};
pub use segment::{
    JOURNAL_SEGMENT_INDEX, SEGMENT_HEADER_SIZE, SEGMENT_MAGIC, SegmentHeader, SegmentWriter,
};

/// The segment format version this implementation handles. There is no v1 or
/// v2 read path.
pub const FORMAT_VERSION: u16 = 3;

/// Errors this implementation can produce.
///
/// The variants deliberately mirror the specification's vocabulary rather than
/// Rust convention, so a conformance failure names the same thing the spec and
/// the Go implementation name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FormatError {
    /// Not a Cairn v3 segment.
    BadMagic,
    /// The segment header checksum failed. The segment is unusable — but must
    /// not be deleted, since the offline salvage tool may still recover records.
    BadHeaderCrc { computed: u32, stored: u32 },
    /// The input is too small to hold a segment header.
    ShortHeader,
    /// A format version this implementation does not read.
    UnsupportedFormatVersion(u16),
    /// A header_len below the fixed minimum.
    BadHeaderLen(usize),
    /// A header_len that claims more bytes than the segment holds.
    HeaderLenPastEnd { header_len: usize, size: usize },
    /// A payload larger than a frame can carry.
    PayloadTooLarge { size: usize, max: usize },
    /// Valid CBOR, but not in the deterministic encoding, so its bytes are not
    /// reproducible.
    NonCanonicalCbor(String),
    /// CBOR input ended mid-item.
    TruncatedCbor,
    /// CBOR using a feature outside the permitted subset.
    UnsupportedCbor(String),
    /// A structural problem in a manifest, receipt, descriptor or payload.
    Malformed(String),
    /// A signature did not verify.
    BadSignature,
    /// The content root does not match the member list it is supposed to cover.
    ContentRootMismatch {
        computed: [u8; 32],
        declared: [u8; 32],
    },
    /// A receipt is validly signed but acknowledges different content.
    ReceiptRootMismatch {
        receipt: [u8; 32],
        uploaded: [u8; 32],
    },

    // ── v3: keys and authentication ─────────────────────────────────────────
    /// The verifier holds no storage root for the segment's device. The
    /// segment can still be scanned structurally, but not decrypted.
    NoKey,
    /// The segment was sealed under a different `storage_key_version` than the
    /// root the verifier holds.
    ///
    /// Kept distinct from [`FormatError::AuthFailed`] on purpose. Deriving with
    /// the wrong root would fail every tag, which is indistinguishable from
    /// tampering; refusing up front says what is actually wrong — a key is
    /// missing, nothing was altered, and the segment must not be deleted.
    KeyVersionMismatch { segment: u32, held: u32 },
    /// A frame's Poly1305 tag did not verify: its ciphertext, its frame
    /// header, or the segment header it is bound to is not what the writer
    /// sealed — or the key is wrong. Never the result of a power cut.
    AuthFailed,
    /// The nonce source failed. A writer must stop rather than fall back to a
    /// predictable nonce, which would be worse than writing nothing.
    NonceUnavailable(String),
    /// A segment header disagrees with the signed manifest about whose data it
    /// is (§5.4). `field` names the first field that differed.
    BindingMismatch {
        member: String,
        field: &'static str,
        detail: String,
    },
}

impl std::fmt::Display for FormatError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::BadMagic => write!(f, "bad segment magic"),
            Self::BadHeaderCrc { computed, stored } => write!(
                f,
                "segment header CRC mismatch: computed {computed:08x}, stored {stored:08x}"
            ),
            Self::ShortHeader => write!(f, "segment shorter than header"),
            Self::UnsupportedFormatVersion(v) => write!(
                f,
                "unsupported format version {v} (this implementation reads {FORMAT_VERSION} only)"
            ),
            Self::BadHeaderLen(n) => {
                write!(
                    f,
                    "header_len {n} is below the minimum {SEGMENT_HEADER_SIZE}"
                )
            }
            Self::HeaderLenPastEnd { header_len, size } => write!(
                f,
                "header_len {header_len} extends past the {size}-byte segment"
            ),
            Self::PayloadTooLarge { size, max } => {
                write!(f, "payload {size} bytes exceeds maximum {max}")
            }
            Self::NonCanonicalCbor(why) => write!(f, "non-canonical CBOR encoding: {why}"),
            Self::TruncatedCbor => write!(f, "truncated CBOR"),
            Self::UnsupportedCbor(why) => write!(f, "unsupported CBOR feature: {why}"),
            Self::Malformed(why) => write!(f, "malformed: {why}"),
            Self::BadSignature => write!(f, "signature verification failed"),
            Self::ContentRootMismatch { computed, declared } => write!(
                f,
                "content root does not match members: computed {}, declared {}",
                hex(computed),
                hex(declared)
            ),
            Self::ReceiptRootMismatch { receipt, uploaded } => write!(
                f,
                "receipt covers {}, uploaded {}",
                hex(receipt),
                hex(uploaded)
            ),
            Self::NoKey => write!(f, "no storage key available for segment"),
            Self::KeyVersionMismatch { segment, held } => write!(
                f,
                "segment storage_key_version {segment} does not match the provider's key \
                 (version {held}): a key is missing, the segment is not tampered with"
            ),
            Self::AuthFailed => write!(f, "frame authentication failed"),
            Self::NonceUnavailable(why) => write!(f, "nonce source failed: {why}"),
            Self::BindingMismatch {
                member,
                field,
                detail,
            } => write!(
                f,
                "segment header disagrees with manifest: member {member:?} {field} {detail}"
            ),
        }
    }
}

impl std::error::Error for FormatError {}

pub type Result<T> = std::result::Result<T, FormatError>;

/// Lowercase hex, used throughout for identifiers and digests.
pub fn hex(bytes: &[u8]) -> String {
    use std::fmt::Write;
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        let _ = write!(out, "{b:02x}");
    }
    out
}

/// Parse lowercase or uppercase hex into a byte vector.
pub fn unhex(s: &str) -> Option<Vec<u8>> {
    let s = s.trim();
    if !s.len().is_multiple_of(2) {
        return None;
    }
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok())
        .collect()
}

/// Parse hex into a fixed-size array.
pub fn unhex_array<const N: usize>(s: &str) -> Option<[u8; N]> {
    let raw = unhex(s)?;
    if raw.len() != N {
        return None;
    }
    let mut out = [0u8; N];
    out.copy_from_slice(&raw);
    Some(out)
}
