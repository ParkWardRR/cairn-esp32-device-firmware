//! Per-frame sealing with XChaCha20-Poly1305 (spec §3.6).
//!
//! | Input     | Value                                                         |
//! |-----------|---------------------------------------------------------------|
//! | key       | `K_seg`                                                       |
//! | nonce     | 24 random bytes per frame, carried in the frame               |
//! | plaintext | the §4 payload                                                |
//! | AAD       | the 24-byte frame header as written ‖ segment header `[0, header_len-4)` |
//!
//! The sealed body stored in a frame is `nonce[24] ‖ ciphertext[N] ‖ tag[16]`.

use chacha20poly1305::{
    KeyInit, XChaCha20Poly1305, XNonce,
    aead::{Aead, Payload},
};

use super::{
    FormatError, Result,
    frame::FRAME_HEADER_SIZE,
    keys::{AEAD_OVERHEAD, NONCE_SIZE, SEGMENT_KEY_SIZE},
    segment::SEGMENT_HEADER_SIZE,
};

/// Seals and opens the frames of one segment.
///
/// Bound to the segment at construction: the AAD suffix is the segment
/// header's bytes, everything but its CRC. A frame sealed here therefore
/// authenticates only inside this exact segment — moving it to another
/// segment, bundle, vehicle, device, assignment, key version or device counter
/// changes the AAD and fails the tag, even when (as with the counter) the key
/// itself is unchanged.
///
/// The cipher does not choose nonces; the writer supplies one per frame. That
/// keeps the scan path free of any RNG and keeps nonce policy in one place
/// (`SegmentWriter`), where the reason it must be random is written down.
pub struct SegmentCipher {
    aead: XChaCha20Poly1305,
    /// Segment header bytes `[0, header_len - 4)`.
    segment_aad: Vec<u8>,
}

impl SegmentCipher {
    /// Build the cipher for a segment from its encoded header and its key.
    ///
    /// `segment_header` must hold at least `header_len` bytes. `header_len` is
    /// read from the bytes rather than assumed to be 128, so a longer header
    /// from a future version is authenticated in full rather than truncated
    /// to the part this version understands.
    pub fn new(segment_header: &[u8], key: &[u8; SEGMENT_KEY_SIZE]) -> Result<Self> {
        if segment_header.len() < SEGMENT_HEADER_SIZE {
            return Err(FormatError::ShortHeader);
        }
        let header_len = u16::from_le_bytes([segment_header[6], segment_header[7]]) as usize;
        if header_len < SEGMENT_HEADER_SIZE {
            return Err(FormatError::BadHeaderLen(header_len));
        }
        if header_len > segment_header.len() {
            return Err(FormatError::HeaderLenPastEnd {
                header_len,
                size: segment_header.len(),
            });
        }

        Ok(Self {
            aead: XChaCha20Poly1305::new(key.into()),
            segment_aad: segment_header[..header_len - 4].to_vec(),
        })
    }

    /// The associated data for one frame: its 24-byte header exactly as
    /// written, then the segment header minus its CRC.
    ///
    /// Frame header first, so no frame-header field can ever be reinterpreted
    /// as part of the segment binding. The frame header carries `frame_len`,
    /// `seq` and `prev_crc32`, so a frame cannot be re-chained, renumbered or
    /// resized without failing its tag either.
    fn frame_aad(&self, frame_header: &[u8]) -> Vec<u8> {
        let mut aad = Vec::with_capacity(FRAME_HEADER_SIZE + self.segment_aad.len());
        aad.extend_from_slice(&frame_header[..FRAME_HEADER_SIZE]);
        aad.extend_from_slice(&self.segment_aad);
        aad
    }

    /// Seal `plaintext`, returning `nonce ‖ ciphertext ‖ tag`.
    ///
    /// `frame_header` is the encoded 24-byte header of the frame being written;
    /// its `frame_len` must already account for the sealed body.
    pub fn seal(
        &self,
        frame_header: &[u8],
        nonce: &[u8; NONCE_SIZE],
        plaintext: &[u8],
    ) -> Result<Vec<u8>> {
        let ct = self
            .aead
            .encrypt(
                XNonce::from_slice(nonce),
                Payload {
                    msg: plaintext,
                    aad: &self.frame_aad(frame_header),
                },
            )
            // The only failure mode is a plaintext beyond ChaCha20's 256 GiB
            // keystream, which a 4 KiB frame cannot reach.
            .map_err(|_| FormatError::Malformed("AEAD refused to seal the payload".into()))?;

        let mut out = Vec::with_capacity(NONCE_SIZE + ct.len());
        out.extend_from_slice(nonce);
        out.extend_from_slice(&ct);
        Ok(out)
    }

    /// Verify and decrypt a sealed body (`nonce ‖ ciphertext ‖ tag`).
    ///
    /// Returns [`FormatError::AuthFailed`] if the tag does not verify; no
    /// plaintext is ever returned for a frame that did not authenticate.
    pub fn open(&self, frame_header: &[u8], sealed: &[u8]) -> Result<Vec<u8>> {
        if sealed.len() < AEAD_OVERHEAD {
            return Err(FormatError::AuthFailed);
        }
        let (nonce, body) = sealed.split_at(NONCE_SIZE);
        self.aead
            .decrypt(
                XNonce::from_slice(nonce),
                Payload {
                    msg: body,
                    aad: &self.frame_aad(frame_header),
                },
            )
            .map_err(|_| FormatError::AuthFailed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::format::unhex;

    /// draft-irtf-cfrg-xchacha-03 §A.3.1, the AEAD_XChaCha20_Poly1305 test
    /// vector, run through the same crate entry points the cipher uses.
    ///
    /// The conformance vectors already prove this implementation agrees with
    /// Go's x/crypto; this pins both to the published construction, so a
    /// shared bug in the two libraries could not pass for agreement.
    #[test]
    fn xchacha20poly1305_matches_the_draft_vector() {
        let key: [u8; 32] =
            unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f")
                .unwrap()
                .try_into()
                .unwrap();
        let nonce = unhex("404142434445464748494a4b4c4d4e4f5051525354555657").unwrap();
        let aad = unhex("50515253c0c1c2c3c4c5c6c7").unwrap();
        let plaintext = b"Ladies and Gentlemen of the class of '99: If I could offer you \
only one tip for the future, sunscreen would be it.";
        let want_ct = unhex(concat!(
            "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb",
            "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452",
            "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9",
            "21f9664c97637da9768812f615c68b13b52e"
        ))
        .unwrap();
        let want_tag = unhex("c0875924c1c7987947deafd8780acf49").unwrap();

        let aead = XChaCha20Poly1305::new((&key).into());
        let out = aead
            .encrypt(
                XNonce::from_slice(&nonce),
                Payload {
                    msg: plaintext,
                    aad: &aad,
                },
            )
            .unwrap();

        assert_eq!(&out[..out.len() - 16], &want_ct[..], "ciphertext");
        assert_eq!(&out[out.len() - 16..], &want_tag[..], "tag");
    }
}
