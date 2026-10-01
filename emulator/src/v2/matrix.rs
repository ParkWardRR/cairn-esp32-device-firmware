//! The property matrix.
//!
//! Each row states a property the architecture claims, arms a fault that would
//! violate it, and asserts what survived. Assertions are made against the
//! device's durable state and the server's reported state — never against
//! stdout, because a log line proves nothing about what is on disk.
//!
//! Every run is reproducible from its seed, which is printed on failure.

use std::fs;
use std::path::{Path, PathBuf};

use ed25519_dalek::{SigningKey, VerifyingKey};

use crate::format::{self, StopReason};

use super::fault::{FaultPlan, FaultPoint, Injector};
use super::store::{CaptureSpec, DeviceStore, SealedBundle};
use super::sync::{SyncClient, SyncError};

/// A fixed device key, so a device identity survives a simulated reboot.
const DEVICE_KEY_SEED: &[u8; 32] = b"cairn-intake-test-device-key-see";

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
    fn pass(family: &str, property: &str, detail: String, seed: u64) -> Self {
        Self {
            family: family.into(),
            property: property.into(),
            detail,
            passed: true,
            skipped: false,
            seed,
        }
    }

    fn fail(family: &str, property: &str, detail: String, seed: u64) -> Self {
        Self {
            family: family.into(),
            property: property.into(),
            detail,
            passed: false,
            skipped: false,
            seed,
        }
    }

    fn skip(family: &str, property: &str, why: String) -> Self {
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
}

fn device_key() -> SigningKey {
    SigningKey::from_bytes(DEVICE_KEY_SEED)
}

fn device_id() -> [u8; 16] {
    let mut id = [0u8; 16];
    for (i, b) in id.iter_mut().enumerate() {
        *b = 0x10 + i as u8;
    }
    id
}

fn bundle_id(n: u8) -> [u8; 16] {
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
        rows.extend(network_loss_per_chunk(cfg));
        rows.push(corrupt_chunk_in_transit(cfg));
        rows.push(duplicate_upload_stored_once(cfg));
        rows.push(receipt_lost_in_transit(cfg));
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
        let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
            .map_err(|e| std::io::Error::other(e.to_string()))?;

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
        let rebooted = DeviceStore::open(&root, device_key(), device_id(), boot_id(1))
            .map_err(|e| std::io::Error::other(e.to_string()))?;
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
        let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
            .map_err(|e| std::io::Error::other(e.to_string()))?;

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

        let rebooted = DeviceStore::open(&root, device_key(), device_id(), boot_id(1))
            .map_err(|e| std::io::Error::other(e.to_string()))?;
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

    // A GNSS frame is 28 bytes of envelope plus a 32-byte payload.
    let frame_len = format::FRAME_OVERHEAD + 32;

    for drop_bytes in [4usize, 20, 40] {
        let root = fresh_root(cfg, &format!("torn-{drop_bytes}"))?;
        let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
            .map_err(|e| std::io::Error::other(e.to_string()))?;

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

        let rebooted = DeviceStore::open(&root, device_key(), device_id(), boot_id(1))
            .map_err(|e| std::io::Error::other(e.to_string()))?;
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
    let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
        .map_err(|e| std::io::Error::other(e.to_string()))?;

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
    let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
        .map_err(|e| std::io::Error::other(e.to_string()))?;

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
    let scanned = format::scan_segment(&bytes, format::ScanState::default())
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
    let offset = i32::from_le_bytes([
        jumped.payload[26],
        jumped.payload[27],
        jumped.payload[28],
        jumped.payload[29],
    ]);
    let acc = u16::from_le_bytes([jumped.payload[30], jumped.payload[31]]);

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
    let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
        .map_err(|e| std::io::Error::other(e.to_string()))?;

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
    let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
        .map_err(|e| std::io::Error::other(e.to_string()))?;

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

    let rebooted = DeviceStore::open(&root, device_key(), device_id(), boot_id(1))
        .map_err(|e| std::io::Error::other(e.to_string()))?;

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
        let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
            .map_err(|e| std::io::Error::other(e.to_string()))?;

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
        let rebooted = DeviceStore::open(&root, device_key(), device_id(), boot_id(1))
            .map_err(|e| std::io::Error::other(e.to_string()))?;
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

// ── protocol rows ──────────────────────────────────────────────────────────

/// Build and seal a bundle for a protocol row.
fn sealed_for_protocol(
    cfg: &Config,
    name: &str,
    id: [u8; 16],
    gnss: usize,
) -> std::io::Result<(DeviceStore, SealedBundle)> {
    let root = fresh_root(cfg, name)?;
    let store = DeviceStore::open(&root, device_key(), device_id(), boot_id(0))
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    let spec = CaptureSpec::new(id)
        .with_gnss(gnss)
        .with_chunk_size(cfg.chunk_size);
    let sealed = store
        .capture_and_seal(&spec, &mut Injector::none())
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    Ok((store, sealed))
}

fn receipt_key(client: &SyncClient) -> std::result::Result<VerifyingKey, String> {
    client.fetch_receipt_key().map_err(|e| e.to_string())
}

/// Property: the device eventually obtains a valid receipt despite the network
/// dropping at every chunk boundary, and the server stores one bundle.
fn network_loss_per_chunk(cfg: &Config) -> Vec<RowResult> {
    const FAMILY: &str = "network-loss-per-chunk";
    const PROPERTY: &str = "device eventually obtains a valid receipt; resume, not restart";

    let client = SyncClient::new(&cfg.server);
    let key = match receipt_key(&client) {
        Ok(k) => k,
        Err(e) => {
            return vec![RowResult::fail(
                FAMILY,
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
    let (store, sealed) = match sealed_for_protocol(&small, "net-loss", bundle_id(8), 40) {
        Ok(v) => v,
        Err(e) => return vec![RowResult::fail(FAMILY, PROPERTY, e.to_string(), cfg.seed)],
    };

    let total = sealed.chunk_count();
    if total < 3 {
        return vec![RowResult::fail(
            FAMILY,
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
                    FAMILY,
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
                    FAMILY,
                    PROPERTY,
                    "sync reported success with no receipt".into(),
                    cfg.seed,
                )];
            };
            if receipt.content_root != sealed.manifest.content_root {
                return vec![RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    "the receipt acknowledges different content".into(),
                    cfg.seed,
                )];
            }
            if let Err(e) = store.store_receipt(&sealed.manifest.bundle_id, &outcome.receipt_bytes)
            {
                return vec![RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    format!("store receipt: {e}"),
                    cfg.seed,
                )];
            }
        }
        Err(e) => {
            return vec![RowResult::fail(
                FAMILY,
                PROPERTY,
                format!("the final attempt failed: {e}"),
                cfg.seed,
            )];
        }
    }

    vec![RowResult::pass(
        FAMILY,
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
fn corrupt_chunk_in_transit(cfg: &Config) -> RowResult {
    const FAMILY: &str = "corrupt-chunk-in-transit";
    const PROPERTY: &str = "rejected; retry succeeds without operator action";

    let client = SyncClient::new(&cfg.server);
    let key = match receipt_key(&client) {
        Ok(k) => k,
        Err(e) => return RowResult::fail(FAMILY, PROPERTY, format!("receipt key: {e}"), cfg.seed),
    };

    let small = Config {
        chunk_size: 256,
        ..clone_cfg(cfg)
    };
    let (_store, sealed) = match sealed_for_protocol(&small, "corrupt-chunk", bundle_id(9), 30) {
        Ok(v) => v,
        Err(e) => return RowResult::fail(FAMILY, PROPERTY, e.to_string(), cfg.seed),
    };

    let mut inj = Injector::armed(
        FaultPlan::corrupt(FaultPoint::CorruptChunkInTransit(1)).with_seed(cfg.seed),
    );

    match client.sync(&sealed, &key, &mut inj) {
        Ok(outcome) => {
            if outcome.chunks_rejected == 0 {
                return RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    "the corrupted chunk was not rejected".into(),
                    cfg.seed,
                );
            }
            if outcome.receipt.is_none() {
                return RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    "no receipt after retrying the corrupted chunk".into(),
                    cfg.seed,
                );
            }
            RowResult::pass(
                FAMILY,
                PROPERTY,
                format!(
                    "chunk 1 corrupted in flight: rejected ({} rejection(s)), \
                     retried with correct bytes, receipt obtained",
                    outcome.chunks_rejected
                ),
                cfg.seed,
            )
        }
        Err(e) => RowResult::fail(FAMILY, PROPERTY, format!("sync failed: {e}"), cfg.seed),
    }
}

/// Property: re-offering identical content stores it exactly once and returns
/// the receipt already earned.
fn duplicate_upload_stored_once(cfg: &Config) -> RowResult {
    const FAMILY: &str = "duplicate-upload";
    const PROPERTY: &str = "immutable bundle stored exactly once";

    let client = SyncClient::new(&cfg.server);
    let key = match receipt_key(&client) {
        Ok(k) => k,
        Err(e) => return RowResult::fail(FAMILY, PROPERTY, format!("receipt key: {e}"), cfg.seed),
    };

    let (_store, sealed) = match sealed_for_protocol(cfg, "duplicate", bundle_id(10), 25) {
        Ok(v) => v,
        Err(e) => return RowResult::fail(FAMILY, PROPERTY, e.to_string(), cfg.seed),
    };

    let before = client.decode_backlog().unwrap_or(-1);

    let first = match client.sync(&sealed, &key, &mut Injector::none()) {
        Ok(o) => o,
        Err(e) => return RowResult::fail(FAMILY, PROPERTY, format!("first sync: {e}"), cfg.seed),
    };
    let after_first = client.decode_backlog().unwrap_or(-1);

    let second = match client.sync(&sealed, &key, &mut Injector::none()) {
        Ok(o) => o,
        Err(e) => return RowResult::fail(FAMILY, PROPERTY, format!("second sync: {e}"), cfg.seed),
    };
    let after_second = client.decode_backlog().unwrap_or(-1);

    let (Some(r1), Some(r2)) = (first.receipt.as_ref(), second.receipt.as_ref()) else {
        return RowResult::fail(
            FAMILY,
            PROPERTY,
            "a sync produced no receipt".into(),
            cfg.seed,
        );
    };

    if r1.receipt_id != r2.receipt_id {
        return RowResult::fail(
            FAMILY,
            PROPERTY,
            "the second upload minted a different receipt".into(),
            cfg.seed,
        );
    }
    if second.chunks_sent != 0 {
        return RowResult::fail(
            FAMILY,
            PROPERTY,
            format!("the second upload re-sent {} chunks", second.chunks_sent),
            cfg.seed,
        );
    }

    // Exactly one decode job, not two.
    if after_first >= 0 && after_second != after_first {
        return RowResult::fail(
            FAMILY,
            PROPERTY,
            format!(
                "decode backlog went {before} -> {after_first} -> {after_second}; \
                 the duplicate upload queued extra work"
            ),
            cfg.seed,
        );
    }

    RowResult::pass(
        FAMILY,
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
fn receipt_lost_in_transit(cfg: &Config) -> RowResult {
    const FAMILY: &str = "receipt-lost-in-transit";
    const PROPERTY: &str = "a retry returns the same receipt";

    let client = SyncClient::new(&cfg.server);
    let key = match receipt_key(&client) {
        Ok(k) => k,
        Err(e) => return RowResult::fail(FAMILY, PROPERTY, format!("receipt key: {e}"), cfg.seed),
    };

    let (_store, sealed) = match sealed_for_protocol(cfg, "receipt-lost", bundle_id(11), 22) {
        Ok(v) => v,
        Err(e) => return RowResult::fail(FAMILY, PROPERTY, e.to_string(), cfg.seed),
    };

    // The server commits and receipts; the device never sees the response.
    let mut inj =
        Injector::armed(FaultPlan::interrupt(FaultPoint::ReceiptLostInTransit).with_seed(cfg.seed));
    match client.sync(&sealed, &key, &mut inj) {
        Err(SyncError::Interrupted(_)) => {}
        Ok(_) => {
            return RowResult::fail(FAMILY, PROPERTY, "the fault never fired".into(), cfg.seed);
        }
        Err(e) => {
            return RowResult::fail(FAMILY, PROPERTY, format!("unexpected error: {e}"), cfg.seed);
        }
    }

    // The device retries, not knowing whether the server committed.
    match client.sync(&sealed, &key, &mut Injector::none()) {
        Ok(outcome) => {
            let Some(receipt) = outcome.receipt else {
                return RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    "the retry produced no receipt".into(),
                    cfg.seed,
                );
            };
            if receipt.content_root != sealed.manifest.content_root {
                return RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    "the retry's receipt acknowledges different content".into(),
                    cfg.seed,
                );
            }
            if !outcome.already_committed {
                return RowResult::fail(
                    FAMILY,
                    PROPERTY,
                    "the retry was not recognised as already committed".into(),
                    cfg.seed,
                );
            }
            RowResult::pass(
                FAMILY,
                PROPERTY,
                "the lost-response retry returned the already-committed receipt".into(),
                cfg.seed,
            )
        }
        Err(e) => RowResult::fail(FAMILY, PROPERTY, format!("the retry failed: {e}"), cfg.seed),
    }
}

fn clone_cfg(cfg: &Config) -> Config {
    Config {
        work_dir: cfg.work_dir.clone(),
        server: cfg.server.clone(),
        seed: cfg.seed,
        chunk_size: cfg.chunk_size,
        verbose: cfg.verbose,
    }
}

/// Ensure the work directory exists.
pub fn prepare(dir: &Path) -> std::io::Result<()> {
    fs::create_dir_all(dir)
}
