//! Protocol rows only the phone relay can have.
//!
//! The rows shared with the legacy listener (torn uploads, duplicates, a corrupted chunk,
//! a lost receipt) live in `matrix.rs` and run on either path. These are the ones whose
//! subject exists only on `/v1/relay/bundles/*`:
//!
//! - the CALLER is authenticated, so a request can be forged, replayed or swapped;
//! - the phone is a relay and not the device, so chunks can arrive in any order and a
//!   commit can arrive before the data does;
//! - the dongle never talks to the server, so what protects it from a hostile or buggy
//!   hop is its own check of the receipt it is handed.
//!
//! Each row states what the server (or the dongle) must do, makes the fault, and asserts the
//! answer, by status AND by the server's machine-readable code where it has one: a 401 for
//! the wrong reason would pass a status-only check and prove nothing.

use ed25519_dalek::SigningKey;

use crate::format::{self, FormatError, Receipt};

use super::fault::Injector;
use super::matrix::{Config, RowResult, Target, clone_cfg, sealed_for_protocol};
use super::relay::{self, AuthOverrides};
use super::store::SealedBundle;
use super::sync::SyncError;

type Outcome = std::result::Result<String, String>;

/// Run every relay-only row.
pub fn rows(cfg: &Config, t: &Target) -> Vec<RowResult> {
    type Row = fn(&Config, &Target) -> Outcome;
    let table: [(&str, &str, Row); 6] = [
        (
            "chunks-out-of-order",
            "the relay accepts chunks in any order, and a repeated chunk, and still commits one receipt",
            chunks_out_of_order,
        ),
        (
            "commit-before-complete",
            "a commit with chunks outstanding is refused, issues no receipt, and the upload still completes",
            commit_before_complete,
        ),
        (
            "forged-receipt",
            "the dongle rejects a receipt that is altered, re-signed by another key, or for other content",
            forged_receipt,
        ),
        (
            "forged-offer",
            "an offer whose manifest or signature was tampered with is refused and leaves nothing on record",
            forged_offer,
        ),
        (
            "unauthenticated-requests-refused",
            "a request with no, wrong, stale or swapped-body caller signature is refused",
            unauthenticated_requests,
        ),
        (
            "replayed-request-refused",
            "a captured, validly signed request cannot be replayed",
            replayed_request,
        ),
    ];

    table
        .into_iter()
        .map(|(name, property, row)| {
            let family = t.family(name);
            match &t.key {
                Err(e) => RowResult::fail(&family, property, format!("receipt key: {e}"), cfg.seed),
                Ok(_) => match row(cfg, t) {
                    Ok(detail) => RowResult::pass(&family, property, detail, cfg.seed),
                    Err(why) => RowResult::fail(&family, property, why, cfg.seed),
                },
            }
        })
        .collect()
}

/// A sealed bundle with several chunks, so ordering means something.
fn multi_chunk(cfg: &Config, t: &Target, name: &str, n: u8) -> Result<SealedBundle, String> {
    let small = Config {
        chunk_size: 128,
        ..clone_cfg(cfg)
    };
    let (_store, sealed) =
        sealed_for_protocol(&small, &t.tag(name), t.bundle_id(n), 40).map_err(|e| e.to_string())?;
    if sealed.chunk_count() < 3 {
        return Err(format!(
            "only {} chunks; need at least 3",
            sealed.chunk_count()
        ));
    }
    Ok(sealed)
}

fn rejected(e: SyncError) -> Result<(u16, String), String> {
    match e {
        SyncError::Rejected { status, body } => Ok((status, body)),
        other => Err(format!("expected a refusal, got: {other}")),
    }
}

fn want_code(status: u16, body: &str, want_status: u16, want_code: &str) -> Result<(), String> {
    let code = serde_json::from_str::<serde_json::Value>(body)
        .ok()
        .and_then(|v| v.get("error").and_then(|e| e.as_str().map(String::from)))
        .unwrap_or_default();
    if status == want_status && code == want_code {
        Ok(())
    } else {
        Err(format!(
            "expected {want_status} {want_code}, the server said {status} {code:?}"
        ))
    }
}

fn hexid(b: &SealedBundle) -> String {
    format::hex(&b.manifest.bundle_id)
}

// ── reordering ──────────────────────────────────────────────────────────────

/// The phone reads chunk ranges from the dongle however it likes (a flaky link re-reads,
/// and a second thread may race the first). Chunks are addressed by content hash, so the
/// server must not care about order or repeats.
fn chunks_out_of_order(cfg: &Config, t: &Target) -> Outcome {
    let c = &t.client;
    let key = t.key.clone()?;
    let sealed = multi_chunk(cfg, t, "reorder", 20)?;
    let id = hexid(&sealed);
    let total = sealed.chunk_count();

    let offer = c.offer(&sealed).map_err(|e| e.to_string())?;
    if offer.missing_chunks.len() != total {
        return Err(format!(
            "a first offer asked for {} of {total} chunks",
            offer.missing_chunks.len()
        ));
    }

    // Last first, then back down, then the first one again.
    let mut order: Vec<usize> = (0..total).rev().collect();
    order.push(total - 1);
    for &i in &order {
        c.put_chunk(
            &id,
            &sealed.manifest.chunk_descriptors[i].sha256,
            sealed.chunk(i),
        )
        .map_err(|e| format!("chunk {i}: {e}"))?;
    }

    let (bytes, _) = c.commit(&id).map_err(|e| e.to_string())?;
    let receipt = Receipt::from_cbor(&bytes).map_err(|e| e.to_string())?;
    receipt
        .verify_acknowledges(&key, &sealed.manifest.content_root)
        .map_err(|e| format!("the receipt for the reordered upload does not verify: {e}"))?;

    // Whatever order it arrived in, the bundle is one bundle: a second offer is complete.
    let again = c.offer(&sealed).map_err(|e| e.to_string())?;
    if !again.missing_chunks.is_empty() {
        return Err(format!(
            "after commit the server still wants {} chunk(s)",
            again.missing_chunks.len()
        ));
    }
    Ok(format!(
        "{total} chunks sent in reverse order plus one repeat: committed, receipt verifies"
    ))
}

/// A commit that outruns the data: the phone must not be handed a receipt for bytes the
/// server does not have, and the upload must still be completable afterwards.
fn commit_before_complete(cfg: &Config, t: &Target) -> Outcome {
    let c = &t.client;
    let key = t.key.clone()?;
    let sealed = multi_chunk(cfg, t, "early-commit", 21)?;
    let id = hexid(&sealed);

    c.offer(&sealed).map_err(|e| e.to_string())?;
    c.put_chunk(
        &id,
        &sealed.manifest.chunk_descriptors[0].sha256,
        sealed.chunk(0),
    )
    .map_err(|e| e.to_string())?;

    let (status, body) = match c.commit(&id) {
        Ok(_) => return Err("a commit with chunks missing returned a receipt".into()),
        Err(e) => rejected(e)?,
    };
    want_code(status, &body, 409, "chunks_missing")?;

    let (status, body) = match c.fetch_receipt(&id) {
        Ok(_) => return Err("a receipt exists for a bundle that was never completed".into()),
        Err(e) => rejected(e)?,
    };
    want_code(status, &body, 404, "no_receipt")?;

    // The refusal must not have poisoned the upload.
    let offer = c.offer(&sealed).map_err(|e| e.to_string())?;
    if offer.missing_chunks.len() != sealed.chunk_count() - 1 {
        return Err(format!(
            "after the refused commit the server wants {} chunks, expected {}",
            offer.missing_chunks.len(),
            sealed.chunk_count() - 1
        ));
    }
    for &i in &offer.missing_chunks {
        c.put_chunk(
            &id,
            &sealed.manifest.chunk_descriptors[i as usize].sha256,
            sealed.chunk(i as usize),
        )
        .map_err(|e| e.to_string())?;
    }
    let (bytes, _) = c.commit(&id).map_err(|e| e.to_string())?;
    let stored = c.fetch_receipt(&id).map_err(|e| e.to_string())?;
    if stored != bytes {
        return Err("GET receipt returned different bytes than the commit did".into());
    }
    Receipt::from_cbor(&bytes)
        .and_then(|r| r.verify_acknowledges(&key, &sealed.manifest.content_root))
        .map_err(|e| e.to_string())?;
    Ok(
        "early commit: 409 chunks_missing, GET receipt 404 no_receipt, then the upload \
        completed and the stored receipt equals the committed one"
            .into(),
    )
}

// ── forgery ─────────────────────────────────────────────────────────────────

/// The dongle never talks to the server: it is handed a receipt by a phone it cannot
/// authenticate, and deletes the capture on the strength of it. The receipt check is the
/// only thing between a hostile hop and the loss of a trip, so it is tried with every
/// forgery a hop could attempt, against a genuine receipt from this same server.
fn forged_receipt(cfg: &Config, t: &Target) -> Outcome {
    let c = &t.client;
    let key = t.key.clone()?;
    let a = multi_chunk(cfg, t, "forge-a", 22)?;
    let b = multi_chunk(cfg, t, "forge-b", 23)?;

    let genuine = c
        .sync(&a, &key, &mut Injector::none())
        .map_err(|e| format!("could not earn a genuine receipt to forge from: {e}"))?;
    let good = Receipt::from_cbor(&genuine.receipt_bytes).map_err(|e| e.to_string())?;
    good.verify_acknowledges(&key, &a.manifest.content_root)
        .map_err(|e| format!("the genuine receipt does not verify: {e}"))?;

    let mut refused = Vec::new();

    // Each forgery must fail, and for the reason that makes it a forgery.
    let mut check = |name: &str, bytes: Vec<u8>, want_root: &[u8; 32], bad_sig: bool| {
        match Receipt::from_cbor(&bytes).and_then(|r| r.verify_acknowledges(&key, want_root)) {
            Ok(()) => Err(format!("{name}: the dongle ACCEPTED a forged receipt")),
            Err(FormatError::BadSignature) if bad_sig => {
                refused.push(name.to_string());
                Ok(())
            }
            Err(FormatError::ReceiptRootMismatch { .. }) if !bad_sig => {
                refused.push(name.to_string());
                Ok(())
            }
            // Malformed or non-canonical bytes are refused before the signature is even
            // reached, which is also a refusal; only the unexpected acceptance is a failure.
            Err(FormatError::NonCanonicalCbor(_))
            | Err(FormatError::Malformed(_))
            | Err(FormatError::TruncatedCbor)
            | Err(FormatError::UnsupportedCbor(_)) => {
                refused.push(format!("{name} (malformed)"));
                Ok(())
            }
            Err(e) => Err(format!("{name}: refused, but for the wrong reason: {e}")),
        }
    };

    // 1. a flipped signature bit
    let mut r = good.clone();
    r.signature[0] ^= 1;
    check(
        "flipped signature",
        r.to_cbor().map_err(|e| e.to_string())?,
        &a.manifest.content_root,
        true,
    )?;

    // 2. the content root altered under the original signature
    let mut r = good.clone();
    r.content_root = b.manifest.content_root;
    check(
        "altered content root",
        r.to_cbor().map_err(|e| e.to_string())?,
        &b.manifest.content_root,
        true,
    )?;

    // 3. a receipt another key signed, claiming to be the server's
    let attacker = SigningKey::from_bytes(b"cairn-forged-receipt-attacker-ke");
    let mut r = good.clone();
    let signed = r.sign(&attacker).map_err(|e| e.to_string())?;
    check(
        "signed by another key",
        signed,
        &a.manifest.content_root,
        true,
    )?;

    // 4. a genuine receipt, offered as the acknowledgement of different content
    check(
        "genuine receipt for other content",
        genuine.receipt_bytes.clone(),
        &b.manifest.content_root,
        false,
    )?;

    // 5. one byte flipped anywhere in the encoding
    let mut raw = genuine.receipt_bytes.clone();
    let mid = raw.len() / 2;
    raw[mid] ^= 0x40;
    check("one byte flipped", raw, &a.manifest.content_root, true)?;

    // 6. truncated in transit
    let mut raw = genuine.receipt_bytes.clone();
    raw.truncate(raw.len() - 9);
    check("truncated", raw, &a.manifest.content_root, true)?;

    // The server's side of the same property: it hands out no receipt for what it has not
    // committed. B is offered, never completed.
    c.offer(&b).map_err(|e| e.to_string())?;
    match c.fetch_receipt(&hexid(&b)) {
        Ok(_) => return Err("the server produced a receipt for an uncommitted bundle".into()),
        Err(e) => {
            let (s, body) = rejected(e)?;
            want_code(s, &body, 404, "no_receipt")?;
        }
    }

    Ok(format!(
        "refused: {}; and no server receipt exists for an offered-only bundle",
        refused.join(", ")
    ))
}

/// The offer is where the device's authority is checked: the manifest signature is the
/// device's, so a phone that tampers with the manifest, or presents a signature it does not
/// hold, must be turned away before anything is recorded.
fn forged_offer(cfg: &Config, t: &Target) -> Outcome {
    let c = &t.client;
    let sealed = multi_chunk(cfg, t, "forged-offer", 24)?;
    let id = hexid(&sealed);
    let mut seen = Vec::new();

    let mut expect_refused = |name: &str, manifest: &[u8], sig: &[u8; 64]| -> Result<(), String> {
        let r = c.offer_raw(manifest, sig).map_err(|e| e.to_string())?;
        if r.status / 100 == 2 {
            return Err(format!(
                "{name}: the server accepted the offer ({})",
                r.status
            ));
        }
        want_code(
            r.status,
            &String::from_utf8_lossy(&r.body),
            401,
            "bad_manifest_signature",
        )
        .map_err(|e| format!("{name}: {e}"))?;
        seen.push(name.to_string());
        Ok(())
    };

    // 1. a flipped signature bit
    let mut sig = sealed.signature;
    sig[10] ^= 1;
    expect_refused("flipped signature", &sealed.manifest_bytes, &sig)?;

    // 2. a manifest altered after the device signed it (still a valid, canonical manifest)
    let mut altered = sealed.manifest.clone();
    altered.firmware_version.push('!');
    let altered_bytes = altered.to_cbor().map_err(|e| e.to_string())?;
    expect_refused(
        "manifest altered after signing",
        &altered_bytes,
        &sealed.signature,
    )?;

    // 3. the real manifest, signed by a key that is not the device's
    let attacker = SigningKey::from_bytes(b"cairn-forged-offer-attacker-key!");
    let (bytes, forged_sig) = sealed.manifest.sign(&attacker).map_err(|e| e.to_string())?;
    expect_refused("signed by another key", &bytes, &forged_sig)?;

    // None of that may have left an offer on record.
    match c.fetch_receipt(&id) {
        Ok(_) => {
            Err("a receipt exists for a bundle only ever offered with forged signatures".into())
        }
        Err(e) => {
            let (s, body) = rejected(e)?;
            want_code(s, &body, 404, "unknown_bundle")?;
            Ok(format!(
                "refused with 401 bad_manifest_signature: {}; nothing recorded",
                seen.join(", ")
            ))
        }
    }
}

// ── the caller ──────────────────────────────────────────────────────────────

/// A harmless authenticated probe: a receipt for a bundle nobody offered. An authenticated
/// caller gets 404 unknown_bundle; an unauthenticated one never reaches the handler.
const PROBE: &str = "/00000000000000000000000000000001/receipt";

fn expect_401(name: &str, r: relay::Raw) -> Result<(), String> {
    if r.status == 401 && r.code() == "unauthenticated" {
        Ok(())
    } else {
        Err(format!(
            "{name}: expected 401 unauthenticated, got {} {:?}",
            r.status,
            r.code()
        ))
    }
}

fn unauthenticated_requests(_cfg: &Config, t: &Target) -> Outcome {
    let c = &t.client;
    let phone = c.phone().ok_or("not a relay target")?;

    // Control: the same probe, properly signed, is answered by the handler.
    let ok = c
        .send("GET", PROBE, &[], &[], &AuthOverrides::default())
        .map_err(|e| e.to_string())?;
    if !(ok.status == 404 && ok.code() == "unknown_bundle") {
        return Err(format!(
            "the control probe was not answered by the handler: {} {:?}",
            ok.status,
            ok.code()
        ));
    }

    let target = format!("/v1/relay/bundles{PROBE}");
    let raw = |auth: Option<String>| {
        relay::raw_call(
            &agent(),
            c.base(),
            "GET",
            &target,
            auth.as_deref(),
            &[],
            &[],
        )
    };

    expect_401("no Authorization", raw(None)?)?;
    expect_401(
        "malformed Authorization",
        raw(Some("Cairn-Sig nonsense".into()))?,
    )?;

    // A stranger's key under this client's name.
    let stranger = AuthOverrides {
        key: Some(relay::random_key()),
        ..Default::default()
    };
    expect_401(
        "another key, this client id",
        raw(Some(phone.authorization("GET", &target, &[], &stranger)))?,
    )?;

    // An id nobody enrolled.
    let unknown = AuthOverrides {
        client_id: Some("ffffffffffffffffffffffffffffffff".into()),
        ..Default::default()
    };
    expect_401(
        "unknown client id",
        raw(Some(phone.authorization("GET", &target, &[], &unknown)))?,
    )?;

    // Outside the timestamp window, both directions (the window is 120 s).
    for (name, skew) in [("an hour old", -3600), ("an hour ahead", 3600)] {
        let o = AuthOverrides {
            ts: Some(relay::now_unix() + skew),
            ..Default::default()
        };
        expect_401(
            name,
            raw(Some(phone.authorization("GET", &target, &[], &o)))?,
        )?;
    }

    // Signed for one body, sent with another: a chunk swapped in flight.
    let swapped = relay::raw_call(
        &agent(),
        c.base(),
        "POST",
        "/v1/relay/bundles/00000000000000000000000000000001/commit",
        Some(&phone.authorization(
            "POST",
            "/v1/relay/bundles/00000000000000000000000000000001/commit",
            b"the body that was signed",
            &AuthOverrides::default(),
        )),
        &[],
        b"a different body",
    )?;
    expect_401("body swapped after signing", swapped)?;

    // Signed for another path: a signature for GET receipt reused as a commit.
    let other_path = relay::raw_call(
        &agent(),
        c.base(),
        "POST",
        "/v1/relay/bundles/00000000000000000000000000000001/commit",
        Some(&phone.authorization("GET", &target, &[], &AuthOverrides::default())),
        &[],
        &[],
    )?;
    expect_401("signature for another method and path", other_path)?;

    Ok(
        "refused with 401 unauthenticated: no header, malformed header, another key, unknown \
        client, an hour old, an hour ahead, body swapped, signature for another request"
            .into(),
    )
}

fn replayed_request(_cfg: &Config, t: &Target) -> Outcome {
    let c = &t.client;
    let phone = c.phone().ok_or("not a relay target")?;
    let target = format!("/v1/relay/bundles{PROBE}");

    // One captured request, sent twice, byte for byte.
    // (Waiting out the per-client rate limit first, with fresh signatures: a 429 is not what
    // this row is about, and a refused request still spends its nonce.)
    let mut header = phone.authorization("GET", &target, &[], &AuthOverrides::default());
    let go = |h: &str| relay::raw_call(&agent(), c.base(), "GET", &target, Some(h), &[], &[]);
    let mut first = go(&header)?;
    for _ in 0..100 {
        if first.status != 429 {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(250));
        header = phone.authorization("GET", &target, &[], &AuthOverrides::default());
        first = go(&header)?;
    }
    if !(first.status == 404 && first.code() == "unknown_bundle") {
        return Err(format!(
            "the first send was not answered by the handler: {} {:?}",
            first.status,
            first.code()
        ));
    }
    expect_401("the identical request, replayed", go(&header)?)?;

    // A fresh nonce is a fresh request: replay protection is not a lockout.
    let again = c
        .send("GET", PROBE, &[], &[], &AuthOverrides::default())
        .map_err(|e| e.to_string())?;
    if again.status != 404 {
        return Err(format!(
            "after the replay was refused a new, properly signed request got {}",
            again.status
        ));
    }
    Ok(
        "first send answered 404 unknown_bundle, the byte-identical replay 401 unauthenticated, \
        a fresh nonce answered normally"
            .into(),
    )
}

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout(std::time::Duration::from_secs(30))
        .build()
}
