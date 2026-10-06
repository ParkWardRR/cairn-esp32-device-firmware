//! The property matrix.
//!
//! Each row states a property the architecture claims, arms a fault that would
//! violate it, and asserts what survived. Assertions are made against the
//! device's durable state and the server's reported state — never against
//! stdout, because a log line proves nothing about what is on disk.
//!
//! Every run is reproducible from its seed, which is printed on failure. The
//! seed also drives every frame nonce (see `DeviceStore::open`), so a failing
//! run reproduces byte for byte, ciphertext included.
//!
//! A simulated device is two directories: its **card** and its **NVS**. Only
//! the card is ever damaged, truncated, swapped or restored by a row; NVS holds
//! the bundle counter, and keeping it off the card is what several v3 rows
//! exist to prove matters.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use ed25519_dalek::{SigningKey, VerifyingKey};
use sha2::{Digest, Sha256};

use crate::format::{self, StopReason, segment::parse_segment_header};

use super::fault::{FaultPlan, FaultPoint, Injector};
use super::relay::Phone;
use super::store::{
    CaptureSpec, DeviceIdentity, DeviceStore, SealedBundle, StoreError, gnss_payload,
};
use super::sync::{SyncClient, SyncError};

/// A fixed device key, so a device identity survives a simulated reboot.
const DEVICE_KEY_SEED: &[u8; 32] = b"cairn-intake-test-device-key-see";

/// The default storage root `K_root`. A **public test key**, in the same spirit
/// as the conformance vectors' keys: it protects nothing. It is a fixed value
/// rather than random so a server can escrow it for the protocol rows
/// (`cairn-server -enroll-root <hex of these bytes>`).
pub const DEFAULT_ROOT_KEY: &[u8; 32] = b"cairn-emulator-public-test-root!";

/// Default vehicle and assignment for the local rows. Protocol rows against a
/// real server must be given the ids that server issued (`--vehicle-id`,
/// `--assignment-id`), since intake refuses an assignment it did not issue.
pub const DEFAULT_VEHICLE_ID: [u8; 16] = ids(0x60);
pub const DEFAULT_ASSIGNMENT_ID: [u8; 16] = ids(0x70);

const fn ids(first: u8) -> [u8; 16] {
    let mut id = [0u8; 16];
    let mut i = 0;
    while i < 16 {
        id[i] = first.wrapping_add(i as u8);
        i += 1;
    }
    id
}

/// Outcome of one matrix row.
#[derive(Debug, Clone)]
pub struct RowResult {
    pub family: String,
    pub property: String,
    pub detail: String,
    pub passed: bool,
    pub skipped: bool,
    pub seed: u64,
}

impl RowResult {
    pub(super) fn pass(family: &str, property: &str, detail: String, seed: u64) -> Self {
        Self {
            family: family.into(),
            property: property.into(),
            detail,
            passed: true,
            skipped: false,
            seed,
        }
    }

    pub(super) fn fail(family: &str, property: &str, detail: String, seed: u64) -> Self {
        Self {
            family: family.into(),
            property: property.into(),
            detail,
            passed: false,
            skipped: false,
            seed,
        }
    }

    pub(super) fn skip(family: &str, property: &str, why: String) -> Self {
        Self {
            family: family.into(),
            property: property.into(),
            detail: why,
            passed: true,
            skipped: true,
            seed: 0,
        }
    }
}

pub struct MatrixReport {
    pub rows: Vec<RowResult>,
}

impl MatrixReport {
    pub fn passed(&self) -> usize {
        self.rows.iter().filter(|r| r.passed && !r.skipped).count()
    }
    pub fn failed(&self) -> usize {
        self.rows.iter().filter(|r| !r.passed).count()
    }
    pub fn skipped(&self) -> usize {
        self.rows.iter().filter(|r| r.skipped).count()
    }
    pub fn ok(&self) -> bool {
        self.failed() == 0
    }
}

pub struct Config {
    /// Working directory for simulated device storage.
    pub work_dir: PathBuf,
    /// Ingest server base URL. When empty, network rows are skipped.
    pub server: String,
    pub seed: u64,
    pub chunk_size: usize,
    pub verbose: bool,
    /// The provisioned identity of the simulated device under test.
    pub identity: IdentityConfig,
    /// The phone relay to run the protocol rows against, as an enrolled app client.
    /// The legacy `server` and this are independent: either, both or neither.
    pub relay: Option<RelayConfig>,
}

/// An authenticated client acting as the phone, against `/v1/relay/bundles/*`.
#[derive(Clone)]
pub struct RelayConfig {
    /// The app API's base URL (not the device listener's).
    pub base: String,
    pub phone: Phone,
    /// The server's receipt-signing public key, as a dongle pins it at provisioning
    /// (`cairn-server -print-receipt-key`). Absent: fetched from the legacy listener
    /// when `server` is also given, otherwise the relay rows fail saying so.
    pub receipt_key: Option<[u8; 32]>,
}

/// One server path the protocol rows run against: the same rows, a different wire.
pub struct Target {
    /// "" for the legacy listener, "relay/" for the phone relay. Prefixes row families,
    /// so a report shows which path a row ran on.
    pub label: &'static str,
    pub client: SyncClient,
    /// The pinned receipt key, or why there is none.
    pub key: std::result::Result<VerifyingKey, String>,
    /// Added to every bundle id, so two paths against one server never offer different
    /// content under one id (intake would quarantine the second, correctly).
    pub id_offset: u8,
}

impl Target {
    pub fn family(&self, name: &str) -> String {
        format!("{}{name}", self.label)
    }

    /// A row's working-directory name, distinct per path.
    pub fn tag(&self, name: &str) -> String {
        format!("{}{name}", self.label.replace('/', "-"))
    }

    pub fn bundle_id(&self, n: u8) -> [u8; 16] {
        bundle_id(n.wrapping_add(self.id_offset))
    }
}

fn legacy_target(cfg: &Config) -> Target {
    let client = SyncClient::new(&cfg.server);
    let key = client.fetch_receipt_key().map_err(|e| e.to_string());
    Target {
        label: "",
        client,
        key,
        id_offset: 0,
    }
}

fn relay_target(cfg: &Config, r: &RelayConfig) -> Target {
    let client = SyncClient::relay(&r.base, r.phone.clone()).with_backlog_base(&cfg.server);
    let key = match (&r.receipt_key, cfg.server.is_empty()) {
        (Some(raw), _) => VerifyingKey::from_bytes(raw).map_err(|e| format!("--receipt-key: {e}")),
        (None, false) => SyncClient::new(&cfg.server)
            .fetch_receipt_key()
            .map_err(|e| e.to_string()),
        (None, true) => Err(
            "the relay has no receipt-key endpoint: pass --receipt-key (cairn-server \
             -print-receipt-key) or --server to fetch it from the legacy listener"
                .into(),
        ),
    };
    Target {
        label: "relay/",
        client,
        key,
        id_offset: 0x40,
    }
}

/// What provisioning gives the device: its storage root and key version, and
/// the vehicle assignment it captures under.
#[derive(Clone)]
pub struct IdentityConfig {
    pub root_key: [u8; 32],
    pub storage_key_version: u32,
    pub vehicle_id: [u8; 16],
    pub assignment_id: [u8; 16],
}

impl Default for IdentityConfig {
    fn default() -> Self {
        Self {
            root_key: *DEFAULT_ROOT_KEY,
            storage_key_version: 1,
            vehicle_id: DEFAULT_VEHICLE_ID,
            assignment_id: DEFAULT_ASSIGNMENT_ID,
        }
    }
}

fn device_key() -> SigningKey {
    SigningKey::from_bytes(DEVICE_KEY_SEED)
}

/// The device under test.
fn identity(cfg: &Config) -> DeviceIdentity {
    DeviceIdentity {
        signing_key: device_key(),
        device_id: device_id(),
        root_key: cfg.identity.root_key,
        storage_key_version: cfg.identity.storage_key_version,
        vehicle_id: cfg.identity.vehicle_id,
        assignment_id: cfg.identity.assignment_id,
    }
}

/// A different, genuinely provisioned device: its own id, signing key and
/// storage root, under the same key version so that reading its card is a
/// question of authentication rather than of version selection.
fn other_device(cfg: &Config) -> DeviceIdentity {
    DeviceIdentity {
        signing_key: SigningKey::from_bytes(b"cairn-matrix-other-device-key-se"),
        device_id: ids(0x20),
        root_key: *b"cairn-matrix-other-device-root!!",
        storage_key_version: cfg.identity.storage_key_version,
        vehicle_id: ids(0x80),
        assignment_id: ids(0x90),
    }
}

/// Boot the device under test over `<root>/card` and `<root>/nvs`.
fn boot(cfg: &Config, root: &Path, boot_n: u8) -> std::io::Result<DeviceStore> {
    boot_as(
        cfg,
        identity(cfg),
        &root.join("card"),
        &root.join("nvs"),
        boot_n,
    )
}

fn boot_as(
    cfg: &Config,
    id: DeviceIdentity,
    card: &Path,
    nvs: &Path,
    boot_n: u8,
) -> std::io::Result<DeviceStore> {
    DeviceStore::open(card, nvs, id, boot_id(boot_n), cfg.seed)
        .map_err(|e| std::io::Error::other(e.to_string()))
}

fn device_id() -> [u8; 16] {
    let mut id = [0u8; 16];
    for (i, b) in id.iter_mut().enumerate() {
        *b = 0x10 + i as u8;
    }
    id
}

pub(super) fn bundle_id(n: u8) -> [u8; 16] {
    let mut id = [0u8; 16];
    for (i, b) in id.iter_mut().enumerate() {
        *b = n.wrapping_add(i as u8);
    }
    id
}

fn boot_id(n: u8) -> [u8; 16] {
    let mut id = [0u8; 16];
    for (i, b) in id.iter_mut().enumerate() {
        *b = 0xA0u8.wrapping_add(n).wrapping_add(i as u8);
    }
    id
}

/// Run the matrix.
pub fn run(cfg: &Config) -> std::io::Result<MatrixReport> {
    let mut rows = Vec::new();

    // ── local durability: these need no server ───────────────────────────────
    rows.extend(power_cut_during_capture(cfg)?);
    rows.push(power_cut_during_seal(cfg)?);
    rows.push(torn_tail_is_isolated(cfg)?);
    rows.push(corrupted_storage_is_detected(cfg)?);
    rows.push(clock_jump_preserves_ordering(cfg)?);
    rows.push(prune_requires_receipt(cfg)?);
    rows.push(reboot_between_receipt_and_prune(cfg)?);
    rows.extend(prune_is_transactional(cfg)?);

    // ── v3: encryption, identity and the off-card counter ────────────────────
    rows.push(torn_tail_under_encryption(cfg)?);
    rows.push(card_restored_to_older_image(cfg)?);
    rows.push(card_from_another_device(cfg)?);
    rows.push(counter_survives_cut_mid_seal(cfg)?);

    // ── protocol: these need a running server ────────────────────────────────
    if cfg.server.is_empty() {
        for (family, property) in [
            (
                "network-loss-per-chunk",
                "device eventually obtains a valid receipt",
            ),
            (
                "corrupt-chunk-in-transit",
                "rejected; retry succeeds without operator action",
            ),
            ("duplicate-upload", "immutable bundle stored exactly once"),
            (
                "receipt-lost-in-transit",
                "a retry returns the same receipt",
            ),
        ] {
            rows.push(RowResult::skip(
                family,
                property,
                "no --server given".into(),
            ));
        }
    } else {
        // Kept until front door #7 removes the listener.
        let t = legacy_target(cfg);
        rows.extend(network_loss_per_chunk(cfg, &t));
        rows.push(corrupt_chunk_in_transit(cfg, &t));
        rows.push(duplicate_upload_stored_once(cfg, &t));
        rows.push(receipt_lost_in_transit(cfg, &t));
    }

    // ── protocol, over the phone relay: the real path ────────────────────────
    // The same four rows as above, then the rows only the relay can have: an
    // authenticated caller, and chunks that arrive in the wrong order.
    if let Some(r) = &cfg.relay {
        let t = relay_target(cfg, r);
        rows.extend(network_loss_per_chunk(cfg, &t));
        rows.push(corrupt_chunk_in_transit(cfg, &t));
        rows.push(duplicate_upload_stored_once(cfg, &t));
        rows.push(receipt_lost_in_transit(cfg, &t));
        rows.extend(super::relay_matrix::rows(cfg, &t));
    }

    Ok(MatrixReport { rows })
}

/// Fresh storage root per row, so rows cannot contaminate each other.
fn fresh_root(cfg: &Config, name: &str) -> std::io::Result<PathBuf> {
    let root = cfg.work_dir.join(name);
    if root.exists() {
        fs::remove_dir_all(&root)?;
    }
    fs::create_dir_all(&root)?;
    Ok(root)
}

// ── power cut during capture ────────────────────────────────────────────────

/// Property: every boot resumes or discards only an incomplete tail; no frame
/// that was durably written ever vanishes.
///
/// The fault is armed after each of several frame boundaries, because a key-off
/// can land anywhere.
fn power_cut_during_capture(cfg: &Config) -> std::io::Result<Vec<RowResult>> {
    const FAMILY: &str = "power-cut-during-capture";
    const PROPERTY: &str =
        "frames durably written are recoverable; only an incomplete tail is lost";

    let mut rows = Vec::new();

    for cut_after in [1usize, 5, 12, 19] {
        let root = fresh_root(cfg, &format!("capture-cut-{cut_after}"))?;
        let store = boot(cfg, &root, 0)?;

        let mut inj = Injector::armed(
            FaultPlan::interrupt(FaultPoint::AfterFrameWrite(cut_after)).with_seed(cfg.seed),
        );

        let spec = CaptureSpec::new(bundle_id(1)).with_chunk_size(cfg.chunk_size);
        let outcome = store.capture_and_seal(&spec, &mut inj);

        if outcome.is_ok() {
            rows.push(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("the fault at frame {cut_after} never fired"),
                cfg.seed,
            ));
            continue;
        }

        // Reboot: a new store over the same directory, running recovery.
        let rebooted = boot(cfg, &root, 1)?;
        let recovered = rebooted
            .recover_at_boot()
            .map_err(|e| std::io::Error::other(e.to_string()))?;

        if recovered.len() != 1 {
            rows.push(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("recovery found {} bundles, want 1", recovered.len()),
                cfg.seed,
            ));
            continue;
        }

        let r = &recovered[0];

        // Every frame written before the cut must be recoverable. The interrupt
        // syncs before returning, so all of them are durable.
        if r.recovered_frames != cut_after {
            rows.push(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "cut after frame {cut_after}: recovered {} frames, want {cut_after} \
                     (stop {}, {} bytes discarded)",
                    r.recovered_frames, r.stop, r.discarded_tail_bytes
                ),
                cfg.seed,
            ));
            continue;
        }

        // An unsealed bundle must not claim to be sealed.
        if r.sealed || r.manifest_present {
            rows.push(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "cut during capture left the bundle looking sealed (sealed={}, manifest={})",
                    r.sealed, r.manifest_present
                ),
                cfg.seed,
            ));
            continue;
        }

        rows.push(RowResult::pass(
            FAMILY,
            PROPERTY,
            format!("cut after frame {cut_after}: all {cut_after} frames recovered, not sealed"),
            cfg.seed,
        ));
    }

    Ok(rows)
}

// ── power cut during sealing ───────────────────────────────────────────────

/// Property: segments are never discarded because sealing failed. A crash
/// during sealing leaves the raw data intact and the bundle simply unsealed, so
/// sealing is re-run at boot.
fn power_cut_during_seal(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "power-cut-during-seal";
    const PROPERTY: &str = "sealing failure never discards captured data";

    for point in [
        FaultPoint::AfterSegmentSync,
        FaultPoint::BeforeManifestSync,
        FaultPoint::AfterManifestSync,
    ] {
        let root = fresh_root(cfg, &format!("seal-{}", point.label()))?;
        let store = boot(cfg, &root, 0)?;

        let mut inj = Injector::armed(FaultPlan::interrupt(point).with_seed(cfg.seed));
        let spec = CaptureSpec::new(bundle_id(2)).with_chunk_size(cfg.chunk_size);
        let outcome = store.capture_and_seal(&spec, &mut inj);

        if outcome.is_ok() {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("the fault at {point} never fired"),
                cfg.seed,
            ));
        }

        let rebooted = boot(cfg, &root, 1)?;
        let recovered = rebooted
            .recover_at_boot()
            .map_err(|e| std::io::Error::other(e.to_string()))?;

        if recovered.len() != 1 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{point}: recovery found {} bundles, want 1",
                    recovered.len()
                ),
                cfg.seed,
            ));
        }

        let r = &recovered[0];

        // All 25 capture frames were synced before sealing began.
        if r.recovered_frames != 25 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{point}: recovered {} capture frames, want 25 — sealing failure \
                     must not cost captured data",
                    r.recovered_frames
                ),
                cfg.seed,
            ));
        }
        if r.journal_frames != 4 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{point}: recovered {} journal frames, want 4",
                    r.journal_frames
                ),
                cfg.seed,
            ));
        }

        // A bundle interrupted before the sealing rename must not be sealed.
        if r.sealed {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("{point}: the bundle is marked sealed despite the interruption"),
                cfg.seed,
            ));
        }
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        "all 25 capture frames and 4 journal frames survive a cut at each sealing step".into(),
        cfg.seed,
    ))
}

// ── torn tail ──────────────────────────────────────────────────────────────

/// Property: a partial frame on the medium is detected as a torn tail, every
/// preceding frame is retained, and the discarded byte count is exact.
///
/// This is the failure unframed records cannot distinguish from valid data,
/// which is the reason the format has framing at all.
fn torn_tail_is_isolated(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "torn-tail";
    const PROPERTY: &str = "a partial frame is isolated; preceding frames remain usable";

    // A GNSS frame is 68 bytes of sealed envelope (header, nonce, tag, CRC)
    // plus a 32-byte payload.
    let frame_len = format::FRAME_OVERHEAD + 32;

    for drop_bytes in [4usize, 20, 40] {
        let root = fresh_root(cfg, &format!("torn-{drop_bytes}"))?;
        let store = boot(cfg, &root, 0)?;

        let mut inj = Injector::armed(
            FaultPlan::interrupt(FaultPoint::MidFrameWrite {
                frame: 10,
                drop_bytes,
            })
            .with_seed(cfg.seed),
        );

        let spec = CaptureSpec::new(bundle_id(3)).with_chunk_size(cfg.chunk_size);
        let outcome = store.capture_and_seal(&spec, &mut inj);
        if outcome.is_ok() {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("the torn-write fault (dropping {drop_bytes} bytes) never fired"),
                cfg.seed,
            ));
        }

        let rebooted = boot(cfg, &root, 1)?;
        let recovered = rebooted
            .recover_at_boot()
            .map_err(|e| std::io::Error::other(e.to_string()))?;

        let r = &recovered[0];

        // Nine complete frames preceded the torn tenth.
        if r.recovered_frames != 9 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "dropping {drop_bytes} bytes of frame 10: recovered {} frames, want 9 (stop {})",
                    r.recovered_frames, r.stop
                ),
                cfg.seed,
            ));
        }
        if r.stop != StopReason::TornTail {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("stop = {}, want TORN_TAIL", r.stop),
                cfg.seed,
            ));
        }
        // The discarded tail is the part of frame 10 that did land. Reporting
        // it exactly is what lets an operator tell a torn write from real loss.
        let want_discarded = frame_len - drop_bytes;
        if r.discarded_tail_bytes as usize != want_discarded {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "dropping {drop_bytes} of {frame_len} bytes: discarded {} bytes, \
                     want exactly {want_discarded} — the count must be exact",
                    r.discarded_tail_bytes
                ),
                cfg.seed,
            ));
        }
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        "torn tails at 3 truncation depths each retain 9 frames and report the exact byte count"
            .into(),
        cfg.seed,
    ))
}

// ── corrupted storage ──────────────────────────────────────────────────────

/// Property: a flipped bit is detected, isolated to one frame, and every
/// preceding frame remains usable.
fn corrupted_storage_is_detected(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "corrupted-storage-bytes";
    const PROPERTY: &str = "corruption is detected and isolated; preceding records survive";

    let root = fresh_root(cfg, "corrupt-storage")?;
    let store = boot(cfg, &root, 0)?;

    let spec = CaptureSpec::new(bundle_id(4)).with_chunk_size(cfg.chunk_size);
    let sealed = store
        .capture_and_seal(&spec, &mut Injector::none())
        .map_err(|e| std::io::Error::other(e.to_string()))?;

    // Flip a payload bit inside frame index 6 of the first segment.
    let seg_path = sealed.dir.join("seg-00000000.seg");
    let mut bytes = fs::read(&seg_path)?;
    let frame_len = format::FRAME_OVERHEAD + 32;
    let target = format::SEGMENT_HEADER_SIZE + 6 * frame_len + format::FRAME_HEADER_SIZE + 4;
    if target >= bytes.len() {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "segment too small to corrupt the intended frame".into(),
            cfg.seed,
        ));
    }
    bytes[target] ^= 0x01;
    fs::write(&seg_path, &bytes)?;

    let recovered = store
        .recover_at_boot()
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    let r = &recovered[0];

    if r.stop != StopReason::CorruptFrame {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!("stop = {}, want CORRUPT_FRAME", r.stop),
            cfg.seed,
        ));
    }
    if r.recovered_frames != 6 {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!(
                "recovered {} frames, want 6 — corruption at frame 6 must not \
                 invalidate the frames before it",
                r.recovered_frames
            ),
            cfg.seed,
        ));
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        "a single flipped bit at frame 6 is detected there; frames 0-5 remain usable".into(),
        cfg.seed,
    ))
}

// ── clock jump ─────────────────────────────────────────────────────────────

/// Property: ordering stays valid on `(boot_id, seq)` across a UTC
/// discontinuity, because UTC is an annotation rather than an index.
fn clock_jump_preserves_ordering(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "clock-reset-gnss-jump";
    const PROPERTY: &str = "ordering stays valid on (boot_id, seq) across a UTC jump";

    let root = fresh_root(cfg, "clock-jump")?;
    let store = boot(cfg, &root, 0)?;

    // The jump is written during capture, as a real device would: the receiver
    // reports a 30 s backwards discontinuity and the device records it honestly.
    // Patching a frame afterwards would break the CRC chain — which is exactly
    // what the chain exists to detect, and not what a clock jump looks like.
    let spec = CaptureSpec::new(bundle_id(5))
        .with_chunk_size(cfg.chunk_size)
        .with_clock_jump_at(10);

    let sealed = store
        .capture_and_seal(&spec, &mut Injector::none())
        .map_err(|e| std::io::Error::other(e.to_string()))?;

    let bytes = fs::read(sealed.dir.join("seg-00000000.seg"))?;
    // Keyed: the row reads the jumped sample's payload, which only an
    // authenticated scan exposes.
    let scanned = format::scan_segment(
        &bytes,
        format::ScanState::default(),
        Some(store.key_provider()),
    )
    .map_err(|e| std::io::Error::other(e.to_string()))?;

    if scanned.stop != StopReason::Eof {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!(
                "stop = {} ({}) — a clock jump is not corruption",
                scanned.stop, scanned.stop_detail
            ),
            cfg.seed,
        ));
    }

    // Sequence numbers must still be contiguous: ordering truth is
    // (boot_id, seq), and UTC is only an annotation.
    for (i, f) in scanned.frames.iter().enumerate() {
        if f.seq != i as u32 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "frame {i} has seq {}, want {i} — ordering was disturbed",
                    f.seq
                ),
                cfg.seed,
            ));
        }
    }

    let jumped = &scanned.frames[10];
    if jumped.flags & format::frame::flags::ESTIMATED_UTC == 0 {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "the jumped sample does not carry ESTIMATED_UTC".into(),
            cfg.seed,
        ));
    }

    // Its UTC offset must be the backwards value, and its accuracy unknown —
    // never a plausible guess.
    let Some(p) = jumped.payload.as_deref() else {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "the keyed scan exposed no plaintext for the jumped sample".into(),
            cfg.seed,
        ));
    };
    let offset = i32::from_le_bytes([p[26], p[27], p[28], p[29]]);
    let acc = u16::from_le_bytes([p[30], p[31]]);

    if offset >= 0 {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!("the jumped sample's UTC offset is {offset} ms, expected a backwards jump"),
            cfg.seed,
        ));
    }
    if acc != 0xFFFF {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!("utc_acc_ms = {acc}, want 0xFFFF (unknown) — precision must not be invented"),
            cfg.seed,
        ));
    }

    // Monotonic time must still advance across the jump.
    if jumped.monotonic_ms <= scanned.frames[9].monotonic_ms {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "monotonic time did not advance across the UTC jump".into(),
            cfg.seed,
        ));
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        format!(
            "a {} ms backwards UTC jump at frame 10 leaves all {} frames in sequence, \
             flagged ESTIMATED_UTC with accuracy unknown, monotonic time still advancing",
            offset,
            scanned.frames.len()
        ),
        cfg.seed,
    ))
}

// ── prune gating ───────────────────────────────────────────────────────────

/// Property: no verified receipt means no prune, at any age, under any storage
/// pressure. This is the invariant the whole design exists to protect.
fn prune_requires_receipt(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "prune-without-receipt";
    const PROPERTY: &str = "no verified receipt => never pruned";

    let root = fresh_root(cfg, "prune-no-receipt")?;
    let store = boot(cfg, &root, 0)?;

    let id = bundle_id(6);
    let sealed = store
        .capture_and_seal(
            &CaptureSpec::new(id).with_chunk_size(cfg.chunk_size),
            &mut Injector::none(),
        )
        .map_err(|e| std::io::Error::other(e.to_string()))?;

    match store.prune(&id, &mut Injector::none()) {
        Ok(()) => Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "a bundle with no receipt was pruned".into(),
            cfg.seed,
        )),
        Err(super::store::StoreError::NoReceipt(_)) => {
            if !sealed.dir.exists() {
                return Ok(RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    "the prune was refused but the payload is gone anyway".into(),
                    cfg.seed,
                ));
            }
            Ok(RowResult::pass(
                FAMILY,
                PROPERTY,
                "pruning is refused and the payload is fully intact".into(),
                cfg.seed,
            ))
        }
        Err(e) => Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!("unexpected error: {e}"),
            cfg.seed,
        )),
    }
}

/// Property: a reboot in the window between verifying a receipt and beginning
/// the prune leaves the bundle fully present with its receipt intact, so the
/// prune simply happens later.
///
/// This is the earliest of the three prune windows, and the one where nothing
/// should have happened yet. A device that lost the receipt here would have to
/// re-upload; one that pruned anyway would be deleting on the strength of a
/// receipt it had not finished recording.
fn reboot_between_receipt_and_prune(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "reboot-after-receipt-before-prune";
    const PROPERTY: &str = "the bundle and its receipt both survive; the prune happens later";

    let root = fresh_root(cfg, "reboot-after-receipt")?;
    let store = boot(cfg, &root, 0)?;

    let id = bundle_id(12);
    let spec = CaptureSpec::new(id).with_chunk_size(cfg.chunk_size);
    let sealed = store
        .capture_and_seal(&spec, &mut Injector::none())
        .map_err(|e| std::io::Error::other(e.to_string()))?;

    store
        .store_receipt(&id, b"stand-in receipt bytes")
        .map_err(|e| std::io::Error::other(e.to_string()))?;

    // The reboot lands here: receipt recorded, prune not yet begun.
    let mut inj =
        Injector::armed(FaultPlan::interrupt(FaultPoint::AfterReceiptVerify).with_seed(cfg.seed));
    if inj.check(FaultPoint::AfterReceiptVerify).is_ok() {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "the fault at after-receipt-verify never fired".into(),
            cfg.seed,
        ));
    }

    let rebooted = boot(cfg, &root, 1)?;

    // Nothing was journalled, so replay must find nothing to finish.
    let finished = rebooted
        .replay_prune_journal()
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    if !finished.is_empty() {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!(
                "replay finished {} prunes, want 0 — no prune had been started",
                finished.len()
            ),
            cfg.seed,
        ));
    }

    if !sealed.dir.exists() {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "the payload is gone despite no prune having been started".into(),
            cfg.seed,
        ));
    }
    if !rebooted.has_receipt(&id) {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "the receipt did not survive the reboot — the device would have to re-upload".into(),
            cfg.seed,
        ));
    }

    // And the prune must still be possible afterwards.
    rebooted
        .prune(&id, &mut Injector::none())
        .map_err(|e| std::io::Error::other(e.to_string()))?;

    if sealed.dir.exists() {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "the deferred prune did not remove the payload".into(),
            cfg.seed,
        ));
    }
    if !rebooted.has_receipt(&id) {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "the deferred prune destroyed the receipt".into(),
            cfg.seed,
        ));
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        "payload and receipt both intact after the reboot; the deferred prune then succeeded"
            .into(),
        cfg.seed,
    ))
}

/// Property: a prune interrupted at any point leaves the bundle fully present
/// or fully pruned, and the journal says which.
///
/// The dangerous window is between deleting the payload and recording
/// completion, which is exactly why pruning is transactional.
fn prune_is_transactional(cfg: &Config) -> std::io::Result<Vec<RowResult>> {
    const FAMILY: &str = "reboot-mid-prune";
    const PROPERTY: &str = "fully present or fully pruned; the journal explains which";

    let mut rows = Vec::new();

    for point in [FaultPoint::AfterPruneIntent, FaultPoint::AfterPayloadDelete] {
        let root = fresh_root(cfg, &format!("prune-{}", point.label()))?;
        let store = boot(cfg, &root, 0)?;

        let id = bundle_id(7);
        let spec = CaptureSpec::new(id).with_chunk_size(cfg.chunk_size);
        store
            .capture_and_seal(&spec, &mut Injector::none())
            .map_err(|e| std::io::Error::other(e.to_string()))?;

        // A receipt must exist for the prune to be permitted at all. Its
        // contents do not matter to this row; its presence does.
        store
            .store_receipt(&id, b"stand-in receipt bytes")
            .map_err(|e| std::io::Error::other(e.to_string()))?;

        let mut inj = Injector::armed(FaultPlan::interrupt(point).with_seed(cfg.seed));
        let outcome = store.prune(&id, &mut inj);

        if outcome.is_ok() {
            rows.push(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("the fault at {point} never fired"),
                cfg.seed,
            ));
            continue;
        }

        // Reboot and replay the journal.
        let rebooted = boot(cfg, &root, 1)?;
        let finished = rebooted
            .replay_prune_journal()
            .map_err(|e| std::io::Error::other(e.to_string()))?;

        if finished.len() != 1 {
            rows.push(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{point}: replay finished {} prunes, want 1 — an interrupted prune \
                     must be resolved at boot",
                    finished.len()
                ),
                cfg.seed,
            ));
            continue;
        }

        let dir = rebooted
            .bundles_dir()
            .join(format!("{}.sealed", format::hex(&id)));
        if dir.exists() {
            rows.push(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("{point}: payload still present after journal replay"),
                cfg.seed,
            ));
            continue;
        }

        // The receipt must outlive the payload: it is the proof of delivery.
        if !rebooted.has_receipt(&id) {
            rows.push(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("{point}: pruning destroyed the receipt"),
                cfg.seed,
            ));
            continue;
        }

        rows.push(RowResult::pass(
            FAMILY,
            PROPERTY,
            format!("{point}: replay completed the prune; the receipt survived"),
            cfg.seed,
        ));
    }

    Ok(rows)
}

// ── v3: torn tail under encryption ─────────────────────────────────────────

/// Property: a power cut that tears a sealed frame — anywhere in it — is found
/// and isolated by a scan that holds no key, and the keyed verdict agrees.
///
/// Encryption must add nothing to recovery's requirements. A device whose key
/// store is unreadable at boot (or a recovery tool that was never given the
/// root) must still find the tear, keep every intact frame and report the
/// discarded bytes exactly, because the CRC and the chain are over ciphertext.
/// And a tear must never be reported as tampering: the keyed scan stops with
/// TORN_TAIL at the same place, not AUTH_FAILED.
fn torn_tail_under_encryption(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "torn-tail-under-encryption";
    const PROPERTY: &str =
        "a tear anywhere in a sealed frame is recovered with no key; the keyed verdict agrees";

    let frame_len = format::FRAME_OVERHEAD + 32; // 100

    // Each depth leaves the tear in a different part of the sealed frame:
    // header [0,24) nonce [24,48) ciphertext [48,80) tag [80,96) CRC [96,100).
    let cases = [
        (2usize, "CRC"),
        (12, "Poly1305 tag"),
        (30, "ciphertext"),
        (60, "nonce"),
        (90, "frame header"),
    ];

    for (drop_bytes, region) in cases {
        let root = fresh_root(cfg, &format!("torn-sealed-{drop_bytes}"))?;
        let store = boot(cfg, &root, 0)?;

        let mut inj = Injector::armed(
            FaultPlan::interrupt(FaultPoint::MidFrameWrite {
                frame: 10,
                drop_bytes,
            })
            .with_seed(cfg.seed),
        );
        let spec = CaptureSpec::new(bundle_id(13)).with_chunk_size(cfg.chunk_size);
        if store.capture_and_seal(&spec, &mut inj).is_ok() {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("the tear in the {region} never fired"),
                cfg.seed,
            ));
        }
        let want_discarded = (frame_len - drop_bytes) as u32;

        // Recovery by a device that has lost its key store: a different root
        // entirely. If recovery ever came to depend on the key, this device
        // would fail every tag and recover nothing.
        let keyless = boot_as(
            cfg,
            DeviceIdentity {
                root_key: [0u8; 32],
                ..identity(cfg)
            },
            &root.join("card"),
            &root.join("nvs"),
            1,
        )?;
        let recovered = keyless
            .recover_at_boot()
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        let Some(r) = recovered.first() else {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("tear in the {region}: recovery found no bundle"),
                cfg.seed,
            ));
        };
        if r.recovered_frames != 9
            || r.stop != StopReason::TornTail
            || r.discarded_tail_bytes != want_discarded
        {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "tear in the {region} ({drop_bytes} of {frame_len} bytes lost), no key: \
                     recovered {} frames, stop {}, discarded {} — want 9, TORN_TAIL, {want_discarded}",
                    r.recovered_frames, r.stop, r.discarded_tail_bytes
                ),
                cfg.seed,
            ));
        }

        // The device itself, with its key: the same stop at the same place,
        // and every retained frame authenticates to exactly what was written.
        let rebooted = boot(cfg, &root, 1)?;
        let keyed = rebooted
            .verify_bundle(&r.dir)
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        if keyed.capture_stop() != StopReason::TornTail || keyed.capture_frames() != 9 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "tear in the {region}, keyed: stop {} with {} frames, want TORN_TAIL with 9 \
                     — a power cut must never read as tampering",
                    keyed.capture_stop(),
                    keyed.capture_frames()
                ),
                cfg.seed,
            ));
        }
        let frames = keyed.capture.iter().flat_map(|s| s.frames.iter());
        for (i, f) in frames.enumerate() {
            if f.payload.as_deref() != Some(&gnss_payload(i)[..]) {
                return Ok(RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    format!("tear in the {region}: frame {i} does not decrypt to what was written"),
                    cfg.seed,
                ));
            }
        }

        // And what recovery kept can be sealed, honestly labelled.
        let resealed = rebooted
            .reseal_at_boot(cfg.chunk_size)
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        match resealed.as_slice() {
            [Ok(b)]
                if b.manifest.recovery_state == format::RecoveryState::RecoveredTail
                    && b.manifest.discarded_tail_bytes == want_discarded => {}
            [Ok(b)] => {
                return Ok(RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    format!(
                        "tear in the {region}: re-sealed as {:?} with {} bytes discarded, \
                         want RecoveredTail with {want_discarded}",
                        b.manifest.recovery_state, b.manifest.discarded_tail_bytes
                    ),
                    cfg.seed,
                ));
            }
            other => {
                return Ok(RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    format!(
                        "tear in the {region}: re-seal gave {} result(s), first {:?}",
                        other.len(),
                        other
                            .first()
                            .map(|r| r.as_ref().err().map(|e| e.to_string()))
                    ),
                    cfg.seed,
                ));
            }
        }
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        "tears in the CRC, tag, ciphertext, nonce and header each: 9 frames and the exact tail \
         recovered by a device with no usable key; the keyed scan agrees (TORN_TAIL, never \
         AUTH_FAILED), every frame decrypts to what was written, and the bundle re-seals as \
         RecoveredTail"
            .into(),
        cfg.seed,
    ))
}

// ── v3: card restored to an older image ────────────────────────────────────

/// The server's counter rule (spec §5.4, trust-model-v3 §2) as a pure
/// function, used as an oracle over what the device produced.
///
/// A counter is spent by the first content bound to it. The same pair again is
/// an idempotent duplicate; different content under a spent counter is
/// quarantined. The matrix does not need a server to ask the question that
/// matters locally: would anything this device wrote ever be quarantined?
#[derive(Default)]
struct CounterLedger {
    spent: BTreeMap<u64, [u8; 32]>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Admission {
    Fresh,
    Duplicate,
    Quarantine,
}

impl CounterLedger {
    fn admit(&mut self, counter: u64, content_root: [u8; 32]) -> Admission {
        match self.spent.get(&counter) {
            None => {
                self.spent.insert(counter, content_root);
                Admission::Fresh
            }
            Some(r) if *r == content_root => Admission::Duplicate,
            Some(_) => Admission::Quarantine,
        }
    }
}

/// Property: restoring the card to an older image cannot rewind the bundle
/// counter. Restored bundles are exact duplicates of what was already spent,
/// and every new bundle takes a counter above anything ever issued — so
/// nothing the device writes afterwards is ever quarantined.
///
/// This is the reason the counter lives in NVS and not on the card. Were it on
/// the card, the restore would rewind it, and the next bundle would reuse a
/// spent counter with different content.
fn card_restored_to_older_image(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "card-restored-to-older-image";
    const PROPERTY: &str =
        "a restored card cannot rewind the counter; old bundles are duplicates, new ones are fresh";

    let root = fresh_root(cfg, "card-restore")?;
    let card = root.join("card");
    let image = root.join("card-image");
    let mut ledger = CounterLedger::default();

    let store = boot(cfg, &root, 0)?;
    let seal = |store: &DeviceStore, n: u8| {
        store
            .capture_and_seal(
                &CaptureSpec::new(bundle_id(n)).with_chunk_size(cfg.chunk_size),
                &mut Injector::none(),
            )
            .map_err(|e| std::io::Error::other(e.to_string()))
    };

    // Bundle A, then image the card.
    let a = seal(&store, 20)?;
    ledger.admit(a.manifest.device_counter, a.manifest.content_root);
    copy_dir_all(&card, &image)?;

    // A is delivered and pruned; B and C are captured after the image.
    store
        .store_receipt(&a.manifest.bundle_id, b"stand-in receipt bytes")
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    store
        .prune(&a.manifest.bundle_id, &mut Injector::none())
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    for n in [21u8, 22] {
        let b = seal(&store, n)?;
        let admitted = ledger.admit(b.manifest.device_counter, b.manifest.content_root);
        if admitted != Admission::Fresh {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("before any restore, bundle {n} was {admitted:?}"),
                cfg.seed,
            ));
        }
    }
    drop(store);

    // Restore the old image over the card. NVS is not on the card and is
    // untouched — that is the point.
    fs::remove_dir_all(&card)?;
    copy_dir_all(&image, &card)?;

    let rebooted = boot(cfg, &root, 1)?;
    let high_water = rebooted
        .device_counter()
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    if high_water != 3 {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!("NVS counter after restore is {high_water}, want 3 — the restore reached NVS"),
            cfg.seed,
        ));
    }

    let recovered = rebooted
        .recover_at_boot()
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    let restored: Vec<_> = recovered.iter().filter(|r| r.sealed).collect();
    if restored.len() != 1 || restored[0].device_counter != Some(a.manifest.device_counter) {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!(
                "after restore the card holds {} sealed bundle(s) with counters {:?}, want only A \
                 (counter {})",
                restored.len(),
                restored
                    .iter()
                    .map(|r| r.device_counter)
                    .collect::<Vec<_>>(),
                a.manifest.device_counter
            ),
            cfg.seed,
        ));
    }

    // The restored A is A, byte for byte: re-offering it is an idempotent
    // duplicate, not new data and not a conflict.
    let restored_bytes = fs::read(restored[0].dir.join("manifest.cbor"))?;
    let restored_manifest = format::Manifest::from_cbor(&restored_bytes)
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    if restored_bytes != a.manifest_bytes {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            "the restored bundle's manifest differs from the one originally sealed".into(),
            cfg.seed,
        ));
    }
    let dup = ledger.admit(
        restored_manifest.device_counter,
        restored_manifest.content_root,
    );
    if dup != Admission::Duplicate {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!("the restored bundle is {dup:?}, want Duplicate"),
            cfg.seed,
        ));
    }
    // A restored bundle is genuine, not forged: it still authenticates.
    let keyed = rebooted
        .verify_bundle(&restored[0].dir)
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    if keyed.capture_stop() != StopReason::Eof {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!(
                "the restored bundle no longer authenticates: {}",
                keyed.capture_stop()
            ),
            cfg.seed,
        ));
    }

    // A new bundle after the restore takes the next counter above the high
    // water mark — never one the restored card would suggest.
    let d = seal(&rebooted, 23)?;
    let on_card = header_counters(&d.dir)?;
    let admitted = ledger.admit(d.manifest.device_counter, d.manifest.content_root);
    if d.manifest.device_counter != high_water + 1
        || on_card.iter().any(|&c| c != d.manifest.device_counter)
        || admitted != Admission::Fresh
    {
        return Ok(RowResult::fail(
            FAMILY,
            PROPERTY,
            format!(
                "the first bundle after the restore has counter {} (headers {on_card:?}) and is \
                 {admitted:?}; want {} and Fresh — a rewound counter would reuse a spent one",
                d.manifest.device_counter,
                high_water + 1
            ),
            cfg.seed,
        ));
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        format!(
            "card restored to an image holding counter {}: NVS kept {high_water}, the restored \
             bundle is a byte-identical Duplicate that still authenticates, and the next bundle \
             took counter {} (Fresh)",
            a.manifest.device_counter, d.manifest.device_counter
        ),
        cfg.seed,
    ))
}

// ── v3: a card from another device ─────────────────────────────────────────

/// Property: a card written by another device fails authentication on this
/// one. It is still recovered structurally — it is somebody's data — but this
/// device never signs a manifest over it, never re-seals it, never spends a
/// counter on it, and never deletes or alters a byte of it.
///
/// Two foreign writers are tried: a genuinely different device, and a forgery
/// that copies this device's id, vehicle and assignment but not its root. The
/// second is the one that matters: identity fields are readable by anyone, so
/// only the key distinguishes this device's frames from an imitation.
fn card_from_another_device(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "wrong-key-card";
    const PROPERTY: &str =
        "a card from another device fails authentication; recovered, never signed, never altered";

    let forger = DeviceIdentity {
        signing_key: SigningKey::from_bytes(b"cairn-matrix-forged-device-key-s"),
        root_key: *b"cairn-matrix-forged-device-root!",
        ..identity(cfg)
    };

    for (who, slug, foreign) in [
        ("another device", "other", other_device(cfg)),
        ("a forgery of this device", "forged", forger),
    ] {
        let root = fresh_root(cfg, &format!("foreign-card-{slug}"))?;
        let card = root.join("card");
        let foreign_nvs = root.join("foreign-nvs");

        // The foreign device fills the card: one sealed bundle, one left
        // unsealed by a power cut mid-seal.
        let writer = boot_as(cfg, foreign.clone(), &card, &foreign_nvs, 0)?;
        let sealed = writer
            .capture_and_seal(
                &CaptureSpec::new(bundle_id(50)).with_chunk_size(cfg.chunk_size),
                &mut Injector::none(),
            )
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        let cut = writer.capture_and_seal(
            &CaptureSpec::new(bundle_id(51)).with_chunk_size(cfg.chunk_size),
            &mut Injector::armed(
                FaultPlan::interrupt(FaultPoint::AfterManifestSync).with_seed(cfg.seed),
            ),
        );
        if cut.is_ok() {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                "the mid-seal cut on the foreign device never fired".into(),
                cfg.seed,
            ));
        }
        drop(writer);
        let before = tree_digests(&card)?;

        // The card goes into this device.
        let ours = boot(cfg, &root, 1)?;

        // Structurally it is fine: anyone can see the frames are intact.
        let recovered = ours
            .recover_at_boot()
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        if recovered.len() != 2
            || recovered
                .iter()
                .any(|r| r.recovered_frames != 25 || r.stop != StopReason::Eof)
        {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{who}: structural recovery found {:?}, want two intact bundles of 25 frames",
                    recovered
                        .iter()
                        .map(|r| (r.recovered_frames, r.stop.name()))
                        .collect::<Vec<_>>()
                ),
                cfg.seed,
            ));
        }

        // Cryptographically it is not ours: the very first frame of every
        // chain fails, and nothing is decrypted.
        for r in &recovered {
            let keyed = ours
                .verify_bundle(&r.dir)
                .map_err(|e| std::io::Error::other(e.to_string()))?;
            let journal = keyed.journal.as_ref();
            if keyed.capture_stop() != StopReason::AuthFailed
                || keyed.capture_frames() != 0
                || journal.map(|j| (j.stop, j.frames.len())) != Some((StopReason::AuthFailed, 0))
            {
                return Ok(RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    format!(
                        "{who}: keyed verification of {} gave capture {} with {} frames, journal \
                         {:?}; want AUTH_FAILED with 0 frames on both chains",
                        r.dir.display(),
                        keyed.capture_stop(),
                        keyed.capture_frames(),
                        journal.map(|j| (j.stop.name(), j.frames.len()))
                    ),
                    cfg.seed,
                ));
            }
        }

        // Boot-time sealing refuses the unsealed one rather than vouching for it.
        let resealed = ours
            .reseal_at_boot(cfg.chunk_size)
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        if !matches!(
            resealed.as_slice(),
            [Err(StoreError::Unauthenticated { .. })]
        ) {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{who}: re-sealing the foreign unsealed bundle gave {:?}, want a refusal",
                    resealed
                        .iter()
                        .map(|r| r.as_ref().map(|_| "sealed").map_err(|e| e.to_string()))
                        .collect::<Vec<_>>()
                ),
                cfg.seed,
            ));
        }

        // Nothing on the card changed, nothing was deleted, and no counter was
        // spent on somebody else's data.
        if tree_digests(&card)? != before {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("{who}: this device altered or deleted bytes on a card it cannot read"),
                cfg.seed,
            ));
        }
        let spent = ours
            .device_counter()
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        if spent != 0 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("{who}: reading a foreign card advanced this device's counter to {spent}"),
                cfg.seed,
            ));
        }

        // Positive control: the writer, given its card back, reads it fine. A
        // verifier that failed everything would otherwise pass this row.
        let back = boot_as(cfg, foreign, &card, &foreign_nvs, 2)?;
        let control = back
            .verify_bundle(&sealed.dir)
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        if control.capture_stop() != StopReason::Eof || control.capture_frames() != 25 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{who}: control failed — the writer itself got {} with {} frames",
                    control.capture_stop(),
                    control.capture_frames()
                ),
                cfg.seed,
            ));
        }
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        "cards from another device and from a forgery of this one: both bundles recovered \
         structurally (25 frames, EOF), every chain AUTH_FAILED at frame 0 under this device's \
         root, re-seal refused, card byte-identical, no counter spent; the writer still reads its \
         own card"
            .into(),
        cfg.seed,
    ))
}

// ── v3: the counter across a power cut mid-seal ────────────────────────────

/// Property: the bundle counter survives a power cut mid-seal. The re-sealed
/// bundle keeps the counter its frames were sealed under, and the next bundle
/// takes the next value — none is reused, none is lost track of.
///
/// The counter is in every frame's AAD, so a re-seal that took a fresh counter
/// would describe frames that no longer authenticate against their own
/// manifest; one that rewound NVS would hand the next bundle a spent value.
fn counter_survives_cut_mid_seal(cfg: &Config) -> std::io::Result<RowResult> {
    const FAMILY: &str = "counter-across-power-cut-mid-seal";
    const PROPERTY: &str = "the counter survives a cut mid-seal; the re-seal keeps it and the next bundle takes the next";

    for point in [
        FaultPoint::AfterSegmentSync,
        FaultPoint::BeforeManifestSync,
        FaultPoint::AfterManifestSync,
    ] {
        let root = fresh_root(cfg, &format!("counter-{}", point.label()))?;
        let store = boot(cfg, &root, 0)?;

        // One clean bundle first, so the cut lands on counter 2 and the row
        // tests persistence of a value NVS had to advance, not its default.
        let first = store
            .capture_and_seal(
                &CaptureSpec::new(bundle_id(40)).with_chunk_size(cfg.chunk_size),
                &mut Injector::none(),
            )
            .map_err(|e| std::io::Error::other(e.to_string()))?;

        let cut = store.capture_and_seal(
            &CaptureSpec::new(bundle_id(41)).with_chunk_size(cfg.chunk_size),
            &mut Injector::armed(FaultPlan::interrupt(point).with_seed(cfg.seed)),
        );
        if cut.is_ok() {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("the fault at {point} never fired"),
                cfg.seed,
            ));
        }
        drop(store);

        // Power returns. Nothing in memory survived; only the card and NVS.
        let rebooted = boot(cfg, &root, 1)?;
        let nvs = rebooted
            .device_counter()
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        if nvs != first.manifest.device_counter + 1 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{point}: NVS holds {nvs} after the cut, want {}",
                    first.manifest.device_counter + 1
                ),
                cfg.seed,
            ));
        }

        let resealed = rebooted
            .reseal_at_boot(cfg.chunk_size)
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        let q = match resealed.as_slice() {
            [Ok(b)] => b.clone(),
            other => {
                return Ok(RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    format!(
                        "{point}: re-seal gave {:?}, want one sealed bundle",
                        other
                            .iter()
                            .map(|r| r.as_ref().map(|_| "sealed").map_err(|e| e.to_string()))
                            .collect::<Vec<_>>()
                    ),
                    cfg.seed,
                ));
            }
        };

        // The re-sealed manifest, as durably written and signed, claims the
        // counter the frames carry — and the capture boot, not this one.
        let on_disk = fs::read(q.dir.join("manifest.cbor"))?;
        let sig: [u8; 64] = fs::read(q.dir.join("manifest.sig"))?
            .try_into()
            .map_err(|_| std::io::Error::other("manifest.sig is not 64 bytes"))?;
        let m = match format::manifest::verify_manifest(
            &on_disk,
            &sig,
            &device_key().verifying_key(),
        ) {
            Ok(m) => m,
            Err(e) => {
                return Ok(RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    format!("{point}: the re-sealed manifest does not verify: {e}"),
                    cfg.seed,
                ));
            }
        };
        let headers = header_counters(&q.dir)?;
        let members: BTreeMap<String, Vec<u8>> = m
            .members
            .iter()
            .map(|mem| Ok((mem.name.clone(), fs::read(q.dir.join(&mem.name))?)))
            .collect::<std::io::Result<_>>()?;
        let bound =
            format::verify_members_against_manifest(&m, |n| members.get(n).map(|v| v.as_slice()));
        if m.device_counter != nvs
            || headers.iter().any(|&c| c != nvs)
            || m.boot_id != boot_id(0)
            || bound.is_err()
        {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{point}: re-sealed manifest claims counter {} boot {} (binding {:?}); \
                     headers carry {headers:?}; want {nvs} and the capture boot",
                    m.device_counter,
                    format::hex(&m.boot_id),
                    bound.err().map(|e| e.to_string())
                ),
                cfg.seed,
            ));
        }
        let keyed = rebooted
            .verify_bundle(&q.dir)
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        if keyed.capture_stop() != StopReason::Eof || keyed.capture_frames() != 25 {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{point}: the re-sealed bundle verifies to {} with {} frames",
                    keyed.capture_stop(),
                    keyed.capture_frames()
                ),
                cfg.seed,
            ));
        }

        // The next bundle takes the next counter.
        let next = rebooted
            .capture_and_seal(
                &CaptureSpec::new(bundle_id(42)).with_chunk_size(cfg.chunk_size),
                &mut Injector::none(),
            )
            .map_err(|e| std::io::Error::other(e.to_string()))?;
        let next_headers = header_counters(&next.dir)?;
        if next.manifest.device_counter != nvs + 1 || next_headers.iter().any(|&c| c != nvs + 1) {
            return Ok(RowResult::fail(
                FAMILY,
                PROPERTY,
                format!(
                    "{point}: the bundle after the re-seal has counter {} (headers \
                     {next_headers:?}), want {}",
                    next.manifest.device_counter,
                    nvs + 1
                ),
                cfg.seed,
            ));
        }
    }

    Ok(RowResult::pass(
        FAMILY,
        PROPERTY,
        "a cut after segment sync, before and after manifest sync: NVS held 2 across the reboot, \
         the re-seal signed counter 2 for the capture boot (binding and tags verified), and the \
         next bundle took 3"
            .into(),
        cfg.seed,
    ))
}

// ── helpers for the v3 rows ────────────────────────────────────────────────

/// The `device_counter` of every segment header in a bundle directory, read
/// from the card without a key.
fn header_counters(dir: &Path) -> std::io::Result<Vec<u64>> {
    let mut names: Vec<_> = fs::read_dir(dir)?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "seg"))
        .collect();
    names.sort();
    names
        .iter()
        .map(|p| {
            let b = fs::read(p)?;
            parse_segment_header(&b)
                .map(|(h, _)| h.device_counter)
                .map_err(|e| std::io::Error::other(format!("{}: {e}", p.display())))
        })
        .collect()
}

/// SHA-256 of every file under `dir`, keyed by relative path, so "nothing on
/// the card changed" is a comparison of durable state rather than a belief.
fn tree_digests(dir: &Path) -> std::io::Result<BTreeMap<PathBuf, [u8; 32]>> {
    let mut out = BTreeMap::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for entry in fs::read_dir(&d)? {
            let p = entry?.path();
            if p.is_dir() {
                stack.push(p);
            } else {
                let rel = p.strip_prefix(dir).unwrap_or(&p).to_path_buf();
                out.insert(rel, Sha256::digest(fs::read(&p)?).into());
            }
        }
    }
    Ok(out)
}

/// Copy a directory tree: imaging a card, and restoring the image.
fn copy_dir_all(src: &Path, dst: &Path) -> std::io::Result<()> {
    fs::create_dir_all(dst)?;
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let to = dst.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            copy_dir_all(&entry.path(), &to)?;
        } else {
            fs::copy(entry.path(), &to)?;
        }
    }
    Ok(())
}

// ── protocol rows ──────────────────────────────────────────────────────────

/// Build and seal a bundle for a protocol row.
pub(super) fn sealed_for_protocol(
    cfg: &Config,
    name: &str,
    id: [u8; 16],
    gnss: usize,
) -> std::io::Result<(DeviceStore, SealedBundle)> {
    // One physical device across every protocol row, so one NVS: each row's
    // bundle takes the next counter. A fresh NVS per row would hand every row
    // counter 1, and the server would quite rightly quarantine the second
    // bundle to arrive under a spent counter with different content. The card
    // is still fresh per row. The NVS directory deliberately survives between
    // runs too, as a real device's would, so re-running against a persistent
    // server only ever moves the counter forward.
    let root = fresh_root(cfg, name)?;
    let store = boot_as(
        cfg,
        identity(cfg),
        &root.join("card"),
        &cfg.work_dir.join("protocol-device-nvs"),
        0,
    )?;
    let spec = CaptureSpec::new(id)
        .with_gnss(gnss)
        .with_chunk_size(cfg.chunk_size);
    let sealed = store
        .capture_and_seal(&spec, &mut Injector::none())
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    Ok((store, sealed))
}

/// Property: the device eventually obtains a valid receipt despite the network
/// dropping at every chunk boundary, and the server stores one bundle.
fn network_loss_per_chunk(cfg: &Config, t: &Target) -> Vec<RowResult> {
    let family = t.family("network-loss-per-chunk");
    let family = family.as_str();
    const PROPERTY: &str = "device eventually obtains a valid receipt; resume, not restart";

    let client = &t.client;
    let key = match t.key.clone() {
        Ok(k) => k,
        Err(e) => {
            return vec![RowResult::fail(
                family,
                PROPERTY,
                format!("receipt key: {e}"),
                cfg.seed,
            )];
        }
    };

    // Small chunks so there are several boundaries to drop at.
    let small = Config {
        chunk_size: 128,
        ..clone_cfg(cfg)
    };
    let (store, sealed) = match sealed_for_protocol(&small, &t.tag("net-loss"), t.bundle_id(8), 40)
    {
        Ok(v) => v,
        Err(e) => return vec![RowResult::fail(family, PROPERTY, e.to_string(), cfg.seed)],
    };

    let total = sealed.chunk_count();
    if total < 3 {
        return vec![RowResult::fail(
            family,
            PROPERTY,
            format!("only {total} chunks; need at least 3 boundaries"),
            cfg.seed,
        )];
    }

    let mut attempts = 0usize;

    // Drop the connection after each chunk in turn, then retry. A correct
    // protocol resumes; a broken one would restart from zero every time.
    for drop_after in 0..total.min(4) as u32 {
        attempts += 1;
        let mut inj = Injector::armed(
            FaultPlan::interrupt(FaultPoint::AfterChunk(drop_after)).with_seed(cfg.seed),
        );
        match client.sync(&sealed, &key, &mut inj) {
            Err(SyncError::Interrupted(_)) => {}
            Ok(_) => break,
            Err(e) => {
                return vec![RowResult::fail(
                    family,
                    PROPERTY,
                    format!("unexpected error while dropping after chunk {drop_after}: {e}"),
                    cfg.seed,
                )];
            }
        }
    }

    // Now let it finish.
    attempts += 1;
    match client.sync(&sealed, &key, &mut Injector::none()) {
        Ok(outcome) => {
            let Some(receipt) = outcome.receipt else {
                return vec![RowResult::fail(
                    family,
                    PROPERTY,
                    "sync reported success with no receipt".into(),
                    cfg.seed,
                )];
            };
            if receipt.content_root != sealed.manifest.content_root {
                return vec![RowResult::fail(
                    family,
                    PROPERTY,
                    "the receipt acknowledges different content".into(),
                    cfg.seed,
                )];
            }
            if let Err(e) = store.store_receipt(&sealed.manifest.bundle_id, &outcome.receipt_bytes)
            {
                return vec![RowResult::fail(
                    family,
                    PROPERTY,
                    format!("store receipt: {e}"),
                    cfg.seed,
                )];
            }
        }
        Err(e) => {
            return vec![RowResult::fail(
                family,
                PROPERTY,
                format!("the final attempt failed: {e}"),
                cfg.seed,
            )];
        }
    }

    vec![RowResult::pass(
        family,
        PROPERTY,
        format!(
            "{total} chunks, connection dropped after each of the first {} boundaries; \
             a receipt was obtained in {attempts} attempts",
            total.min(4)
        ),
        cfg.seed,
    )]
}

/// Property: a corrupted chunk is rejected, and a retry succeeds without
/// operator action.
fn corrupt_chunk_in_transit(cfg: &Config, t: &Target) -> RowResult {
    let family = t.family("corrupt-chunk-in-transit");
    let family = family.as_str();
    const PROPERTY: &str = "rejected; retry succeeds without operator action";

    let client = &t.client;
    let key = match t.key.clone() {
        Ok(k) => k,
        Err(e) => return RowResult::fail(family, PROPERTY, format!("receipt key: {e}"), cfg.seed),
    };

    let small = Config {
        chunk_size: 256,
        ..clone_cfg(cfg)
    };
    let (_store, sealed) =
        match sealed_for_protocol(&small, &t.tag("corrupt-chunk"), t.bundle_id(9), 30) {
            Ok(v) => v,
            Err(e) => return RowResult::fail(family, PROPERTY, e.to_string(), cfg.seed),
        };

    let mut inj = Injector::armed(
        FaultPlan::corrupt(FaultPoint::CorruptChunkInTransit(1)).with_seed(cfg.seed),
    );

    match client.sync(&sealed, &key, &mut inj) {
        Ok(outcome) => {
            if outcome.chunks_rejected == 0 {
                return RowResult::fail(
                    family,
                    PROPERTY,
                    "the corrupted chunk was not rejected".into(),
                    cfg.seed,
                );
            }
            if outcome.receipt.is_none() {
                return RowResult::fail(
                    family,
                    PROPERTY,
                    "no receipt after retrying the corrupted chunk".into(),
                    cfg.seed,
                );
            }
            RowResult::pass(
                family,
                PROPERTY,
                format!(
                    "chunk 1 corrupted in flight: rejected ({} rejection(s)), \
                     retried with correct bytes, receipt obtained",
                    outcome.chunks_rejected
                ),
                cfg.seed,
            )
        }
        Err(e) => RowResult::fail(family, PROPERTY, format!("sync failed: {e}"), cfg.seed),
    }
}

/// Property: re-offering identical content stores it exactly once and returns
/// the receipt already earned.
fn duplicate_upload_stored_once(cfg: &Config, t: &Target) -> RowResult {
    let family = t.family("duplicate-upload");
    let family = family.as_str();
    const PROPERTY: &str = "immutable bundle stored exactly once";

    let client = &t.client;
    let key = match t.key.clone() {
        Ok(k) => k,
        Err(e) => return RowResult::fail(family, PROPERTY, format!("receipt key: {e}"), cfg.seed),
    };

    let (_store, sealed) = match sealed_for_protocol(cfg, &t.tag("duplicate"), t.bundle_id(10), 25)
    {
        Ok(v) => v,
        Err(e) => return RowResult::fail(family, PROPERTY, e.to_string(), cfg.seed),
    };

    let before = client.decode_backlog().unwrap_or(-1);

    let first = match client.sync(&sealed, &key, &mut Injector::none()) {
        Ok(o) => o,
        Err(e) => return RowResult::fail(family, PROPERTY, format!("first sync: {e}"), cfg.seed),
    };
    let after_first = client.decode_backlog().unwrap_or(-1);

    let second = match client.sync(&sealed, &key, &mut Injector::none()) {
        Ok(o) => o,
        Err(e) => return RowResult::fail(family, PROPERTY, format!("second sync: {e}"), cfg.seed),
    };
    let after_second = client.decode_backlog().unwrap_or(-1);

    let (Some(r1), Some(r2)) = (first.receipt.as_ref(), second.receipt.as_ref()) else {
        return RowResult::fail(
            family,
            PROPERTY,
            "a sync produced no receipt".into(),
            cfg.seed,
        );
    };

    if r1.receipt_id != r2.receipt_id {
        return RowResult::fail(
            family,
            PROPERTY,
            "the second upload minted a different receipt".into(),
            cfg.seed,
        );
    }
    if second.chunks_sent != 0 {
        return RowResult::fail(
            family,
            PROPERTY,
            format!("the second upload re-sent {} chunks", second.chunks_sent),
            cfg.seed,
        );
    }

    // Exactly one decode job, not two.
    if after_first >= 0 && after_second != after_first {
        return RowResult::fail(
            family,
            PROPERTY,
            format!(
                "decode backlog went {before} -> {after_first} -> {after_second}; \
                 the duplicate upload queued extra work"
            ),
            cfg.seed,
        );
    }

    RowResult::pass(
        family,
        PROPERTY,
        format!(
            "identical content uploaded twice: same receipt, zero chunks re-sent, \
             decode backlog unchanged at {after_second}"
        ),
        cfg.seed,
    )
}

/// Property: when the commit response is lost, a retry returns the same
/// receipt rather than minting a second one.
fn receipt_lost_in_transit(cfg: &Config, t: &Target) -> RowResult {
    let family = t.family("receipt-lost-in-transit");
    let family = family.as_str();
    const PROPERTY: &str = "a retry returns the same receipt";

    let client = &t.client;
    let key = match t.key.clone() {
        Ok(k) => k,
        Err(e) => return RowResult::fail(family, PROPERTY, format!("receipt key: {e}"), cfg.seed),
    };

    let (_store, sealed) =
        match sealed_for_protocol(cfg, &t.tag("receipt-lost"), t.bundle_id(11), 22) {
            Ok(v) => v,
            Err(e) => return RowResult::fail(family, PROPERTY, e.to_string(), cfg.seed),
        };

    // The server commits and receipts; the device never sees the response.
    let mut inj =
        Injector::armed(FaultPlan::interrupt(FaultPoint::ReceiptLostInTransit).with_seed(cfg.seed));
    match client.sync(&sealed, &key, &mut inj) {
        Err(SyncError::Interrupted(_)) => {}
        Ok(_) => {
            return RowResult::fail(family, PROPERTY, "the fault never fired".into(), cfg.seed);
        }
        Err(e) => {
            return RowResult::fail(family, PROPERTY, format!("unexpected error: {e}"), cfg.seed);
        }
    }

    // The device retries, not knowing whether the server committed.
    match client.sync(&sealed, &key, &mut Injector::none()) {
        Ok(outcome) => {
            let Some(receipt) = outcome.receipt else {
                return RowResult::fail(
                    family,
                    PROPERTY,
                    "the retry produced no receipt".into(),
                    cfg.seed,
                );
            };
            if receipt.content_root != sealed.manifest.content_root {
                return RowResult::fail(
                    family,
                    PROPERTY,
                    "the retry's receipt acknowledges different content".into(),
                    cfg.seed,
                );
            }
            if !outcome.already_committed {
                return RowResult::fail(
                    family,
                    PROPERTY,
                    "the retry was not recognised as already committed".into(),
                    cfg.seed,
                );
            }
            RowResult::pass(
                family,
                PROPERTY,
                "the lost-response retry returned the already-committed receipt".into(),
                cfg.seed,
            )
        }
        Err(e) => RowResult::fail(family, PROPERTY, format!("the retry failed: {e}"), cfg.seed),
    }
}

pub(super) fn clone_cfg(cfg: &Config) -> Config {
    Config {
        work_dir: cfg.work_dir.clone(),
        server: cfg.server.clone(),
        seed: cfg.seed,
        chunk_size: cfg.chunk_size,
        verbose: cfg.verbose,
        identity: cfg.identity.clone(),
        relay: cfg.relay.clone(),
    }
}

/// Ensure the work directory exists.
pub fn prepare(dir: &Path) -> std::io::Result<()> {
    fs::create_dir_all(dir)
}
