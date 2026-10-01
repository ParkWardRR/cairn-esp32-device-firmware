//! The server's durable, signed proof that it committed a reconstructable
//! bundle.
//!
//! A chunk acknowledgement is not a receipt: it means bytes were accepted, not
//! that a recoverable record exists. Only a receipt justifies deletion, and only
//! when both its signature verifies against the pinned server key *and* its
//! content root equals what the device uploaded.

use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};

use super::{
    FormatError, Result,
    cbor::{Decoder, Encoder},
    manifest::SIGNATURE_ALGORITHM_ED25519,
};

/// The receipt schema version this implementation handles.
pub const RECEIPT_VERSION: u8 = 2;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Receipt {
    pub receipt_version: u8,
    pub receipt_id: [u8; 16],
    pub device_id: [u8; 16],
    pub bundle_id: [u8; 16],
    pub content_root: [u8; 32],
    pub server_ingest_utc_ms: u64,
    pub server_key_id: [u8; 8],
    pub ingest_schema_version: u8,
    pub stored_object_ids: Vec<String>,
    pub signature_algorithm: String,
    pub signature: [u8; 64],
}

// Keys 1 through 10 are covered by the signature; key 11 is the signature.
const KEY_RECEIPT_VERSION: u64 = 1;
const KEY_RECEIPT_ID: u64 = 2;
const KEY_DEVICE_ID: u64 = 3;
const KEY_BUNDLE_ID: u64 = 4;
const KEY_CONTENT_ROOT: u64 = 5;
const KEY_SERVER_INGEST_UTC: u64 = 6;
const KEY_SERVER_KEY_ID: u64 = 7;
const KEY_INGEST_SCHEMA_VER: u64 = 8;
const KEY_STORED_OBJECT_IDS: u64 = 9;
const KEY_SIGNATURE_ALGO: u64 = 10;
const KEY_SIGNATURE: u64 = 11;

const RECEIPT_SIGNED_FIELD_COUNT: usize = 10;
const RECEIPT_FIELD_COUNT: usize = 11;

impl Receipt {
    fn encode_fields(&self, include_signature: bool) -> Result<Vec<u8>> {
        if self.signature_algorithm != SIGNATURE_ALGORITHM_ED25519 {
            return Err(FormatError::Malformed(format!(
                "unsupported signature algorithm {}",
                self.signature_algorithm
            )));
        }

        let count = if include_signature {
            RECEIPT_FIELD_COUNT
        } else {
            RECEIPT_SIGNED_FIELD_COUNT
        };

        let mut e = Encoder::new();
        e.map_header(count);

        e.key(KEY_RECEIPT_VERSION);
        e.uint(self.receipt_version as u64);
        e.key(KEY_RECEIPT_ID);
        e.bytes(&self.receipt_id);
        e.key(KEY_DEVICE_ID);
        e.bytes(&self.device_id);
        e.key(KEY_BUNDLE_ID);
        e.bytes(&self.bundle_id);
        e.key(KEY_CONTENT_ROOT);
        e.bytes(&self.content_root);
        e.key(KEY_SERVER_INGEST_UTC);
        e.uint(self.server_ingest_utc_ms);
        e.key(KEY_SERVER_KEY_ID);
        e.bytes(&self.server_key_id);
        e.key(KEY_INGEST_SCHEMA_VER);
        e.uint(self.ingest_schema_version as u64);

        e.key(KEY_STORED_OBJECT_IDS);
        e.array_header(self.stored_object_ids.len());
        for id in &self.stored_object_ids {
            e.text(id);
        }

        e.key(KEY_SIGNATURE_ALGO);
        e.text(&self.signature_algorithm);

        if include_signature {
            e.key(KEY_SIGNATURE);
            e.bytes(&self.signature);
        }

        Ok(e.into_bytes())
    }

    /// The deterministic encoding of keys 1 through 10, which the signature
    /// covers.
    pub fn signing_bytes(&self) -> Result<Vec<u8>> {
        self.encode_fields(false)
    }

    /// The complete receipt, signature included.
    pub fn to_cbor(&self) -> Result<Vec<u8>> {
        self.encode_fields(true)
    }

    /// Populate the signature and return the complete encoded receipt.
    pub fn sign(&mut self, key: &SigningKey) -> Result<Vec<u8>> {
        let signing = self.signing_bytes()?;
        self.signature = key.sign(&signing).to_bytes();
        self.to_cbor()
    }

    /// Check the signature against a pinned server public key.
    ///
    /// This is the device-side gate on deletion. It re-derives the signed byte
    /// sequence from the parsed fields rather than trusting any portion of the
    /// received bytes, so a receipt that is not canonically encoded cannot
    /// verify.
    pub fn verify(&self, key: &VerifyingKey) -> Result<()> {
        let signing = self.signing_bytes()?;
        let sig = Signature::from_bytes(&self.signature);
        key.verify(&signing, &sig)
            .map_err(|_| FormatError::BadSignature)
    }

    /// Check that the receipt both verifies and acknowledges the specific
    /// content root the device uploaded.
    ///
    /// Both conditions are required. A validly signed receipt for a different
    /// bundle is not an acknowledgement of this one, and treating it as one
    /// would let a misconfigured or hostile server induce deletion of
    /// unacknowledged data.
    pub fn verify_acknowledges(&self, key: &VerifyingKey, uploaded: &[u8; 32]) -> Result<()> {
        self.verify(key)?;
        if &self.content_root != uploaded {
            return Err(FormatError::ReceiptRootMismatch {
                receipt: self.content_root,
                uploaded: *uploaded,
            });
        }
        Ok(())
    }

    /// Decode a receipt and verify that it is canonically encoded.
    ///
    /// The canonicality check matters more here than for the manifest: a receipt
    /// carries its signature inline, so verification must re-encode the signed
    /// fields. Accepting a non-canonical receipt would mean verifying bytes
    /// other than those received.
    pub fn from_cbor(b: &[u8]) -> Result<Self> {
        let r = Self::decode(b)?;

        let reencoded = r.to_cbor()?;
        if reencoded != b {
            return Err(FormatError::NonCanonicalCbor(
                "receipt does not round-trip to identical bytes".into(),
            ));
        }

        Ok(r)
    }

    fn decode(b: &[u8]) -> Result<Self> {
        let mut d = Decoder::new(b);

        let n = d.map_header()?;
        if n != RECEIPT_FIELD_COUNT {
            return Err(FormatError::Malformed(format!(
                "receipt has {n} fields, expected {RECEIPT_FIELD_COUNT}"
            )));
        }

        let mut r = Receipt {
            receipt_version: 0,
            receipt_id: [0; 16],
            device_id: [0; 16],
            bundle_id: [0; 16],
            content_root: [0; 32],
            server_ingest_utc_ms: 0,
            server_key_id: [0; 8],
            ingest_schema_version: 0,
            stored_object_ids: Vec::new(),
            signature_algorithm: String::new(),
            signature: [0; 64],
        };

        for i in 0..n {
            let key = d
                .uint()
                .map_err(|e| FormatError::Malformed(format!("receipt key {i}: {e}")))?;

            match key {
                KEY_RECEIPT_VERSION => {
                    r.receipt_version = d.u8()?;
                    if r.receipt_version != RECEIPT_VERSION {
                        return Err(FormatError::Malformed(format!(
                            "unsupported receipt_version {} (this implementation reads {RECEIPT_VERSION} only)",
                            r.receipt_version
                        )));
                    }
                }
                KEY_RECEIPT_ID => r.receipt_id = d.bytes_n()?,
                KEY_DEVICE_ID => r.device_id = d.bytes_n()?,
                KEY_BUNDLE_ID => r.bundle_id = d.bytes_n()?,
                KEY_CONTENT_ROOT => r.content_root = d.bytes_n()?,
                KEY_SERVER_INGEST_UTC => r.server_ingest_utc_ms = d.uint()?,
                KEY_SERVER_KEY_ID => r.server_key_id = d.bytes_n()?,
                KEY_INGEST_SCHEMA_VER => r.ingest_schema_version = d.u8()?,
                KEY_STORED_OBJECT_IDS => {
                    let count = d.array_header()?;
                    for _ in 0..count {
                        r.stored_object_ids.push(d.text()?);
                    }
                }
                KEY_SIGNATURE_ALGO => {
                    r.signature_algorithm = d.text()?;
                    if r.signature_algorithm != SIGNATURE_ALGORITHM_ED25519 {
                        return Err(FormatError::Malformed(format!(
                            "unsupported signature algorithm {}",
                            r.signature_algorithm
                        )));
                    }
                }
                KEY_SIGNATURE => r.signature = d.bytes_n()?,
                other => {
                    return Err(FormatError::Malformed(format!(
                        "unknown receipt key {other}"
                    )));
                }
            }
        }

        if !d.at_end() {
            return Err(FormatError::Malformed(format!(
                "{} trailing bytes after receipt",
                d.remaining()
            )));
        }

        Ok(r)
    }
}
