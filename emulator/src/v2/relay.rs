//! The phone, as the server's relay sees it.
//!
//! The real path is dongle -> BLE -> phone -> `/v1/relay/bundles/*` (front door #7 retires
//! the device listener). The relay adds no trust decisions of its own: the device's
//! manifest signature, the assignment, the counter guard and the content root are still
//! the intake service's. What it adds is authentication of the CALLER, which is what this
//! module is: an enrolled app client with a P-256 key, signing every request.
//!
//! The wire format is `contracts/sync/v1/spec.md` and the server's `internal/syncapi`
//! (`sign.go`, `relay.go`): the string signed is
//!
//! ```text
//! CAIRN-SIG-V1 \n METHOD \n request-target \n ts \n nonce \n hex(sha256(body)) \n client_id
//! ```
//!
//! with an ASN.1 DER ECDSA-P256-SHA256 signature, carried as
//! `Authorization: Cairn-Sig client="..",ts="..",nonce="..",sig="<base64>"`.
//! This is an independent implementation of that, written from the spec and the server's
//! reference signer; a disagreement shows up as a 401 on the first request.

use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as B64;
use p256::ecdsa::{Signature, SigningKey, signature::Signer};
use rand::RngCore;
use sha2::{Digest, Sha256};

use crate::format;

/// An enrolled phone: the client id the server issued and the key it enrolled.
#[derive(Clone)]
pub struct Phone {
    pub client_id: String,
    key: SigningKey,
}

/// What the phone's request carries, spelled out so a fault row can alter exactly one
/// part of an otherwise valid request.
#[derive(Clone, Default)]
pub struct AuthOverrides {
    /// Signed timestamp (unix seconds). Default: now.
    pub ts: Option<i64>,
    /// Nonce. Default: 128 random bits.
    pub nonce: Option<String>,
    /// Sign these bytes as the body while sending different ones (a swapped body).
    pub signed_body: Option<Vec<u8>>,
    /// Claim this client id (a stranger's key under someone else's name).
    pub client_id: Option<String>,
    /// Sign with this key instead of the phone's.
    pub key: Option<SigningKey>,
}

pub fn now_unix() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

pub fn new_nonce() -> String {
    let mut n = [0u8; 16];
    rand::rngs::OsRng.fill_bytes(&mut n);
    format::hex(&n)
}

/// A fresh P-256 key, for a row that needs a stranger's.
pub fn random_key() -> SigningKey {
    SigningKey::random(&mut rand::rngs::OsRng)
}

/// The exact byte string a request signature covers (`contracts/sync/v1/spec.md` section 2).
pub fn signing_string(
    method: &str,
    request_target: &str,
    ts: &str,
    nonce: &str,
    body: &[u8],
    client_id: &str,
) -> String {
    format!(
        "CAIRN-SIG-V1\n{method}\n{request_target}\n{ts}\n{nonce}\n{}\n{client_id}",
        format::hex(&Sha256::digest(body)),
    )
}

/// What the enrolling key signs to prove it is held (`spec.md` section 3).
pub fn enrol_proof_message(code: &str, public_key_hex: &str) -> String {
    format!("CAIRN-ENROLL-V1\n{code}\n{}", public_key_hex.to_lowercase())
}

fn public_hex(key: &SigningKey) -> String {
    format::hex(key.verifying_key().to_encoded_point(false).as_bytes())
}

fn der(key: &SigningKey, msg: &[u8]) -> Vec<u8> {
    let sig: Signature = key.sign(msg);
    sig.to_der().as_bytes().to_vec()
}

impl Phone {
    /// Enrol with a one-time invitation (`cairn-admin client invite`).
    ///
    /// The proof of possession signs the code with the new public key, so the server
    /// knows the enrolled key is one this phone actually holds.
    pub fn enrol(base: &str, code: &str, name: &str) -> Result<Self, String> {
        let norm: String = code
            .chars()
            .filter(|c| !c.is_whitespace() && *c != '-')
            .collect::<String>()
            .to_lowercase();
        let key = random_key();
        let pub_hex = public_hex(&key);
        let proof = der(&key, enrol_proof_message(&norm, &pub_hex).as_bytes());
        let body = serde_json::json!({
            "code": norm,
            "name": name,
            "public_key": pub_hex,
            "proof": B64.encode(proof),
        });

        let url = format!("{}/v1/enroll/app", base.trim_end_matches('/'));
        let resp = ureq::post(&url).send_json(body).map_err(|e| match e {
            ureq::Error::Status(s, r) => {
                format!(
                    "enrolment refused ({s}): {}",
                    r.into_string().unwrap_or_default()
                )
            }
            e => e.to_string(),
        })?;
        let out: serde_json::Value = resp.into_json().map_err(|e| e.to_string())?;
        let client_id = out
            .get("client_id")
            .and_then(|v| v.as_str())
            .ok_or("enrolment response has no client_id")?
            .to_string();
        Ok(Self { client_id, key })
    }

    /// Persist the phone so a re-run against a persistent server reuses the enrolment
    /// (an invitation is single use). The file holds a private key: test keys only.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let doc = serde_json::json!({
            "client_id": self.client_id,
            "private_key_hex": format::hex(&self.key.to_bytes()),
        });
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::write(path, serde_json::to_vec_pretty(&doc)?)
    }

    pub fn load(path: &Path) -> Result<Self, String> {
        let raw = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let v: serde_json::Value = serde_json::from_slice(&raw).map_err(|e| e.to_string())?;
        let client_id = v["client_id"].as_str().ok_or("no client_id")?.to_string();
        let d = format::unhex_array::<32>(v["private_key_hex"].as_str().ok_or("no key")?)
            .ok_or("private_key_hex must be 64 hex characters")?;
        let key = SigningKey::from_bytes((&d).into()).map_err(|e| e.to_string())?;
        Ok(Self { client_id, key })
    }

    /// The `Authorization` header value for one request.
    pub fn authorization(
        &self,
        method: &str,
        request_target: &str,
        body: &[u8],
        o: &AuthOverrides,
    ) -> String {
        let ts = o.ts.unwrap_or_else(now_unix).to_string();
        let nonce = o.nonce.clone().unwrap_or_else(new_nonce);
        let client = o
            .client_id
            .clone()
            .unwrap_or_else(|| self.client_id.clone());
        let signed_body = o.signed_body.as_deref().unwrap_or(body);
        let msg = signing_string(method, request_target, &ts, &nonce, signed_body, &client);
        let key = o.key.as_ref().unwrap_or(&self.key);
        format!(
            "Cairn-Sig client=\"{client}\",ts=\"{ts}\",nonce=\"{nonce}\",sig=\"{}\"",
            B64.encode(der(key, msg.as_bytes()))
        )
    }
}

/// A response seen from the relay, whatever its status.
#[derive(Debug, Clone)]
pub struct Raw {
    pub status: u16,
    pub body: Vec<u8>,
    pub headers: Vec<(String, String)>,
}

impl Raw {
    /// The server's machine-readable error code (`{"error": "..."}`), if any.
    pub fn code(&self) -> String {
        serde_json::from_slice::<serde_json::Value>(&self.body)
            .ok()
            .and_then(|v| v.get("error").and_then(|e| e.as_str().map(String::from)))
            .unwrap_or_default()
    }

    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v.as_str())
    }
}

/// Send one request and return the answer whatever the status, so a row can assert on a
/// refusal as precisely as on a success. `authorization` of `None` sends none.
pub fn raw_call(
    agent: &ureq::Agent,
    base: &str,
    method: &str,
    request_target: &str,
    authorization: Option<&str>,
    headers: &[(&str, String)],
    body: &[u8],
) -> Result<Raw, String> {
    let url = format!("{}{}", base.trim_end_matches('/'), request_target);
    let mut req = agent.request(method, &url);
    if let Some(a) = authorization {
        req = req.set("Authorization", a);
    }
    for (k, v) in headers {
        req = req.set(k, v);
    }
    let resp = match req.send_bytes(body) {
        Ok(r) => r,
        Err(ureq::Error::Status(_, r)) => r,
        Err(e) => return Err(e.to_string()),
    };
    let status = resp.status();
    let headers = resp
        .headers_names()
        .into_iter()
        .filter_map(|n| resp.header(&n).map(|v| (n.clone(), v.to_string())))
        .collect();
    let mut buf = Vec::new();
    use std::io::Read;
    resp.into_reader()
        .read_to_end(&mut buf)
        .map_err(|e| format!("read body: {e}"))?;
    Ok(Raw {
        status,
        body: buf,
        headers,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use p256::ecdsa::{VerifyingKey, signature::Verifier};

    /// The pinned vectors (`contracts/sync/v1/vectors/vectors.json`). A missing file is a
    /// failure, not a skip: a skipped test looks exactly like a passing one.
    fn vectors() -> serde_json::Value {
        let path = crate::contracts_dir().join("sync/v1/vectors/vectors.json");
        let raw = std::fs::read(&path).unwrap_or_else(|e| {
            panic!(
                "cannot read {}: {e} (run scripts/fetch-contracts.sh or set CAIRN_CONTRACTS)",
                path.display()
            )
        });
        serde_json::from_slice(&raw).expect("vectors.json is JSON")
    }

    fn verifies(public_hex: &str, message: &[u8], der_b64: &str) -> bool {
        let pubkey = VerifyingKey::from_sec1_bytes(&format::unhex(public_hex).unwrap()).unwrap();
        let sig = Signature::from_der(&B64.decode(der_b64).unwrap()).unwrap();
        pubkey.verify(message, &sig).is_ok()
    }

    /// Agreeing on the string AND on verification is what proves compatibility with the
    /// server (the vectors' own instruction): ECDSA is randomised, so our signature cannot
    /// equal theirs, only verify under the same key.
    #[test]
    fn request_signing_agrees_with_the_pinned_vectors() {
        let v = vectors();
        let scalar =
            format::unhex_array::<32>(v["test_key"]["private_scalar_hex"].as_str().unwrap())
                .unwrap();
        let public = v["test_key"]["public_key_x963_hex"].as_str().unwrap();
        let client = v["test_key"]["client_id"].as_str().unwrap();
        let phone = Phone {
            client_id: client.to_string(),
            key: SigningKey::from_bytes((&scalar).into()).unwrap(),
        };
        assert_eq!(
            public_hex(&phone.key),
            public,
            "the test scalar must give the test public key"
        );

        let cases = v["signing"].as_array().unwrap();
        assert!(cases.len() >= 4, "expected the four pinned signing vectors");
        for c in cases {
            let name = c["name"].as_str().unwrap();
            let body = format::unhex(c["body_hex"].as_str().unwrap()).unwrap();
            let method = c["method"].as_str().unwrap();
            let target = c["request_target"].as_str().unwrap();
            let ts = c["ts"].as_str().unwrap();
            let nonce = c["nonce"].as_str().unwrap();

            // our string is theirs, byte for byte
            let ours = signing_string(method, target, ts, nonce, &body, client);
            assert_eq!(
                ours,
                c["signing_string"].as_str().unwrap(),
                "{name}: signing string"
            );

            // their signature verifies over our string
            assert!(
                verifies(
                    public,
                    ours.as_bytes(),
                    c["signature_der_base64"].as_str().unwrap()
                ),
                "{name}: the pinned signature does not verify over our signing string"
            );

            // our header has their shape and its signature verifies over their string
            let o = AuthOverrides {
                ts: Some(ts.parse().unwrap()),
                nonce: Some(nonce.to_string()),
                ..Default::default()
            };
            let header = phone.authorization(method, target, &body, &o);
            let prefix =
                format!("Cairn-Sig client=\"{client}\",ts=\"{ts}\",nonce=\"{nonce}\",sig=\"");
            let sig = header
                .strip_prefix(&prefix)
                .unwrap_or_else(|| panic!("{name}: header shape: {header}"));
            let sig = sig.strip_suffix('"').unwrap();
            assert!(
                verifies(
                    public,
                    c["signing_string"].as_str().unwrap().as_bytes(),
                    sig
                ),
                "{name}: our signature does not verify over the pinned signing string"
            );
        }
    }

    #[test]
    fn enrolment_proof_agrees_with_the_pinned_vector() {
        let v = vectors();
        let e = &v["enrolment"];
        let code = e["code"].as_str().unwrap();
        let public = e["public_key_hex"].as_str().unwrap();
        assert_eq!(
            enrol_proof_message(code, public),
            e["proof_message"].as_str().unwrap()
        );
        assert!(
            verifies(
                public,
                enrol_proof_message(code, public).as_bytes(),
                e["proof_der_base64"].as_str().unwrap()
            ),
            "the pinned enrolment proof does not verify over our message"
        );
    }

    /// A signature must not verify for a different request: the body, the target, the
    /// method and the client id each change the signed string.
    #[test]
    fn every_part_of_a_request_is_signed() {
        let base = signing_string("GET", "/v1/x", "1", "n", b"", "c");
        for other in [
            signing_string("PUT", "/v1/x", "1", "n", b"", "c"),
            signing_string("GET", "/v1/y", "1", "n", b"", "c"),
            signing_string("GET", "/v1/x", "2", "n", b"", "c"),
            signing_string("GET", "/v1/x", "1", "m", b"", "c"),
            signing_string("GET", "/v1/x", "1", "n", b"b", "c"),
            signing_string("GET", "/v1/x", "1", "n", b"", "d"),
        ] {
            assert_ne!(base, other);
        }
    }
}
