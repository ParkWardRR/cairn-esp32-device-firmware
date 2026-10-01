#include "cairn_sync.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_log.h"
#include "cairn_prune.h"
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

/* ── transport ────────────────────────────────────────────────────────────── */

/*
 * mTLS when a CA is pinned at build time and the device's own certificate is on
 * the card; plain HTTP otherwise.
 *
 * The split is deliberate. The CA is compiled in because it is the trust
 * anchor — a CA read from the card could be swapped by anyone holding the card,
 * which would make verifying the server pointless. The client certificate and
 * key are on the card because they are rotatable credentials, and because the
 * certificate's CommonName must be the device id, which is not known until the
 * hardware has booted once. Requiring a reflash to issue a certificate would
 * make that a two-step dance every time.
 *
 * Falling back to HTTP is a real fallback, not a silent one: it is logged at
 * WARN on every sync. The receipt signature, never the transport, is what
 * authorizes deleting data, so HTTP changes who can read an upload rather than
 * whether a prune is legitimate.
 */

static bool  s_tls_ready;
static char *s_client_cert;
static char *s_client_key;

#if CAIRN_TLS_AVAILABLE
static WiFiClientSecure s_tls_client;
#endif

static char *read_text_file(const char *path)
{
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    if (f == nullptr) return nullptr;

    size_t len = (size_t)cairn_fs_size(f);
    if (len == 0 || len > 8192) {
        cairn_fs_close(f);
        return nullptr;
    }

    char *buf = (char *)malloc(len + 1);
    if (buf == nullptr) {
        cairn_fs_close(f);
        return nullptr;
    }

    size_t got = cairn_fs_read(f, buf, len);
    cairn_fs_close(f);

    if (got != len) {
        free(buf);
        return nullptr;
    }

    buf[len] = '\0';
    return buf;
}

/*
 * Load the client credentials. Called once after the card mounts; safe to call
 * again, which is what lets a certificate be dropped on the card and picked up
 * at the next boot without a firmware change.
 */
bool cairn_sync_load_credentials(void)
{
#if !CAIRN_TLS_AVAILABLE
    CAIRN_LOGW(TAG, "no CA pinned at build time, so uploads will use plain "
                    "HTTP. Define CAIRN_SERVER_CA_PEM in secrets.h to enable "
                    "mTLS.");
    return false;
#else
    free(s_client_cert);
    free(s_client_key);
    s_client_cert = read_text_file(CAIRN_PATH_CLIENT_CERT);
    s_client_key  = read_text_file(CAIRN_PATH_CLIENT_KEY);

    if (s_client_cert == nullptr || s_client_key == nullptr) {
        /*
         * A pinned CA with no client certificate means mTLS was intended but is
         * not provisioned. Say exactly what is missing and what the CommonName
         * has to be — the server rejects a certificate whose CN is not the
         * device id, and that is a confusing failure to debug from a TLS alert.
         */
        CAIRN_LOGE(TAG, "a CA is pinned but %s / %s are missing, so mTLS cannot "
                        "be used and uploads fall back to plain HTTP",
                   CAIRN_PATH_CLIENT_CERT, CAIRN_PATH_CLIENT_KEY);
        CAIRN_LOGE(TAG, "issue a certificate whose CommonName is this device's "
                        "id (printed above as device_id) and copy it to the card");
        free(s_client_cert);
        free(s_client_key);
        s_client_cert = nullptr;
        s_client_key = nullptr;
        s_tls_ready = false;
        return false;
    }

    s_tls_client.setCACert(CAIRN_SERVER_CA_PEM);
    s_tls_client.setCertificate(s_client_cert);
    s_tls_client.setPrivateKey(s_client_key);

    s_tls_ready = true;
    CAIRN_LOGI(TAG, "mTLS ready: CA pinned in firmware, client credentials from "
                    "the card");
    return true;
#endif
}

bool cairn_sync_tls_active(void) { return s_tls_ready; }

void cairn_sync_base_url(char *out, size_t cap)
{
    if (s_tls_ready) {
        snprintf(out, cap, "https://%s:%d", CAIRN_SERVER_HOST,
                 (int)CAIRN_SERVER_TLS_PORT);
    } else {
        snprintf(out, cap, "http://%s:%d", CAIRN_SERVER_HOST,
                 (int)CAIRN_SERVER_PORT);
    }
}

/* Begin a request on whichever transport is active. */
bool cairn_sync_begin_request(HTTPClient &http, const char *url)
{
    http.setTimeout(CAIRN_SYNC_HTTP_TIMEOUT_MS);

#if CAIRN_TLS_AVAILABLE
    if (s_tls_ready) return http.begin(s_tls_client, url);
#endif
    return http.begin(url);
}

static uint8_t *read_whole_file(const char *path, size_t *len)
{
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_READ);
    if (f == nullptr) return nullptr;

    size_t size = (size_t)cairn_fs_size(f);
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? size : 1);
    if (buf == nullptr) {
        cairn_fs_close(f);
        return nullptr;
    }

    size_t got = cairn_fs_read(f, buf, size);
    cairn_fs_close(f);

    if (got != size) {
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
        if (file_ != nullptr) cairn_fs_close(file_);
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
            if (file_ == nullptr) {
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

            size_t got = cairn_fs_read(file_, buf + done, want);
            if (got == 0) {
                ok_ = false;
                break;
            }

            done        += got;
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

        file_ = cairn_fs_open(path, CAIRN_FS_READ);
        if (file_ == nullptr || !cairn_fs_seek(file_, member_pos_)) ok_ = false;
    }

    void advanceMember()
    {
        if (file_ != nullptr) cairn_fs_close(file_);
        file_ = nullptr;
        member_++;
        member_pos_ = 0;
        openCurrent();
    }

    const stream_ctx_t *ctx_;
    cairn_file_t *file_ = nullptr;
    uint64_t remaining_;
    size_t   member_;
    uint64_t member_pos_;
    bool     ok_;
};

/* ── pruning ──────────────────────────────────────────────────────────────── */

/*
 * Pruning lives in cairn_prune.c, as portable C away from this file's HTTP
 * code. It is the invariant with the worst failure mode in the system — a
 * wrongly authorized prune deletes data permanently and reports success — so it
 * is kept where a host test can drive it with a forged receipt, a receipt for a
 * different bundle, and a receipt signed by the wrong key.
 */
int cairn_sync_resume_interrupted_prunes(void)
{
    return cairn_prune_resume_interrupted();
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
    cairn_sync_base_url(base, sizeof(base));

    /* ── OFFER ────────────────────────────────────────────────────────────── */

    char sig_hex[129];
    tohex(sig, 64, sig_hex);

    snprintf(url, sizeof(url), "%s/api/v2/bundles/offer", base);

    HTTPClient http;
    if (!cairn_sync_begin_request(http, url)) {
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
        if (!cairn_sync_begin_request(put, url)) return CAIRN_SYNC_NO_NETWORK;

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
    if (!cairn_sync_begin_request(commit, url)) return CAIRN_SYNC_NO_NETWORK;

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

    /* ── VERIFY AND PRUNE ─────────────────────────────────────────────────── */

    /*
     * Store the receipt before acting on it. A receipt is the only evidence
     * that the data is safe elsewhere; losing it to a reboot would mean
     * re-uploading a bundle that was already durable, and in the pruning path
     * it is the record that authorizes deletion.
     */
    if (!cairn_receipt_store(id_text, receipt_buf, (size_t)got)) {
        CAIRN_LOGW(TAG, "cannot store the receipt for %s; it will be re-fetched",
                   id_text);
    }

    /*
     * The gate itself is in cairn_prune.c. Both conditions are checked there,
     * every time: the signature must verify against the pinned key, and the
     * receipt must acknowledge the content root actually uploaded.
     */
    cairn_prune_result_t pr =
        cairn_prune_if_receipted(id_text, receipt_buf, (size_t)got,
                                 have_server_key ? server_key : nullptr,
                                 m.content_root);

    switch (pr) {
    case CAIRN_PRUNE_OK:
        stats->bundles_receipted++;
        stats->bundles_pruned++;
        return CAIRN_SYNC_OK;

    case CAIRN_PRUNE_NO_PINNED_KEY:
        /* Uploaded and receipted, but nothing may be reclaimed. Not a failure:
         * the data is safe, the card just keeps filling. */
        CAIRN_LOGW(TAG, "%s is uploaded but no server key is pinned, so nothing "
                        "will be pruned. Set CAIRN_SERVER_RECEIPT_KEY_HEX in "
                        "secrets.h to enable reclaiming space.", id_text);
        stats->bundles_receipted++;
        return CAIRN_SYNC_OK;

    case CAIRN_PRUNE_STORE_FAILED:
        /* The receipt was good; only the deletion faltered. The bundle is still
         * durable on the server, so this is not a receipt problem. */
        stats->bundles_receipted++;
        CAIRN_LOGW(TAG, "%s is receipted but could not be pruned; the next boot "
                        "will retry", id_text);
        return CAIRN_SYNC_OK;

    default:
        stats->receipts_rejected++;
        CAIRN_LOGE(TAG, "receipt for %s rejected (%s); the bundle stays on the "
                        "card", id_text, cairn_prune_result_name(pr));
        return CAIRN_SYNC_RECEIPT_INVALID;
    }
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

    /* Enumerate first: uploading prunes directories out from under an open
     * iterator. */
    cairn_dir_t *d = cairn_fs_opendir(CAIRN_DIR_BUNDLES);
    if (d == nullptr) return CAIRN_SYNC_LOCAL_ERROR;

    char   ids[16][40];
    size_t count = 0;
    char   name[64];
    bool   is_dir = false;

    while (cairn_fs_readdir(d, name, sizeof(name), &is_dir, nullptr)) {
        if (!is_dir || count >= sizeof(ids) / sizeof(ids[0])) continue;
        snprintf(ids[count], sizeof(ids[0]), "%s", name);
        count++;
    }
    cairn_fs_closedir(d);

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
