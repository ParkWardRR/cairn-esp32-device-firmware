//! The device-side sync protocol, with injectable network faults.
//!
//! Mirrors what the firmware's sync task must do, in the order it must do it:
//! offer the signed manifest, transfer only the chunks the server asks for
//! addressed by content hash, commit, then verify the receipt locally before
//! the bundle becomes eligible for pruning.

use ed25519_dalek::VerifyingKey;

use crate::format::{self, Receipt};

use super::fault::{FaultPoint, Injector};
use super::store::SealedBundle;

#[derive(Debug)]
pub enum SyncError {
    Http(String),
    /// The server refused permanently: retrying will not help.
    Rejected {
        status: u16,
        body: String,
    },
    Format(format::FormatError),
    Interrupted(super::fault::Interrupted),
    /// The receipt did not verify. The bundle must NOT be pruned.
    ReceiptUnverified(String),
}

impl std::fmt::Display for SyncError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Http(e) => write!(f, "transport: {e}"),
            Self::Rejected { status, body } => write!(f, "server rejected ({status}): {body}"),
            Self::Format(e) => write!(f, "format: {e}"),
            Self::Interrupted(i) => write!(f, "{i}"),
            Self::ReceiptUnverified(why) => {
                write!(
                    f,
                    "receipt did not verify, bundle must not be pruned: {why}"
                )
            }
        }
    }
}

impl From<format::FormatError> for SyncError {
    fn from(e: format::FormatError) -> Self {
        Self::Format(e)
    }
}

impl From<super::fault::Interrupted> for SyncError {
    fn from(e: super::fault::Interrupted) -> Self {
        Self::Interrupted(e)
    }
}

pub type Result<T> = std::result::Result<T, SyncError>;

/// What one sync attempt achieved.
#[derive(Debug, Clone, Default)]
pub struct SyncOutcome {
    pub chunks_sent: usize,
    pub chunks_rejected: usize,
    pub already_committed: bool,
    pub receipt: Option<Receipt>,
    pub receipt_bytes: Vec<u8>,
}

pub struct SyncClient {
    base: String,
    agent: ureq::Agent,
}

impl SyncClient {
    pub fn new(base: &str) -> Self {
        Self {
            base: base.trim_end_matches('/').to_string(),
            agent: ureq::AgentBuilder::new()
                .timeout(std::time::Duration::from_secs(30))
                .build(),
        }
    }

    /// Fetch and pin the receipt verification key.
    ///
    /// Provisioning, not part of a sync: a device that has not pinned this key
    /// cannot act on any receipt, and must not prune.
    pub fn fetch_receipt_key(&self) -> Result<VerifyingKey> {
        let resp = self
            .agent
            .get(&format!("{}/api/v2/server/receipt-key", self.base))
            .call()
            .map_err(|e| SyncError::Http(e.to_string()))?;

        let body: serde_json::Value = resp
            .into_json()
            .map_err(|e| SyncError::Http(e.to_string()))?;

        let hex_key = body
            .get("public_key")
            .and_then(|v| v.as_str())
            .ok_or_else(|| SyncError::Http("receipt-key response has no public_key".into()))?;

        let raw = format::unhex_array::<32>(hex_key)
            .ok_or_else(|| SyncError::Http("public_key is not 32 bytes of hex".into()))?;

        VerifyingKey::from_bytes(&raw).map_err(|e| SyncError::Http(format!("bad key: {e}")))
    }

    /// Run one full sync attempt, honouring any armed fault.
    ///
    /// A single attempt may legitimately fail partway: the caller retries, and
    /// the protocol is designed so a retry resumes rather than restarts.
    pub fn sync(
        &self,
        bundle: &SealedBundle,
        receipt_key: &VerifyingKey,
        inj: &mut Injector,
    ) -> Result<SyncOutcome> {
        let mut outcome = SyncOutcome::default();
        let bundle_hex = format::hex(&bundle.manifest.bundle_id);

        inj.check(FaultPoint::BeforeOffer)?;

        let offer = self.offer(bundle)?;

        inj.check(FaultPoint::AfterOffer)?;

        for &idx in &offer.missing_chunks {
            inj.check(FaultPoint::BeforeChunk(idx))?;

            let data = bundle.chunk(idx as usize);
            let descriptor = &bundle.manifest.chunk_descriptors[idx as usize];

            // Corrupting in flight must be caught by the server, not by us.
            if inj.should_corrupt(FaultPoint::CorruptChunkInTransit(idx)) {
                let mut corrupted = data.to_vec();
                if !corrupted.is_empty() {
                    corrupted[0] ^= 0xFF;
                }
                match self.put_chunk(&bundle_hex, &descriptor.sha256, &corrupted) {
                    Ok(_) => {
                        return Err(SyncError::ReceiptUnverified(
                            "the server accepted a corrupted chunk".into(),
                        ));
                    }
                    Err(SyncError::Rejected { .. }) => {
                        outcome.chunks_rejected += 1;
                        // Retry with the real bytes: a corrupt chunk must not
                        // need operator action to recover from.
                        self.put_chunk(&bundle_hex, &descriptor.sha256, data)?;
                        outcome.chunks_sent += 1;
                    }
                    Err(e) => return Err(e),
                }
            } else {
                self.put_chunk(&bundle_hex, &descriptor.sha256, data)?;
                outcome.chunks_sent += 1;
            }

            inj.check(FaultPoint::AfterChunk(idx))?;
        }

        inj.check(FaultPoint::BeforeCommit)?;

        let (receipt_bytes, already) = self.commit(&bundle_hex)?;
        outcome.already_committed = already;

        // The server committed and receipted, but the response never arrived.
        // The device must retry and get the same receipt.
        inj.check(FaultPoint::ReceiptLostInTransit)?;

        let receipt = Receipt::from_cbor(&receipt_bytes)?;
        receipt
            .verify_acknowledges(receipt_key, &bundle.manifest.content_root)
            .map_err(|e| SyncError::ReceiptUnverified(e.to_string()))?;

        outcome.receipt = Some(receipt);
        outcome.receipt_bytes = receipt_bytes;
        Ok(outcome)
    }

    fn offer(&self, bundle: &SealedBundle) -> Result<OfferResponse> {
        let resp = self
            .agent
            .post(&format!("{}/api/v2/bundles/offer", self.base))
            .set("Content-Type", "application/cbor")
            .set("X-Cairn-Signature", &format::hex(&bundle.signature))
            .send_bytes(&bundle.manifest_bytes);

        match resp {
            Ok(r) => r
                .into_json::<OfferResponse>()
                .map_err(|e| SyncError::Http(format!("decode offer response: {e}"))),
            Err(ureq::Error::Status(status, r)) => Err(SyncError::Rejected {
                status,
                body: r.into_string().unwrap_or_default(),
            }),
            Err(e) => Err(SyncError::Http(e.to_string())),
        }
    }

    fn put_chunk(&self, bundle_hex: &str, digest: &[u8; 32], data: &[u8]) -> Result<()> {
        let url = format!(
            "{}/api/v2/bundles/{}/chunks/{}",
            self.base,
            bundle_hex,
            format::hex(digest)
        );

        match self.agent.put(&url).send_bytes(data) {
            Ok(_) => Ok(()),
            Err(ureq::Error::Status(status, r)) => Err(SyncError::Rejected {
                status,
                body: r.into_string().unwrap_or_default(),
            }),
            Err(e) => Err(SyncError::Http(e.to_string())),
        }
    }

    /// Commit, returning the raw receipt bytes and whether it was already
    /// committed.
    ///
    /// The bytes are returned verbatim because the device verifies a signature
    /// over exactly them; any envelope would create a re-encode step on the
    /// device, which is precisely where a canonical-encoding bug would hide.
    fn commit(&self, bundle_hex: &str) -> Result<(Vec<u8>, bool)> {
        let url = format!("{}/api/v2/bundles/{}/commit", self.base, bundle_hex);

        match self.agent.post(&url).call() {
            Ok(r) => {
                let already = r.header("X-Cairn-Already-Committed") == Some("1");
                let mut buf = Vec::new();
                r.into_reader()
                    .read_to_end(&mut buf)
                    .map_err(|e| SyncError::Http(format!("read receipt: {e}")))?;
                Ok((buf, already))
            }
            Err(ureq::Error::Status(status, r)) => Err(SyncError::Rejected {
                status,
                body: r.into_string().unwrap_or_default(),
            }),
            Err(e) => Err(SyncError::Http(e.to_string())),
        }
    }

    /// The server's reported decode backlog, used to assert that work was
    /// queued exactly once.
    pub fn decode_backlog(&self) -> Result<i64> {
        let resp = self
            .agent
            .get(&format!("{}/api/v2/health", self.base))
            .call()
            .map_err(|e| SyncError::Http(e.to_string()))?;

        let body: serde_json::Value = resp
            .into_json()
            .map_err(|e| SyncError::Http(e.to_string()))?;

        Ok(body
            .get("decode_backlog")
            .and_then(|v| v.as_i64())
            .unwrap_or(-1))
    }
}

#[derive(Debug, serde::Deserialize)]
struct OfferResponse {
    missing_chunks: Vec<u32>,
    #[allow(dead_code)]
    total_chunks: usize,
    #[allow(dead_code)]
    #[serde(default)]
    bytes_outstanding: i64,
    #[serde(default)]
    #[allow(dead_code)]
    receipt_available: bool,
}

use std::io::Read;
