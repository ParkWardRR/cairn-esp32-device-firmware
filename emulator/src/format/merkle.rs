//! Domain-separated binary Merkle tree, and the bundle content root.

use sha2::{Digest, Sha256};

use super::{FormatError, Result};

/// Domain tags. A leaf must never be reinterpretable as an internal node, and
/// an empty tree must not be confusable with either.
const DOMAIN_LEAF: u8 = 0x00;
const DOMAIN_INTERNAL: u8 = 0x01;
const DOMAIN_EMPTY: u8 = 0x02;

/// `SHA256(0x00 || d)`.
pub fn leaf_hash(d: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update([DOMAIN_LEAF]);
    h.update(d);
    h.finalize().into()
}

/// `SHA256(0x01 || l || r)`.
fn internal_hash(l: &[u8; 32], r: &[u8; 32]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update([DOMAIN_INTERNAL]);
    h.update(l);
    h.update(r);
    h.finalize().into()
}

/// Build the binary Merkle root over an ordered leaf list.
///
/// When a level has an odd node count the final node is **promoted unchanged**
/// to the next level. It is deliberately not duplicated: duplicating the last
/// node admits two distinct leaf lists that produce the same root, which would
/// let a bundle's member list be altered without changing its identity.
///
/// The empty tree has root `SHA256(0x02)`.
pub fn merkle_root(leaves: &[[u8; 32]]) -> [u8; 32] {
    if leaves.is_empty() {
        let mut h = Sha256::new();
        h.update([DOMAIN_EMPTY]);
        return h.finalize().into();
    }

    let mut level = leaves.to_vec();
    while level.len() > 1 {
        let mut next = Vec::with_capacity(level.len().div_ceil(2));
        let mut i = 0;
        while i + 1 < level.len() {
            next.push(internal_hash(&level[i], &level[i + 1]));
            i += 2;
        }
        if !level.len().is_multiple_of(2) {
            // Promote, do not duplicate.
            next.push(level[level.len() - 1]);
        }
        level = next;
    }
    level[0]
}

/// One file in a bundle.
///
/// `manifest.cbor` and `manifest.sig` are not members: the content root is an
/// input to the manifest, so it cannot cover it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Member {
    pub name: String,
    pub length: u64,
    pub sha256: [u8; 32],
}

/// `u16le(len(name)) || name || sha256(contents)`.
fn member_leaf_input(m: &Member) -> Vec<u8> {
    let name = m.name.as_bytes();
    let mut buf = Vec::with_capacity(2 + name.len() + 32);
    buf.extend_from_slice(&(name.len() as u16).to_le_bytes());
    buf.extend_from_slice(name);
    buf.extend_from_slice(&m.sha256);
    buf
}

/// Order members canonically: by raw name bytes, ascending.
pub fn sort_members(members: &mut [Member]) {
    members.sort_by(|a, b| a.name.as_bytes().cmp(b.name.as_bytes()));
}

/// Compute the bundle's content identity.
///
/// Identity is defined over members rather than over an archive's bytes so that
/// it stays independent of archive framing. Hashing a tar stream would make
/// identity depend on mtime, uid and padding, so repacking identical data would
/// yield a different identity.
///
/// The input slice is not modified.
pub fn content_root(members: &[Member]) -> Result<[u8; 32]> {
    validate_members(members)?;

    let mut sorted = members.to_vec();
    sort_members(&mut sorted);

    let leaves: Vec<[u8; 32]> = sorted
        .iter()
        .map(|m| leaf_hash(&member_leaf_input(m)))
        .collect();

    Ok(merkle_root(&leaves))
}

fn validate_members(members: &[Member]) -> Result<()> {
    let mut seen = std::collections::HashSet::new();
    for m in members {
        if m.name.is_empty() {
            return Err(FormatError::Malformed("member with empty name".into()));
        }
        if m.name.len() > u16::MAX as usize {
            return Err(FormatError::Malformed(format!(
                "member name exceeds 65535 bytes: {}",
                m.name
            )));
        }
        if !seen.insert(m.name.clone()) {
            return Err(FormatError::Malformed(format!(
                "duplicate member name {}",
                m.name
            )));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn digest(s: &str) -> [u8; 32] {
        Sha256::digest(s.as_bytes()).into()
    }

    #[test]
    fn empty_tree_has_its_own_domain() {
        let empty = merkle_root(&[]);
        let single = merkle_root(&[leaf_hash(&[])]);
        assert_ne!(
            empty, single,
            "empty tree and single-empty-leaf tree share a root; domain separation failed"
        );
    }

    /// Duplicating the last node would make these two leaf lists agree, which
    /// is a second-preimage weakness.
    #[test]
    fn odd_leaves_promote_rather_than_duplicate() {
        let a = leaf_hash(b"a");
        let b = leaf_hash(b"b");
        let c = leaf_hash(b"c");

        let got = merkle_root(&[a, b, c]);
        assert_eq!(got, internal_hash(&internal_hash(&a, &b), &c));
        assert_ne!(
            got,
            internal_hash(&internal_hash(&a, &b), &internal_hash(&c, &c))
        );

        assert_ne!(
            merkle_root(&[a, b, c]),
            merkle_root(&[a, b, c, c]),
            "[a,b,c] and [a,b,c,c] share a root"
        );
    }

    #[test]
    fn leaf_cannot_be_reinterpreted_as_node() {
        let l = leaf_hash(b"x");
        let r = leaf_hash(b"y");

        let mut concat = Vec::new();
        concat.extend_from_slice(&l);
        concat.extend_from_slice(&r);

        assert_ne!(internal_hash(&l, &r), leaf_hash(&concat));
    }

    #[test]
    fn content_root_is_order_independent() {
        let members = vec![
            Member {
                name: "journal.seg".into(),
                length: 512,
                sha256: digest("journal"),
            },
            Member {
                name: "seg-00000000.seg".into(),
                length: 4096,
                sha256: digest("seg0"),
            },
            Member {
                name: "seg-00000001.seg".into(),
                length: 2048,
                sha256: digest("seg1"),
            },
        ];
        let permuted = vec![members[2].clone(), members[0].clone(), members[1].clone()];

        let a = content_root(&members).unwrap();
        let b = content_root(&permuted).unwrap();
        assert_eq!(a, b, "permuted members produced a different root");

        // A changed digest must change the root.
        let mut altered = members.clone();
        altered[1].sha256[0] ^= 0xFF;
        assert_ne!(content_root(&altered).unwrap(), a);

        // So must a rename: the name is part of the leaf.
        let mut renamed = members.clone();
        renamed[0].name = "journal2.seg".into();
        assert_ne!(content_root(&renamed).unwrap(), a);
    }

    #[test]
    fn duplicate_member_names_are_rejected() {
        let members = vec![
            Member {
                name: "a.seg".into(),
                length: 1,
                sha256: [0; 32],
            },
            Member {
                name: "a.seg".into(),
                length: 2,
                sha256: [0; 32],
            },
        ];
        assert!(content_root(&members).is_err());
    }
}
