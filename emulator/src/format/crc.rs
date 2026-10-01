//! Frame and header checksum.

/// CRC-32 with the reflected IEEE/zlib polynomial `0xEDB88320`.
///
/// The specification uses CRC-32 rather than CRC32C deliberately: there is no
/// meaningful error-detection advantage at these frame sizes, and CRC-32 is
/// available in ESP32 ROM as `esp_rom_crc32_le`, so the most constrained of the
/// three implementations gets an optimized routine for free.
pub fn crc32(data: &[u8]) -> u32 {
    let mut h = crc32fast::Hasher::new();
    h.update(data);
    h.finalize()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Known values, so a dependency swap cannot silently change the checksum
    /// and desynchronise this implementation from the other two.
    #[test]
    fn matches_known_vectors() {
        assert_eq!(crc32(b""), 0x0000_0000);
        assert_eq!(crc32(b"a"), 0xe8b7_be43);
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
        assert_eq!(
            crc32(b"The quick brown fox jumps over the lazy dog"),
            0x414f_a339
        );
    }
}
