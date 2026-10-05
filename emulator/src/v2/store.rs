//! Device-side bundle storage: framed, encrypted capture; atomic sealing; boot
//! recovery; the off-card bundle counter; and the transactional prune journal.
//!
//! This models what the firmware must do, in the order it must do it, so the
//! harness can interrupt each step and assert what survived. The orderings here
//! are the ones the specification makes normative — they are not incidental, and
//! getting them wrong is exactly how a power cut in a parked car loses a drive.
//!
//! Bundle format v3 adds three things the device must get right, and this is
//! where the emulator gets them right first:
//!
//! - **Every frame is sealed** under a key derived from the device's storage
//!   root (`K_root`), which never touches the card. Recovery at boot needs no
//!   key — torn tails, CRCs and the chain are all computed over ciphertext — so
//!   a device whose key store is unavailable still recovers its data.
//! - **Every bundle names its vehicle and assignment**, fixed in every segment
//!   header and authenticated by every frame.
//! - **Every bundle carries a device counter** taken from NVS, which is *not*
//!   on the card. A card restored to an older image therefore presents
//!   counters the device has already passed, and can never cause a new bundle
//!   to reuse one.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use ed25519_dalek::SigningKey;
use rand::rngs::StdRng;
use rand::{Rng, RngCore, SeedableRng};
use sha2::{Digest, Sha256};

use crate::format::{
    self, ChunkDescriptor, ENCRYPTION_SUITE_V1, JOURNAL_SEGMENT_INDEX, KeyProvider, Manifest,
    Member, RecordType, RecoveryState, RootKeyProvider, ScanResult, ScanState, SegmentHeader,
    SegmentWriter, StopReason, bind::JOURNAL_MEMBER_NAME, content_root, manifest::device_key_id,
    scan_segment, segment::parse_segment_header, verify_members_against_manifest,
};

use super::fault::{FaultPoint, Injector, Interrupted};

/// Segments rotate at this size, so FAT metadata damage costs one segment
/// rather than a whole trip.
pub const SEGMENT_ROTATE_BYTES: usize = 32 * 1024;

/// Default transfer chunk size.
pub const DEFAULT_CHUNK_SIZE: usize = 256 * 1024;

#[derive(Debug)]
pub enum StoreError {
    Io(String),
    Format(format::FormatError),
    Interrupted(Interrupted),
    /// A prune was attempted without a verified receipt on disk. This is the
    /// invariant the whole design exists to protect, so it is its own error.
    NoReceipt(String),
    /// Sealing was refused because a frame did not authenticate under this
    /// device's root: the device will not sign a manifest vouching for data it
    /// cannot show it wrote. The bytes are left exactly as they were.
    Unauthenticated {
        bundle: String,
        detail: String,
    },
    /// The NVS counter record is unreadable. The device must stop rather than
    /// guess a counter: guessing low reuses a spent counter, which the server
    /// quarantines; there is no safe default.
    Nvs(String),
}

impl std::fmt::Display for StoreError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "io: {e}"),
            Self::Format(e) => write!(f, "format: {e}"),
            Self::Interrupted(i) => write!(f, "{i}"),
            Self::NoReceipt(b) => write!(
                f,
                "refusing to prune {b}: no verified receipt on disk — this must never happen"
            ),
            Self::Unauthenticated { bundle, detail } => write!(
                f,
                "refusing to seal {bundle}: its frames do not authenticate under this device's \
                 storage root ({detail}); left untouched"
            ),
            Self::Nvs(e) => write!(f, "nvs: {e}"),
        }
    }
}

impl From<std::io::Error> for StoreError {
    fn from(e: std::io::Error) -> Self {
        Self::Io(e.to_string())
    }
}

impl From<format::FormatError> for StoreError {
    fn from(e: format::FormatError) -> Self {
        Self::Format(e)
    }
}

impl From<Interrupted> for StoreError {
    fn from(e: Interrupted) -> Self {
        Self::Interrupted(e)
    }
}

pub type Result<T> = std::result::Result<T, StoreError>;

// ── identity ────────────────────────────────────────────────────────────────

/// Everything a provisioned device holds that is not on its card.
///
/// On the ESP32 the signing key and `K_root` live in flash-encrypted NVS and the
/// assignment arrives at provisioning. Here they are plain values, passed to
/// every simulated boot, because a reboot must not change who the device is.
#[derive(Clone)]
pub struct DeviceIdentity {
    pub signing_key: SigningKey,
    pub device_id: [u8; 16],
    /// `K_root`. Escrowed to the server at enrolment; never written to the card.
    pub root_key: [u8; 32],
    pub storage_key_version: u32,
    pub vehicle_id: [u8; 16],
    pub assignment_id: [u8; 16],
}

// Key material is never printed, including in a failing row's debug output.
impl std::fmt::Debug for DeviceIdentity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("DeviceIdentity")
            .field("device_id", &format::hex(&self.device_id))
            .field("storage_key_version", &self.storage_key_version)
            .field("vehicle_id", &format::hex(&self.vehicle_id))
            .field("assignment_id", &format::hex(&self.assignment_id))
            .finish_non_exhaustive()
    }
}

// ── NVS: the off-card bundle counter ────────────────────────────────────────

/// The device's non-volatile store, modelled as a directory that is **not**
/// part of the card.
///
/// Only the bundle counter lives here in the emulator. What matters is where
/// it lives: restoring, cloning or swapping the card cannot touch it, which is
/// the property that lets the server tell a restored old card from new data.
pub struct Nvs {
    dir: PathBuf,
}

const NVS_COUNTER: &str = "device_counter";

impl Nvs {
    pub fn open(dir: &Path) -> Result<Self> {
        fs::create_dir_all(dir)?;
        Ok(Self {
            dir: dir.to_path_buf(),
        })
    }

    /// The last counter value committed, or 0 if none ever was. Counters start
    /// at 1, so 0 unambiguously means "no bundle yet".
    pub fn device_counter(&self) -> Result<u64> {
        match fs::read(self.dir.join(NVS_COUNTER)) {
            Ok(raw) => std::str::from_utf8(&raw)
                .ok()
                .and_then(|s| s.trim().parse().ok())
                .ok_or_else(|| StoreError::Nvs(format!("counter record {raw:02x?} is unreadable"))),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(0),
            Err(e) => Err(e.into()),
        }
    }

    /// Increment the counter and commit it durably, returning the new value.
    ///
    /// Committed **before** the value is used in any segment header. A power
    /// cut after the commit and before the bundle is written burns the value —
    /// a gap, which is harmless. The other order would let a cut leave a bundle
    /// on the card under a counter NVS does not remember, and the next bundle
    /// would reuse it with different content: exactly what the server
    /// quarantines. The commit itself is write-temp, fsync, rename, fsync-dir,
    /// so a cut mid-commit leaves the old value, never a torn one.
    pub fn next_device_counter(&self) -> Result<u64> {
        let next = self
            .device_counter()?
            .checked_add(1)
            .ok_or_else(|| StoreError::Nvs("device counter exhausted".into()))?;
        let tmp = self.dir.join(format!("{NVS_COUNTER}.tmp"));
        write_sync(&tmp, format!("{next}\n").as_bytes())?;
        fs::rename(&tmp, self.dir.join(NVS_COUNTER))?;
        sync_dir(&self.dir)?;
        Ok(next)
    }
}

// ── capture spec and sealed bundle ──────────────────────────────────────────

/// What to capture.
///
/// A struct rather than a long positional argument list, because the options
/// that matter here are the ones a test varies and a reader needs to see named.
#[derive(Debug, Clone)]
pub struct CaptureSpec {
    pub bundle_id: [u8; 16],
    pub gnss_samples: usize,
    pub obd_samples: usize,
    pub journal_entries: usize,
    pub chunk_size: usize,

    /// Write a GNSS sample whose UTC estimate jumps backwards at this index.
    ///
    /// The jump is written *during* capture, as a real device would: the
    /// receiver reports a discontinuity and the device records it honestly,
    /// flagged and with its accuracy marked unknown. Editing a frame afterwards
    /// would break the CRC chain and the tag, which is precisely what they are
    /// for.
    pub clock_jump_at: Option<usize>,
}

impl CaptureSpec {
    pub fn new(bundle_id: [u8; 16]) -> Self {
        Self {
            bundle_id,
            gnss_samples: 20,
            obd_samples: 5,
            journal_entries: 4,
            chunk_size: DEFAULT_CHUNK_SIZE,
            clock_jump_at: None,
        }
    }

    pub fn with_chunk_size(mut self, n: usize) -> Self {
        self.chunk_size = n;
        self
    }

    pub fn with_gnss(mut self, n: usize) -> Self {
        self.gnss_samples = n;
        self
    }

    pub fn with_clock_jump_at(mut self, i: usize) -> Self {
        self.clock_jump_at = Some(i);
        self
    }
}

/// A sealed bundle ready to offer.
#[derive(Debug, Clone)]
pub struct SealedBundle {
    pub dir: PathBuf,
    pub manifest: Manifest,
    pub manifest_bytes: Vec<u8>,
    pub signature: [u8; 64],
    /// The bundle byte stream, partitioned by the manifest's chunk descriptors.
    pub stream: Vec<u8>,
}

impl SealedBundle {
    /// Chunk `i`'s bytes, sliced from the stream per the descriptors.
    pub fn chunk(&self, i: usize) -> &[u8] {
        let mut start = 0usize;
        for d in &self.manifest.chunk_descriptors[..i] {
            start += d.byte_length as usize;
        }
        let len = self.manifest.chunk_descriptors[i].byte_length as usize;
        &self.stream[start..start + len]
    }

    pub fn chunk_count(&self) -> usize {
        self.manifest.chunk_descriptors.len()
    }
}

// ── the store ───────────────────────────────────────────────────────────────

/// The device's on-disk layout.
///
/// ```text
/// <card>/bundles/<bundle_id>.open     capture in progress; manifest absent
/// <card>/bundles/<bundle_id>.sealed   manifest + signature present
/// <card>/receipts/<bundle_id>.cbor    outside the bundle, so pruning payload
///                                     can never destroy the proof
/// <card>/journal/prune.log            prune intents and completions
/// <nvs>/device_counter                NOT on the card
/// ```
pub struct DeviceStore {
    root: PathBuf,
    nvs: Nvs,
    id: DeviceIdentity,
    keys: RootKeyProvider,
    boot_id: [u8; 16],
    /// The nonce stream for this boot. See [`DeviceStore::open`].
    nonces: RefCell<StdRng>,
}

impl DeviceStore {
    /// Boot a device over a card and an NVS directory.
    ///
    /// `seed` makes the run reproducible. Frame nonces are drawn from a
    /// generator seeded with `SHA-256(seed ‖ boot_id)`, never from the seed
    /// alone, so every simulated boot draws a different stream — the property
    /// the hardware RNG has. Nonce reuse across boots would be harmless anyway
    /// (`boot_id` is an HKDF input, so each boot's keys differ), but a model
    /// that only stays safe by that argument would hide a firmware bug that
    /// seeds its RNG badly.
    pub fn open(
        card: &Path,
        nvs: &Path,
        id: DeviceIdentity,
        boot_id: [u8; 16],
        seed: u64,
    ) -> Result<Self> {
        for sub in ["bundles", "receipts", "journal"] {
            fs::create_dir_all(card.join(sub))?;
        }

        let mut h = Sha256::new();
        h.update(b"cairn-emulator/frame-nonces");
        h.update(seed.to_le_bytes());
        h.update(boot_id);
        let rng = StdRng::from_seed(h.finalize().into());

        Ok(Self {
            root: card.to_path_buf(),
            nvs: Nvs::open(nvs)?,
            keys: RootKeyProvider::new(id.root_key, id.storage_key_version),
            id,
            boot_id,
            nonces: RefCell::new(rng),
        })
    }

    pub fn bundles_dir(&self) -> PathBuf {
        self.root.join("bundles")
    }

    pub fn receipt_path(&self, bundle_id: &[u8; 16]) -> PathBuf {
        self.root
            .join("receipts")
            .join(format!("{}.cbor", format::hex(bundle_id)))
    }

    fn prune_log(&self) -> PathBuf {
        self.root.join("journal").join("prune.log")
    }

    /// The device's own key provider, for keyed verification.
    pub fn key_provider(&self) -> &RootKeyProvider {
        &self.keys
    }

    pub fn identity(&self) -> &DeviceIdentity {
        &self.id
    }

    /// The last bundle counter committed to NVS.
    pub fn device_counter(&self) -> Result<u64> {
        self.nvs.device_counter()
    }

    /// A per-segment nonce stream, split off this boot's generator.
    fn segment_nonces(&self) -> Box<dyn RngCore> {
        Box::new(StdRng::from_seed(self.nonces.borrow_mut().r#gen()))
    }

    /// Capture a trip and seal it, honouring any armed fault.
    ///
    /// The bundle counter is taken from NVS — incremented and committed —
    /// before anything is written, and then fixed in every segment header.
    ///
    /// The sealing order is normative: flush and sync every segment, rescan to
    /// verify every frame, write the manifest to a temporary file, sync it,
    /// write the signature, sync, rename both into place, sync the directory,
    /// then rename the bundle directory to `.sealed`. A crash at any step leaves
    /// the raw segments intact, and the absence of `.sealed` simply means
    /// sealing is re-run at boot ([`DeviceStore::reseal_at_boot`]). Segments
    /// are never discarded because sealing failed.
    pub fn capture_and_seal(&self, spec: &CaptureSpec, inj: &mut Injector) -> Result<SealedBundle> {
        let CaptureSpec {
            bundle_id,
            gnss_samples,
            obd_samples,
            journal_entries,
            chunk_size,
            clock_jump_at,
        } = *spec;

        let counter = self.nvs.next_device_counter()?;

        let open_dir = self
            .bundles_dir()
            .join(format!("{}.open", format::hex(&bundle_id)));
        fs::create_dir_all(&open_dir)?;

        // ── capture ──────────────────────────────────────────────────────────
        // Capture segments share one chain; the journal is a separate chain
        // with its own sequence space (spec §3.2.1).
        let mut capture_segments: Vec<(String, Vec<u8>)> = Vec::new();
        let mut segment_index = 0u32;
        let mut frame_counter = 0usize;

        let mut writer = self.writer(segment_index, counter, ScanState::default())?;

        for i in 0..gnss_samples {
            // A receiver reporting a backwards time jump is recorded honestly:
            // flagged, with its accuracy marked unknown, and never reordered.
            let (payload, flags) = match clock_jump_at {
                Some(j) if j == i => (
                    gnss_payload_with_utc(i, -30_000, 0xFFFF),
                    format::frame::flags::ESTIMATED_UTC,
                ),
                _ => (gnss_payload(i), 0),
            };
            writer.append(
                RecordType::GNSS_SAMPLE,
                1,
                flags,
                (i * 1000) as u32,
                &payload,
            )?;
            frame_counter += 1;

            // A power cut can land between any two frames.
            self.maybe_interrupt_frame(inj, frame_counter, &open_dir, &writer, segment_index)?;

            if writer.bytes().len() >= SEGMENT_ROTATE_BYTES {
                let state = writer.next_state();
                capture_segments.push((segment_name(segment_index), writer.into_bytes()));
                segment_index += 1;
                writer = self.writer(segment_index, counter, state)?;
            }
        }

        for i in 0..obd_samples {
            writer.append(
                RecordType::OBD_SNAPSHOT,
                1,
                0,
                (gnss_samples * 1000 + i * 1000) as u32,
                &obd_payload(),
            )?;
            frame_counter += 1;
            self.maybe_interrupt_frame(inj, frame_counter, &open_dir, &writer, segment_index)?;
        }

        capture_segments.push((segment_name(segment_index), writer.into_bytes()));

        // Journal chain: its own reserved index, so its own key.
        let mut jw = self.writer(JOURNAL_SEGMENT_INDEX, counter, ScanState::default())?;
        for i in 0..journal_entries {
            jw.append(
                RecordType::STATE_TRANSITION,
                1,
                0,
                (i * 500) as u32,
                &[0u8; 20],
            )?;
        }
        let journal = jw.into_bytes();

        // ── write segments durably ───────────────────────────────────────────
        for (name, bytes) in &capture_segments {
            write_sync(&open_dir.join(name), bytes)?;
        }
        write_sync(&open_dir.join(JOURNAL_MEMBER_NAME), &journal)?;
        sync_dir(&open_dir)?;

        inj.check(FaultPoint::AfterSegmentSync)?;

        self.seal_open_dir(&open_dir, bundle_id, chunk_size, inj)
    }

    /// A writer for one segment of the bundle with counter `counter`.
    fn writer(&self, segment_index: u32, counter: u64, state: ScanState) -> Result<SegmentWriter> {
        let header = SegmentHeader {
            device_id: self.id.device_id,
            boot_id: self.boot_id,
            vehicle_id: self.id.vehicle_id,
            assignment_id: self.id.assignment_id,
            segment_index,
            opened_monotonic_us: 1_000_000,
            storage_key_version: self.id.storage_key_version,
            device_counter: counter,
            ..SegmentHeader::default()
        };
        Ok(SegmentWriter::new(
            header,
            state,
            &self.keys,
            self.segment_nonces(),
        )?)
    }

    /// Seal a bundle directory whose segments are already durable.
    ///
    /// Used both at the end of capture and at boot for a bundle whose sealing
    /// was interrupted. Everything the manifest says is derived from the bytes
    /// on the medium, never from memory: identity from the segment headers,
    /// counts and sequence bounds from a rescan. That is what makes a re-seal
    /// after a power cut produce the same claim the original seal would have —
    /// and in particular the *same* `device_counter`, which is authenticated by
    /// every frame and so cannot be replaced by a fresh one.
    fn seal_open_dir(
        &self,
        open_dir: &Path,
        bundle_id: [u8; 16],
        chunk_size: usize,
        inj: &mut Injector,
    ) -> Result<SealedBundle> {
        let label = format::hex(&bundle_id);

        // ── rescan before sealing ────────────────────────────────────────────
        // Sealing must not vouch for data it has not verified, so every frame
        // is re-checked from the medium rather than trusted from memory — and
        // with the key, because this device holds it and is about to sign.
        let scan = scan_dir(open_dir, Some(&self.keys))?;
        if let Some(e) = scan.journal_error.clone() {
            return Err(e.into());
        }
        if let Some(why) = scan.auth_failure() {
            return Err(StoreError::Unauthenticated {
                bundle: label,
                detail: why,
            });
        }
        let first = scan
            .capture
            .first()
            .ok_or_else(|| StoreError::Io(format!("{label}: no capture segment to seal")))?;
        let identity = first.header.clone();

        // One bundle, one identity. A segment that disagrees is not this
        // bundle's, and signing a manifest over it would be the binding
        // failure §5.4 exists to catch.
        for res in scan.capture.iter().chain(scan.journal.iter()) {
            let h = &res.header;
            if h.device_id != identity.device_id
                || h.vehicle_id != identity.vehicle_id
                || h.assignment_id != identity.assignment_id
                || h.device_counter != identity.device_counter
                || h.storage_key_version != identity.storage_key_version
            {
                return Err(StoreError::Unauthenticated {
                    bundle: label,
                    detail: format!("segment {} carries a different identity", h.segment_index),
                });
            }
        }

        let mut counts: BTreeMap<RecordType, u32> = BTreeMap::new();
        let mut recovery = RecoveryState::Clean;
        let mut discarded = 0u32;
        let mut last_frame = None;
        for res in &scan.capture {
            if res.stop != StopReason::Eof {
                recovery = RecoveryState::RecoveredTail;
                discarded += res.discarded_tail_bytes;
            }
            for (rt, n) in &res.record_counts {
                *counts.entry(*rt).or_insert(0) += *n as u32;
            }
            if let Some(f) = res.frames.last() {
                last_frame = Some((f.seq, f.monotonic_ms, res.header.opened_monotonic_us));
            }
        }
        if let Some(j) = &scan.journal {
            for (rt, n) in &j.record_counts {
                *counts.entry(*rt).or_insert(0) += *n as u32;
            }
        }

        let mut members = Vec::new();
        for name in scan.member_names() {
            let on_disk = fs::read(open_dir.join(&name))?;
            members.push(Member {
                name,
                length: on_disk.len() as u64,
                sha256: Sha256::digest(&on_disk).into(),
            });
        }
        format::merkle::sort_members(&mut members);

        // The bundle byte stream is members concatenated in canonical order.
        let mut stream = Vec::new();
        for m in &members {
            stream.extend_from_slice(&fs::read(open_dir.join(&m.name))?);
        }

        let chunk_descriptors = chunk_stream(&stream, chunk_size);
        let root = content_root(&members)?;

        let (last_seq, ended_us) = match last_frame {
            Some((seq, ms, opened)) => (seq, opened + ms as u64 * 1000),
            None => (first.header.first_seq, first.header.opened_monotonic_us),
        };

        let manifest = Manifest {
            manifest_version: format::manifest::MANIFEST_VERSION,
            bundle_id,
            device_id: identity.device_id,
            device_key_id: device_key_id(&self.id.signing_key.verifying_key()),
            boot_id: identity.boot_id,
            firmware_version: "cairn-emulator-v3".into(),
            schema_version: 1,
            capture_started_monotonic_us: identity.opened_monotonic_us,
            capture_ended_monotonic_us: ended_us,
            utc_basis_ms: 1_790_000_000_000,
            utc_basis_acc_ms: 250,
            first_seq: identity.first_seq,
            last_seq,
            record_counts: counts,
            members: members.clone(),
            chunk_descriptors,
            content_root: root,
            previous_bundle_root: None,
            policy_version: 1,
            recovery_state: recovery,
            discarded_tail_bytes: discarded,
            signature_algorithm: format::manifest::SIGNATURE_ALGORITHM_ED25519.into(),
            trip_seq: None,
            vehicle_id: identity.vehicle_id,
            assignment_id: identity.assignment_id,
            device_counter: identity.device_counter,
            storage_key_version: identity.storage_key_version,
            encryption_suite: ENCRYPTION_SUITE_V1.into(),
        };

        // Check the claim against the bytes before signing it: the same §5.4
        // check intake will run, so a device bug surfaces here rather than as a
        // refused upload from a car.
        let on_disk: BTreeMap<String, Vec<u8>> = members
            .iter()
            .map(|m| Ok((m.name.clone(), fs::read(open_dir.join(&m.name))?)))
            .collect::<std::io::Result<_>>()?;
        verify_members_against_manifest(&manifest, |n| on_disk.get(n).map(|v| v.as_slice()))?;

        let (manifest_bytes, signature) = manifest.sign(&self.id.signing_key)?;

        // ── seal ─────────────────────────────────────────────────────────────
        let tmp_manifest = open_dir.join("manifest.cbor.tmp");
        fs::write(&tmp_manifest, &manifest_bytes)?;

        inj.check(FaultPoint::BeforeManifestSync)?;

        sync_file(&tmp_manifest)?;
        let tmp_sig = open_dir.join("manifest.sig.tmp");
        write_sync(&tmp_sig, &signature)?;

        inj.check(FaultPoint::AfterManifestSync)?;

        fs::rename(&tmp_manifest, open_dir.join("manifest.cbor"))?;
        fs::rename(&tmp_sig, open_dir.join("manifest.sig"))?;
        sync_dir(open_dir)?;

        let sealed_dir = self.bundles_dir().join(format!("{label}.sealed"));
        fs::rename(open_dir, &sealed_dir)?;
        sync_dir(&self.bundles_dir())?;

        inj.check(FaultPoint::AfterSealRename)?;

        Ok(SealedBundle {
            dir: sealed_dir,
            manifest,
            manifest_bytes,
            signature,
            stream,
        })
    }

    /// Interrupt mid-capture, modelling a power cut between frames or part-way
    /// through one.
    fn maybe_interrupt_frame(
        &self,
        inj: &mut Injector,
        frame_counter: usize,
        open_dir: &Path,
        writer: &SegmentWriter,
        segment_index: u32,
    ) -> Result<()> {
        // A clean cut between frames: everything written so far is durable.
        if inj
            .should_fire(FaultPoint::AfterFrameWrite(frame_counter))
            .is_some()
        {
            write_sync(&open_dir.join(segment_name(segment_index)), writer.bytes())?;
            sync_dir(open_dir)?;
            return Err(StoreError::Interrupted(Interrupted {
                point: FaultPoint::AfterFrameWrite(frame_counter),
            }));
        }

        // A torn write: only part of the final frame reached the medium. This is
        // the case that unframed records cannot distinguish from valid data.
        if let Some(FaultPoint::MidFrameWrite { frame, drop_bytes }) = inj.plan().map(|p| p.point)
            && frame == frame_counter
            && inj
                .should_fire(FaultPoint::MidFrameWrite { frame, drop_bytes })
                .is_some()
        {
            let full = writer.bytes();
            let keep = full.len().saturating_sub(drop_bytes);
            write_sync(&open_dir.join(segment_name(segment_index)), &full[..keep])?;
            sync_dir(open_dir)?;
            return Err(StoreError::Interrupted(Interrupted {
                point: FaultPoint::MidFrameWrite { frame, drop_bytes },
            }));
        }

        Ok(())
    }

    // ── recovery ────────────────────────────────────────────────────────────

    /// Scan every bundle directory at boot, as the firmware must.
    ///
    /// This runs automatically rather than only from a CLI tool, because a
    /// device that waits for an operator to recover it has already lost the
    /// data in practice. It is a **structural** scan and needs no key: a
    /// device whose key store is unreadable at boot still finds its torn tails
    /// and keeps every intact frame.
    pub fn recover_at_boot(&self) -> Result<Vec<BundleRecovery>> {
        let mut out = Vec::new();
        for dir in self.bundle_dirs()? {
            out.push(recover_bundle(&dir)?);
        }
        Ok(out)
    }

    /// Re-run sealing for every bundle a power cut left unsealed.
    ///
    /// Each `.open` directory holding at least one capture segment is sealed
    /// from its bytes on the medium (see [`DeviceStore::seal_open_dir`]). A
    /// bundle whose frames do not authenticate under this device's root is
    /// refused and left exactly as it is — reported, never deleted, never
    /// signed.
    pub fn reseal_at_boot(&self, chunk_size: usize) -> Result<Vec<Result<SealedBundle>>> {
        let mut out = Vec::new();
        for dir in self.bundle_dirs()? {
            let name = dir.file_name().unwrap().to_string_lossy().to_string();
            let Some(hex_id) = name.strip_suffix(".open") else {
                continue;
            };
            let Some(bundle_id) = format::unhex_array::<16>(hex_id) else {
                continue;
            };
            out.push(self.seal_open_dir(&dir, bundle_id, chunk_size, &mut Injector::none()));
        }
        Ok(out)
    }

    /// Scan one bundle directory with this device's key: the keyed verdict.
    pub fn verify_bundle(&self, dir: &Path) -> Result<BundleScan> {
        scan_dir(dir, Some(&self.keys))
    }

    fn bundle_dirs(&self) -> Result<Vec<PathBuf>> {
        let entries = match fs::read_dir(self.bundles_dir()) {
            Ok(e) => e,
            Err(_) => return Ok(Vec::new()),
        };
        let mut dirs: Vec<PathBuf> = entries
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| p.is_dir())
            .collect();
        dirs.sort();
        Ok(dirs)
    }

    // ── prune ───────────────────────────────────────────────────────────────

    /// Persist a verified receipt, outside the bundle directory.
    ///
    /// Receipts live outside so that pruning payload can never destroy the
    /// proof of delivery, and they are retained longer than the bundles they
    /// acknowledge.
    pub fn store_receipt(&self, bundle_id: &[u8; 16], encoded: &[u8]) -> Result<()> {
        write_sync(&self.receipt_path(bundle_id), encoded)?;
        sync_dir(&self.root.join("receipts"))?;
        Ok(())
    }

    pub fn has_receipt(&self, bundle_id: &[u8; 16]) -> bool {
        self.receipt_path(bundle_id).exists()
    }

    /// Prune a bundle's payload transactionally.
    ///
    /// Journal `prune_intent`, delete the payload, journal `prune_complete`. A
    /// crash in the middle is resolved by replaying the journal at boot, so the
    /// bundle is either fully present or fully pruned and the journal says
    /// which.
    ///
    /// The receipt check is the hard invariant: no verified receipt means no
    /// prune, at any age, under any storage pressure.
    pub fn prune(&self, bundle_id: &[u8; 16], inj: &mut Injector) -> Result<()> {
        if !self.has_receipt(bundle_id) {
            return Err(StoreError::NoReceipt(format::hex(bundle_id)));
        }

        let hex_id = format::hex(bundle_id);
        let dir = self.bundles_dir().join(format!("{hex_id}.sealed"));

        self.append_prune_journal(&format!("prune_intent {hex_id}"))?;
        inj.check(FaultPoint::AfterPruneIntent)?;

        if dir.exists() {
            // Contents before the directory: removing a non-empty directory
            // fails, which is how v1 silently left payload behind.
            for entry in fs::read_dir(&dir)? {
                let entry = entry?;
                fs::remove_file(entry.path())?;
            }
            fs::remove_dir(&dir)?;
            sync_dir(&self.bundles_dir())?;
        }

        inj.check(FaultPoint::AfterPayloadDelete)?;

        self.append_prune_journal(&format!("prune_complete {hex_id}"))?;
        Ok(())
    }

    fn append_prune_journal(&self, line: &str) -> Result<()> {
        let mut f = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(self.prune_log())?;
        writeln!(f, "{line}")?;
        f.sync_all()?;
        Ok(())
    }

    /// Replay the prune journal, finishing any interrupted prune.
    ///
    /// An intent without a completion means the device died mid-prune. The
    /// payload may be partly or wholly gone; the receipt proves the data is
    /// safe on the server, so finishing the deletion is correct and leaves the
    /// device in a defined state.
    pub fn replay_prune_journal(&self) -> Result<Vec<String>> {
        let log = match fs::read_to_string(self.prune_log()) {
            Ok(s) => s,
            Err(_) => return Ok(Vec::new()),
        };

        let mut intents = Vec::new();
        let mut completed = std::collections::HashSet::new();

        for line in log.lines() {
            let mut parts = line.split_whitespace();
            match (parts.next(), parts.next()) {
                (Some("prune_intent"), Some(id)) => intents.push(id.to_string()),
                (Some("prune_complete"), Some(id)) => {
                    completed.insert(id.to_string());
                }
                _ => {}
            }
        }

        let mut finished = Vec::new();
        for id in intents {
            if completed.contains(&id) {
                continue;
            }

            let dir = self.bundles_dir().join(format!("{id}.sealed"));
            if dir.exists() {
                for entry in fs::read_dir(&dir)? {
                    let entry = entry?;
                    fs::remove_file(entry.path())?;
                }
                fs::remove_dir(&dir)?;
                sync_dir(&self.bundles_dir())?;
            }
            self.append_prune_journal(&format!("prune_complete {id}"))?;
            finished.push(id);
        }

        Ok(finished)
    }
}

// ── scanning a bundle directory ─────────────────────────────────────────────

/// Both chains of one bundle directory, scanned with or without a key.
#[derive(Debug, Clone)]
pub struct BundleScan {
    /// Capture segments in index order, stopping after the first that does not
    /// end cleanly: past a damaged segment the chain expectation is unknowable.
    pub capture: Vec<ScanResult>,
    /// The journal, scanned from the zero state as its own chain. `None` when
    /// the bundle has no journal.
    pub journal: Option<ScanResult>,
    /// Why the journal could not be scanned at all (bad header, key refusal).
    /// Kept apart from the capture verdict: an unreadable journal must not cost
    /// the capture frames at boot, though it does block sealing.
    pub journal_error: Option<format::FormatError>,
    /// Capture segments that were present but not scanned because an earlier
    /// one did not end cleanly.
    pub unscanned: Vec<String>,
}

impl BundleScan {
    pub fn capture_frames(&self) -> usize {
        self.capture.iter().map(|r| r.frames.len()).sum()
    }

    /// The first non-clean capture stop, or EOF.
    pub fn capture_stop(&self) -> StopReason {
        self.capture
            .iter()
            .map(|r| r.stop)
            .find(|s| !s.clean())
            .unwrap_or(StopReason::Eof)
    }

    /// Why sealing must be refused, if any frame failed authentication.
    fn auth_failure(&self) -> Option<String> {
        self.capture
            .iter()
            .chain(self.journal.iter())
            .find(|r| r.stop == StopReason::AuthFailed)
            .map(|r| r.stop_detail.clone())
    }

    /// Every member a manifest for this directory would list: the scanned and
    /// unscanned capture segments, and the journal.
    fn member_names(&self) -> Vec<String> {
        let mut names: Vec<String> = self
            .capture
            .iter()
            .map(|r| segment_name(r.header.segment_index))
            .chain(self.unscanned.iter().cloned())
            .collect();
        if self.journal.is_some() {
            names.push(JOURNAL_MEMBER_NAME.to_string());
        }
        names
    }
}

/// Scan a bundle directory's capture chain and journal.
///
/// A header error, or a key refusal such as a version mismatch, is returned as
/// an error: the segment is unusable with what the caller holds, and is left
/// alone.
pub fn scan_dir(dir: &Path, keys: Option<&dyn KeyProvider>) -> Result<BundleScan> {
    let mut segments: Vec<String> = fs::read_dir(dir)?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().to_string())
        .filter(|n| n.starts_with("seg-") && n.ends_with(".seg"))
        .collect();
    segments.sort();

    let mut out = BundleScan {
        capture: Vec::new(),
        journal: None,
        journal_error: None,
        unscanned: Vec::new(),
    };

    let mut state = ScanState::default();
    let mut stopped = false;
    for seg in &segments {
        if stopped {
            out.unscanned.push(seg.clone());
            continue;
        }
        let bytes = fs::read(dir.join(seg))?;
        let res = scan_segment(&bytes, state, keys)?;
        state = res.next;
        stopped = !res.stop.clean();
        out.capture.push(res);
    }

    let journal = dir.join(JOURNAL_MEMBER_NAME);
    if journal.exists() {
        // The journal is an independent chain, so it scans from zero.
        match scan_segment(&fs::read(&journal)?, ScanState::default(), keys) {
            Ok(res) => out.journal = Some(res),
            Err(e) => out.journal_error = Some(e),
        }
    }

    Ok(out)
}

/// What a boot-time scan found in one bundle directory.
#[derive(Debug, Clone)]
pub struct BundleRecovery {
    pub dir: PathBuf,
    pub sealed: bool,
    pub manifest_present: bool,
    pub signature_present: bool,
    pub recovered_frames: usize,
    pub journal_frames: usize,
    pub discarded_tail_bytes: u32,
    pub stop: StopReason,
    /// The bundle counter its first segment header carries, readable without a
    /// key. `None` if no segment header parsed.
    pub device_counter: Option<u64>,
}

/// The structural, keyless recovery scan of one bundle directory.
fn recover_bundle(dir: &Path) -> Result<BundleRecovery> {
    let name = dir.file_name().unwrap().to_string_lossy().to_string();
    let sealed = name.ends_with(".sealed");

    let scan = scan_dir(dir, None)?;
    let discarded_tail_bytes = scan.capture.iter().map(|r| r.discarded_tail_bytes).sum();

    // Identity is readable in the clear by design: a device (or intake) must
    // be able to tell which counter a bundle claims before any key is involved.
    let device_counter = scan
        .capture
        .first()
        .map(|r| r.header.device_counter)
        .or_else(|| {
            fs::read(dir.join(JOURNAL_MEMBER_NAME))
                .ok()
                .and_then(|b| parse_segment_header(&b).ok())
                .map(|(h, _)| h.device_counter)
        });

    Ok(BundleRecovery {
        dir: dir.to_path_buf(),
        sealed,
        manifest_present: dir.join("manifest.cbor").exists(),
        signature_present: dir.join("manifest.sig").exists(),
        recovered_frames: scan.capture_frames(),
        journal_frames: scan.journal.as_ref().map_or(0, |j| j.frames.len()),
        discarded_tail_bytes,
        stop: scan.capture_stop(),
        device_counter,
    })
}

fn segment_name(index: u32) -> String {
    format::bind::segment_member_name(index)
}

/// Partition a stream into chunk descriptors.
pub fn chunk_stream(stream: &[u8], chunk_size: usize) -> Vec<ChunkDescriptor> {
    let size = chunk_size.max(1);
    let mut out = Vec::new();
    let mut off = 0usize;

    while off < stream.len() {
        let end = (off + size).min(stream.len());
        let piece = &stream[off..end];
        out.push(ChunkDescriptor {
            index: out.len() as u32,
            byte_length: piece.len() as u32,
            sha256: Sha256::digest(piece).into(),
        });
        off = end;
    }

    out
}

/// A GNSS sample carrying an explicit UTC offset and accuracy.
///
/// `utc_acc_ms` of `0xFFFF` means unknown — never a plausible guess, because a
/// decoder must not infer a precision the receiver did not report.
pub fn gnss_payload_with_utc(i: usize, utc_offset_ms: i32, utc_acc_ms: u16) -> [u8; 32] {
    let mut p = gnss_payload(i);
    p[26..30].copy_from_slice(&utc_offset_ms.to_le_bytes());
    p[30..32].copy_from_slice(&utc_acc_ms.to_le_bytes());
    p
}

/// A 32-byte GNSS sample with a 3D fix, per spec §4.1.
pub fn gnss_payload(i: usize) -> [u8; 32] {
    let mut p = [0u8; 32];
    let lat = 34_000_000i32 + (i as i32) * 100;
    let lon = -118_500_000i32 + (i as i32) * 100;
    p[0..4].copy_from_slice(&lat.to_le_bytes());
    p[4..8].copy_from_slice(&lon.to_le_bytes());
    p[8..12].copy_from_slice(&5000i32.to_le_bytes());
    p[12..14].copy_from_slice(&1200u16.to_le_bytes());
    p[14..16].copy_from_slice(&9000u16.to_le_bytes());
    p[16..18].copy_from_slice(&110u16.to_le_bytes());
    p[18..20].copy_from_slice(&350u16.to_le_bytes());
    p[20..22].copy_from_slice(&500u16.to_le_bytes());
    p[22] = 2; // 3D fix
    p[23] = 9; // satellites used
    p[24] = 14; // satellites visible
    p[25] = 0x01;
    p[30..32].copy_from_slice(&120u16.to_le_bytes());
    p
}

/// A 24-byte OBD snapshot with every value available, per spec §4.4.
pub fn obd_payload() -> [u8; 24] {
    let mut p = [0u8; 24];
    p[0..2].copy_from_slice(&64i16.to_le_bytes());
    p[2..4].copy_from_slice(&2100i16.to_le_bytes());
    p[4..6].copy_from_slice(&380u16.to_le_bytes());
    p[6] = 22;
    p[7] = 41;
    p[8] = 88;
    p[9] = 31;
    p[10] = 12;
    p[11] = 0;
    p[12..16].copy_from_slice(&0x0Fu32.to_le_bytes());
    p[16..20].copy_from_slice(&0x0Fu32.to_le_bytes());
    p[20..22].copy_from_slice(&1000u16.to_le_bytes());
    p
}

// ── durability helpers ──────────────────────────────────────────────────────

fn write_sync(path: &Path, data: &[u8]) -> std::io::Result<()> {
    let mut f = fs::File::create(path)?;
    f.write_all(data)?;
    f.sync_all()
}

fn sync_file(path: &Path) -> std::io::Result<()> {
    fs::File::open(path)?.sync_all()
}

/// fsync a directory, so a rename into it is durable. Without this the file
/// contents survive a power cut but the name may not.
fn sync_dir(path: &Path) -> std::io::Result<()> {
    fs::File::open(path)?.sync_all()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("cairn-nvs-test-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        d
    }

    /// The counter starts at 0 ("no bundle yet"), each bundle takes the next
    /// value, and the value survives dropping every in-memory handle — the
    /// emulator's model of a power cut.
    #[test]
    fn nvs_counter_is_monotonic_and_durable() {
        let dir = scratch("durable");
        {
            let nvs = Nvs::open(&dir).unwrap();
            assert_eq!(nvs.device_counter().unwrap(), 0);
            assert_eq!(nvs.next_device_counter().unwrap(), 1);
            assert_eq!(nvs.next_device_counter().unwrap(), 2);
        }
        let reopened = Nvs::open(&dir).unwrap();
        assert_eq!(reopened.device_counter().unwrap(), 2);
        assert_eq!(reopened.next_device_counter().unwrap(), 3);
        let _ = fs::remove_dir_all(&dir);
    }

    /// An unreadable record stops the device rather than letting it guess: a
    /// low guess reuses a spent counter.
    #[test]
    fn nvs_refuses_an_unreadable_counter() {
        let dir = scratch("corrupt");
        let nvs = Nvs::open(&dir).unwrap();
        fs::write(dir.join(NVS_COUNTER), b"\xff\xfe").unwrap();
        assert!(matches!(nvs.next_device_counter(), Err(StoreError::Nvs(_))));
        let _ = fs::remove_dir_all(&dir);
    }
}
