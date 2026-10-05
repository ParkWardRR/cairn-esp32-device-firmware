//! Binding segment headers to the signed manifest (spec §5.4).
//!
//! A valid manifest signature does not by itself tie the manifest to its
//! segments. The signature covers the member digests, so swapping one segment
//! for another would break a digest — but a bundle assembled by someone holding
//! the device key could pair any manifest with any validly written segments.
//! Checking every header against the manifest is what ties the signed claim to
//! the bytes it describes, and it needs no storage key: the identity fields are
//! readable in the clear precisely so this can run before anything is
//! decrypted.

use super::{
    FormatError, Result, hex,
    manifest::Manifest,
    segment::{JOURNAL_SEGMENT_INDEX, SegmentHeader, parse_segment_header},
};

/// The member name of journal.seg.
pub const JOURNAL_MEMBER_NAME: &str = "journal.seg";

/// The member name of capture segment `index`.
pub fn segment_member_name(index: u32) -> String {
    format!("seg-{index:08}.seg")
}

/// Check one member's parsed header against the manifest.
///
/// `name` is the member name the manifest listed it under. For a capture
/// segment the header's `segment_index` must equal the index in the name, and
/// for the journal it must be [`JOURNAL_SEGMENT_INDEX`]. Otherwise a segment
/// could be renamed into another position while keeping its own key-derivation
/// inputs, and a structural scan would happily thread it into the wrong chain.
///
/// `boot_id` is compared for capture segments only, as §5.4 states it ("the
/// segment's boot_id equals the manifest's boot_id (capture segments)"). The
/// journal records transitions that happen when no capture segment is open, so
/// the spec does not tie it to the capture boot. The Go reference compares it
/// for the journal too; for any bundle the emulator or firmware writes the two
/// agree, because the journal is opened in the same boot as the capture.
pub fn verify_segment_against_manifest(m: &Manifest, name: &str, h: &SegmentHeader) -> Result<()> {
    let mismatch = |field: &'static str, got: String, want: String| FormatError::BindingMismatch {
        member: name.to_string(),
        field,
        detail: format!("is {got}, manifest says {want}"),
    };

    if h.device_id != m.device_id {
        return Err(mismatch("device_id", hex(&h.device_id), hex(&m.device_id)));
    }
    if name != JOURNAL_MEMBER_NAME && h.boot_id != m.boot_id {
        return Err(mismatch("boot_id", hex(&h.boot_id), hex(&m.boot_id)));
    }
    if h.vehicle_id != m.vehicle_id {
        return Err(mismatch(
            "vehicle_id",
            hex(&h.vehicle_id),
            hex(&m.vehicle_id),
        ));
    }
    if h.assignment_id != m.assignment_id {
        return Err(mismatch(
            "assignment_id",
            hex(&h.assignment_id),
            hex(&m.assignment_id),
        ));
    }
    if h.device_counter != m.device_counter {
        return Err(mismatch(
            "device_counter",
            h.device_counter.to_string(),
            m.device_counter.to_string(),
        ));
    }
    if h.storage_key_version != m.storage_key_version {
        return Err(mismatch(
            "storage_key_version",
            h.storage_key_version.to_string(),
            m.storage_key_version.to_string(),
        ));
    }

    if name == JOURNAL_MEMBER_NAME {
        if h.segment_index != JOURNAL_SEGMENT_INDEX {
            return Err(mismatch(
                "segment_index",
                h.segment_index.to_string(),
                format!("{JOURNAL_SEGMENT_INDEX} (journal)"),
            ));
        }
        return Ok(());
    }
    if name != segment_member_name(h.segment_index) {
        return Err(mismatch(
            "segment_index",
            h.segment_index.to_string(),
            "the index in the member name".into(),
        ));
    }
    Ok(())
}

/// Check every available segment member against the manifest, returning the
/// first disagreement.
///
/// `member_bytes` looks up a member's bytes by name. A manifest member it does
/// not have is skipped: this is the check to run on whatever members are in
/// hand, and whether a missing member is acceptable is the caller's policy
/// (intake holds all of them, a partial mirror may not). Members that are not
/// segments are ignored. A segment whose header does not parse is an error,
/// since it cannot be shown to belong to the bundle.
///
/// The manifest's capture segments must also be named contiguously from
/// `seg-00000000`, whether or not their bytes are present: a gap would mean a
/// segment, and every frame chained through it, was removed.
pub fn verify_members_against_manifest<'a>(
    m: &Manifest,
    member_bytes: impl Fn(&str) -> Option<&'a [u8]>,
) -> Result<()> {
    let mut captures = 0u32;
    for mem in &m.members {
        let is_capture = mem.name.starts_with("seg-");
        if is_capture {
            let want = segment_member_name(captures);
            if mem.name != want {
                return Err(FormatError::BindingMismatch {
                    member: mem.name.clone(),
                    field: "segment_index",
                    detail: format!(
                        "manifest capture segment {captures} is named {:?}, want {want:?}",
                        mem.name
                    ),
                });
            }
            captures += 1;
        } else if mem.name != JOURNAL_MEMBER_NAME {
            continue;
        }

        let Some(data) = member_bytes(&mem.name) else {
            continue;
        };
        let (h, _) = parse_segment_header(data)?;
        verify_segment_against_manifest(m, &mem.name, &h)?;
    }
    Ok(())
}
