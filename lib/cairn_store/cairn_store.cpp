#include "cairn_store.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cairn_log.h"

static const char *TAG = "STORE";

/*
 * 256 KiB chunks, the specification's default. With CAIRN_MAX_CHUNKS at 64 this
 * bounds a bundle at 16 MiB, comfortably above the 15 MiB that
 * CAIRN_MAX_MEMBERS segments can hold.
 */
#define CHUNK_BYTES (256u * 1024u)

/* Streaming I/O block. Large enough to keep SPI efficient, small enough to sit
 * in DRAM alongside everything else. */
#define IO_BLOCK 2048

/* ── ULID ─────────────────────────────────────────────────────────────────── */

static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/*
 * A ULID is the operational handle for a bundle, not its identity: identity is
 * content_root. The timestamp prefix exists only so directory listings sort
 * chronologically, so a device with no valid clock yields a zero prefix rather
 * than a fabricated time.
 */
static void ulid_generate(uint8_t out[16], uint64_t utc_ms)
{
    out[0] = (uint8_t)(utc_ms >> 40);
    out[1] = (uint8_t)(utc_ms >> 32);
    out[2] = (uint8_t)(utc_ms >> 24);
    out[3] = (uint8_t)(utc_ms >> 16);
    out[4] = (uint8_t)(utc_ms >> 8);
    out[5] = (uint8_t)(utc_ms);

    esp_fill_random(out + 6, 10);
}

static void ulid_encode(const uint8_t id[16], char out[27])
{
    /* 128 bits into 26 base32 characters, most significant first. The top
     * character carries only 3 bits, which is inherent to 26 * 5 = 130. */
    for (int i = 0; i < 26; i++) {
        int bit_pos = i * 5 - 2; /* the first character is short by 2 bits */
        uint32_t v = 0;

        for (int b = 0; b < 5; b++) {
            int pos = bit_pos + b;
            uint32_t bit = 0;
            if (pos >= 0 && pos < 128) {
                bit = (id[pos / 8] >> (7 - (pos % 8))) & 1u;
            }
            v = (v << 1) | bit;
        }
        out[i] = CROCKFORD[v & 0x1f];
    }
    out[26] = '\0';
}

static int crockford_value(char c)
{
    for (int i = 0; i < 32; i++) {
        if (CROCKFORD[i] == c) return i;
    }
    return -1;
}

static bool ulid_decode(const char *s, uint8_t out[16])
{
    if (strlen(s) != 26) return false;

    memset(out, 0, 16);
    for (int i = 0; i < 26; i++) {
        int v = crockford_value(s[i]);
        if (v < 0) return false;

        int bit_pos = i * 5 - 2;
        for (int b = 0; b < 5; b++) {
            int pos = bit_pos + b;
            if (pos < 0 || pos >= 128) continue;
            uint32_t bit = (uint32_t)(v >> (4 - b)) & 1u;
            if (bit) out[pos / 8] |= (uint8_t)(1u << (7 - (pos % 8)));
        }
    }
    return true;
}

/* ── identity ─────────────────────────────────────────────────────────────── */

bool cairn_identity_load(uint8_t device_id[16], uint8_t seed[32],
                         uint8_t pub[32], uint32_t *boot_count)
{
    Preferences prefs;
    if (!prefs.begin("cairn", false)) {
        CAIRN_LOGE(TAG, "NVS open failed; identity unavailable");
        return false;
    }

    /*
     * device_id is derived from the efuse MAC, so wiping NVS does not change
     * which vehicle the data came from.
     */
    if (prefs.getBytesLength("device_id") != 16) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);

        uint8_t digest[32];
        cairn_sha256(mac, sizeof(mac), digest);
        memcpy(device_id, digest, 16);

        prefs.putBytes("device_id", device_id, 16);
        CAIRN_LOGW(TAG, "device_id initialized from the efuse MAC");
    } else {
        prefs.getBytes("device_id", device_id, 16);
    }

    /*
     * The signing seed is generated once from the hardware RNG. It cannot be
     * re-derived, so losing NVS means a new key and re-enrolment on the server
     * — which is the correct outcome, and loud rather than silent.
     *
     * Flash encryption is deliberately not enabled (see partitions-ab.csv), so
     * this seed is readable from a physically extracted chip. That is an
     * accepted trade for a device that must be recoverable on a bench; the key
     * authorizes uploads, not deletion, and the server can revoke it.
     */
    if (prefs.getBytesLength("key_seed") != 32) {
        esp_fill_random(seed, 32);
        prefs.putBytes("key_seed", seed, 32);
        CAIRN_LOGW(TAG, "generated a new device signing key; the server must "
                        "enrol this device before it can upload");
    } else {
        prefs.getBytes("key_seed", seed, 32);
    }

    uint32_t count = prefs.getUInt("boot_count", 0) + 1;
    prefs.putUInt("boot_count", count);
    *boot_count = count;

    prefs.end();

    cairn_ed25519_public_from_seed(seed, pub);

    uint8_t key_id[8];
    cairn_device_key_id(pub, key_id);
    CAIRN_LOGI(TAG,
               "identity: device %02x%02x%02x%02x.. key_id %02x%02x%02x%02x%02x%02x%02x%02x boot %u",
               device_id[0], device_id[1], device_id[2], device_id[3],
               key_id[0], key_id[1], key_id[2], key_id[3],
               key_id[4], key_id[5], key_id[6], key_id[7], (unsigned)count);

    return true;
}

void cairn_new_boot_id(uint8_t boot_id[16])
{
    esp_fill_random(boot_id, 16);
}

/* ── paths ────────────────────────────────────────────────────────────────── */

static void segment_path(const cairn_capture_t *cap, uint32_t index,
                         char *out, size_t cap_len)
{
    snprintf(out, cap_len, "%s/seg-%08u.seg", cap->dir, (unsigned)index);
}

static void journal_path(const cairn_capture_t *cap, char *out, size_t cap_len)
{
    snprintf(out, cap_len, "%s/journal.seg", cap->dir);
}

bool cairn_store_init(void)
{
    const char *dirs[] = {
        CAIRN_DIR_ROOT, CAIRN_DIR_CAPTURE, CAIRN_DIR_BUNDLES,
        CAIRN_DIR_RECEIPTS, CAIRN_DIR_LOGS, CAIRN_DIR_STATE,
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (!SD.exists(dirs[i]) && !SD.mkdir(dirs[i])) {
            CAIRN_LOGE(TAG, "mkdir %s failed", dirs[i]);
            return false;
        }
    }
    return true;
}

/* ── segment writing ──────────────────────────────────────────────────────── */

static bool write_segment_header(const cairn_capture_t *cap, const char *path,
                                 uint32_t segment_index, uint32_t first_seq)
{
    cairn_segment_header_t h;
    memset(&h, 0, sizeof(h));

    h.format_version = CAIRN_FORMAT_VERSION;
    memcpy(h.device_id, cap->device_id, 16);
    memcpy(h.boot_id, cap->boot_id, 16);
    h.segment_index       = segment_index;
    h.first_seq           = first_seq;
    h.opened_monotonic_us = (uint64_t)esp_timer_get_time();

    uint8_t buf[CAIRN_SEGMENT_HEADER_SIZE];
    cairn_err_t err = cairn_encode_segment_header(&h, buf, sizeof(buf));
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "encode segment header: %s", cairn_strerror(err));
        return false;
    }

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        CAIRN_LOGE(TAG, "cannot create %s", path);
        return false;
    }

    size_t n = f.write(buf, sizeof(buf));
    f.flush();
    f.close();

    if (n != sizeof(buf)) {
        CAIRN_LOGE(TAG, "short write on %s header", path);
        return false;
    }

    CAIRN_LOGD(TAG, "opened %s (index %u, first_seq %u)", path,
               (unsigned)segment_index, (unsigned)first_seq);
    return true;
}

/* ── recovery ─────────────────────────────────────────────────────────────── */

/* Reader backing the streaming scan with an open file on the card. */
struct sd_reader {
    File *f;
};

static bool sd_read(void *user, uint64_t offset, uint8_t *buf, size_t len)
{
    struct sd_reader *r = (struct sd_reader *)user;

    if (!r->f->seek((uint32_t)offset)) return false;
    return r->f->read(buf, len) == (int)len;
}

/*
 * Truncate a segment to `keep` bytes.
 *
 * FatFs has no truncate through the Arduino File API, so the surviving prefix is
 * copied to a sibling and renamed over the original. The copy is written and
 * flushed before the original is removed, so an interruption leaves either the
 * original (recoverable again next boot) or the complete replacement — never a
 * half-truncated segment.
 */
static bool truncate_segment(const char *path, uint32_t keep)
{
    char tmp[112];
    snprintf(tmp, sizeof(tmp), "%s.trunc", path);

    File src = SD.open(path, FILE_READ);
    if (!src) return false;

    SD.remove(tmp);
    File dst = SD.open(tmp, FILE_WRITE);
    if (!dst) {
        src.close();
        return false;
    }

    uint8_t  block[IO_BLOCK];
    uint32_t copied = 0;
    bool     ok = true;

    while (copied < keep) {
        size_t want = keep - copied;
        if (want > sizeof(block)) want = sizeof(block);

        int got = src.read(block, want);
        if (got <= 0) {
            ok = false;
            break;
        }
        if (dst.write(block, (size_t)got) != (size_t)got) {
            ok = false;
            break;
        }
        copied += (uint32_t)got;
    }

    dst.flush();
    dst.close();
    src.close();

    if (!ok || copied != keep) {
        SD.remove(tmp);
        CAIRN_LOGE(TAG, "truncate copy failed for %s (%u of %u bytes)", path,
                   (unsigned)copied, (unsigned)keep);
        return false;
    }

    if (!SD.remove(path) || !SD.rename(tmp, path)) {
        CAIRN_LOGE(TAG, "truncate rename failed for %s", path);
        return false;
    }

    return true;
}

/*
 * Scan one segment, truncate any torn tail, and fold its tallies into the
 * capture state.
 *
 * `chain` is carried in and out so capture segments continue a single sequence
 * across rotations. A header error leaves the file untouched: the segment is
 * unusable, but deleting it would discard data the server might still salvage,
 * and nothing here is permitted to delete bundle data.
 */
static bool recover_segment(cairn_capture_t *cap, const char *path,
                            cairn_chain_t *chain, bool fold_counts,
                            uint32_t *bytes_out)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    uint32_t size = (uint32_t)f.size();
    struct sd_reader reader = { &f };

    cairn_scan_state_t state;
    state.expected_seq  = chain->next_seq;
    state.expected_prev = chain->prev_crc32;

    cairn_scan_result_t res;
    cairn_err_t err =
        cairn_scan_segment_stream(sd_read, &reader, size, state, &res, NULL, NULL);
    f.close();

    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "%s is unusable (%s); leaving it in place for the server",
                   path, cairn_strerror(err));
        return false;
    }

    CAIRN_LOGI(TAG, "%s: %s, %u frames, seq %u..%u, %u byte tail discarded",
               path, cairn_stop_reason_name(res.stop), (unsigned)res.frames,
               (unsigned)res.first_seq, (unsigned)res.last_seq,
               (unsigned)res.discarded_tail_bytes);

    if (res.discarded_tail_bytes > 0) {
        /*
         * Honest incompleteness: the exact byte count goes into the manifest,
         * and the recovery state is raised so the server knows this bundle was
         * reconstructed rather than cleanly sealed.
         */
        cap->discarded_tail_bytes += res.discarded_tail_bytes;
        cap->recovery_state =
            (res.stop == CAIRN_STOP_TORN_TAIL) ? CAIRN_RECOVERY_RECOVERED_TAIL
                                               : CAIRN_RECOVERY_SALVAGED;

        if (!truncate_segment(path, (uint32_t)res.stop_offset)) {
            CAIRN_LOGW(TAG, "could not truncate %s; appending would corrupt the "
                            "chain, so this capture will be sealed as-is", path);
            return false;
        }
    }

    if (fold_counts) {
        for (int t = 0; t < 256; t++) cap->record_counts[t] += res.record_counts[t];

        if (res.frames > 0) {
            if (!cap->have_any_frame) {
                cap->first_seq = res.first_seq;
                cap->have_any_frame = true;
            }
            cap->last_seq = res.last_seq;
        }
    }

    chain->next_seq    = res.next.expected_seq;
    chain->prev_crc32  = res.next.expected_prev;
    *bytes_out         = (uint32_t)res.stop_offset;

    return true;
}

/* ── open or resume ───────────────────────────────────────────────────────── */

static bool find_open_capture(char *out, size_t cap_len)
{
    File dir = SD.open(CAIRN_DIR_CAPTURE);
    if (!dir) return false;

    bool found = false;
    for (;;) {
        File entry = dir.openNextFile();
        if (!entry) break;

        if (entry.isDirectory()) {
            const char *name = entry.name();
            const char *base = strrchr(name, '/');
            base = (base != nullptr) ? base + 1 : name;

            uint8_t probe[16];
            if (ulid_decode(base, probe)) {
                snprintf(out, cap_len, "%s", base);
                found = true;
            }
        }
        entry.close();
        if (found) break;
    }
    dir.close();
    return found;
}

bool cairn_capture_open_or_resume(cairn_capture_t *cap,
                                  const uint8_t device_id[16],
                                  const uint8_t boot_id[16])
{
    memset(cap, 0, sizeof(*cap));
    memcpy(cap->device_id, device_id, 16);
    memcpy(cap->boot_id, boot_id, 16);
    cap->capture_started_monotonic_us = (uint64_t)esp_timer_get_time();
    cap->last_flush_ms = millis();

    char id_text[27];

    if (find_open_capture(id_text, sizeof(id_text))) {
        if (!ulid_decode(id_text, cap->bundle_id)) return false;
        snprintf(cap->dir, sizeof(cap->dir), "%s/%s", CAIRN_DIR_CAPTURE, id_text);

        CAIRN_LOGW(TAG, "resuming interrupted capture %s", id_text);

        /*
         * Walk capture segments in index order. A single chain spans them, so
         * they must be scanned in order for the continuity check to mean
         * anything.
         */
        uint32_t index = 0;
        uint32_t last_bytes = CAIRN_SEGMENT_HEADER_SIZE;
        bool     chain_intact = true;

        for (; index < CAIRN_MAX_MEMBERS; index++) {
            char path[112];
            segment_path(cap, index, path, sizeof(path));
            if (!SD.exists(path)) break;

            uint32_t bytes = 0;
            if (!recover_segment(cap, path, &cap->capture_chain, true, &bytes)) {
                chain_intact = false;
                break;
            }
            last_bytes = bytes;
        }

        if (index == 0) {
            /* A capture directory with no segments: treat it as fresh. */
            CAIRN_LOGW(TAG, "capture %s had no segments; starting segment 0",
                       id_text);

            char path0[112];
            segment_path(cap, 0, path0, sizeof(path0));
            if (!write_segment_header(cap, path0, 0, 0)) return false;

            cap->segment_index = 0;
            cap->segment_bytes = CAIRN_SEGMENT_HEADER_SIZE;
        } else {
            cap->segment_index = index - 1;
            cap->segment_bytes = last_bytes;
        }

        char jpath[112];
        journal_path(cap, jpath, sizeof(jpath));
        if (SD.exists(jpath)) {
            uint32_t jbytes = 0;
            if (recover_segment(cap, jpath, &cap->journal_chain, false, &jbytes)) {
                cap->journal_bytes = jbytes;
            } else {
                chain_intact = false;
            }
        } else {
            if (!write_segment_header(cap, jpath, 0, 0)) return false;
            cap->journal_bytes = CAIRN_SEGMENT_HEADER_SIZE;
        }

        /*
         * If any segment could not be made appendable, the chain cannot be
         * continued without fabricating continuity. Seal what exists instead:
         * an honestly short bundle is worth more than a plausible-looking one.
         */
        if (!chain_intact) {
            cap->needs_seal = true;
            cap->recovery_state = CAIRN_RECOVERY_SALVAGED;
            CAIRN_LOGW(TAG, "capture %s cannot be safely extended; it will be "
                            "sealed at the next opportunity", id_text);
        }

        cap->active = true;
        CAIRN_LOGI(TAG, "resumed: segment %u at %u bytes, next seq %u, "
                        "%u bytes discarded, recovery_state %u",
                   (unsigned)cap->segment_index, (unsigned)cap->segment_bytes,
                   (unsigned)cap->capture_chain.next_seq,
                   (unsigned)cap->discarded_tail_bytes,
                   (unsigned)cap->recovery_state);
        return true;
    }

    /* Nothing open: start a fresh capture. */
    ulid_generate(cap->bundle_id, 0);
    ulid_encode(cap->bundle_id, id_text);
    snprintf(cap->dir, sizeof(cap->dir), "%s/%s", CAIRN_DIR_CAPTURE, id_text);

    if (!SD.mkdir(cap->dir)) {
        CAIRN_LOGE(TAG, "mkdir %s failed", cap->dir);
        return false;
    }

    char path[112];
    segment_path(cap, 0, path, sizeof(path));
    if (!write_segment_header(cap, path, 0, 0)) return false;
    cap->segment_bytes = CAIRN_SEGMENT_HEADER_SIZE;

    journal_path(cap, path, sizeof(path));
    if (!write_segment_header(cap, path, 0, 0)) return false;
    cap->journal_bytes = CAIRN_SEGMENT_HEADER_SIZE;

    cap->active = true;
    CAIRN_LOGI(TAG, "opened capture %s", id_text);
    return true;
}

/* ── append ───────────────────────────────────────────────────────────────── */

static bool rotate_segment(cairn_capture_t *cap)
{
    /*
     * A bundle may hold at most CAIRN_MAX_MEMBERS members, one of which is
     * journal.seg. Running out of members is a reason to seal, not a reason to
     * fail an append, so the lifecycle is told rather than the data dropped.
     */
    if (cap->segment_index + 1 >= CAIRN_MAX_MEMBERS - 1) {
        cap->needs_seal = true;
        CAIRN_LOGW(TAG, "segment budget reached at index %u; sealing is required",
                   (unsigned)cap->segment_index);
        return false;
    }

    uint32_t next = cap->segment_index + 1;
    char path[112];
    segment_path(cap, next, path, sizeof(path));

    if (!write_segment_header(cap, path, next, cap->capture_chain.next_seq)) {
        return false;
    }

    cap->segment_index    = next;
    cap->segment_bytes    = CAIRN_SEGMENT_HEADER_SIZE;
    cap->segment_first_seq = cap->capture_chain.next_seq;

    return true;
}

bool cairn_capture_append(cairn_capture_t *cap, cairn_chain_id_t chain_id,
                          uint8_t record_type, uint8_t schema_version,
                          uint16_t flags, uint32_t monotonic_ms,
                          const uint8_t *payload, size_t payload_len)
{
    if (!cap->active) return false;

    cairn_chain_t *chain =
        (chain_id == CAIRN_CHAIN_JOURNAL) ? &cap->journal_chain : &cap->capture_chain;

    uint8_t stage[CAIRN_STAGE_BYTES];
    size_t  written = 0;
    uint32_t crc = 0;

    cairn_err_t err = cairn_encode_frame(record_type, schema_version, flags,
                                         chain->next_seq, monotonic_ms,
                                         chain->prev_crc32, payload, payload_len,
                                         stage, sizeof(stage), &written, &crc);
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "encode frame type 0x%02x: %s", record_type,
                   cairn_strerror(err));
        return false;
    }

    char path[112];
    if (chain_id == CAIRN_CHAIN_JOURNAL) {
        journal_path(cap, path, sizeof(path));
    } else {
        if (cap->segment_bytes + written > CAIRN_SEGMENT_MAX_BYTES) {
            if (!rotate_segment(cap)) return false;
        }
        segment_path(cap, cap->segment_index, path, sizeof(path));
    }

    File f = SD.open(path, FILE_APPEND);
    if (!f) {
        cap->write_errors++;
        CAIRN_LOGE(TAG, "cannot append to %s", path);
        return false;
    }

    size_t n = f.write(stage, written);

    /*
     * Flush on a cadence rather than per frame. The format tolerates a torn
     * tail exactly — that is what the length prefix and per-frame CRC are for —
     * so the trade is bounded data loss against a third of the write load.
     */
    cap->frames_since_flush++;
    uint32_t now = millis();
    if (cap->frames_since_flush >= CAIRN_FLUSH_EVERY_FRAMES ||
        now - cap->last_flush_ms >= CAIRN_FLUSH_EVERY_MS) {
        f.flush();
        cap->frames_since_flush = 0;
        cap->last_flush_ms = now;
    }
    f.close();

    if (n != written) {
        cap->write_errors++;
        CAIRN_LOGE(TAG, "short write to %s: %u of %u bytes", path, (unsigned)n,
                   (unsigned)written);
        /*
         * The chain is not advanced. The partial bytes become a torn tail that
         * the next boot's scan will find and truncate, which is strictly better
         * than advancing past a record that was never fully stored.
         */
        return false;
    }

    /* Only now is the frame real. */
    if (chain_id == CAIRN_CHAIN_JOURNAL) {
        cap->journal_bytes += (uint32_t)written;
    } else {
        cap->segment_bytes += (uint32_t)written;

        if (!cap->have_any_frame) {
            cap->first_seq = chain->next_seq;
            cap->have_any_frame = true;
        }
        cap->last_seq = chain->next_seq;
    }

    cap->record_counts[record_type]++;
    chain->next_seq++;
    chain->prev_crc32 = crc;
    cap->capture_ended_monotonic_us = (uint64_t)esp_timer_get_time();

    CAIRN_LOGT(TAG, "frame %s seq %u, %u bytes, crc %08x",
               cairn_record_type_name(record_type),
               (unsigned)(chain->next_seq - 1), (unsigned)written, (unsigned)crc);

    return true;
}

void cairn_capture_set_utc_basis(cairn_capture_t *cap, uint64_t utc_ms,
                                 uint32_t acc_ms)
{
    cap->utc_basis_ms     = utc_ms;
    cap->utc_basis_acc_ms = acc_ms;
}

bool cairn_capture_flush(cairn_capture_t *cap)
{
    /* Each append opens, writes and closes, so closing is the flush. This exists
     * for the durability points that must be explicit at the call site. */
    cap->frames_since_flush = 0;
    cap->last_flush_ms = millis();
    return true;
}

void cairn_capture_tick(cairn_capture_t *cap)
{
    (void)cap;
}

/* ── seal ─────────────────────────────────────────────────────────────────── */

/* Collect the members of a capture directory, sorted canonically. */
static size_t collect_members(const char *dir, cairn_member_t *members,
                              size_t max_members)
{
    File d = SD.open(dir);
    if (!d) return 0;

    size_t count = 0;
    for (;;) {
        File entry = d.openNextFile();
        if (!entry) break;

        if (!entry.isDirectory()) {
            const char *name = entry.name();
            const char *base = strrchr(name, '/');
            base = (base != nullptr) ? base + 1 : name;

            /* Only .seg files are members. A manifest written by an interrupted
             * seal must not become a member of the bundle it describes. */
            size_t nlen = strlen(base);
            bool is_seg = nlen > 4 && strcmp(base + nlen - 4, ".seg") == 0;

            if (is_seg && count < max_members && nlen + 1 <= CAIRN_MAX_MEMBER_NAME) {
                snprintf(members[count].name, CAIRN_MAX_MEMBER_NAME, "%s", base);
                members[count].length = (uint64_t)entry.size();
                memset(members[count].sha256, 0, 32);
                count++;
            }
        }
        entry.close();
    }
    d.close();

    cairn_sort_members(members, count);
    return count;
}

/*
 * One pass over the bundle byte stream computes both the per-member digests and
 * the chunk descriptors.
 *
 * Members and chunks are different partitions of the same bytes (spec §6.1), so
 * computing them together is the only way to guarantee they describe the same
 * stream. Two separate passes could disagree if a file changed in between.
 */
static bool digest_members_and_chunks(const char *dir, cairn_member_t *members,
                                      size_t member_count,
                                      cairn_chunk_t *chunks, size_t *chunk_count)
{
    cairn_sha256_t chunk_ctx;
    uint32_t chunk_bytes = 0;
    size_t   chunks_used = 0;

    cairn_sha256_init(&chunk_ctx);

    for (size_t i = 0; i < member_count; i++) {
        char path[112];
        snprintf(path, sizeof(path), "%s/%s", dir, members[i].name);

        File f = SD.open(path, FILE_READ);
        if (!f) {
            CAIRN_LOGE(TAG, "cannot read member %s", members[i].name);
            return false;
        }

        cairn_sha256_t member_ctx;
        cairn_sha256_init(&member_ctx);

        uint8_t  block[IO_BLOCK];
        uint64_t total = 0;

        for (;;) {
            int got = f.read(block, sizeof(block));
            if (got <= 0) break;

            cairn_sha256_update(&member_ctx, block, (size_t)got);
            total += (uint64_t)got;

            /* Feed the same bytes into the chunk rolling hash, closing a chunk
             * whenever it fills. A chunk may span members. */
            size_t consumed = 0;
            while (consumed < (size_t)got) {
                size_t room = CHUNK_BYTES - chunk_bytes;
                size_t take = (size_t)got - consumed;
                if (take > room) take = room;

                cairn_sha256_update(&chunk_ctx, block + consumed, take);
                chunk_bytes += (uint32_t)take;
                consumed    += take;

                if (chunk_bytes == CHUNK_BYTES) {
                    if (chunks_used >= CAIRN_MAX_CHUNKS) {
                        CAIRN_LOGE(TAG, "bundle exceeds %d chunks", CAIRN_MAX_CHUNKS);
                        f.close();
                        return false;
                    }
                    chunks[chunks_used].index       = (uint32_t)chunks_used;
                    chunks[chunks_used].byte_length = chunk_bytes;
                    cairn_sha256_final(&chunk_ctx, chunks[chunks_used].sha256);
                    chunks_used++;

                    cairn_sha256_init(&chunk_ctx);
                    chunk_bytes = 0;
                }
            }
        }
        f.close();

        cairn_sha256_final(&member_ctx, members[i].sha256);

        /* The recorded length must be the bytes actually hashed, or the member
         * digest and the manifest would describe different things. */
        members[i].length = total;
    }

    /* Close the final partial chunk. */
    if (chunk_bytes > 0) {
        if (chunks_used >= CAIRN_MAX_CHUNKS) {
            CAIRN_LOGE(TAG, "bundle exceeds %d chunks", CAIRN_MAX_CHUNKS);
            return false;
        }
        chunks[chunks_used].index       = (uint32_t)chunks_used;
        chunks[chunks_used].byte_length = chunk_bytes;
        cairn_sha256_final(&chunk_ctx, chunks[chunks_used].sha256);
        chunks_used++;
    }

    *chunk_count = chunks_used;
    return true;
}

static bool write_exact(const char *path, const uint8_t *data, size_t len)
{
    SD.remove(path);

    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;

    size_t n = f.write(data, len);
    f.flush();
    f.close();

    return n == len;
}

/*
 * Build, sign and write the manifest for a capture directory, then move it to
 * CAIRN_DIR_BUNDLES.
 *
 * Split from cairn_capture_seal so that a seal interrupted after the manifest
 * was written can be finished at the next boot without re-signing.
 */
static bool finish_seal(const char *dir, const char *id_text)
{
    char from[112], to[112];
    snprintf(from, sizeof(from), "%s", dir);
    snprintf(to, sizeof(to), "%s/%s", CAIRN_DIR_BUNDLES, id_text);

    if (SD.exists(to)) {
        CAIRN_LOGE(TAG, "%s already exists; refusing to overwrite a sealed "
                        "bundle", to);
        return false;
    }

    if (!SD.rename(from, to)) {
        CAIRN_LOGE(TAG, "rename %s -> %s failed", from, to);
        return false;
    }

    CAIRN_LOGI(TAG, "sealed bundle %s", id_text);
    return true;
}

bool cairn_capture_seal(cairn_capture_t *cap, const uint8_t seed[32],
                        const uint8_t pub[32], const char *firmware_version,
                        uint8_t policy_version, uint8_t out_bundle_id[16])
{
    if (!cap->active) return false;

    cairn_capture_flush(cap);
    cap->capture_ended_monotonic_us = (uint64_t)esp_timer_get_time();

    static cairn_manifest_t m;
    memset(&m, 0, sizeof(m));

    m.manifest_version = CAIRN_MANIFEST_VERSION;
    memcpy(m.bundle_id, cap->bundle_id, 16);
    memcpy(m.device_id, cap->device_id, 16);
    memcpy(m.boot_id, cap->boot_id, 16);
    cairn_device_key_id(pub, m.device_key_id);
    snprintf(m.firmware_version, sizeof(m.firmware_version), "%s", firmware_version);
    m.schema_version = 1;

    m.capture_started_monotonic_us = cap->capture_started_monotonic_us;
    m.capture_ended_monotonic_us   = cap->capture_ended_monotonic_us;
    m.utc_basis_ms                 = cap->utc_basis_ms;
    m.utc_basis_acc_ms             = cap->utc_basis_acc_ms;
    m.first_seq                    = cap->first_seq;
    m.last_seq                     = cap->last_seq;

    memcpy(m.record_counts, cap->record_counts, sizeof(m.record_counts));

    m.policy_version        = policy_version;
    m.recovery_state        = cap->recovery_state;
    m.discarded_tail_bytes  = cap->discarded_tail_bytes;
    snprintf(m.signature_algorithm, sizeof(m.signature_algorithm), "%s",
             CAIRN_SIGALG_ED25519);

    m.member_count = collect_members(cap->dir, m.members, CAIRN_MAX_MEMBERS);
    if (m.member_count == 0) {
        CAIRN_LOGE(TAG, "no members in %s; nothing to seal", cap->dir);
        return false;
    }

    if (!digest_members_and_chunks(cap->dir, m.members, m.member_count,
                                   m.chunks, &m.chunk_count)) {
        return false;
    }

    cairn_err_t err = cairn_content_root(m.members, m.member_count, m.content_root);
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "content_root: %s", cairn_strerror(err));
        return false;
    }

    static uint8_t encoded[4096];
    size_t  encoded_len = 0;
    uint8_t sig[64];

    err = cairn_manifest_sign(&m, seed, pub, encoded, sizeof(encoded),
                              &encoded_len, sig);
    if (err != CAIRN_OK) {
        CAIRN_LOGE(TAG, "manifest_sign: %s", cairn_strerror(err));
        return false;
    }

    /*
     * Verify what was just produced before relying on it. A manifest that does
     * not verify on the device will not verify on the server either, and
     * discovering that here keeps a broken bundle out of the upload queue.
     */
    if (cairn_manifest_verify(encoded, encoded_len, sig, pub) != CAIRN_OK) {
        CAIRN_LOGE(TAG, "freshly signed manifest fails verification; refusing "
                        "to seal");
        return false;
    }

    char id_text[27];
    ulid_encode(cap->bundle_id, id_text);

    char path[112];
    snprintf(path, sizeof(path), "%s/manifest.cbor", cap->dir);
    if (!write_exact(path, encoded, encoded_len)) {
        CAIRN_LOGE(TAG, "cannot write %s", path);
        return false;
    }

    snprintf(path, sizeof(path), "%s/manifest.sig", cap->dir);
    if (!write_exact(path, sig, sizeof(sig))) {
        CAIRN_LOGE(TAG, "cannot write %s", path);
        return false;
    }

    CAIRN_LOGI(TAG, "manifest written: %u members, %u chunks, %u bytes, "
                    "content_root %02x%02x%02x%02x..",
               (unsigned)m.member_count, (unsigned)m.chunk_count,
               (unsigned)encoded_len, m.content_root[0], m.content_root[1],
               m.content_root[2], m.content_root[3]);

    if (!finish_seal(cap->dir, id_text)) return false;

    if (out_bundle_id != nullptr) memcpy(out_bundle_id, cap->bundle_id, 16);

    cap->active = false;
    return true;
}

int cairn_store_resume_interrupted_seals(void)
{
    File dir = SD.open(CAIRN_DIR_CAPTURE);
    if (!dir) return 0;

    /* Collect first, act second: moving a directory while iterating it would
     * invalidate the iterator. */
    char     candidates[CAIRN_MAX_MEMBERS][27];
    int      candidate_count = 0;

    for (;;) {
        File entry = dir.openNextFile();
        if (!entry) break;

        if (entry.isDirectory() && candidate_count < CAIRN_MAX_MEMBERS) {
            const char *name = entry.name();
            const char *base = strrchr(name, '/');
            base = (base != nullptr) ? base + 1 : name;

            uint8_t probe[16];
            if (ulid_decode(base, probe)) {
                snprintf(candidates[candidate_count], 27, "%s", base);
                candidate_count++;
            }
        }
        entry.close();
    }
    dir.close();

    int finished = 0;

    for (int i = 0; i < candidate_count; i++) {
        char cdir[112], mpath[128], spath[128];
        snprintf(cdir, sizeof(cdir), "%s/%s", CAIRN_DIR_CAPTURE, candidates[i]);
        snprintf(mpath, sizeof(mpath), "%s/manifest.cbor", cdir);
        snprintf(spath, sizeof(spath), "%s/manifest.sig", cdir);

        /* No manifest means the seal had not started: this is a live capture to
         * resume, not an interrupted seal. */
        if (!SD.exists(mpath) || !SD.exists(spath)) continue;

        CAIRN_LOGW(TAG, "capture %s already holds a manifest; finishing the "
                        "interrupted seal", candidates[i]);

        if (finish_seal(cdir, candidates[i])) finished++;
    }

    if (finished > 0) CAIRN_LOGI(TAG, "completed %d interrupted seal(s)", finished);
    return finished;
}

bool cairn_store_pending_stats(uint32_t *bundle_count, uint64_t *total_bytes)
{
    File dir = SD.open(CAIRN_DIR_BUNDLES);
    if (!dir) return false;

    uint32_t count = 0;
    uint64_t bytes = 0;

    for (;;) {
        File entry = dir.openNextFile();
        if (!entry) break;

        if (entry.isDirectory()) {
            count++;

            /* name() may be absolute depending on the core version, so take the
             * basename before rebuilding the path. */
            const char *nm = entry.name();
            const char *base = strrchr(nm, '/');
            base = (base != nullptr) ? base + 1 : nm;

            char sub[112];
            snprintf(sub, sizeof(sub), "%s/%s", CAIRN_DIR_BUNDLES, base);

            File b = SD.open(sub);
            if (b) {
                for (;;) {
                    File f = b.openNextFile();
                    if (!f) break;
                    if (!f.isDirectory()) bytes += (uint64_t)f.size();
                    f.close();
                }
                b.close();
            }
        }
        entry.close();
    }
    dir.close();

    *bundle_count = count;
    *total_bytes  = bytes;
    return true;
}
