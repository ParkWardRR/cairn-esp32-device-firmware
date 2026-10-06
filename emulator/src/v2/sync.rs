//! The device-side sync protocol, with injectable network faults.
//!
//! Mirrors what the firmware's sync task must do, in the order it must do it:
//! offer the signed manifest, transfer only the chunks the server asks for
//! addressed by content hash, commit, then verify the receipt locally before
//! the bundle becomes eligible for pruning.
//!
//! Two server paths speak this protocol. The legacy device listener
//! (`/api/v2/bundles/*`) is being retired (front door #7); the real path is
//! dongle -> BLE -> phone -> the relay (`/v1/relay/bundles/*`), where an enrolled
//! app client signs every request. [`SyncClient`] speaks either, so one fault row
//! runs against both and a divergence between them is a finding.

use ed25519_dalek::VerifyingKey;

use crate::format::{self, Receipt};

use super::fault::{FaultPoint, Injector};
use super::relay::{self, AuthOverrides, Phone, Raw};
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

/// Which server path this client speaks.
#[derive(Clone)]
enum Mode {
    /// The legacy device listener (`/api/v2/...`).
    Legacy,
    /// The phone relay (`/v1/relay/bundles/...`): an enrolled app client signs every
    /// request and uploads on the dongle's behalf.
    Relay(Phone),
}

pub struct SyncClient {
    base: String,
    agent: ureq::Agent,
    mode: Mode,
    /// Where `/api/v2/health` can be read for the decode backlog. The relay has no such
    /// endpoint; this is a diagnostic, never part of the protocol under test.
    backlog_base: Option<String>,
}

fn new_agent() -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout(std::time::Duration::from_secs(30))
        .build()
}

impl SyncClient {
    /// A client for the legacy device listener.
    pub fn new(base: &str) -> Self {
        let base = base.trim_end_matches('/').to_string();
        Self {
            backlog_base: Some(base.clone()),
            base,
            agent: new_agent(),
            mode: Mode::Legacy,
        }
    }

    /// A client acting as the enrolled phone, against the app API's relay.
    pub fn relay(base: &str, phone: Phone) -> Self {
        Self {
            base: base.trim_end_matches('/').to_string(),
            agent: new_agent(),
            mode: Mode::Relay(phone),
            backlog_base: None,
        }
    }

    /// Read the decode backlog from this (legacy) base while running over the relay.
    pub fn with_backlog_base(mut self, base: &str) -> Self {
        if !base.is_empty() {
            self.backlog_base = Some(base.trim_end_matches('/').to_string());
        }
        self
    }

    pub fn is_relay(&self) -> bool {
        matches!(self.mode, Mode::Relay(_))
    }

    pub fn base(&self) -> &str {
        &self.base
    }

    pub fn phone(&self) -> Option<&Phone> {
        match &self.mode {
            Mode::Relay(p) => Some(p),
            Mode::Legacy => None,
        }
    }

    fn prefix(&self) -> &'static str {
        match self.mode {
            Mode::Legacy => "/api/v2/bundles",
            Mode::Relay(_) => "/v1/relay/bundles",
        }
    }

    /// One request on this client's path, returning whatever the server said. The relay
    /// signs it (with `auth` altering exactly what a fault row wants altered); the legacy
    /// listener takes it as is.
    pub fn send(
        &self,
        method: &str,
        suffix: &str,
        headers: &[(&str, String)],
        body: &[u8],
        auth: &AuthOverrides,
    ) -> Result<Raw> {
        let target = format!("{}{}", self.prefix(), suffix);
        // A phone backs off on 429 (the relay rate-limits per client: a burst of 240, then
        // 4 a second) and tries again with a FRESH signature, because a repeated nonce is
        // refused. Anything else is returned as the answer.
        let mut tries = 0;
        loop {
            let authorization = self
                .phone()
                .map(|p| p.authorization(method, &target, body, auth));
            let r = relay::raw_call(
                &self.agent,
                &self.base,
                method,
                &target,
                authorization.as_deref(),
                headers,
                body,
            )
            .map_err(SyncError::Http)?;
            if r.status == 429 && tries < 100 {
                tries += 1;
                std::thread::sleep(std::time::Duration::from_millis(250));
                continue;
            }
            return Ok(r);
        }
    }

    /// `send` with the default request, a non-200 answer becoming `Rejected`.
    fn call(
        &self,
        method: &str,
        suffix: &str,
        headers: &[(&str, String)],
        body: &[u8],
    ) -> Result<Raw> {
        let r = self.send(method, suffix, headers, body, &AuthOverrides::default())?;
        if (200..300).contains(&r.status) {
            Ok(r)
        } else {
            Err(SyncError::Rejected {
                status: r.status,
                body: String::from_utf8_lossy(&r.body).into_owned(),
            })
        }
    }

    /// Fetch and pin the receipt verification key.
    ///
    /// Provisioning, not part of a sync: a device that has not pinned this key
    /// cannot act on any receipt, and must not prune. Only the legacy listener serves
    /// it; for the relay the key comes from `--receipt-key` (the server's
    /// `-print-receipt-key`), exactly as a real dongle gets it at provisioning.
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

    /// Offer the manifest. Returns the chunks the server still wants.
    ///
    /// Over the relay the answer also says where each wanted chunk lives in the byte
    /// stream (the phone reads exactly those ranges from the dongle and has no CBOR
    /// parser), so those ranges are checked against the manifest the dongle sealed: a
    /// server that points the phone at the wrong bytes would corrupt every upload.
    pub fn offer(&self, bundle: &SealedBundle) -> Result<OfferResponse> {
        let r = self.offer_raw(&bundle.manifest_bytes, &bundle.signature)?;
        let r = if (200..300).contains(&r.status) {
            r
        } else {
            return Err(SyncError::Rejected {
                status: r.status,
                body: String::from_utf8_lossy(&r.body).into_owned(),
            });
        };
        let wire: OfferWire = serde_json::from_slice(&r.body)
            .map_err(|e| SyncError::Http(format!("decode offer response: {e}")))?;

        let mut missing = Vec::with_capacity(wire.missing_chunks.len());
        let mut at = Vec::with_capacity(bundle.chunk_count());
        let mut off = 0u64;
        for d in &bundle.manifest.chunk_descriptors {
            at.push(off);
            off += d.byte_length as u64;
        }
        for m in &wire.missing_chunks {
            match m {
                // Legacy: a bare index.
                serde_json::Value::Number(n) => {
                    let idx = n
                        .as_u64()
                        .ok_or_else(|| SyncError::Http("bad chunk index".into()))?;
                    missing.push(idx as u32);
                }
                // Relay: {index, offset, length, sha256}.
                serde_json::Value::Object(o) => {
                    let idx = o.get("index").and_then(|v| v.as_u64()).ok_or_else(|| {
                        SyncError::Http("relay offer: chunk without an index".into())
                    })? as usize;
                    let d = bundle.manifest.chunk_descriptors.get(idx).ok_or_else(|| {
                        SyncError::Http(format!("relay offer names chunk {idx}, not in manifest"))
                    })?;
                    let ok = o.get("offset").and_then(|v| v.as_u64()) == Some(at[idx])
                        && o.get("length").and_then(|v| v.as_u64()) == Some(d.byte_length as u64)
                        && o.get("sha256").and_then(|v| v.as_str())
                            == Some(format::hex(&d.sha256).as_str());
                    if !ok {
                        return Err(SyncError::Http(format!(
                            "relay offer's range for chunk {idx} disagrees with the manifest"
                        )));
                    }
                    missing.push(idx as u32);
                }
                other => {
                    return Err(SyncError::Http(format!("unrecognised chunk entry {other}")));
                }
            }
        }
        Ok(OfferResponse {
            missing_chunks: missing,
            total_chunks: wire.total_chunks,
            bytes_outstanding: wire.bytes_outstanding,
            receipt_available: wire.receipt_available,
        })
    }

    /// The offer request alone, with whatever signature and manifest bytes the caller
    /// chooses (a forged-offer row sends the wrong ones).
    pub fn offer_raw(&self, manifest: &[u8], signature: &[u8; 64]) -> Result<Raw> {
        self.send(
            "POST",
            "/offer",
            &[
                ("Content-Type", "application/cbor".into()),
                ("X-Cairn-Signature", format::hex(signature)),
            ],
            manifest,
            &AuthOverrides::default(),
        )
    }

    /// PUT one chunk, addressed by its content hash.
    pub fn put_chunk(&self, bundle_hex: &str, digest: &[u8; 32], data: &[u8]) -> Result<()> {
        self.call(
            "PUT",
            &format!("/{}/chunks/{}", bundle_hex, format::hex(digest)),
            &[("Content-Type", "application/octet-stream".into())],
            data,
        )
        .map(|_| ())
    }

    /// Commit, returning the raw receipt bytes and whether it was already
    /// committed.
    ///
    /// The bytes are returned verbatim because the device verifies a signature
    /// over exactly them; any envelope would create a re-encode step on the
    /// device, which is precisely where a canonical-encoding bug would hide.
    pub fn commit(&self, bundle_hex: &str) -> Result<(Vec<u8>, bool)> {
        let r = self.call("POST", &format!("/{bundle_hex}/commit"), &[], &[])?;
        let already = r.header("X-Cairn-Already-Committed") == Some("1");
        Ok((r.body, already))
    }

    /// The server's stored receipt for a bundle, without committing anything (relay only;
    /// the legacy listener has no such endpoint).
    pub fn fetch_receipt(&self, bundle_hex: &str) -> Result<Vec<u8>> {
        Ok(self
            .call("GET", &format!("/{bundle_hex}/receipt"), &[], &[])?
            .body)
    }

    /// The server's reported decode backlog, used to assert that work was
    /// queued exactly once. -1 when this client has nowhere to read it.
    pub fn decode_backlog(&self) -> Result<i64> {
        let Some(base) = &self.backlog_base else {
            return Ok(-1);
        };
        let resp = self
            .agent
            .get(&format!("{base}/api/v2/health"))
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

/// The offer answer, normalised across both paths.
#[derive(Debug, Clone)]
pub struct OfferResponse {
    pub missing_chunks: Vec<u32>,
    #[allow(dead_code)]
    pub total_chunks: usize,
    #[allow(dead_code)]
    pub bytes_outstanding: i64,
    #[allow(dead_code)]
    pub receipt_available: bool,
}

#[derive(Debug, serde::Deserialize)]
struct OfferWire {
    missing_chunks: Vec<serde_json::Value>,
    #[serde(default)]
    total_chunks: usize,
    #[serde(default)]
    bytes_outstanding: i64,
    #[serde(default)]
    receipt_available: bool,
}
