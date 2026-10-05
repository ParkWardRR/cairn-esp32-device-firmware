//! Cross-module tests of the v3 sealing and binding rules.
//!
//! The conformance vectors prove agreement with the Go reference on fixed
//! bytes. These prove the *properties* on bytes this implementation writes
//! itself, so a change that kept the vectors passing by accident — or that the
//! vectors cannot see, such as how the writer chooses nonces — still fails.
//! Each test names the single property it guards; they are the targets of the
//! mutation checks recorded with the v3 port.

use rand::SeedableRng;
use rand::rngs::StdRng;

use super::{
    FRAME_HEADER_SIZE, FormatError, KeyProvider, Manifest, RecordType, RecoveryState,
    RootKeyProvider, SEGMENT_HEADER_SIZE, ScanState, SegmentHeader, SegmentWriter, StopReason,
    crc::crc32,
    derive_segment_key,
    keys::{ENCRYPTION_SUITE_V1, NONCE_SIZE},
    manifest::{MANIFEST_VERSION, SIGNATURE_ALGORITHM_ED25519},
    scan_segment,
    segment::{JOURNAL_SEGMENT_INDEX, append_segment_header},
    verify_segment_against_manifest,
};

const ROOT: [u8; 32] = [7u8; 32];

/// A named edit to one header field.
type HeaderEdit = (&'static str, fn(&mut SegmentHeader));

fn keys() -> RootKeyProvider {
    RootKeyProvider::new(ROOT, 1)
}

fn header(counter: u64) -> SegmentHeader {
    SegmentHeader {
        device_id: [0x10; 16],
        boot_id: [0xa0; 16],
        vehicle_id: [0x30; 16],
        assignment_id: [0x50; 16],
        segment_index: 0,
        opened_monotonic_us: 1_000,
        storage_key_version: 1,
        device_counter: counter,
        ..SegmentHeader::default()
    }
}

fn nonces(seed: u64) -> Box<StdRng> {
    Box::new(StdRng::seed_from_u64(seed))
}

/// A segment of `n` GNSS-sized frames with distinct plaintexts.
fn write(h: SegmentHeader, n: usize, seed: u64) -> Vec<u8> {
    let mut w = SegmentWriter::new(h, ScanState::default(), &keys(), nonces(seed)).unwrap();
    for i in 0..n {
        w.append(
            RecordType::GNSS_SAMPLE,
            1,
            0,
            i as u32 * 1000,
            &plaintext(i),
        )
        .unwrap();
    }
    w.into_bytes()
}

fn plaintext(i: usize) -> [u8; 32] {
    [i as u8; 32]
}

fn keyed(b: &[u8]) -> super::ScanResult {
    scan_segment(b, ScanState::default(), Some(&keys() as &dyn KeyProvider)).unwrap()
}

fn structural(b: &[u8]) -> super::ScanResult {
    scan_segment(b, ScanState::default(), None).unwrap()
}

/// Recompute a frame's trailing CRC after an edit, so only the tag can object.
fn repair_crc(seg: &mut [u8], frame_off: usize) {
    let len = u16::from_le_bytes([seg[frame_off], seg[frame_off + 1]]) as usize;
    let crc = crc32(&seg[frame_off..frame_off + len - 4]);
    seg[frame_off + len - 4..frame_off + len].copy_from_slice(&crc.to_le_bytes());
}

#[test]
fn round_trip_in_both_modes() {
    let seg = write(header(1), 5, 1);

    let s = structural(&seg);
    assert_eq!(s.stop, StopReason::Eof);
    assert_eq!(s.frames.len(), 5);
    assert!(!s.decrypted);
    assert!(
        s.frames.iter().all(|f| f.payload.is_none()),
        "a keyless scan exposed a payload"
    );

    let k = keyed(&seg);
    assert_eq!(k.stop, StopReason::Eof);
    assert!(k.decrypted);
    for (i, f) in k.frames.iter().enumerate() {
        assert_eq!(f.payload.as_deref(), Some(&plaintext(i)[..]));
        // Ciphertext, not plaintext, is what the card holds.
        assert_ne!(&f.sealed[NONCE_SIZE..NONCE_SIZE + 32], &plaintext(i)[..]);
    }
}

/// The AAD carries the segment header. Frames moved under a header that
/// differs only in `device_counter` — which is not a KDF input, so the key is
/// the same — must fail. Without the header in the AAD they would open.
#[test]
fn frames_cannot_move_to_another_segment_header() {
    let original = write(header(42), 3, 2);
    assert_eq!(
        derive_segment_key(&ROOT, &header(42)),
        derive_segment_key(&ROOT, &header(43)),
        "precondition: the counter is not a KDF input"
    );

    let mut moved = Vec::new();
    let mut h43 = header(43);
    h43.first_seq = 0;
    append_segment_header(&mut moved, &h43);
    moved.extend_from_slice(&original[SEGMENT_HEADER_SIZE..]);

    let s = structural(&moved);
    assert_eq!(
        (s.stop, s.frames.len()),
        (StopReason::Eof, 3),
        "structure is untouched by the move"
    );

    let k = keyed(&moved);
    assert_eq!(k.stop, StopReason::AuthFailed);
    assert_eq!(k.frames.len(), 0);
    assert_eq!(k.stop_offset, SEGMENT_HEADER_SIZE);
}

/// The AAD carries the frame header as written. Editing a header field (here
/// the flags) and repairing the CRC leaves the structure valid but must fail
/// the tag.
#[test]
fn frame_header_is_authenticated() {
    // The last frame, so repairing its CRC does not break a successor's
    // prev_crc32 link — the edit must be invisible to everything but the tag.
    let mut seg = write(header(1), 3, 3);
    let last = SEGMENT_HEADER_SIZE + 2 * 100;
    seg[last + 4] ^= 0x02; // DEGRADED flag
    repair_crc(&mut seg, last);

    assert_eq!(structural(&seg).stop, StopReason::Eof);
    let k = keyed(&seg);
    assert_eq!((k.stop, k.frames.len()), (StopReason::AuthFailed, 2));
}

/// A ciphertext bit flip with a repaired CRC: only the tag catches it, and the
/// keyed scan stops there rather than skipping past.
#[test]
fn ciphertext_tamper_with_repaired_crc_fails_auth() {
    let mut seg = write(header(1), 4, 4);
    let last = SEGMENT_HEADER_SIZE + 3 * 100;
    seg[last + FRAME_HEADER_SIZE + NONCE_SIZE] ^= 0x01;
    repair_crc(&mut seg, last);

    assert_eq!(structural(&seg).stop, StopReason::Eof);
    let k = keyed(&seg);
    assert_eq!(
        (k.stop, k.frames.len(), k.stop_offset),
        (StopReason::AuthFailed, 3, last)
    );
}

/// Every identity input reaches the key: a header edited to another vehicle
/// (CRC repaired) derives a different key and fails at frame 0.
#[test]
fn relabelled_vehicle_fails_at_the_first_frame() {
    let seg = write(header(1), 2, 5);
    let mut relabelled = Vec::new();
    let mut h = header(1);
    h.vehicle_id = [0x31; 16];
    append_segment_header(&mut relabelled, &h);
    relabelled.extend_from_slice(&seg[SEGMENT_HEADER_SIZE..]);

    let k = keyed(&relabelled);
    assert_eq!((k.stop, k.frames.len()), (StopReason::AuthFailed, 0));
}

/// Nonces are random, never a function of `seq`. Two writers under the same
/// key writing the same `seq` and plaintext — exactly what happens when a torn
/// tail is truncated and the device writes that `seq` again — must not reuse a
/// nonce. And within a segment no two frames share one.
#[test]
fn nonces_are_never_derived_from_seq() {
    let a = write(header(1), 8, 100);
    let b = write(header(1), 8, 200);

    let nonce_at = |seg: &[u8], i: usize| {
        let off = SEGMENT_HEADER_SIZE + i * 100 + FRAME_HEADER_SIZE;
        seg[off..off + NONCE_SIZE].to_vec()
    };

    for i in 0..8 {
        assert_ne!(
            nonce_at(&a, i),
            nonce_at(&b, i),
            "seq {i} was sealed under the same (key, nonce) twice"
        );
    }
    let mut seen: Vec<_> = (0..8).map(|i| nonce_at(&a, i)).collect();
    seen.sort();
    seen.dedup();
    assert_eq!(seen.len(), 8, "a nonce repeated within one segment");
}

/// The writer must be reproducible from its nonce seed, which is what lets a
/// failing matrix run be replayed byte for byte.
#[test]
fn writer_is_reproducible_from_its_nonce_stream() {
    assert_eq!(write(header(1), 4, 9), write(header(1), 4, 9));
}

/// 4096 is the largest frame; 4097 and anything under 68 are torn tails, so a
/// corrupt length can neither overrun nor make zero progress.
#[test]
fn frame_len_bounds() {
    let mut w = SegmentWriter::new(header(1), ScanState::default(), &keys(), nonces(6)).unwrap();
    w.append(RecordType::IMU_RAW_WINDOW, 1, 0, 0, &[0u8; 4028])
        .unwrap();
    assert!(matches!(
        w.append(RecordType::IMU_RAW_WINDOW, 1, 0, 0, &[0u8; 4029]),
        Err(FormatError::PayloadTooLarge { .. })
    ));
    let seg = w.into_bytes();
    assert_eq!(seg.len(), SEGMENT_HEADER_SIZE + 4096);
    assert_eq!(keyed(&seg).stop, StopReason::Eof);

    for bad in [4097u16, 67, 0] {
        let mut b = seg.clone();
        b[SEGMENT_HEADER_SIZE..SEGMENT_HEADER_SIZE + 2].copy_from_slice(&bad.to_le_bytes());
        let s = structural(&b);
        assert_eq!(
            (s.stop, s.frames.len()),
            (StopReason::TornTail, 0),
            "frame_len {bad}"
        );
    }
}

/// A version mismatch is a refusal of the whole segment, before any frame is
/// read — not 3 auth failures that would read as tampering.
#[test]
fn version_mismatch_is_a_refusal_not_a_tag_failure() {
    let seg = write(header(1), 3, 7);
    let other = RootKeyProvider::new(ROOT, 2);
    assert_eq!(
        scan_segment(&seg, ScanState::default(), Some(&other as &dyn KeyProvider)).unwrap_err(),
        FormatError::KeyVersionMismatch {
            segment: 1,
            held: 2
        }
    );
}

/// The journal's reserved index gives it its own key, and it is its own chain
/// starting from the zero state.
#[test]
fn journal_has_its_own_key_and_chain() {
    let mut jh = header(1);
    jh.segment_index = JOURNAL_SEGMENT_INDEX;
    assert_ne!(
        derive_segment_key(&ROOT, &jh),
        derive_segment_key(&ROOT, &header(1))
    );
    let seg = write(jh, 3, 8);
    assert_eq!(keyed(&seg).stop, StopReason::Eof);
}

// ── manifest ────────────────────────────────────────────────────────────────

fn manifest(trip_seq: Option<u32>) -> Manifest {
    Manifest {
        manifest_version: MANIFEST_VERSION,
        bundle_id: [1; 16],
        device_id: [0x10; 16],
        device_key_id: [2; 8],
        boot_id: [0xa0; 16],
        firmware_version: "test".into(),
        schema_version: 1,
        capture_started_monotonic_us: 1,
        capture_ended_monotonic_us: 2,
        utc_basis_ms: 3,
        utc_basis_acc_ms: 4,
        first_seq: 0,
        last_seq: 0,
        record_counts: Default::default(),
        members: Vec::new(),
        chunk_descriptors: Vec::new(),
        content_root: [0; 32],
        previous_bundle_root: None,
        policy_version: 1,
        recovery_state: RecoveryState::Clean,
        discarded_tail_bytes: 0,
        signature_algorithm: SIGNATURE_ALGORITHM_ED25519.into(),
        trip_seq,
        vehicle_id: [0x30; 16],
        assignment_id: [0x50; 16],
        device_counter: 1,
        storage_key_version: 1,
        encryption_suite: ENCRYPTION_SUITE_V1.into(),
    }
}

#[test]
fn manifest_round_trips_with_and_without_trip_seq() {
    for (trip_seq, fields) in [(None, 27u8), (Some(9), 28)] {
        let m = manifest(trip_seq);
        let b = m.to_cbor().unwrap();
        assert_eq!(b[0], 0xb8, "a map with a one-byte length");
        assert_eq!(b[1], fields);
        assert_eq!(Manifest::from_cbor(&b).unwrap(), m);
    }
}

#[test]
fn manifest_refuses_another_encryption_suite() {
    let mut m = manifest(None);
    m.encryption_suite = "aes-256-gcm/v1".into();
    assert!(m.to_cbor().is_err());
}

/// 27 fields with trip_seq present means a mandatory key is missing, and the
/// decoder must say so rather than default it to zero.
#[test]
fn manifest_missing_mandatory_key_is_named() {
    // Encode a 28-field manifest, then drop key 28 (encryption_suite, the last
    // entry) and rewrite the count to 27.
    let full = manifest(Some(9)).to_cbor().unwrap();
    // Key 28 is `18 1c`; the 32-byte suite name is `78 20` plus its bytes.
    let last_entry = 2 + 2 + ENCRYPTION_SUITE_V1.len();
    let mut b = full[..full.len() - last_entry].to_vec();
    b[1] = 27;
    match Manifest::from_cbor(&b) {
        Err(FormatError::Malformed(why)) => assert!(why.contains("28"), "{why}"),
        other => panic!("want a missing-key error, got {other:?}"),
    }
}

// ── binding ─────────────────────────────────────────────────────────────────

#[test]
fn binding_names_the_first_mismatched_field() {
    let m = manifest(None);
    let mut h = header(1);
    assert!(verify_segment_against_manifest(&m, "seg-00000000.seg", &h).is_ok());

    let cases: [HeaderEdit; 6] = [
        ("device_id", |h| h.device_id[0] ^= 1),
        ("boot_id", |h| h.boot_id[0] ^= 1),
        ("vehicle_id", |h| h.vehicle_id[0] ^= 1),
        ("assignment_id", |h| h.assignment_id[0] ^= 1),
        ("device_counter", |h| h.device_counter += 1),
        ("storage_key_version", |h| h.storage_key_version += 1),
    ];
    for (want, mutate) in cases {
        let mut bad = header(1);
        mutate(&mut bad);
        match verify_segment_against_manifest(&m, "seg-00000000.seg", &bad) {
            Err(FormatError::BindingMismatch { field, .. }) => assert_eq!(field, want),
            other => panic!("{want}: got {other:?}"),
        }
    }

    // A segment renamed into another position keeps its own index.
    h.segment_index = 1;
    assert!(matches!(
        verify_segment_against_manifest(&m, "seg-00000000.seg", &h),
        Err(FormatError::BindingMismatch {
            field: "segment_index",
            ..
        })
    ));
}

/// §5.4 ties boot_id to capture segments only; the journal must carry the
/// reserved index instead.
#[test]
fn journal_binding_uses_the_reserved_index() {
    let m = manifest(None);
    let mut j = header(1);
    j.segment_index = JOURNAL_SEGMENT_INDEX;
    j.boot_id = [0xee; 16];
    assert!(verify_segment_against_manifest(&m, "journal.seg", &j).is_ok());

    j.segment_index = 0;
    assert!(verify_segment_against_manifest(&m, "journal.seg", &j).is_err());
}
