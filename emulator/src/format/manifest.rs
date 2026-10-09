//! The immutable, signed description of a sealed bundle.

use std::collections::BTreeMap;

use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
use sha2::{Digest, Sha256};

use super::{
    FormatError, Result,
    cbor::{Decoder, Encoder},
    frame::RecordType,
    keys::ENCRYPTION_SUITE_V1,
    merkle::{content_root, sort_members},
};

pub use super::merkle::Member;

/// The manifest schema version this implementation handles.
pub const MANIFEST_VERSION: u8 = 3;

/// The only algorithm v3 defines. "SHA-256 sign" conflates a digest with a
/// signature; these are separate operations and the manifest names the
/// signature algorithm explicitly.
pub const SIGNATURE_ALGORITHM_ED25519: &str = "ed25519";

/// How the bundle's data came to be, so a consumer can tell clean capture from
/// recovered capture without re-deriving it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecoveryState {
    /// Every segment scanned to EOF with no damage.
    Clean,
    /// An incomplete tail was discarded at boot. The normal outcome of a power
    /// cut, and not a defect.
    RecoveredTail,
    /// The offline tool resynchronised past a damaged region. Salvaged bundles
    /// are ineligible for the normal upload path.
    Salvaged,
}

impl RecoveryState {
    pub fn as_u8(self) -> u8 {
        match self {
            Self::Clean => 0,
            Self::RecoveredTail => 1,
            Self::Salvaged => 2,
        }
    }

    pub fn from_u8(v: u8) -> Result<Self> {
        match v {
            0 => Ok(Self::Clean),
            1 => Ok(Self::RecoveredTail),
            2 => Ok(Self::Salvaged),
            other => Err(FormatError::Malformed(format!(
                "recovery_state {other} is not defined"
            ))),
        }
    }
}

/// One transfer chunk. Chunks are a transport concern: they are addressed by
/// hash during upload so re-chunking and deduplication both work, and they carry
/// no identity of their own.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChunkDescriptor {
    pub index: u32,
    pub byte_length: u32,
    pub sha256: [u8; 32],
}

/// Which engine profile produced a bundle's numbers (manifest key 29, §5.1.1).
///
/// The profile decides the multi-PID request, every value conversion, the
/// hot/cold tiering, the polling cadences and the engine-on thresholds, so a
/// bundle that does not name it leaves a reader unable to say which formula a
/// figure came through.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EngineProfileRef {
    pub id: String,
    pub version: u8,
    /// SHA-256 of the profile document as `contracts/engine/v1` defines it —
    /// the same value the generator reports.
    pub sha256: [u8; 32],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Manifest {
    pub manifest_version: u8,
    /// ULID: the operational handle for retries, receipts and support.
    pub bundle_id: [u8; 16],
    pub device_id: [u8; 16],
    pub device_key_id: [u8; 8],
    pub boot_id: [u8; 16],
    pub firmware_version: String,
    pub schema_version: u8,
    pub capture_started_monotonic_us: u64,
    pub capture_ended_monotonic_us: u64,

    /// The basis GNSS sample UTC deltas are relative to, with its own
    /// uncertainty. UTC is an annotation here, not an ordering key — ordering
    /// truth is `(boot_id, seq)`.
    pub utc_basis_ms: u64,
    pub utc_basis_acc_ms: u32,

    pub first_seq: u32,
    pub last_seq: u32,

    pub record_counts: BTreeMap<RecordType, u32>,
    pub members: Vec<Member>,
    pub chunk_descriptors: Vec<ChunkDescriptor>,

    pub content_root: [u8; 32],

    /// Chains bundle history. Populated but not enforced in v3: detecting
    /// deleted historical bundles is a different threat model from detecting
    /// corruption within one bundle.
    pub previous_bundle_root: Option<[u8; 32]>,

    pub policy_version: u8,
    pub recovery_state: RecoveryState,
    pub discarded_tail_bytes: u32,
    pub signature_algorithm: String,

    /// Ties the capture to the sequence of drives, not just the boot. Optional:
    /// when absent, key 23 is omitted from the encoding entirely rather than
    /// written as null.
    pub trip_seq: Option<u32>,

    /// The engine profile that produced this bundle's numbers: its id, its
    /// version, and the SHA-256 of the profile document (§5.1.1). Optional and
    /// independent of [`Self::trip_seq`], so a manifest has 27 to 29 fields.
    ///
    /// The **digest, not just the name**, because a profile edited without a
    /// version bump is a different profile and only the digest says so. A reader
    /// must not require this key: a bundle from a device with no profile, or one
    /// built before the key existed, is still valid and still signed over
    /// whatever keys are present.
    pub engine_profile: Option<EngineProfileRef>,

    // ── v3 binding (keys 24–28, mandatory) ──────────────────────────────────
    // The first four must each equal the corresponding field of every segment
    // header in the bundle, journal included (§5.4). The signature makes them
    // the device's claim; the binding check makes the segments agree with it.
    /// The vehicle the bundle was captured in.
    pub vehicle_id: [u8; 16],
    /// The device→vehicle assignment it was captured under.
    pub assignment_id: [u8; 16],
    /// The device's monotonic bundle counter. Starts at 1. Kept in NVS off the
    /// card, so a restored card cannot rewind it.
    pub device_counter: u64,
    /// Which escrowed `K_root` the segments were sealed under.
    pub storage_key_version: u32,
    /// The AEAD and KDF, [`ENCRYPTION_SUITE_V1`]. Named in the signed bytes so
    /// a future suite is an explicit, detectable change.
    pub encryption_suite: String,
}

// Manifest CBOR keys. Integer keys keep the encoding compact and unambiguous.
const KEY_MANIFEST_VERSION: u64 = 1;
const KEY_BUNDLE_ID: u64 = 2;
const KEY_DEVICE_ID: u64 = 3;
const KEY_DEVICE_KEY_ID: u64 = 4;
const KEY_BOOT_ID: u64 = 5;
const KEY_FIRMWARE_VERSION: u64 = 6;
const KEY_SCHEMA_VERSION: u64 = 7;
const KEY_CAPTURE_STARTED: u64 = 8;
const KEY_CAPTURE_ENDED: u64 = 9;
const KEY_UTC_BASIS_MS: u64 = 10;
const KEY_UTC_BASIS_ACC_MS: u64 = 11;
const KEY_FIRST_SEQ: u64 = 12;
const KEY_LAST_SEQ: u64 = 13;
const KEY_RECORD_COUNTS: u64 = 14;
const KEY_MEMBERS: u64 = 15;
const KEY_CHUNK_DESCS: u64 = 16;
const KEY_CONTENT_ROOT: u64 = 17;
const KEY_PREVIOUS_ROOT: u64 = 18;
const KEY_POLICY_VERSION: u64 = 19;
const KEY_RECOVERY_STATE: u64 = 20;
const KEY_DISCARDED_TAIL: u64 = 21;
const KEY_SIGNATURE_ALGO: u64 = 22;
const KEY_TRIP_SEQ: u64 = 23; // optional
// Keys 24-28 are mandatory. They sit after the optional trip_seq so the encoder
// emits keys in ascending order with a single conditional.
const KEY_VEHICLE_ID: u64 = 24;
const KEY_ASSIGNMENT_ID: u64 = 25;
const KEY_DEVICE_COUNTER: u64 = 26;
const KEY_STORAGE_KEY_VERSION: u64 = 27;
const KEY_ENCRYPTION_SUITE: u64 = 28;
const KEY_ENGINE_PROFILE: u64 = 29; // optional

/// Keys 23 and 29 are independently optional, so every count in this range is
/// well-formed. Which keys are actually present is checked per key.
const MANIFEST_FIELD_COUNT_BASE: usize = 27;
const MANIFEST_FIELD_COUNT_MAX: usize = 29;

/// Derive the 8-byte key identifier from a public key: truncated SHA-256.
pub fn device_key_id(pub_key: &VerifyingKey) -> [u8; 8] {
    let digest = Sha256::digest(pub_key.as_bytes());
    let mut id = [0u8; 8];
    id.copy_from_slice(&digest[..8]);
    id
}

impl Manifest {
    /// Encode in deterministic CBOR.
    ///
    /// The result is exactly the bytes of `manifest.cbor` and exactly the bytes
    /// the signature covers, so there is nothing to strip or re-encode before
    /// verifying.
    pub fn to_cbor(&self) -> Result<Vec<u8>> {
        if self.signature_algorithm != SIGNATURE_ALGORITHM_ED25519 {
            return Err(FormatError::Malformed(format!(
                "unsupported signature algorithm {}",
                self.signature_algorithm
            )));
        }

        if self.encryption_suite != ENCRYPTION_SUITE_V1 {
            return Err(FormatError::Malformed(format!(
                "unsupported encryption suite {:?}",
                self.encryption_suite
            )));
        }

        let mut e = Encoder::new();
        e.map_header(
            MANIFEST_FIELD_COUNT_BASE
                + usize::from(self.trip_seq.is_some())
                + usize::from(self.engine_profile.is_some()),
        );

        e.key(KEY_MANIFEST_VERSION);
        e.uint(self.manifest_version as u64);
        e.key(KEY_BUNDLE_ID);
        e.bytes(&self.bundle_id);
        e.key(KEY_DEVICE_ID);
        e.bytes(&self.device_id);
        e.key(KEY_DEVICE_KEY_ID);
        e.bytes(&self.device_key_id);
        e.key(KEY_BOOT_ID);
        e.bytes(&self.boot_id);
        e.key(KEY_FIRMWARE_VERSION);
        e.text(&self.firmware_version);
        e.key(KEY_SCHEMA_VERSION);
        e.uint(self.schema_version as u64);
        e.key(KEY_CAPTURE_STARTED);
        e.uint(self.capture_started_monotonic_us);
        e.key(KEY_CAPTURE_ENDED);
        e.uint(self.capture_ended_monotonic_us);
        e.key(KEY_UTC_BASIS_MS);
        e.uint(self.utc_basis_ms);
        e.key(KEY_UTC_BASIS_ACC_MS);
        e.uint(self.utc_basis_acc_ms as u64);
        e.key(KEY_FIRST_SEQ);
        e.uint(self.first_seq as u64);
        e.key(KEY_LAST_SEQ);
        e.uint(self.last_seq as u64);

        // Record counts: a nested map, keys ascending by record type. BTreeMap
        // already orders them, which is why it is the chosen container.
        e.key(KEY_RECORD_COUNTS);
        let mut inner = Encoder::new();
        inner.map_header(self.record_counts.len());
        for (rt, count) in &self.record_counts {
            inner.key(rt.0 as u64);
            inner.uint(*count as u64);
        }
        e.raw(&inner.into_bytes());

        // Members: sorted by raw name bytes, as the content root requires.
        e.key(KEY_MEMBERS);
        let mut members = self.members.clone();
        sort_members(&mut members);
        e.array_header(members.len());
        for m in &members {
            e.array_header(3);
            e.text(&m.name);
            e.uint(m.length);
            e.bytes(&m.sha256);
        }

        e.key(KEY_CHUNK_DESCS);
        e.array_header(self.chunk_descriptors.len());
        for c in &self.chunk_descriptors {
            e.array_header(3);
            e.uint(c.index as u64);
            e.uint(c.byte_length as u64);
            e.bytes(&c.sha256);
        }

        e.key(KEY_CONTENT_ROOT);
        e.bytes(&self.content_root);

        e.key(KEY_PREVIOUS_ROOT);
        match &self.previous_bundle_root {
            None => e.null(),
            Some(root) => e.bytes(root),
        }

        e.key(KEY_POLICY_VERSION);
        e.uint(self.policy_version as u64);
        e.key(KEY_RECOVERY_STATE);
        e.uint(self.recovery_state.as_u8() as u64);
        e.key(KEY_DISCARDED_TAIL);
        e.uint(self.discarded_tail_bytes as u64);
        e.key(KEY_SIGNATURE_ALGO);
        e.text(&self.signature_algorithm);

        if let Some(t) = self.trip_seq {
            e.key(KEY_TRIP_SEQ);
            e.uint(t as u64);
        }

        e.key(KEY_VEHICLE_ID);
        e.bytes(&self.vehicle_id);
        e.key(KEY_ASSIGNMENT_ID);
        e.bytes(&self.assignment_id);
        e.key(KEY_DEVICE_COUNTER);
        e.uint(self.device_counter);
        e.key(KEY_STORAGE_KEY_VERSION);
        e.uint(self.storage_key_version as u64);
        e.key(KEY_ENCRYPTION_SUITE);
        e.text(&self.encryption_suite);

        if let Some(p) = &self.engine_profile {
            e.key(KEY_ENGINE_PROFILE);
            e.array_header(3);
            e.text(&p.id);
            e.uint(p.version as u64);
            e.bytes(&p.sha256);
        }

        Ok(e.into_bytes())
    }

    /// Decode a manifest and verify that it is canonically encoded.
    ///
    /// Canonicality is checked by re-encoding and requiring byte equality with
    /// the input. This is deliberately strict: a manifest whose bytes we would
    /// not ourselves have produced cannot be safely re-serialized, and any
    /// discrepancy would silently break signature verification.
    pub fn from_cbor(b: &[u8]) -> Result<Self> {
        let m = Self::decode(b)?;

        let reencoded = m.to_cbor()?;
        if reencoded != b {
            return Err(FormatError::NonCanonicalCbor(
                "manifest does not round-trip to identical bytes".into(),
            ));
        }

        Ok(m)
    }

    fn decode(b: &[u8]) -> Result<Self> {
        let mut d = Decoder::new(b);

        let n = d.map_header()?;
        // A range rather than a pair of totals: trip_seq and engine_profile are
        // independently optional, so 27, 28 and 29 are all well-formed.
        if !(MANIFEST_FIELD_COUNT_BASE..=MANIFEST_FIELD_COUNT_MAX).contains(&n) {
            return Err(FormatError::Malformed(format!(
                "manifest has {n} fields, expected {MANIFEST_FIELD_COUNT_BASE} to {MANIFEST_FIELD_COUNT_MAX}"
            )));
        }

        let mut m = Manifest {
            manifest_version: 0,
            bundle_id: [0; 16],
            device_id: [0; 16],
            device_key_id: [0; 8],
            boot_id: [0; 16],
            firmware_version: String::new(),
            schema_version: 0,
            capture_started_monotonic_us: 0,
            capture_ended_monotonic_us: 0,
            utc_basis_ms: 0,
            utc_basis_acc_ms: 0,
            first_seq: 0,
            last_seq: 0,
            record_counts: BTreeMap::new(),
            members: Vec::new(),
            chunk_descriptors: Vec::new(),
            content_root: [0; 32],
            previous_bundle_root: None,
            policy_version: 0,
            recovery_state: RecoveryState::Clean,
            discarded_tail_bytes: 0,
            signature_algorithm: String::new(),
            trip_seq: None,
            engine_profile: None,
            vehicle_id: [0; 16],
            assignment_id: [0; 16],
            device_counter: 0,
            storage_key_version: 0,
            encryption_suite: String::new(),
        };

        let mut seen: u64 = 0;
        for i in 0..n {
            let key = d
                .uint()
                .map_err(|e| FormatError::Malformed(format!("manifest key {i}: {e}")))?;

            // Duplicates are refused here rather than left to the round-trip
            // check, so the error names the problem instead of reporting a
            // generic non-canonical encoding.
            if (KEY_MANIFEST_VERSION..=KEY_ENCRYPTION_SUITE).contains(&key) {
                if seen & (1 << key) != 0 {
                    return Err(FormatError::Malformed(format!(
                        "manifest key {key} appears twice"
                    )));
                }
                seen |= 1 << key;
            }

            match key {
                KEY_MANIFEST_VERSION => {
                    m.manifest_version = d.u8()?;
                    if m.manifest_version != MANIFEST_VERSION {
                        return Err(FormatError::Malformed(format!(
                            "unsupported manifest_version {} (this implementation reads {MANIFEST_VERSION} only)",
                            m.manifest_version
                        )));
                    }
                }
                KEY_BUNDLE_ID => m.bundle_id = d.bytes_n()?,
                KEY_DEVICE_ID => m.device_id = d.bytes_n()?,
                KEY_DEVICE_KEY_ID => m.device_key_id = d.bytes_n()?,
                KEY_BOOT_ID => m.boot_id = d.bytes_n()?,
                KEY_FIRMWARE_VERSION => m.firmware_version = d.text()?,
                KEY_SCHEMA_VERSION => m.schema_version = d.u8()?,
                KEY_CAPTURE_STARTED => m.capture_started_monotonic_us = d.uint()?,
                KEY_CAPTURE_ENDED => m.capture_ended_monotonic_us = d.uint()?,
                KEY_UTC_BASIS_MS => m.utc_basis_ms = d.uint()?,
                KEY_UTC_BASIS_ACC_MS => m.utc_basis_acc_ms = d.u32()?,
                KEY_FIRST_SEQ => m.first_seq = d.u32()?,
                KEY_LAST_SEQ => m.last_seq = d.u32()?,
                KEY_RECORD_COUNTS => {
                    let count = d.map_header()?;
                    let mut prev: i32 = -1;
                    for _ in 0..count {
                        let rt = d.u8()?;
                        if (rt as i32) <= prev {
                            return Err(FormatError::NonCanonicalCbor(format!(
                                "record_counts key {rt} follows {prev}"
                            )));
                        }
                        prev = rt as i32;
                        m.record_counts.insert(RecordType(rt), d.u32()?);
                    }
                }
                KEY_MEMBERS => {
                    let count = d.array_header()?;
                    for i in 0..count {
                        let fields = d.array_header()?;
                        if fields != 3 {
                            return Err(FormatError::Malformed(format!(
                                "member {i} has {fields} fields, expected 3"
                            )));
                        }
                        let name = d.text()?;
                        let length = d.uint()?;
                        let sha256 = d.bytes_n()?;

                        if i > 0 && name.as_bytes() <= m.members[i - 1].name.as_bytes() {
                            return Err(FormatError::NonCanonicalCbor(format!(
                                "member {name} follows {}",
                                m.members[i - 1].name
                            )));
                        }
                        m.members.push(Member {
                            name,
                            length,
                            sha256,
                        });
                    }
                }
                KEY_CHUNK_DESCS => {
                    let count = d.array_header()?;
                    for i in 0..count {
                        let fields = d.array_header()?;
                        if fields != 3 {
                            return Err(FormatError::Malformed(format!(
                                "chunk {i} has {fields} fields, expected 3"
                            )));
                        }
                        let index = d.u32()?;
                        let byte_length = d.u32()?;
                        let sha256 = d.bytes_n()?;

                        if index as usize != i {
                            return Err(FormatError::Malformed(format!(
                                "chunk at position {i} declares index {index}"
                            )));
                        }
                        m.chunk_descriptors.push(ChunkDescriptor {
                            index,
                            byte_length,
                            sha256,
                        });
                    }
                }
                KEY_CONTENT_ROOT => m.content_root = d.bytes_n()?,
                KEY_PREVIOUS_ROOT => {
                    m.previous_bundle_root = if d.is_null() {
                        None
                    } else {
                        Some(d.bytes_n()?)
                    };
                }
                KEY_POLICY_VERSION => m.policy_version = d.u8()?,
                KEY_RECOVERY_STATE => m.recovery_state = RecoveryState::from_u8(d.u8()?)?,
                KEY_DISCARDED_TAIL => m.discarded_tail_bytes = d.u32()?,
                KEY_SIGNATURE_ALGO => {
                    m.signature_algorithm = d.text()?;
                    if m.signature_algorithm != SIGNATURE_ALGORITHM_ED25519 {
                        return Err(FormatError::Malformed(format!(
                            "unsupported signature algorithm {}",
                            m.signature_algorithm
                        )));
                    }
                }
                KEY_TRIP_SEQ => m.trip_seq = Some(d.u32()?),
                KEY_VEHICLE_ID => m.vehicle_id = d.bytes_n()?,
                KEY_ASSIGNMENT_ID => m.assignment_id = d.bytes_n()?,
                KEY_DEVICE_COUNTER => m.device_counter = d.uint()?,
                KEY_STORAGE_KEY_VERSION => m.storage_key_version = d.u32()?,
                KEY_ENCRYPTION_SUITE => {
                    m.encryption_suite = d.text()?;
                    if m.encryption_suite != ENCRYPTION_SUITE_V1 {
                        return Err(FormatError::Malformed(format!(
                            "unsupported encryption suite {:?}",
                            m.encryption_suite
                        )));
                    }
                }
                KEY_ENGINE_PROFILE => {
                    let fields = d.array_header()?;
                    if fields != 3 {
                        return Err(FormatError::Malformed(format!(
                            "engine_profile has {fields} fields, want 3"
                        )));
                    }
                    m.engine_profile = Some(EngineProfileRef {
                        id: d.text()?,
                        version: d.u8()?,
                        sha256: d.bytes_n()?,
                    });
                }
                other => {
                    return Err(FormatError::Malformed(format!(
                        "unknown manifest key {other}"
                    )));
                }
            }
        }

        // Every key but trip_seq and engine_profile is mandatory. With the field
        // count bounded and duplicates refused, a missing key could only hide
        // behind an optional one; name it, rather than letting a zero-valued
        // field reach the binding check, where an all-zero vehicle_id would read
        // as a claim.
        for key in (KEY_MANIFEST_VERSION..=KEY_ENCRYPTION_SUITE).filter(|&k| k != KEY_TRIP_SEQ) {
            if seen & (1 << key) == 0 {
                return Err(FormatError::Malformed(format!(
                    "manifest is missing mandatory key {key}"
                )));
            }
        }

        if !d.at_end() {
            return Err(FormatError::Malformed(format!(
                "{} trailing bytes after manifest",
                d.remaining()
            )));
        }

        Ok(m)
    }

    /// Sign the manifest, returning its encoded bytes and the signature.
    pub fn sign(&self, key: &SigningKey) -> Result<(Vec<u8>, [u8; 64])> {
        let encoded = self.to_cbor()?;
        let sig = key.sign(&encoded);
        Ok((encoded, sig.to_bytes()))
    }

    /// Recompute the content root from the members and check it against the
    /// signed value.
    ///
    /// A caller holding the actual member bytes must additionally confirm each
    /// member's SHA-256; this only proves the manifest is internally consistent.
    pub fn verify_content_root(&self) -> Result<()> {
        let computed = content_root(&self.members)?;
        if computed != self.content_root {
            return Err(FormatError::ContentRootMismatch {
                computed,
                declared: self.content_root,
            });
        }
        Ok(())
    }
}

/// Verify a signature against raw manifest bytes and return the parsed
/// manifest.
///
/// The signature is checked against the bytes as received, before parsing, so
/// verification never depends on this implementation's ability to re-encode.
pub fn verify_manifest(
    encoded: &[u8],
    signature: &[u8; 64],
    key: &VerifyingKey,
) -> Result<Manifest> {
    let sig = Signature::from_bytes(signature);
    key.verify(encoded, &sig)
        .map_err(|_| FormatError::BadSignature)?;
    Manifest::from_cbor(encoded)
}
