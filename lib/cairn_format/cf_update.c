/*
 * The OTA update descriptor (docs/ota.md).
 *
 * Portable C with no IDF dependency, like the rest of this library, so the
 * parsing that decides whether a device replaces itself is exercised by host
 * tests and cross-checked byte-for-byte against the Go implementation's real
 * output. A bug here does not corrupt a bundle; it bricks a device.
 */

#include <string.h>

#include "cairn_format.h"

#define KEY_DESCRIPTOR_VERSION 1
#define KEY_FIRMWARE_VERSION   2
#define KEY_IMAGE_SHA256       3
#define KEY_IMAGE_LENGTH       4
#define KEY_MIN_FIRMWARE       5
#define KEY_BUILD_UTC_MS       6
#define KEY_SIGNATURE_ALGO     7

#define UPDATE_FIELD_COUNT     7

cairn_err_t cairn_update_encode(const cairn_update_descriptor_t *d,
                                uint8_t *out, size_t out_cap, size_t *written)
{
    cairn_cbor_enc_t e;
    cairn_cbor_init(&e, out, out_cap);

    cairn_cbor_map(&e, UPDATE_FIELD_COUNT);

    cairn_cbor_key(&e, KEY_DESCRIPTOR_VERSION);
    cairn_cbor_uint(&e, d->descriptor_version);
    cairn_cbor_key(&e, KEY_FIRMWARE_VERSION);
    cairn_cbor_text(&e, d->firmware_version);
    cairn_cbor_key(&e, KEY_IMAGE_SHA256);
    cairn_cbor_bytes(&e, d->image_sha256, 32);
    cairn_cbor_key(&e, KEY_IMAGE_LENGTH);
    cairn_cbor_uint(&e, d->image_length);
    cairn_cbor_key(&e, KEY_MIN_FIRMWARE);
    cairn_cbor_text(&e, d->min_firmware_version);
    cairn_cbor_key(&e, KEY_BUILD_UTC_MS);
    cairn_cbor_uint(&e, d->build_utc_ms);
    cairn_cbor_key(&e, KEY_SIGNATURE_ALGO);
    cairn_cbor_text(&e, d->signature_algorithm);

    if (e.overflow) return CAIRN_ERR_BUFFER_TOO_SMALL;

    *written = e.len;
    return CAIRN_OK;
}

cairn_err_t cairn_update_decode(const uint8_t *buf, size_t len,
                                cairn_update_descriptor_t *out,
                                uint8_t *scratch, size_t scratch_cap)
{
    cairn_cbor_dec_t d;
    cairn_cbor_dec_init(&d, buf, len);

    size_t n = 0;
    cairn_err_t err = cairn_cbor_map_header(&d, &n);
    if (err != CAIRN_OK) return err;
    if (n != UPDATE_FIELD_COUNT) return CAIRN_ERR_MALFORMED;

    memset(out, 0, sizeof(*out));

    for (size_t i = 0; i < n; i++) {
        uint64_t key = 0;
        err = cairn_cbor_uint_read(&d, &key);
        if (err != CAIRN_OK) return err;

        switch (key) {
        case KEY_DESCRIPTOR_VERSION: {
            uint64_t v = 0;
            err = cairn_cbor_uint_read(&d, &v);
            if (err != CAIRN_OK) return err;
            if (v > 0xFF) return CAIRN_ERR_MALFORMED;
            out->descriptor_version = (uint8_t)v;
            break;
        }
        case KEY_FIRMWARE_VERSION:
            err = cairn_cbor_text_read(&d, out->firmware_version,
                                       sizeof(out->firmware_version));
            if (err != CAIRN_OK) return err;
            break;
        case KEY_IMAGE_SHA256:
            err = cairn_cbor_bytes_n(&d, out->image_sha256, 32);
            if (err != CAIRN_OK) return err;
            break;
        case KEY_IMAGE_LENGTH: {
            uint64_t v = 0;
            err = cairn_cbor_uint_read(&d, &v);
            if (err != CAIRN_OK) return err;
            if (v > 0xFFFFFFFFu) return CAIRN_ERR_MALFORMED;
            out->image_length = (uint32_t)v;
            break;
        }
        case KEY_MIN_FIRMWARE:
            err = cairn_cbor_text_read(&d, out->min_firmware_version,
                                       sizeof(out->min_firmware_version));
            if (err != CAIRN_OK) return err;
            break;
        case KEY_BUILD_UTC_MS:
            err = cairn_cbor_uint_read(&d, &out->build_utc_ms);
            if (err != CAIRN_OK) return err;
            break;
        case KEY_SIGNATURE_ALGO:
            err = cairn_cbor_text_read(&d, out->signature_algorithm,
                                       sizeof(out->signature_algorithm));
            if (err != CAIRN_OK) return err;
            break;
        default:
            /* Newer firmware signed this. Refuse rather than guess: acting on a
             * descriptor only partly understood is how a device installs
             * something it was not told the constraints for. */
            return CAIRN_ERR_MALFORMED;
        }
    }

    if (d.pos != len) return CAIRN_ERR_MALFORMED; /* trailing bytes */

    if (out->descriptor_version != CAIRN_UPDATE_DESCRIPTOR_VERSION) {
        return CAIRN_ERR_UNSUPPORTED_VERSION;
    }
    if (strcmp(out->signature_algorithm, CAIRN_SIGALG_ED25519) != 0) {
        return CAIRN_ERR_UNSUPPORTED_VERSION;
    }
    if (out->image_length == 0) return CAIRN_ERR_MALFORMED;
    if (out->firmware_version[0] == '\0') return CAIRN_ERR_MALFORMED;

    /*
     * Re-encode and compare. A descriptor that parses but is not canonical
     * would verify here and fail to reproduce elsewhere, and this is the one
     * signature whose failure mode is a device that will not boot.
     */
    if (scratch != NULL) {
        size_t written = 0;
        err = cairn_update_encode(out, scratch, scratch_cap, &written);
        if (err != CAIRN_OK) return err;
        if (written != len || memcmp(scratch, buf, len) != 0) {
            return CAIRN_ERR_NON_CANONICAL;
        }
    }

    return CAIRN_OK;
}

cairn_err_t cairn_update_verify(const uint8_t *encoded, size_t len,
                                const uint8_t sig[64], const uint8_t pub[32],
                                cairn_update_descriptor_t *out,
                                uint8_t *scratch, size_t scratch_cap)
{
    /*
     * Signature before decode, and before any use of the contents. The device
     * downloads megabytes on the strength of this check, so an unsigned or
     * wrongly signed descriptor has to cost nothing.
     */
    if (!cairn_ed25519_verify(encoded, len, sig, pub)) {
        return CAIRN_ERR_BAD_SIGNATURE;
    }
    return cairn_update_decode(encoded, len, out, scratch, scratch_cap);
}
