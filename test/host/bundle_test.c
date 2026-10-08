/*
 * The sealed-bundle reader against a real sealed bundle on a real filesystem.
 *
 * The property under test is the one the transports depend on and the one a
 * reading error would hide: that cairn_bundle_read_at indexes exactly the same
 * byte stream the sealer chunked — the concatenation of the members in the
 * canonical order the signed manifest records, with chunks allowed to straddle
 * a member boundary.
 *
 * So the bundle is not synthesised. It is produced by cairn_store's real seal
 * path, with enough data to force a segment rotation (so there is more than one
 * member and therefore a real boundary to get wrong), and then every chunk the
 * manifest describes is read back and hashed against its signed descriptor. If
 * the offsets were off by one member, or the member order came from a directory
 * listing instead of the manifest, every digest after the first boundary would
 * fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_config.h"
#include "cairn_bundle.h"
#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_kv.h"
#include "cairn_log.h"
#include "cairn_platform.h"
#include "cairn_store.h"

/* Provided by platform_host.c, as in the other host suites. */
void cairn_host_advance(uint32_t ms);
void cairn_host_seed(uint64_t seed);
void cairn_host_rng_seed(uint64_t seed);
void cairn_kv_host_set_path(const char *path);

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [bundle] %s\n", name); }          \
        else      { g_fail++; printf("  FAIL  [bundle] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

static const uint8_t TEST_VEHICLE_ID[16]    = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
static const uint8_t TEST_ASSIGNMENT_ID[16] = { 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1 };

static char g_root[512];
static cairn_storage_identity_t g_ident;

static void rm_rf(const char *path)
{
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static bool fresh_tree(void)
{
    snprintf(g_root, sizeof(g_root), "/tmp/cairn-bundle-test");
    rm_rf(g_root);
    if (mkdir(g_root, 0775) != 0) return false;

    char kv[700];
    snprintf(kv, sizeof(kv), "%s/kv.bin", g_root);
    cairn_kv_host_set_path(kv);

    char card[700];
    snprintf(card, sizeof(card), "%s/card", g_root);
    if (mkdir(card, 0775) != 0) return false;

    if (!cairn_fs_begin(card)) return false;
    cairn_host_seed(12345);
    cairn_host_rng_seed(0xBEEFCAFEULL);

    if (!cairn_store_init()) return false;
    if (!cairn_kv_begin() ||
        !cairn_storage_set_assignment(TEST_VEHICLE_ID, TEST_ASSIGNMENT_ID)) {
        return false;
    }
    return cairn_storage_identity_load(&g_ident);
}

static int append_samples(cairn_capture_t *cap, int n)
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        cairn_gnss_sample_t s;
        memset(&s, 0, sizeof(s));
        s.lat_e7     = 340000000 + i;
        s.lon_e7     = -1185000000;
        s.fix_type   = 3;
        s.sats_used  = 9;
        s.hdop_e2    = 120;
        s.h_acc_cm   = CAIRN_U16_UNKNOWN;
        s.v_acc_cm   = CAIRN_U16_UNKNOWN;
        s.utc_acc_ms = CAIRN_U16_UNKNOWN;

        uint8_t payload[32];
        cairn_encode_gnss_sample(&s, payload);
        cairn_host_advance(100);
        if (cairn_capture_append(cap, CAIRN_CHAIN_CAPTURE, CAIRN_REC_GNSS_SAMPLE,
                                 1, 0, cairn_millis(), payload, sizeof(payload))) {
            ok++;
        }
    }
    return ok;
}

/* The member stream, read the slow obvious way: each member whole, in manifest
 * order, concatenated. This is the reference cairn_bundle_read_at must match. */
static size_t reference_stream(cairn_bundle_t *b, uint8_t *out, size_t cap)
{
    size_t total = 0;

    for (size_t i = 0; i < b->m.member_count; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", b->dir, b->m.members[i].name);

        cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
        if (f == NULL) return 0;

        size_t len = (size_t)b->m.members[i].length;
        if (total + len > cap) { cairn_fs_close(f); return 0; }

        size_t got = cairn_fs_read(f, out + total, len);
        cairn_fs_close(f);
        if (got != len) return 0;
        total += got;
    }
    return total;
}

#define STREAM_CAP (3u * 1024u * 1024u)
static uint8_t g_reference[STREAM_CAP];
static uint8_t g_actual[STREAM_CAP];

int main(void)
{
    printf("bundle: the sealed-bundle reader\n");

    if (!fresh_tree()) {
        printf("  FAIL  [bundle] could not build a test tree\n");
        return 1;
    }

    uint8_t device_id[16], seed[32], pub[32];
    uint32_t boot = 0;
    if (!cairn_identity_load(device_id, seed, pub, &boot)) {
        printf("  FAIL  [bundle] identity load\n");
        return 1;
    }

    uint8_t boot_id[16];
    cairn_new_boot_id(boot_id);

    static cairn_capture_t cap;
    if (!cairn_capture_open_or_resume(&cap, device_id, boot_id, &g_ident)) {
        printf("  FAIL  [bundle] capture open\n");
        return 1;
    }

    /* Past CAIRN_SEGMENT_MAX_BYTES, so the bundle has more than one capture
     * member and the chunk walk crosses a real boundary. */
    const int needed = (int)(CAIRN_SEGMENT_MAX_BYTES / 48u) + 200;
    int wrote = append_samples(&cap, needed);
    CHECK("the capture took the samples", wrote == needed);
    CHECK("the capture rotated, so there is a member boundary to cross",
          cap.segment_index >= 1);

    uint8_t bundle_id[16];
    if (!cairn_capture_seal(&cap, seed, pub, "cairn-bundle-test", 1, 0, bundle_id)) {
        printf("  FAIL  [bundle] seal failed\n");
        return 1;
    }

    char ulid[27];
    cairn_ulid_encode(bundle_id, ulid);

    char dir[200];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, ulid);

    static cairn_bundle_t b;
    CHECK("the sealed bundle opens", cairn_bundle_open(&b, dir));
    CHECK("it has more than one member", b.m.member_count > 1);
    CHECK("it has more than one chunk, so 8 KiB chunking took effect",
          b.m.chunk_count > 1);

    uint64_t stream = cairn_bundle_stream_bytes(&b);
    size_t   ref    = reference_stream(&b, g_reference, sizeof(g_reference));
    CHECK("the reference concatenation read back", ref > 0);
    CHECK("stream_bytes equals the members' total", (uint64_t)ref == stream);

    /* 1. The whole stream in one call. */
    size_t got = cairn_bundle_read_at(&b, 0, g_actual, (size_t)stream);
    CHECK("one read of the whole stream returns every byte", got == (size_t)stream);
    CHECK("and the bytes match the concatenation",
          memcmp(g_actual, g_reference, (size_t)stream) == 0);

    /* 2. Every chunk, at the offset the transport will use: the sum of the
     *    preceding descriptors. This is the exact arithmetic cairn_intake does,
     *    and each chunk is checked against its own signed digest. */
    bool all_chunks_ok = true;
    bool any_straddles = false;
    uint64_t off = 0;

    for (size_t i = 0; i < b.m.chunk_count; i++) {
        uint32_t len = b.m.chunks[i].byte_length;

        /* Does this chunk cross a member boundary? Worth knowing the test
         * actually exercised the hard case rather than passing vacuously. */
        uint64_t base = 0;
        for (size_t k = 0; k < b.m.member_count; k++) {
            uint64_t end = base + b.m.members[k].length;
            if (off < end && off + len > end) any_straddles = true;
            base = end;
        }

        size_t n = cairn_bundle_read_at(&b, off, g_actual, len);
        if (n != len) { all_chunks_ok = false; break; }

        uint8_t digest[32];
        cairn_sha256(g_actual, len, digest);
        if (memcmp(digest, b.m.chunks[i].sha256, 32) != 0) {
            all_chunks_ok = false;
            printf("        chunk %u at offset %llu does not match its digest\n",
                   (unsigned)i, (unsigned long long)off);
            break;
        }
        off += len;
    }
    CHECK("every chunk read at its derived offset matches its signed digest",
          all_chunks_ok);
    CHECK("at least one chunk straddled a member boundary", any_straddles);
    CHECK("the chunks cover the stream exactly", off == stream);

    /* 3. Unaligned and boundary-crossing reads. A transport using a scratch
     *    buffer that does not divide the chunk size hits these constantly. */
    bool unaligned_ok = true;
    const size_t probes[] = { 1, 7, 63, 511, 1000, 4095, 8191, 8192, 8193 };
    for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
        size_t len = probes[p];
        for (uint64_t start = 0; start + len <= stream;
             start += (stream / 5) + 1) {
            size_t n = cairn_bundle_read_at(&b, start, g_actual, len);
            if (n != len || memcmp(g_actual, g_reference + start, len) != 0) {
                unaligned_ok = false;
                printf("        %u bytes at %llu mismatched\n", (unsigned)len,
                       (unsigned long long)start);
                break;
            }
        }
        if (!unaligned_ok) break;
    }
    CHECK("unaligned reads at many offsets match the concatenation", unaligned_ok);

    /* 4. Reading exactly across the first member boundary. */
    uint64_t first = b.m.members[0].length;
    if (first >= 16 && first + 16 <= stream) {
        size_t n = cairn_bundle_read_at(&b, first - 16, g_actual, 32);
        CHECK("a read spanning the first member boundary is contiguous",
              n == 32 && memcmp(g_actual, g_reference + (first - 16), 32) == 0);
    }

    /* 5. Past the end is a failure, not a silent short read treated as EOF. */
    size_t over = cairn_bundle_read_at(&b, stream, g_actual, 16);
    CHECK("a read starting past the end returns nothing", over == 0);

    /* 6. A member truncated after sealing must be refused at open, because
     *    every later offset would otherwise be wrong. */
    cairn_bundle_close(&b);
    {
        char victim[700];
        snprintf(victim, sizeof(victim), "%s/card%s/%s/%s", g_root,
                 CAIRN_DIR_BUNDLES, ulid, b.m.members[0].name);
        if (truncate(victim, (off_t)(b.m.members[0].length - 64)) == 0) {
            static cairn_bundle_t b2;
            CHECK("a member shorter than the signed manifest is refused at open",
                  !cairn_bundle_open(&b2, dir));
        } else {
            printf("  FAIL  [bundle] could not truncate a member to test the check\n");
            g_fail++;
        }
    }

    printf("bundle: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
