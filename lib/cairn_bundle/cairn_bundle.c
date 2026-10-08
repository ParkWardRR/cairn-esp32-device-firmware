/* Reading a sealed bundle off the card. See cairn_bundle.h. */

#include "cairn_bundle.h"

#include <stdio.h>
#include <string.h>

#include "cairn_fs.h"
#include "cairn_log.h"

static const char *TAG = "BUNDLE";

static bool read_whole(const char *path, uint8_t *out, size_t cap, size_t *len)
{
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    if (f == NULL) {
        CAIRN_LOGE(TAG, "cannot open %s", path);
        return false;
    }

    uint64_t size = cairn_fs_size(f);
    if (size > (uint64_t)cap) {
        CAIRN_LOGE(TAG, "%s is %llu bytes, over the %u-byte bound", path,
                   (unsigned long long)size, (unsigned)cap);
        cairn_fs_close(f);
        return false;
    }

    size_t got = cairn_fs_read(f, out, (size_t)size);
    cairn_fs_close(f);

    if (got != (size_t)size) {
        CAIRN_LOGE(TAG, "%s: read %u of %llu bytes", path, (unsigned)got,
                   (unsigned long long)size);
        return false;
    }

    *len = got;
    return true;
}

bool cairn_bundle_open(cairn_bundle_t *b, const char *dir,
                       uint8_t *scratch, size_t scratch_len)
{
    memset(b, 0, sizeof(*b));

    if (scratch == NULL || scratch_len < CAIRN_MANIFEST_ENCODED_MAX) {
        CAIRN_LOGE(TAG, "decode scratch is %u bytes, needs %u",
                   (unsigned)scratch_len, (unsigned)CAIRN_MANIFEST_ENCODED_MAX);
        return false;
    }
    snprintf(b->dir, sizeof(b->dir), "%s", dir);

    char path[CAIRN_BUNDLE_DIR_MAX + 32];

    snprintf(path, sizeof(path), "%s/manifest.cbor", dir);
    if (!read_whole(path, b->manifest, sizeof(b->manifest), &b->manifest_len)) {
        return false;
    }

    snprintf(path, sizeof(path), "%s/manifest.sig", dir);
    size_t sig_len = 0;
    if (!read_whole(path, b->sig, sizeof(b->sig), &sig_len)) return false;
    if (sig_len != 64) {
        CAIRN_LOGE(TAG, "manifest.sig is %u bytes, expected 64", (unsigned)sig_len);
        return false;
    }

    cairn_err_t err = cairn_manifest_decode(b->manifest, b->manifest_len, &b->m,
                                            scratch, scratch_len);
    if (err != CAIRN_OK) {
        /*
         * Say enough to tell the cases apart without the card in hand. A
         * manifest that is all zeroes is a torn write; a plausible-looking head
         * with a short length is a truncation; a full-length body that still
         * will not decode is a format disagreement. Guessing between those from
         * "malformed" alone costs a trip to the car.
         */
        CAIRN_LOGE(TAG, "%s: manifest does not decode: %s (%u bytes, starts "
                        "%02x %02x %02x %02x %02x %02x %02x %02x)",
                   dir, cairn_strerror(err), (unsigned)b->manifest_len,
                   b->manifest_len > 0 ? b->manifest[0] : 0,
                   b->manifest_len > 1 ? b->manifest[1] : 0,
                   b->manifest_len > 2 ? b->manifest[2] : 0,
                   b->manifest_len > 3 ? b->manifest[3] : 0,
                   b->manifest_len > 4 ? b->manifest[4] : 0,
                   b->manifest_len > 5 ? b->manifest[5] : 0,
                   b->manifest_len > 6 ? b->manifest[6] : 0,
                   b->manifest_len > 7 ? b->manifest[7] : 0);
        return false;
    }

    if (b->m.member_count == 0 || b->m.chunk_count == 0) {
        CAIRN_LOGE(TAG, "%s: manifest describes %u members and %u chunks", dir,
                   (unsigned)b->m.member_count, (unsigned)b->m.chunk_count);
        return false;
    }

    /*
     * The members are already in the canonical order the sealer chunked, because
     * cairn_manifest_decode read them from the signed bytes. Confirm each file
     * is on the card at the length the manifest claims: a short member shifts
     * every later offset, and the symptom would otherwise surface as a chunk
     * digest mismatch blamed on a bad card read.
     */
    uint64_t declared = 0;
    for (size_t i = 0; i < b->m.member_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, b->m.members[i].name);

        uint64_t on_card = 0;
        if (!cairn_fs_file_size(path, &on_card)) {
            CAIRN_LOGE(TAG, "member %s named in the manifest is not on the card",
                       b->m.members[i].name);
            return false;
        }
        if (on_card != b->m.members[i].length) {
            CAIRN_LOGE(TAG, "member %s is %llu bytes on the card but the signed "
                            "manifest says %llu; refusing to read it",
                       b->m.members[i].name, (unsigned long long)on_card,
                       (unsigned long long)b->m.members[i].length);
            return false;
        }
        declared += b->m.members[i].length;
    }

    /* The chunk descriptors must cover exactly that stream, or an offset near
     * the end would run past it. */
    uint64_t chunked = 0;
    for (size_t i = 0; i < b->m.chunk_count; i++) {
        chunked += (uint64_t)b->m.chunks[i].byte_length;
    }
    if (chunked != declared) {
        CAIRN_LOGE(TAG, "%s: chunks cover %llu bytes but the members total %llu",
                   dir, (unsigned long long)chunked, (unsigned long long)declared);
        return false;
    }

    CAIRN_LOGI(TAG, "%s: %u members, %llu bytes, %u chunks", dir,
               (unsigned)b->m.member_count, (unsigned long long)declared,
               (unsigned)b->m.chunk_count);
    return true;
}

void cairn_bundle_close(cairn_bundle_t *b)
{
    if (b->have_open && b->open_file != NULL) {
        cairn_fs_close((cairn_file_t *)b->open_file);
    }
    b->open_file = NULL;
    b->have_open = false;
}

uint64_t cairn_bundle_stream_bytes(const cairn_bundle_t *b)
{
    uint64_t total = 0;
    for (size_t i = 0; i < b->m.member_count; i++) {
        total += b->m.members[i].length;
    }
    return total;
}

/* Open member `index`, reusing the handle if it is already the open one. */
static bool ensure_open(cairn_bundle_t *b, size_t index)
{
    if (b->have_open && b->open_index == index) return true;

    cairn_bundle_close(b);

    char path[CAIRN_BUNDLE_DIR_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s", b->dir, b->m.members[index].name);

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    if (f == NULL) {
        CAIRN_LOGE(TAG, "cannot open member %s", b->m.members[index].name);
        return false;
    }

    b->open_file  = f;
    b->open_index = index;
    b->open_pos   = 0;
    b->have_open  = true;
    return true;
}

size_t cairn_bundle_read_at(cairn_bundle_t *b, uint64_t offset,
                            uint8_t *out, size_t len)
{
    size_t done = 0;

    while (done < len) {
        /* Locate the member holding `offset`. Linear over at most
         * CAIRN_MAX_MEMBERS entries, which is cheaper than any index. */
        uint64_t base  = 0;
        size_t   index = b->m.member_count;

        for (size_t i = 0; i < b->m.member_count; i++) {
            uint64_t end = base + b->m.members[i].length;
            if (offset < end) { index = i; break; }
            base = end;
        }

        if (index == b->m.member_count) {
            /* Past the end of the stream. Every offset the transport asks for
             * is covered by a signed descriptor, so this is a bug or a
             * manifest that disagrees with the card, not an EOF to absorb. */
            CAIRN_LOGE(TAG, "offset %llu is past the end of the member stream",
                       (unsigned long long)offset);
            return done;
        }

        if (!ensure_open(b, index)) return done;

        uint64_t within    = offset - base;
        uint64_t available = b->m.members[index].length - within;
        size_t   want      = len - done;
        if ((uint64_t)want > available) want = (size_t)available;

        /* Seek only when the handle is not already there: a sequential walk is
         * the common case and a seek per block is wasted work on SPI. */
        if (b->open_pos != within) {
            if (!cairn_fs_seek((cairn_file_t *)b->open_file, within)) {
                CAIRN_LOGE(TAG, "seek to %llu in %s failed",
                           (unsigned long long)within, b->m.members[index].name);
                return done;
            }
            b->open_pos = within;
        }

        size_t got = cairn_fs_read((cairn_file_t *)b->open_file, out + done, want);
        if (got == 0) {
            CAIRN_LOGE(TAG, "read returned 0 at %llu in %s",
                       (unsigned long long)within, b->m.members[index].name);
            return done;
        }

        b->open_pos += got;
        offset      += got;
        done        += got;
    }

    return done;
}

/* ── the cairn_intake source ──────────────────────────────────────────────── */

static bool src_manifest(void *ctx, const uint8_t **bytes, size_t *len)
{
    cairn_bundle_t *b = ctx;
    *bytes = b->manifest;
    *len   = b->manifest_len;
    return b->manifest_len > 0;
}

static bool src_signature(void *ctx, const uint8_t **sig, size_t *len)
{
    cairn_bundle_t *b = ctx;
    *sig = b->sig;
    *len = 64;
    return true;
}

static size_t src_read_at(void *ctx, uint64_t offset, uint8_t *out, size_t len)
{
    return cairn_bundle_read_at((cairn_bundle_t *)ctx, offset, out, len);
}

void cairn_bundle_as_intake_source(cairn_bundle_t *b, cairn_intake_bundle_t *out)
{
    out->ctx       = b;
    out->manifest  = src_manifest;
    out->signature = src_signature;
    out->read_at   = src_read_at;
}
