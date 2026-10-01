//! Deterministic CBOR, restricted to the subset the manifest and receipt need.
//!
//! RFC 8949 §4.2.1 core deterministic encoding:
//!
//! - definite-length maps, arrays, byte strings and text strings only;
//! - unsigned integer map keys, written in ascending order;
//! - smallest-width integer encoding;
//! - null for absent optional values;
//! - no tags, no floats, no indefinite lengths, no negative integers.
//!
//! A specified encoding is what makes the signed byte sequence reproducible
//! across implementations and firmware versions. Both halves here are strict:
//! the decoder rejects any input it would not itself have produced, because
//! accepting a non-canonical encoding would mean verifying bytes other than
//! those received.

use super::{FormatError, Result};

const MAJOR_UINT: u8 = 0;
const MAJOR_BYTES: u8 = 2;
const MAJOR_TEXT: u8 = 3;
const MAJOR_ARRAY: u8 = 4;
const MAJOR_MAP: u8 = 5;
const MAJOR_SIMPLE: u8 = 7;

const SIMPLE_NULL: u8 = 22; // 0xf6

// ─── encoder ────────────────────────────────────────────────────────────────

pub struct Encoder {
    buf: Vec<u8>,
    /// Guards against a map written with out-of-order keys, which would
    /// silently produce a non-canonical signature. Encoding is hand-written in
    /// key order, so a violation is a programming error and panics.
    in_map: bool,
    last_key: i64,
}

impl Encoder {
    pub fn new() -> Self {
        Self {
            buf: Vec::new(),
            in_map: false,
            last_key: -1,
        }
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.buf
    }

    fn head(&mut self, major: u8, arg: u64) {
        let m = major << 5;
        if arg < 24 {
            self.buf.push(m | arg as u8);
        } else if arg <= u8::MAX as u64 {
            self.buf.push(m | 24);
            self.buf.push(arg as u8);
        } else if arg <= u16::MAX as u64 {
            self.buf.push(m | 25);
            self.buf.extend_from_slice(&(arg as u16).to_be_bytes());
        } else if arg <= u32::MAX as u64 {
            self.buf.push(m | 26);
            self.buf.extend_from_slice(&(arg as u32).to_be_bytes());
        } else {
            self.buf.push(m | 27);
            self.buf.extend_from_slice(&arg.to_be_bytes());
        }
    }

    pub fn uint(&mut self, v: u64) {
        self.head(MAJOR_UINT, v);
    }

    pub fn bytes(&mut self, b: &[u8]) {
        self.head(MAJOR_BYTES, b.len() as u64);
        self.buf.extend_from_slice(b);
    }

    pub fn text(&mut self, s: &str) {
        self.head(MAJOR_TEXT, s.len() as u64);
        self.buf.extend_from_slice(s.as_bytes());
    }

    pub fn array_header(&mut self, n: usize) {
        self.head(MAJOR_ARRAY, n as u64);
    }

    pub fn null(&mut self) {
        self.buf.push((MAJOR_SIMPLE << 5) | SIMPLE_NULL);
    }

    pub fn map_header(&mut self, n: usize) {
        self.head(MAJOR_MAP, n as u64);
        self.in_map = true;
        self.last_key = -1;
    }

    /// Write a map key, enforcing ascending order.
    pub fn key(&mut self, k: u64) {
        if self.in_map && (k as i64) <= self.last_key {
            panic!(
                "CBOR map key {k} written after {}; keys must ascend",
                self.last_key
            );
        }
        self.last_key = k as i64;
        self.uint(k);
    }

    /// Append an already-encoded nested item.
    pub fn raw(&mut self, b: &[u8]) {
        self.buf.extend_from_slice(b);
    }
}

impl Default for Encoder {
    fn default() -> Self {
        Self::new()
    }
}

// ─── decoder ────────────────────────────────────────────────────────────────

pub struct Decoder<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Decoder<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Self { buf, pos: 0 }
    }

    pub fn at_end(&self) -> bool {
        self.pos >= self.buf.len()
    }

    pub fn remaining(&self) -> usize {
        self.buf.len().saturating_sub(self.pos)
    }

    fn head(&mut self) -> Result<(u8, u64)> {
        if self.pos >= self.buf.len() {
            return Err(FormatError::TruncatedCbor);
        }
        let ib = self.buf[self.pos];
        self.pos += 1;

        let major = ib >> 5;
        let ai = ib & 0x1f;

        let arg = match ai {
            0..=23 => return Ok((major, ai as u64)),
            24 => {
                if self.pos + 1 > self.buf.len() {
                    return Err(FormatError::TruncatedCbor);
                }
                let v = self.buf[self.pos] as u64;
                self.pos += 1;
                if v < 24 {
                    return Err(FormatError::NonCanonicalCbor(format!(
                        "value {v} encoded in 1 byte but fits the immediate form"
                    )));
                }
                v
            }
            25 => {
                if self.pos + 2 > self.buf.len() {
                    return Err(FormatError::TruncatedCbor);
                }
                let v = u16::from_be_bytes([self.buf[self.pos], self.buf[self.pos + 1]]) as u64;
                self.pos += 2;
                if v <= u8::MAX as u64 {
                    return Err(FormatError::NonCanonicalCbor(format!(
                        "value {v} encoded in 2 bytes but fits 1"
                    )));
                }
                v
            }
            26 => {
                if self.pos + 4 > self.buf.len() {
                    return Err(FormatError::TruncatedCbor);
                }
                let v = u32::from_be_bytes([
                    self.buf[self.pos],
                    self.buf[self.pos + 1],
                    self.buf[self.pos + 2],
                    self.buf[self.pos + 3],
                ]) as u64;
                self.pos += 4;
                if v <= u16::MAX as u64 {
                    return Err(FormatError::NonCanonicalCbor(format!(
                        "value {v} encoded in 4 bytes but fits 2"
                    )));
                }
                v
            }
            27 => {
                if self.pos + 8 > self.buf.len() {
                    return Err(FormatError::TruncatedCbor);
                }
                let mut arr = [0u8; 8];
                arr.copy_from_slice(&self.buf[self.pos..self.pos + 8]);
                let v = u64::from_be_bytes(arr);
                self.pos += 8;
                if v <= u32::MAX as u64 {
                    return Err(FormatError::NonCanonicalCbor(format!(
                        "value {v} encoded in 8 bytes but fits 4"
                    )));
                }
                v
            }
            31 => {
                return Err(FormatError::UnsupportedCbor(
                    "indefinite-length item".into(),
                ));
            }
            other => {
                return Err(FormatError::UnsupportedCbor(format!(
                    "reserved additional-information value {other}"
                )));
            }
        };

        Ok((major, arg))
    }

    fn expect(&mut self, want: u8) -> Result<u64> {
        let (major, arg) = self.head()?;
        if major != want {
            return Err(FormatError::Malformed(format!(
                "expected CBOR major type {want}, got {major}"
            )));
        }
        Ok(arg)
    }

    pub fn uint(&mut self) -> Result<u64> {
        self.expect(MAJOR_UINT)
    }

    pub fn u8(&mut self) -> Result<u8> {
        let v = self.uint()?;
        u8::try_from(v).map_err(|_| FormatError::Malformed(format!("value {v} exceeds u8")))
    }

    pub fn u32(&mut self) -> Result<u32> {
        let v = self.uint()?;
        u32::try_from(v).map_err(|_| FormatError::Malformed(format!("value {v} exceeds u32")))
    }

    pub fn map_header(&mut self) -> Result<usize> {
        Ok(self.expect(MAJOR_MAP)? as usize)
    }

    pub fn array_header(&mut self) -> Result<usize> {
        Ok(self.expect(MAJOR_ARRAY)? as usize)
    }

    pub fn bytes(&mut self) -> Result<&'a [u8]> {
        let n = self.expect(MAJOR_BYTES)? as usize;
        if self.pos + n > self.buf.len() {
            return Err(FormatError::TruncatedCbor);
        }
        let out = &self.buf[self.pos..self.pos + n];
        self.pos += n;
        Ok(out)
    }

    /// Read a byte string of exactly `N` bytes.
    pub fn bytes_n<const N: usize>(&mut self) -> Result<[u8; N]> {
        let b = self.bytes()?;
        if b.len() != N {
            return Err(FormatError::Malformed(format!(
                "expected {N}-byte string, got {}",
                b.len()
            )));
        }
        let mut out = [0u8; N];
        out.copy_from_slice(b);
        Ok(out)
    }

    pub fn text(&mut self) -> Result<String> {
        let n = self.expect(MAJOR_TEXT)? as usize;
        if self.pos + n > self.buf.len() {
            return Err(FormatError::TruncatedCbor);
        }
        let out = std::str::from_utf8(&self.buf[self.pos..self.pos + n])
            .map_err(|e| FormatError::Malformed(format!("invalid UTF-8 in text string: {e}")))?
            .to_string();
        self.pos += n;
        Ok(out)
    }

    /// Consume a null if present, reporting whether it did.
    pub fn is_null(&mut self) -> bool {
        if self.pos < self.buf.len() && self.buf[self.pos] == (MAJOR_SIMPLE << 5) | SIMPLE_NULL {
            self.pos += 1;
            return true;
        }
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn minimal_width_encoding() {
        let mut e = Encoder::new();
        e.uint(23);
        e.uint(24);
        e.uint(256);
        e.uint(65536);
        e.uint(u32::MAX as u64 + 1);
        let b = e.into_bytes();

        // 23 fits the immediate form; each larger value steps up exactly one width.
        assert_eq!(b[0], 23);
        assert_eq!(b[1], 0x18);
        assert_eq!(b[3], 0x19);
        assert_eq!(b[6], 0x1a);
        assert_eq!(b[11], 0x1b);
    }

    #[test]
    fn decoder_rejects_non_minimal_integers() {
        for input in [
            vec![0x18, 0x05],
            vec![0x19, 0x00, 0x20],
            vec![0x1a, 0x00, 0x00, 0x01, 0x00],
            vec![0x1b, 0, 0, 0, 0, 0, 1, 0, 0],
        ] {
            let mut d = Decoder::new(&input);
            assert!(
                matches!(d.head(), Err(FormatError::NonCanonicalCbor(_))),
                "accepted non-minimal encoding {input:02x?}"
            );
        }
    }

    #[test]
    fn decoder_rejects_indefinite_length() {
        let input = [0x9f];
        let mut d = Decoder::new(&input);
        assert!(matches!(d.head(), Err(FormatError::UnsupportedCbor(_))));
    }

    #[test]
    fn decoder_rejects_truncation() {
        for input in [vec![], vec![0x18], vec![0x19, 0x01], vec![0x1b, 0, 0, 0]] {
            let mut d = Decoder::new(&input);
            assert!(
                matches!(d.head(), Err(FormatError::TruncatedCbor)),
                "accepted truncated input {input:02x?}"
            );
        }
    }

    #[test]
    #[should_panic(expected = "keys must ascend")]
    fn encoder_panics_on_descending_keys() {
        let mut e = Encoder::new();
        e.map_header(2);
        e.key(5);
        e.key(3);
    }

    #[test]
    fn round_trips() {
        let mut e = Encoder::new();
        e.map_header(3);
        e.key(1);
        e.uint(2);
        e.key(2);
        e.bytes(&[0xde, 0xad, 0xbe, 0xef]);
        e.key(3);
        e.text("cairn");
        let encoded = e.into_bytes();

        let mut d = Decoder::new(&encoded);
        assert_eq!(d.map_header().unwrap(), 3);
        assert_eq!(d.uint().unwrap(), 1);
        assert_eq!(d.uint().unwrap(), 2);
        assert_eq!(d.uint().unwrap(), 2);
        assert_eq!(d.bytes().unwrap(), &[0xde, 0xad, 0xbe, 0xef]);
        assert_eq!(d.uint().unwrap(), 3);
        assert_eq!(d.text().unwrap(), "cairn");
        assert!(d.at_end());
    }
}
