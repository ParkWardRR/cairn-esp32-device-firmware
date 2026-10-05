//! Storage key hierarchy (spec §3.6).
//!
//! ```text
//! K_root  32 bytes, per device, versioned by storage_key_version
//!   └─ K_seg = HKDF-SHA256( ikm  = K_root,
//!                           salt = vehicle_id,
//!                           info = "cairn/segment/v3" ‖ device_id ‖ assignment_id
//!                                  ‖ boot_id ‖ segment_index_u32le,
//!                           L    = 32 )
//! ```
//!
//! The segment header — not the caller — says which key applies. A verifier
//! hands the parsed header to a [`KeyProvider`] and gets back the key for that
//! segment or a reason it cannot have one, so there is no API through which a
//! caller could pick a key for a segment the header does not describe.

use hkdf::Hkdf;
use sha2::Sha256;

use super::{FormatError, Result, segment::SegmentHeader};

/// The device storage root `K_root`.
pub const ROOT_KEY_SIZE: usize = 32;
/// A per-segment key `K_seg`.
pub const SEGMENT_KEY_SIZE: usize = 32;
/// The XChaCha20-Poly1305 nonce carried in every frame.
pub const NONCE_SIZE: usize = 24;
/// The Poly1305 authentication tag.
pub const TAG_SIZE: usize = 16;
/// What sealing adds to a payload: nonce plus tag.
///
/// A constant rather than a value read from the cipher, so the frame geometry
/// in `frame.rs` is a compile-time fact: the maximum payload is derived from it,
/// and a disagreement with the AEAD implementation is caught by a test rather
/// than discovered on a card.
pub const AEAD_OVERHEAD: usize = NONCE_SIZE + TAG_SIZE; // 40

/// The AEAD and KDF this format version uses, as named in the signed manifest
/// (key 28). Recorded so a future suite is an explicit, detectable change rather
/// than something inferred from a key length.
pub const ENCRYPTION_SUITE_V1: &str = "xchacha20poly1305+hkdf-sha256/v1";

/// The fixed prefix of the HKDF info string. It binds the derived key to this
/// purpose: the same root is never used for anything else today, and the label
/// keeps that true if a later feature derives other keys from it.
pub const HKDF_INFO_LABEL: &[u8] = b"cairn/segment/v3";

/// Compute `K_seg` for a segment header.
///
/// Every identifier is mixed in, so a key is useless anywhere but the one
/// segment it was derived for. The vehicle id is the *salt* rather than part of
/// `info` because it is the identifier that must never be shared between keys
/// for different vehicles: a thief who obtains one vehicle's derived keys learns
/// nothing about another's, even under the same device root.
///
/// `storage_key_version` and `device_counter` are deliberately not inputs. The
/// version selects which root to use, and the counter is bound through the AAD
/// instead (see [`super::aead::SegmentCipher`]); keeping both out of the KDF
/// means advancing a counter never changes a key.
pub fn derive_segment_key(root: &[u8; ROOT_KEY_SIZE], h: &SegmentHeader) -> [u8; SEGMENT_KEY_SIZE] {
    let mut info = Vec::with_capacity(HKDF_INFO_LABEL.len() + 16 * 3 + 4);
    info.extend_from_slice(HKDF_INFO_LABEL);
    info.extend_from_slice(&h.device_id);
    info.extend_from_slice(&h.assignment_id);
    info.extend_from_slice(&h.boot_id);
    info.extend_from_slice(&h.segment_index.to_le_bytes());

    let hk = Hkdf::<Sha256>::new(Some(&h.vehicle_id), root);
    let mut out = [0u8; SEGMENT_KEY_SIZE];
    // 32 bytes is far below HKDF-SHA256's 255 × 32 ceiling, so expand cannot
    // fail; an error here would mean the hkdf crate changed its contract.
    hk.expand(&info, &mut out)
        .expect("HKDF-SHA256 expand of 32 bytes cannot exceed the output limit");
    out
}

/// Supplies the per-segment key for a segment header.
///
/// `None` wherever an `Option<&dyn KeyProvider>` is accepted is meaningful: it
/// requests a structural scan only. CRC, chain, sequence, Merkle and chunking
/// all work without a key because they are computed over ciphertext.
pub trait KeyProvider {
    fn segment_key(&self, h: &SegmentHeader) -> Result<[u8; SEGMENT_KEY_SIZE]>;
}

/// Derives segment keys from one device storage root.
///
/// This is what a device uses for its own segments and what a tool uses when
/// given one device's escrowed root. Like the Go reference it checks only the
/// version: a device that reads a card from another device derives *a* key —
/// the wrong one — and every frame then fails its tag, which is the honest
/// verdict for frames this root did not seal.
#[derive(Clone)]
pub struct RootKeyProvider {
    pub root: [u8; ROOT_KEY_SIZE],
    /// The `storage_key_version` this root is.
    pub version: u32,
}

impl RootKeyProvider {
    pub fn new(root: [u8; ROOT_KEY_SIZE], version: u32) -> Self {
        Self { root, version }
    }
}

// Never print key material, including in a panic message or a debug dump of a
// struct that happens to hold a provider.
impl std::fmt::Debug for RootKeyProvider {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RootKeyProvider")
            .field("root", &"<redacted>")
            .field("version", &self.version)
            .finish()
    }
}

impl KeyProvider for RootKeyProvider {
    fn segment_key(&self, h: &SegmentHeader) -> Result<[u8; SEGMENT_KEY_SIZE]> {
        // Refuse before deriving anything. A version-1 key applied to a
        // version-2 segment fails every tag, and an operator would be told
        // "tampered" when the truth is "key missing".
        if h.storage_key_version != self.version {
            return Err(FormatError::KeyVersionMismatch {
                segment: h.storage_key_version,
                held: self.version,
            });
        }
        Ok(derive_segment_key(&self.root, h))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::format::{segment::JOURNAL_SEGMENT_INDEX, unhex_array};

    fn ids(first: u8) -> [u8; 16] {
        let mut id = [0u8; 16];
        for (i, b) in id.iter_mut().enumerate() {
            *b = first + i as u8;
        }
        id
    }

    /// The vector identities from fixtures/format-v3/keys.json.
    fn vector_header(segment_index: u32) -> SegmentHeader {
        SegmentHeader {
            device_id: ids(0x10),
            boot_id: ids(0xa0),
            vehicle_id: ids(0x30),
            assignment_id: ids(0x50),
            segment_index,
            storage_key_version: 1,
            device_counter: 42,
            ..SegmentHeader::default()
        }
    }

    /// A named edit to one header field.
    type HeaderEdit = (&'static str, fn(&mut SegmentHeader));

    fn vector_root() -> [u8; 32] {
        unhex_array("7b1a2c5cc080bc43df4922975a48987d92e3209ba6a4bb8853a23651ad9b3821").unwrap()
    }

    /// keys.json's worked example. Pinned here as well as in the conformance
    /// run so a KDF regression is reported as that, not as a puzzling tag
    /// failure three layers up.
    #[test]
    fn matches_worked_example() {
        let root = vector_root();
        assert_eq!(
            crate::format::hex(&derive_segment_key(&root, &vector_header(0))),
            "24f7191a4156d6ac88a626e1922ecf5ce32157734499a3055b9d8b4ebfcae979"
        );
        assert_eq!(
            crate::format::hex(&derive_segment_key(
                &root,
                &vector_header(JOURNAL_SEGMENT_INDEX)
            )),
            "b49dff5df69fbb18337332ed516aa2fe2aa008e01e004dcd45b082a70e03667c"
        );
    }

    /// Every identity input changes the key; the counter and the key version
    /// do not (they are bound by the AAD and by root selection instead).
    #[test]
    fn every_identity_is_an_input_and_nothing_else_is() {
        let root = vector_root();
        let base = derive_segment_key(&root, &vector_header(0));

        let mutations: [HeaderEdit; 5] = [
            ("device_id", |h| h.device_id[0] ^= 1),
            ("boot_id", |h| h.boot_id[0] ^= 1),
            ("vehicle_id", |h| h.vehicle_id[0] ^= 1),
            ("assignment_id", |h| h.assignment_id[0] ^= 1),
            ("segment_index", |h| h.segment_index = 1),
        ];
        for (name, mutate) in mutations {
            let mut h = vector_header(0);
            mutate(&mut h);
            assert_ne!(
                derive_segment_key(&root, &h),
                base,
                "{name} is not an input to the segment key"
            );
        }

        let mut h = vector_header(0);
        h.device_counter = 43;
        h.storage_key_version = 9;
        h.first_seq = 100;
        h.opened_monotonic_us = 5;
        assert_eq!(
            derive_segment_key(&root, &h),
            base,
            "counter, version, first_seq or open time leaked into the KDF"
        );
    }

    #[test]
    fn provider_refuses_other_versions_before_deriving() {
        let p = RootKeyProvider::new(vector_root(), 1);
        let mut h = vector_header(0);
        h.storage_key_version = 2;
        assert_eq!(
            p.segment_key(&h),
            Err(FormatError::KeyVersionMismatch {
                segment: 2,
                held: 1
            })
        );
    }

    #[test]
    fn debug_never_prints_the_root() {
        let p = RootKeyProvider::new(vector_root(), 1);
        let shown = format!("{p:?}");
        assert!(!shown.contains("7b1a2c"), "root key leaked: {shown}");
        assert!(!shown.contains("123"), "root key bytes leaked: {shown}");
    }
}
