#include "cairn_sync.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cairn_format.h"
#include "cairn_log.h"
#include "config.h"

static const char *TAG = "SYNC";

#define IO_BLOCK 2048

/* The chunk staging buffer dominates this module's memory use. 256 KiB does not
 * fit in DRAM, so chunks are streamed from the card rather than assembled. */
#define HTTP_CHUNK_STREAM_BLOCK 4096

const char *cairn_sync_result_name(cairn_sync_result_t r)
{
    switch (r) {
    case CAIRN_SYNC_OK:               return "OK";
    case CAIRN_SYNC_NO_NETWORK:       return "NO_NETWORK";
    case CAIRN_SYNC_NOTHING_TO_DO:    return "NOTHING_TO_DO";
    case CAIRN_SYNC_OFFER_FAILED:     return "OFFER_FAILED";
    case CAIRN_SYNC_TRANSFER_FAILED:  return "TRANSFER_FAILED";
    case CAIRN_SYNC_COMMIT_FAILED:    return "COMMIT_FAILED";
    case CAIRN_SYNC_RECEIPT_INVALID:  return "RECEIPT_INVALID";
    case CAIRN_SYNC_LOCAL_ERROR:      return "LOCAL_ERROR";
    default:                          return "UNKNOWN";
    }
}

/* ── the pinned server key ────────────────────────────────────────────────── */

static bool unhex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return false;

    for (int i = 0; i < 32; i++) {
        int hi = -1, lo = -1;
        char a = hex[i * 2], b = hex[i * 2 + 1];

        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;

        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;

        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/*
 * Returns false when the key is absent or the all-zero placeholder, in which
 * case nothing may be pruned. An unconfigured device fills its card; it does
 * not guess.
 */
static bool server_receipt_key(uint8_t out[32])
{
    if (!unhex32(CAIRN_SERVER_RECEIPT_KEY_HEX, out)) return false;

    bool all_zero = true;
    for (int i = 0; i < 32; i++) {
        if (out[i] != 0) {
            all_zero = false;
            break;
        }
    }
    return !all_zero;
}

/* ── network ──────────────────────────────────────────────────────────────── */

bool cairn_sync_connect(uint32_t timeout_ms)
{
    if (WiFi.status() == WL_CONNECTED) return true;

    CAIRN_LOGI(TAG, "associating with \"%s\"", CAIRN_WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(CAIRN_WIFI_SSID, CAIRN_WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeout_ms) {
            CAIRN_LOGW(TAG, "association timed out after %u ms (status %d)",
                       (unsigned)(millis() - start), (int)WiFi.status());
            WiFi.disconnect(true);
            return false;
        }
        delay(200);
    }

    CAIRN_LOGI(TAG, "associated: ip %s, rssi %d dBm",
               WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

void cairn_sync_disconnect(void)
{
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        CAIRN_LOGD(TAG, "radio off");
    }
}

bool cairn_sync_is_connected(void)
{
    return WiFi.status() == WL_CONNECTED;
}

int cairn_sync_rssi(void)
{
    return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
}

/* ── small helpers ────────────────────────────────────────────────────────── */

static void tohex(const uint8_t *b, size_t len, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = d[b[i] >> 4];
        out[i * 2 + 1] = d[b[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static void base_url(char *out, size_t cap)
{
    snprintf(out, cap, "http://%s:%d", CAIRN_SERVER_HOST, (int)CAIRN_SERVER_PORT);
}

static uint8_t *read_whole_file(const char *path, size_t *len)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return nullptr;

    size_t size = (size_t)f.size();
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? size : 1);
    if (buf == nullptr) {
        f.close();
        return nullptr;
    }

    int got = f.read(buf, size);
    f.close();

    if (got != (int)size) {
        free(buf);
        return nullptr;
    }

    *len = size;
    return buf;
}

/*
 * Extract a JSON number array. Purpose-built rather than a parser: the only
 * JSON this firmware consumes is the offer response, whose shape is fixed by
 * the server, and everything that carries a signature is CBOR.
 */
static size_t parse_uint_array(const char *json, const char *key,
                               uint32_t *out, size_t max_out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char *p = strstr(json, needle);
    if (p == nullptr) return 0;

    p = strchr(p, '[');
    if (p == nullptr) return 0;
    p++;

    size_t count = 0;
    while (*p != '\0' && *p != ']') {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == ']' || *p == '\0') break;

        char *endp = nullptr;
        unsigned long v = strtoul(p, &endp, 10);
        if (endp == p) break;

        if (count < max_out) out[count] = (uint32_t)v;
        count++;
        p = endp;
    }
    return count;
}

static bool json_bool(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    const char *p = strstr(json, needle);
    if (p == nullptr) return false;

    p = strchr(p, ':');
    if (p == nullptr) return false;

    while (*++p == ' ') { }
    return strncmp(p, "true", 4) == 0;
}

/* ── the bundle byte stream ───────────────────────────────────────────────── */

/*
 * The byte stream is the concatenation of member contents in canonical member
 * order (spec §6.1). Chunks partition that stream, so a chunk may span members
 * and a member may span chunks — reading a chunk means walking the members.
 */
typedef struct {
    char     dir[112];
    cairn_member_t members[CAIRN_MAX_MEMBERS];
    size_t   member_count;
} stream_ctx_t;

/*
 * A read-only Stream over a byte range of the bundle stream.
 *
 * Presenting the chunk as a Stream lets HTTPClient send it with a known
 * Content-Length while only one block is ever resident. A 256 KiB chunk does
 * not fit in DRAM, and staging it in PSRAM would make uploads depend on a
 * module that may not be fitted.
 */
class BundleChunkStream : public Stream {
public:
    BundleChunkStream(const stream_ctx_t *ctx, uint64_t offset, uint64_t length)
        : ctx_(ctx), remaining_(length), member_(0), member_pos_(0), ok_(true)
    {
        /* Walk to the member containing `offset`. */
        uint64_t pos = 0;
        while (member_ < ctx_->member_count) {
            uint64_t mlen = ctx_->members[member_].length;
            if (offset < pos + mlen) {
                member_pos_ = offset - pos;
                break;
            }
            pos += mlen;
            member_++;
        }
        openCurrent();
    }

    ~BundleChunkStream() override
    {
        if (file_) file_.close();
    }

    bool ok() const { return ok_; }

    int available() override
    {
        return (remaining_ > 0x7fffffff) ? 0x7fffffff : (int)remaining_;
    }

    int read() override
    {
        uint8_t b;
        return (readBytes(&b, 1) == 1) ? (int)b : -1;
    }

    int peek() override { return -1; }

    size_t readBytes(uint8_t *buf, size_t len) override
    {
        size_t done = 0;

        while (done < len && remaining_ > 0 && ok_) {
            if (!file_) {
                ok_ = false;
                break;
            }

            uint64_t left_in_member =
                ctx_->members[member_].length - member_pos_;
            if (left_in_member == 0) {
                advanceMember();
                continue;
            }

            size_t want = len - done;
            if (want > left_in_member) want = (size_t)left_in_member;
            if (want > remaining_) want = (size_t)remaining_;

            int got = file_.read(buf + done, want);
            if (got <= 0) {
                ok_ = false;
                break;
            }

            done        += (size_t)got;
            member_pos_ += (uint64_t)got;
            remaining_  -= (uint64_t)got;
        }

        return done;
    }

    size_t readBytes(char *buf, size_t len) override
    {
        return readBytes((uint8_t *)buf, len);
    }

    /* Write side is unused; the stream is read-only. */
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t *, size_t) override { return 0; }
    void   flush() override { }

private:
    void openCurrent()
    {
        if (member_ >= ctx_->member_count) {
            ok_ = (remaining_ == 0);
            return;
        }

        char path[160];
        snprintf(path, sizeof(path), "%s/%s", ctx_->dir,
                 ctx_->members[member_].name);

        file_ = SD.open(path, FILE_READ);
        if (!file_ || !file_.seek((uint32_t)member_pos_)) ok_ = false;
    }

    void advanceMember()
    {
        if (file_) file_.close();
        member_++;
        member_pos_ = 0;
        openCurrent();
    }

    const stream_ctx_t *ctx_;
    File     file_;
    uint64_t remaining_;
    size_t   member_;
    uint64_t member_pos_;
    bool     ok_;
};

/* ── the prune journal ───────────────────────────────────────────────────── */

/*
 * Pruning is transactional. The intent record names the bundle and the content
 * root the receipt acknowledged; it is written and flushed before the first
 * delete. An interrupted prune therefore leaves an intent with no completion,
 * which the next boot can recognize and finish — rather than a partially
 * deleted directory that still looks like a sealed bundle awaiting upload.
 */
static void prune_journal_path(const char *id_text, char *out, size_t cap)
{
    snprintf(out, cap, "%s/prune-%s.json", CAIRN_DIR_STATE, id_text);
}

static bool prune_intent_write(const char *id_text, const uint8_t root[32])
{
    char path[160], root_hex[65];
    prune_journal_path(id_text, path, sizeof(path));
    tohex(root, 32, root_hex);

    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;

    f.printf("{\"bundle\":\"%s\",\"content_root\":\"%s\",\"state\":\"intent\"}\n",
             id_text, root_hex);
    f.flush();
    f.close();
    return true;
}

static void prune_intent_complete(const char *id_text)
{
    char path[160];
    prune_journal_path(id_text, path, sizeof(path));
    SD.remove(path);
}

static bool delete_bundle_dir(const char *id_text)
{
    char dir[160];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, id_text);

    File d = SD.open(dir);
    if (!d) return false;

    /* Collect names first: removing while iterating invalidates the iterator. */
    char   names[CAIRN_MAX_MEMBERS + 4][40];
    size_t count = 0;

    for (;;) {
        File entry = d.openNextFile();
        if (!entry) break;

        if (!entry.isDirectory() && count < (sizeof(names) / sizeof(names[0]))) {
            const char *nm = entry.name();
            const char *base = strrchr(nm, '/');
            base = (base != nullptr) ? base + 1 : nm;
            snprintf(names[count], sizeof(names[0]), "%s", base);
            count++;
        }
        entry.close();
    }
    d.close();

    for (size_t i = 0; i < count; i++) {
        char path[220];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (!SD.remove(path)) {
            CAIRN_LOGE(TAG, "cannot remove %s", path);
            return false;
        }
    }

    return SD.rmdir(dir);
}

/*
 * Prune one bundle, but only against a receipt that has already been verified
 * against the pinned key and shown to acknowledge this exact content root.
 */
static bool prune_receipted_bundle(const char *id_text, const uint8_t root[32])
{
    if (!prune_intent_write(id_text, root)) {
        CAIRN_LOGE(TAG, "cannot write the prune intent for %s; refusing to "
                        "delete anything", id_text);
        return false;
    }

    if (!delete_bundle_dir(id_text)) {
        CAIRN_LOGE(TAG, "prune of %s failed part-way; the intent record remains "
                        "so the next boot can finish it", id_text);
        return false;
    }

    prune_intent_complete(id_text);
    CAIRN_LOGI(TAG, "pruned %s (receipt verified)", id_text);
    return true;
}

int cairn_sync_resume_interrupted_prunes(void)
{
    File d = SD.open(CAIRN_DIR_STATE);
    if (!d) return 0;

    char   pending[8][40];
    size_t count = 0;

    for (;;) {
        File entry = d.openNextFile();
        if (!entry) break;

        if (!entry.isDirectory()) {
            const char *nm = entry.name();
            const char *base = strrchr(nm, '/');
            base = (base != nullptr) ? base + 1 : nm;

            if (strncmp(base, "prune-", 6) == 0 &&
                count < sizeof(pending) / sizeof(pending[0])) {
                snprintf(pending[count], sizeof(pending[0]), "%s", base);
                count++;
            }
        }
        entry.close();
    }
    d.close();

    int finished = 0;

    for (size_t i = 0; i < count; i++) {
        /* "prune-<id>.json" → <id> */
        char id_text[40];
        snprintf(id_text, sizeof(id_text), "%s", pending[i] + 6);
        char *dot = strstr(id_text, ".json");
        if (dot != nullptr) *dot = '\0';

        /*
         * The intent is only ever written after a receipt was verified, so
         * finishing the delete is authorized. The receipt on the card is the
         * durable evidence; the bundle bytes are not.
         */
        char receipt[160];
        snprintf(receipt, sizeof(receipt), "%s/%s.cbor", CAIRN_DIR_RECEIPTS, id_text);

        if (!SD.exists(receipt)) {
            CAIRN_LOGE(TAG, "prune intent for %s has no stored receipt; leaving "
                            "the bundle in place and clearing the intent",
                       id_text);
            char path[160];
            prune_journal_path(id_text, path, sizeof(path));
            SD.remove(path);
            continue;
        }

        CAIRN_LOGW(TAG, "finishing interrupted prune of %s", id_text);
        if (delete_bundle_dir(id_text)) {
            prune_intent_complete(id_text);
            finished++;
        }
    }

    if (finished > 0) CAIRN_LOGI(TAG, "completed %d interrupted prune(s)", finished);
    return finished;
}

/* ── upload one bundle ────────────────────────────────────────────────────── */

static cairn_sync_result_t upload_bundle(const char *id_text,
                                         const uint8_t server_key[32],
                                         bool have_server_key,
                                         cairn_sync_stats_t *stats)
{
    char dir[112];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, id_text);

    char mpath[160], spath[160];
    snprintf(mpath, sizeof(mpath), "%s/manifest.cbor", dir);
    snprintf(spath, sizeof(spath), "%s/manifest.sig", dir);

    size_t manifest_len = 0, sig_len = 0;
    uint8_t *manifest = read_whole_file(mpath, &manifest_len);
    uint8_t *sig = read_whole_file(spath, &sig_len);

    if (manifest == nullptr || sig == nullptr || sig_len != 64) {
        CAIRN_LOGE(TAG, "%s is missing a usable manifest; skipping", id_text);
        free(manifest);
        free(sig);
        return CAIRN_SYNC_LOCAL_ERROR;
    }

    /* Decode our own manifest to recover the chunk descriptors and members. */
    static cairn_manifest_t m;
    static uint8_t scratch[4096];
    cairn_err_t derr =
        cairn_manifest_decode(manifest, manifest_len, &m, scratch, sizeof(scratch));
    if (derr != CAIRN_OK) {
        CAIRN_LOGE(TAG, "cannot decode our own manifest for %s: %s", id_text,
                   cairn_strerror(derr));
        free(manifest);
        free(sig);
        return CAIRN_SYNC_LOCAL_ERROR;
    }

    char bundle_hex[33];
    tohex(m.bundle_id, 16, bundle_hex);

    /*
     * The chunk URL is the longest: base + "/api/v2/bundles/" + 32 hex bundle
     * id + "/chunks/" + 64 hex digest. Sized so a digest can never be
     * truncated, which would send bytes to the wrong address and fail the
     * server's hash check for a reason that looks like corruption.
     */
    char url[320], base[96];
    base_url(base, sizeof(base));

    /* ── OFFER ────────────────────────────────────────────────────────────── */

    char sig_hex[129];
    tohex(sig, 64, sig_hex);

    snprintf(url, sizeof(url), "%s/api/v2/bundles/offer", base);

    HTTPClient http;
    http.setTimeout(CAIRN_SYNC_HTTP_TIMEOUT_MS);
    if (!http.begin(url)) {
        free(manifest);
        free(sig);
        return CAIRN_SYNC_NO_NETWORK;
    }
    http.addHeader("Content-Type", "application/cbor");
    http.addHeader("X-Cairn-Signature", sig_hex);

    int code = http.POST(manifest, manifest_len);
    String body = (code > 0) ? http.getString() : String("");
    http.end();

    free(manifest);
    free(sig);

    if (code != 200) {
        CAIRN_LOGE(TAG, "offer for %s failed: HTTP %d %s", id_text, code,
                   body.c_str());
        return CAIRN_SYNC_OFFER_FAILED;
    }

    stats->bundles_offered++;

    uint32_t missing[CAIRN_MAX_CHUNKS];
    size_t missing_count =
        parse_uint_array(body.c_str(), "missing_chunks", missing, CAIRN_MAX_CHUNKS);
    bool receipt_available = json_bool(body.c_str(), "receipt_available");

    CAIRN_LOGI(TAG, "offer %s: %u of %u chunks missing%s", id_text,
               (unsigned)missing_count, (unsigned)m.chunk_count,
               receipt_available ? ", receipt already available" : "");

    if (missing_count > CAIRN_MAX_CHUNKS) {
        CAIRN_LOGE(TAG, "server reported %u missing chunks, more than the "
                        "manifest describes", (unsigned)missing_count);
        return CAIRN_SYNC_OFFER_FAILED;
    }

    stats->chunks_skipped += (uint32_t)(m.chunk_count - missing_count);

    /* ── TRANSFER ─────────────────────────────────────────────────────────── */

    stream_ctx_t stream;
    snprintf(stream.dir, sizeof(stream.dir), "%s", dir);
    memcpy(stream.members, m.members, sizeof(stream.members));
    stream.member_count = m.member_count;

    for (size_t i = 0; i < missing_count; i++) {
        uint32_t idx = missing[i];
        if (idx >= m.chunk_count) {
            CAIRN_LOGE(TAG, "server asked for chunk %u which this manifest does "
                            "not describe", (unsigned)idx);
            return CAIRN_SYNC_TRANSFER_FAILED;
        }

        /* Chunk offset is the sum of preceding chunk lengths; chunks partition
         * the stream in order without gaps (spec §6.1). */
        uint64_t offset = 0;
        for (uint32_t j = 0; j < idx; j++) offset += m.chunks[j].byte_length;
        uint32_t length = m.chunks[idx].byte_length;

        char digest_hex[65];
        tohex(m.chunks[idx].sha256, 32, digest_hex);

        snprintf(url, sizeof(url), "%s/api/v2/bundles/%s/chunks/%s", base,
                 bundle_hex, digest_hex);

        HTTPClient put;
        put.setTimeout(CAIRN_SYNC_HTTP_TIMEOUT_MS);
        if (!put.begin(url)) return CAIRN_SYNC_NO_NETWORK;

        put.addHeader("Content-Type", "application/octet-stream");

        BundleChunkStream chunk_stream(&stream, offset, length);
        if (!chunk_stream.ok()) {
            put.end();
            CAIRN_LOGE(TAG, "chunk %u: cannot open the bundle stream",
                       (unsigned)idx);
            return CAIRN_SYNC_TRANSFER_FAILED;
        }

        int put_code = put.sendRequest("PUT", &chunk_stream, length);
        String put_body = (put_code > 0) ? put.getString() : String("");
        put.end();

        if (!chunk_stream.ok()) {
            CAIRN_LOGE(TAG, "chunk %u: reading the bundle stream failed",
                       (unsigned)idx);
            return CAIRN_SYNC_TRANSFER_FAILED;
        }

        if (put_code != 200) {
            CAIRN_LOGE(TAG, "chunk %u of %s rejected: HTTP %d %s", (unsigned)idx,
                       id_text, put_code, put_body.c_str());
            return CAIRN_SYNC_TRANSFER_FAILED;
        }

        stats->chunks_sent++;
        stats->bytes_sent += length;

        CAIRN_LOGD(TAG, "chunk %u/%u sent (%u bytes, %s..)", (unsigned)(i + 1),
                   (unsigned)missing_count, (unsigned)length,
                   String(digest_hex).substring(0, 8).c_str());
    }

    /* ── COMMIT ───────────────────────────────────────────────────────────── */

    snprintf(url, sizeof(url), "%s/api/v2/bundles/%s/commit", base, bundle_hex);

    HTTPClient commit;
    commit.setTimeout(CAIRN_SYNC_HTTP_TIMEOUT_MS);
    if (!commit.begin(url)) return CAIRN_SYNC_NO_NETWORK;

    int commit_code = commit.POST((uint8_t *)nullptr, 0);
    if (commit_code != 200) {
        String err = (commit_code > 0) ? commit.getString() : String("");
        commit.end();
        CAIRN_LOGE(TAG, "commit for %s failed: HTTP %d %s", id_text, commit_code,
                   err.c_str());
        return CAIRN_SYNC_COMMIT_FAILED;
    }

    /* The receipt is CBOR; the signature covers those exact bytes, so they are
     * taken verbatim with no re-encoding step anywhere in the path. */
    int receipt_len = commit.getSize();
    static uint8_t receipt_buf[1024];

    WiFiClient *rs = commit.getStreamPtr();
    int got = 0;
    if (rs != nullptr) {
        if (receipt_len > 0 && receipt_len <= (int)sizeof(receipt_buf)) {
            got = rs->readBytes(receipt_buf, (size_t)receipt_len);
        } else {
            got = rs->readBytes(receipt_buf, sizeof(receipt_buf));
        }
    }
    commit.end();

    if (got <= 0) {
        CAIRN_LOGE(TAG, "commit for %s returned no receipt body", id_text);
        return CAIRN_SYNC_COMMIT_FAILED;
    }

    /* ── VERIFY ───────────────────────────────────────────────────────────── */

    static cairn_receipt_t r;
    static uint8_t rscratch[1024];
    cairn_err_t rerr =
        cairn_receipt_decode(receipt_buf, (size_t)got, &r, rscratch, sizeof(rscratch));
    if (rerr != CAIRN_OK) {
        stats->receipts_rejected++;
        CAIRN_LOGE(TAG, "receipt for %s does not decode: %s", id_text,
                   cairn_strerror(rerr));
        return CAIRN_SYNC_RECEIPT_INVALID;
    }

    /*
     * Store the receipt before acting on it. A receipt is the only evidence that
     * data is safe elsewhere; losing it to a reboot would mean re-uploading a
     * bundle that was already durable, and in the pruning path it is the record
     * that authorizes deletion.
     */
    char rpath[160];
    snprintf(rpath, sizeof(rpath), "%s/%s.cbor", CAIRN_DIR_RECEIPTS, id_text);

    File rf = SD.open(rpath, FILE_WRITE);
    if (rf) {
        rf.write(receipt_buf, (size_t)got);
        rf.flush();
        rf.close();
    } else {
        CAIRN_LOGW(TAG, "cannot store the receipt for %s; it will be re-fetched",
                   id_text);
    }

    if (!have_server_key) {
        CAIRN_LOGW(TAG, "%s is uploaded but no server key is pinned, so nothing "
                        "will be pruned. Set CAIRN_SERVER_RECEIPT_KEY_HEX in "
                        "secrets.h to enable reclaiming space.", id_text);
        stats->bundles_receipted++;
        return CAIRN_SYNC_OK;
    }

    /*
     * Both conditions, every time: the signature must verify against the pinned
     * key, and the receipt must acknowledge the content root actually uploaded.
     * A valid signature over a different bundle is not an acknowledgement of
     * this one.
     */
    cairn_err_t verr =
        cairn_receipt_verify_acknowledges(&r, server_key, m.content_root);
    if (verr != CAIRN_OK) {
        stats->receipts_rejected++;
        CAIRN_LOGE(TAG, "receipt for %s rejected (%s); the bundle stays on the "
                        "card", id_text, cairn_strerror(verr));
        return CAIRN_SYNC_RECEIPT_INVALID;
    }

    stats->bundles_receipted++;
    CAIRN_LOGI(TAG, "receipt verified for %s", id_text);

    if (prune_receipted_bundle(id_text, m.content_root)) stats->bundles_pruned++;

    return CAIRN_SYNC_OK;
}

/* ── driver ───────────────────────────────────────────────────────────────── */

cairn_sync_result_t cairn_sync_run(cairn_sync_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));

    if (!cairn_sync_is_connected()) return CAIRN_SYNC_NO_NETWORK;

    uint8_t server_key[32];
    bool have_key = server_receipt_key(server_key);
    if (!have_key) {
        CAIRN_LOGW(TAG, "no server receipt key is pinned; bundles will upload "
                        "but none will be pruned");
    }

    /* Enumerate first: uploading mutates the directory. */
    File d = SD.open(CAIRN_DIR_BUNDLES);
    if (!d) return CAIRN_SYNC_LOCAL_ERROR;

    char   ids[16][40];
    size_t count = 0;

    for (;;) {
        File entry = d.openNextFile();
        if (!entry) break;

        if (entry.isDirectory() && count < sizeof(ids) / sizeof(ids[0])) {
            const char *nm = entry.name();
            const char *base = strrchr(nm, '/');
            base = (base != nullptr) ? base + 1 : nm;
            snprintf(ids[count], sizeof(ids[0]), "%s", base);
            count++;
        }
        entry.close();
    }
    d.close();

    if (count == 0) return CAIRN_SYNC_NOTHING_TO_DO;

    CAIRN_LOGI(TAG, "%u sealed bundle(s) to sync", (unsigned)count);

    cairn_sync_result_t worst = CAIRN_SYNC_OK;

    for (size_t i = 0; i < count; i++) {
        cairn_sync_result_t r =
            upload_bundle(ids[i], server_key, have_key, stats);

        /* Keep going: one unreachable or malformed bundle must not strand the
         * rest on the card. */
        if (r != CAIRN_SYNC_OK) {
            worst = r;
            CAIRN_LOGW(TAG, "%s: %s", ids[i], cairn_sync_result_name(r));
            if (r == CAIRN_SYNC_NO_NETWORK) break;
        }
    }

    CAIRN_LOGI(TAG, "sync done: %u offered, %u receipted, %u pruned, "
                    "%u chunks sent, %u already held, %u receipts rejected",
               (unsigned)stats->bundles_offered, (unsigned)stats->bundles_receipted,
               (unsigned)stats->bundles_pruned, (unsigned)stats->chunks_sent,
               (unsigned)stats->chunks_skipped, (unsigned)stats->receipts_rejected);

    return worst;
}
