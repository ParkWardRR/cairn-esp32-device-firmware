//! Device-side bundle storage: framed capture, atomic sealing, boot recovery
//! and the transactional prune journal.
//!
//! This models what the firmware must do, in the order it must do it, so the
//! harness can interrupt each step and assert what survived. The orderings here
//! are the ones the specification makes normative — they are not incidental, and
//! getting them wrong is exactly how a power cut in a parked car loses a drive.

use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use ed25519_dalek::SigningKey;
use sha2::{Digest, Sha256};

use crate::format::{
    self, ChunkDescriptor, Manifest, Member, RecordType, RecoveryState, ScanState, SegmentHeader,
    SegmentWriter, StopReason, content_root, manifest::device_key_id, scan_segment,
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
    /// would break the CRC chain, which is precisely what the chain is for.
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

/// The device's on-disk layout.
///
/// ```text
/// <root>/bundles/<bundle_id>.open     capture in progress; manifest absent
/// <root>/bundles/<bundle_id>.sealed   manifest + signature present
/// <root>/receipts/<bundle_id>.cbor    outside the bundle, so pruning payload
///                                     can never destroy the proof
/// <root>/journal/prune.log            prune intents and completions
/// ```
pub struct DeviceStore {
    root: PathBuf,
    key: SigningKey,
    device_id: [u8; 16],
    boot_id: [u8; 16],
}

impl DeviceStore {
    pub fn open(
        root: &Path,
        key: SigningKey,
        device_id: [u8; 16],
        boot_id: [u8; 16],
    ) -> Result<Self> {
        for sub in ["bundles", "receipts", "journal"] {
            fs::create_dir_all(root.join(sub))?;
        }
        Ok(Self {
            root: root.to_path_buf(),
            key,
            device_id,
            boot_id,
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

    /// Capture a trip and seal it, honouring any armed fault.
    ///
    /// The sealing order is normative: flush and sync every segment, rescan to
    /// verify every frame CRC, write the manifest to a temporary file, sync it,
    /// write the signature, sync, rename both into place, sync the directory,
    /// then rename the bundle directory to `.sealed`. A crash at any step leaves
    /// the raw segments intact, and the absence of `.sealed` simply means
    /// sealing is re-run at boot. Segments are never discarded because sealing
    /// failed.
    pub fn capture_and_seal(&self, spec: &CaptureSpec, inj: &mut Injector) -> Result<SealedBundle> {
        let CaptureSpec {
            bundle_id,
            gnss_samples,
            obd_samples,
            journal_entries,
            chunk_size,
            clock_jump_at,
        } = *spec;

        let open_dir = self
            .bundles_dir()
            .join(format!("{}.open", format::hex(&bundle_id)));
        fs::create_dir_all(&open_dir)?;

        // ── capture ──────────────────────────────────────────────────────────
        // Capture segments share one chain; the journal is a separate chain
        // with its own sequence space (spec §3.2.1).
        let mut capture_segments: Vec<(String, Vec<u8>)> = Vec::new();
        let mut state = ScanState::default();
        let mut segment_index = 0u32;
        let mut frame_counter = 0usize;
        let mut counts: BTreeMap<RecordType, u32> = BTreeMap::new();

        let mut writer = SegmentWriter::new(self.segment_header(segment_index), state);

        for i in 0..gnss_samples {
            // A receiver reporting a backwards time jump is recorded honestly:
            // flagged, with its accuracy marked unknown, and never reordered.
            let (payload, flags) = match clock_jump_at {
                Some(j) if j == i => (
                    gnss_payload_with_utc(i, -30_000, 0xFFFF),
                    crate::format::frame::flags::ESTIMATED_UTC,
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
            *counts.entry(RecordType::GNSS_SAMPLE).or_insert(0) += 1;
            frame_counter += 1;

            // A power cut can land between any two frames.
            self.maybe_interrupt_frame(inj, frame_counter, &open_dir, &writer, segment_index)?;

            if writer.bytes().len() >= SEGMENT_ROTATE_BYTES {
                state = writer.next_state();
                capture_segments.push((segment_name(segment_index), writer.into_bytes()));
                segment_index += 1;
                writer = SegmentWriter::new(self.segment_header(segment_index), state);
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
            *counts.entry(RecordType::OBD_SNAPSHOT).or_insert(0) += 1;
            frame_counter += 1;
            self.maybe_interrupt_frame(inj, frame_counter, &open_dir, &writer, segment_index)?;
        }

        let last_seq = writer.last_seq();
        capture_segments.push((segment_name(segment_index), writer.into_bytes()));

        // Journal chain.
        let mut jw = SegmentWriter::new(self.segment_header(0), ScanState::default());
        for i in 0..journal_entries {
            jw.append(
                RecordType::STATE_TRANSITION,
                1,
                0,
                (i * 500) as u32,
                &[0u8; 20],
            )?;
            *counts.entry(RecordType::STATE_TRANSITION).or_insert(0) += 1;
        }
        let journal = jw.into_bytes();

        // ── write segments durably ───────────────────────────────────────────
        for (name, bytes) in &capture_segments {
            write_sync(&open_dir.join(name), bytes)?;
        }
        write_sync(&open_dir.join("journal.seg"), &journal)?;
        sync_dir(&open_dir)?;

        inj.check(FaultPoint::AfterSegmentSync)?;

        // ── rescan before sealing ────────────────────────────────────────────
        // Sealing must not vouch for data it has not verified, so every frame
        // CRC is re-checked from the medium rather than trusted from memory.
        let mut members = Vec::new();
        let mut recovery = RecoveryState::Clean;
        let mut discarded = 0u32;

        let mut scan_state = ScanState::default();
        for (name, _) in &capture_segments {
            let on_disk = fs::read(open_dir.join(name))?;
            let res = scan_segment(&on_disk, scan_state)?;
            if res.stop != StopReason::Eof {
                recovery = RecoveryState::RecoveredTail;
                discarded += res.discarded_tail_bytes;
            }
            scan_state = res.next;
            members.push(Member {
                name: name.clone(),
                length: on_disk.len() as u64,
                sha256: Sha256::digest(&on_disk).into(),
            });
        }
        let journal_on_disk = fs::read(open_dir.join("journal.seg"))?;
        members.push(Member {
            name: "journal.seg".into(),
            length: journal_on_disk.len() as u64,
            sha256: Sha256::digest(&journal_on_disk).into(),
        });

        format::merkle::sort_members(&mut members);

        // The bundle byte stream is members concatenated in canonical order.
        let mut stream = Vec::new();
        for m in &members {
            stream.extend_from_slice(&fs::read(open_dir.join(&m.name))?);
        }

        let chunk_descriptors = chunk_stream(&stream, chunk_size);
        let root = content_root(&members)?;

        let manifest = Manifest {
            manifest_version: format::manifest::MANIFEST_VERSION,
            bundle_id,
            device_id: self.device_id,
            device_key_id: device_key_id(&self.key.verifying_key()),
            boot_id: self.boot_id,
            firmware_version: "cairn-emulator-v2".into(),
            schema_version: 1,
            capture_started_monotonic_us: 1_000_000,
            capture_ended_monotonic_us: 1_000_000
                + (gnss_samples as u64 + obd_samples as u64) * 1_000_000,
            utc_basis_ms: 1_790_000_000_000,
            utc_basis_acc_ms: 250,
            first_seq: 0,
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
        };

        let (manifest_bytes, signature) = manifest.sign(&self.key)?;

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
        sync_dir(&open_dir)?;

        let sealed_dir = self
            .bundles_dir()
            .join(format!("{}.sealed", format::hex(&bundle_id)));
        fs::rename(&open_dir, &sealed_dir)?;
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
        if let Some(FaultPoint::MidFrameWrite { frame, drop_bytes }) = inj.plan().map(|p| p.point) {
            {
                if frame == frame_counter
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
            }
        }

        Ok(())
    }

    fn segment_header(&self, index: u32) -> SegmentHeader {
        SegmentHeader {
            format_version: format::FORMAT_VERSION,
            device_id: self.device_id,
            boot_id: self.boot_id,
            segment_index: index,
            first_seq: 0,
            opened_monotonic_us: 1_000_000,
        }
    }

    // ── recovery ────────────────────────────────────────────────────────────

    /// Scan every bundle directory at boot, as the firmware must.
    ///
    /// This runs automatically rather than only from a CLI tool, because a
    /// device that waits for an operator to recover it has already lost the
    /// data in practice.
    pub fn recover_at_boot(&self) -> Result<Vec<BundleRecovery>> {
        let mut out = Vec::new();

        let entries = match fs::read_dir(self.bundles_dir()) {
            Ok(e) => e,
            Err(_) => return Ok(out),
        };

        let mut dirs: Vec<PathBuf> = entries
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| p.is_dir())
            .collect();
        dirs.sort();

        for dir in dirs {
            out.push(self.recover_bundle(&dir)?);
        }

        Ok(out)
    }

    fn recover_bundle(&self, dir: &Path) -> Result<BundleRecovery> {
        let name = dir.file_name().unwrap().to_string_lossy().to_string();
        let sealed = name.ends_with(".sealed");

        let mut segments: Vec<String> = fs::read_dir(dir)?
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.starts_with("seg-") && n.ends_with(".seg"))
            .collect();
        segments.sort();

        let mut recovered_frames = 0usize;
        let mut discarded_tail_bytes = 0u32;
        let mut stop = StopReason::Eof;

        let mut state = ScanState::default();
        for seg in &segments {
            let bytes = fs::read(dir.join(seg))?;
            let res = scan_segment(&bytes, state)?;
            recovered_frames += res.frames.len();
            discarded_tail_bytes += res.discarded_tail_bytes;
            if !res.stop.clean() {
                stop = res.stop;
                break;
            }
            state = res.next;
        }

        let mut journal_frames = 0usize;
        if dir.join("journal.seg").exists() {
            let bytes = fs::read(dir.join("journal.seg"))?;
            // The journal is an independent chain, so it scans from zero.
            if let Ok(res) = scan_segment(&bytes, ScanState::default()) {
                journal_frames = res.frames.len();
            }
        }

        Ok(BundleRecovery {
            dir: dir.to_path_buf(),
            sealed,
            manifest_present: dir.join("manifest.cbor").exists(),
            signature_present: dir.join("manifest.sig").exists(),
            recovered_frames,
            journal_frames,
            discarded_tail_bytes,
            stop,
        })
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
}

fn segment_name(index: u32) -> String {
    format!("seg-{index:08}.seg")
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
