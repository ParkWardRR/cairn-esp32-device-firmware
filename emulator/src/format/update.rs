//! OTA update descriptor (docs/ota.md), signed by the update key.
//!
//! The update key is deliberately separate from the receipt key. The receipt
//! key says "this data is safe to delete"; the update key says "this code is
//! safe to run". A server compromised enough to issue false receipts costs
//! stored trips; one that could also sign firmware owns the device.

use ed25519_dalek::{Signature, Verifier, VerifyingKey};

use super::{
    FormatError, Result,
    cbor::{Decoder, Encoder},
    manifest::SIGNATURE_ALGORITHM_ED25519,
};

/// The only descriptor version this build emits or accepts.
pub const UPDATE_DESCRIPTOR_VERSION: u8 = 1;

const KEY_DESCRIPTOR_VERSION: u64 = 1;
const KEY_FIRMWARE_VERSION: u64 = 2;
const KEY_IMAGE_SHA256: u64 = 3;
const KEY_IMAGE_LENGTH: u64 = 4;
const KEY_MIN_FIRMWARE: u64 = 5;
const KEY_BUILD_UTC_MS: u64 = 6;
const KEY_SIGNATURE_ALGO: u64 = 7;
const UPDATE_FIELD_COUNT: usize = 7;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UpdateDescriptor {
    pub descriptor_version: u8,
    pub firmware_version: String,
    pub image_sha256: [u8; 32],
    pub image_length: u32,
    /// Refuse the install over anything older. Empty means no constraint.
    pub min_firmware_version: String,
    /// Informational only and never an ordering key — the same rule the bundle
    /// format applies to wall-clock time.
    pub build_utc_ms: u64,
    pub signature_algorithm: String,
}

impl UpdateDescriptor {
    /// Encode deterministically: exactly the bytes the signature covers.
    pub fn to_cbor(&self) -> Result<Vec<u8>> {
        if self.signature_algorithm.is_empty() {
            return Err(FormatError::Malformed(
                "update descriptor: signature algorithm is empty".into(),
            ));
        }
        if self.firmware_version.is_empty() {
            return Err(FormatError::Malformed(
                "update descriptor: firmware version is empty".into(),
            ));
        }
        if self.image_length == 0 {
            return Err(FormatError::Malformed(
                "update descriptor: image length is zero".into(),
            ));
        }

        let mut e = Encoder::new();
        e.map_header(UPDATE_FIELD_COUNT);
        e.key(KEY_DESCRIPTOR_VERSION);
        e.uint(self.descriptor_version as u64);
        e.key(KEY_FIRMWARE_VERSION);
        e.text(&self.firmware_version);
        e.key(KEY_IMAGE_SHA256);
        e.bytes(&self.image_sha256);
        e.key(KEY_IMAGE_LENGTH);
        e.uint(self.image_length as u64);
        e.key(KEY_MIN_FIRMWARE);
        e.text(&self.min_firmware_version);
        e.key(KEY_BUILD_UTC_MS);
        e.uint(self.build_utc_ms);
        e.key(KEY_SIGNATURE_ALGO);
        e.text(&self.signature_algorithm);
        Ok(e.into_bytes())
    }

    /// Decode, rejecting anything this implementation would not itself have
    /// produced. Strict for the same reason the manifest decoder is: a
    /// descriptor whose bytes cannot be reproduced cannot have its signature
    /// re-checked later, and this is the one signature whose failure mode is an
    /// unbootable device.
    pub fn from_cbor(b: &[u8]) -> Result<Self> {
        let mut d = Decoder::new(b);
        let n = d.map_header()?;
        if n != UPDATE_FIELD_COUNT {
            return Err(FormatError::Malformed(format!(
                "update descriptor has {n} fields, want {UPDATE_FIELD_COUNT}"
            )));
        }

        let mut u = UpdateDescriptor {
            descriptor_version: 0,
            firmware_version: String::new(),
            image_sha256: [0; 32],
            image_length: 0,
            min_firmware_version: String::new(),
            build_utc_ms: 0,
            signature_algorithm: String::new(),
        };

        for _ in 0..n {
            match d.uint()? {
                KEY_DESCRIPTOR_VERSION => u.descriptor_version = d.u8()?,
                KEY_FIRMWARE_VERSION => u.firmware_version = d.text()?,
                KEY_IMAGE_SHA256 => u.image_sha256 = d.bytes_n()?,
                KEY_IMAGE_LENGTH => u.image_length = d.u32()?,
                KEY_MIN_FIRMWARE => u.min_firmware_version = d.text()?,
                KEY_BUILD_UTC_MS => u.build_utc_ms = d.uint()?,
                KEY_SIGNATURE_ALGO => u.signature_algorithm = d.text()?,
                other => {
                    return Err(FormatError::Malformed(format!(
                        "update descriptor has unknown key {other}"
                    )));
                }
            }
        }
        if !d.at_end() {
            return Err(FormatError::Malformed(format!(
                "update descriptor has {} trailing byte(s)",
                d.remaining()
            )));
        }

        if u.descriptor_version != UPDATE_DESCRIPTOR_VERSION {
            return Err(FormatError::Malformed(format!(
                "descriptor version {} is not supported",
                u.descriptor_version
            )));
        }
        if u.signature_algorithm != SIGNATURE_ALGORITHM_ED25519 {
            return Err(FormatError::Malformed(format!(
                "signature algorithm {:?} is not supported",
                u.signature_algorithm
            )));
        }
        if u.image_length == 0 {
            return Err(FormatError::Malformed(
                "update descriptor declares a zero-length image".into(),
            ));
        }

        // Re-encode and compare, so a descriptor that parses but is not
        // canonical is rejected here rather than failing verification on the
        // device for a reason the device cannot explain.
        if u.to_cbor()? != b {
            return Err(FormatError::NonCanonicalCbor(
                "update descriptor is not canonically encoded".into(),
            ));
        }
        Ok(u)
    }
}

/// Check the signature, then parse.
///
/// Signature first and separately from any use of the contents: a device
/// downloads megabytes on the strength of this check, so an unsigned
/// descriptor must cost nothing.
pub fn verify_update_descriptor(
    encoded: &[u8],
    signature: &[u8],
    key: &VerifyingKey,
) -> Result<UpdateDescriptor> {
    let sig: [u8; 64] = signature
        .try_into()
        .map_err(|_| FormatError::BadSignature)?;
    key.verify(encoded, &Signature::from_bytes(&sig))
        .map_err(|_| FormatError::BadSignature)?;
    UpdateDescriptor::from_cbor(encoded)
}
