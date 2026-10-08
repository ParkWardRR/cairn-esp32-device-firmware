/* Uploading the pending bundles. See net_upload.h. */

#include "net_upload.h"

#if CAIRN_WIFI_UPLINK || CAIRN_LTE_UPLINK

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "board_config.h"
#include "cairn_bundle.h"
#include "cairn_fs.h"
#include "cairn_intake.h"
#include "cairn_log.h"
#include "cairn_prune.h"
#include "cairn_store.h"
#include "config.h"
#include "net_http.h"

static const char *TAG = "UPLOAD";

/*
 * One set of buffers for both transports. See net_upload.h: two sets overflowed
 * the static DRAM segment, and the schedule guarantees only one transport runs
 * at a time.
 *
 * The bundle reader is the largest single object in this firmware -- it holds
 * the manifest bytes and a decoded cairn_manifest_t, about 13 KB together -- and
 * it is needed only while an upload is in flight. So it is taken from the 4 MB
 * PSRAM on first use rather than reserved in the static DRAM segment, which is
 * the scarce one and the one that actually failed to link. The allocation is
 * kept for the life of the program rather than freed per session: churning 13 KB
 * of PSRAM every parked session would fragment it for no gain.
 *
 * If PSRAM is unavailable the upload is refused rather than falling back to
 * DRAM, because a silent fallback is how the segment overflows again later.
 */
static cairn_bundle_t *s_bundle;
static cairn_http_t    s_http;

/*
 * One scratch buffer, lent to cairn_bundle_open for the canonical re-encode and
 * then used to stream chunk bytes. Sized by the manifest bound because the
 * decode needs that much; chunk streaming is happy with any size.
 */
static uint8_t *s_scratch;        /* CAIRN_MANIFEST_ENCODED_MAX, in PSRAM */
static uint8_t  s_receipt[CAIRN_INTAKE_MAX_RECEIPT];

static bool ensure_bundle_reader(void)
{
    if (s_bundle != nullptr) return true;

    s_bundle = (cairn_bundle_t *)heap_caps_malloc(sizeof(cairn_bundle_t),
                                                  MALLOC_CAP_SPIRAM);
    s_scratch = (uint8_t *)heap_caps_malloc(CAIRN_MANIFEST_ENCODED_MAX,
                                            MALLOC_CAP_SPIRAM);
    if (s_bundle == nullptr || s_scratch == nullptr) {
        CAIRN_LOGE(TAG, "no PSRAM for the %u-byte bundle reader and %u-byte "
                        "scratch; not uploading",
                   (unsigned)sizeof(cairn_bundle_t),
                   (unsigned)CAIRN_MANIFEST_ENCODED_MAX);
        heap_caps_free(s_bundle);  s_bundle  = nullptr;
        heap_caps_free(s_scratch); s_scratch = nullptr;
        return false;
    }
    CAIRN_LOGI(TAG, "upload buffers in PSRAM: %u + %u bytes",
               (unsigned)sizeof(cairn_bundle_t),
               (unsigned)CAIRN_MANIFEST_ENCODED_MAX);
    return true;
}

static bool hex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char    c = hex[i];
        uint8_t v;
        if      (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
        else return false;
        if (i % 2 == 0) out[i / 2] = (uint8_t)(v << 4);
        else            out[i / 2] |= v;
    }
    return true;
}

void net_upload_pending(Client *client, const char *host, uint16_t port,
                        const char *label, uint32_t max_bundles,
                        bool (*should_abort)(void *), void *abort_ctx,
                        net_upload_result_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t t0 = millis();

    if (!ensure_bundle_reader()) return;

    cairn_http_init(&s_http, client, host, port);

    /*
     * Collect the names before touching any of them: a prune removes a
     * directory, and removing while iterating invalidates the iterator.
     */
    char     ids[16][28];
    uint32_t found = 0;

    cairn_dir_t *d = cairn_fs_opendir(CAIRN_DIR_BUNDLES);
    if (d == nullptr) {
        CAIRN_LOGW(TAG, "[%s] cannot list %s", label, CAIRN_DIR_BUNDLES);
        return;
    }
    char name[64];
    bool is_dir = false;
    while (found < (uint32_t)(sizeof(ids) / sizeof(ids[0])) &&
           cairn_fs_readdir(d, name, sizeof(name), &is_dir, nullptr)) {
        if (!is_dir || strlen(name) != 26) continue;   /* a ULID, not a stray */
        snprintf(ids[found], sizeof(ids[0]), "%s", name);
        found++;
    }
    cairn_fs_closedir(d);

    out->considered = found;
    if (found == 0) {
        CAIRN_LOGI(TAG, "[%s] nothing waiting on the card", label);
        return;
    }

    uint8_t pinned[32];
    const bool have_key = hex32(CAIRN_SERVER_RECEIPT_KEY_HEX, pinned);
    if (!have_key) {
        /* The correct failure direction: hand bundles over, prune nothing, fill
         * the card rather than lose a trip. */
        CAIRN_LOGE(TAG, "[%s] the pinned receipt key is not 64 hex characters; "
                        "bundles will be delivered but never pruned", label);
    }

    CAIRN_LOGI(TAG, "[%s] %u bundle(s) waiting; sending up to %u", label,
               (unsigned)found, (unsigned)max_bundles);

    uint32_t sent = 0;
    for (uint32_t i = 0; i < found && sent < max_bundles; i++) {
        if (should_abort != nullptr && should_abort(abort_ctx)) {
            CAIRN_LOGI(TAG, "[%s] abort ordered; stopping with %u delivered",
                       label, (unsigned)out->delivered);
            break;
        }

        char dir[CAIRN_BUNDLE_DIR_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, ids[i]);

        if (!cairn_bundle_open(s_bundle, dir, s_scratch, CAIRN_MANIFEST_ENCODED_MAX)) {
            /* Not the network's fault and it cost no air time, so it does not
             * spend the budget. */
            out->unreadable++;
            continue;
        }

        cairn_intake_http_t   http;
        cairn_intake_bundle_t src;
        cairn_http_as_intake(&s_http, &http);
        cairn_bundle_as_intake_source(s_bundle, &src);

        size_t               rlen = 0;
        cairn_intake_stats_t st;
        cairn_intake_outcome_t r = cairn_intake_deliver(
            &http, &src, s_bundle->m.chunks, s_bundle->m.chunk_count,
            s_scratch, CAIRN_MANIFEST_ENCODED_MAX, should_abort, abort_ctx,
            s_receipt, sizeof(s_receipt), &rlen, &st);

        out->bytes_up   += st.bytes_up;
        out->bytes_down += st.bytes_down;
        sent++;

        if (r == CAIRN_INTAKE_RECEIPT) {
            /*
             * A receipt in hand is not permission to delete. The gate checks it
             * against the pinned key AND this bundle's content root, so a
             * genuine receipt for different content prunes nothing — which is
             * what stops a misconfigured or hostile server inducing the
             * deletion of data it never received.
             */
            cairn_prune_result_t pr = CAIRN_PRUNE_NO_PINNED_KEY;
            if (have_key) {
                pr = cairn_prune_if_receipted(ids[i], s_receipt, rlen, pinned,
                                              s_bundle->m.content_root);
            }

            if (pr == CAIRN_PRUNE_OK) {
                out->delivered++;
                CAIRN_LOGI(TAG, "[%s] %s: delivered and pruned (%u up, %u chunk(s) "
                                "sent, %u already held, %u ms)",
                           label, ids[i], (unsigned)st.bytes_up,
                           (unsigned)st.chunks_sent, (unsigned)st.chunks_skipped,
                           (unsigned)(millis() - t0));
            } else {
                out->retained++;
                CAIRN_LOGW(TAG, "[%s] %s: receipted but NOT pruned: %s", label,
                           ids[i], cairn_prune_result_name(pr));
            }
        } else if (r == CAIRN_INTAKE_REFUSED) {
            out->refused++;
            CAIRN_LOGE(TAG, "[%s] %s: the server will never accept these bytes "
                            "(HTTP %d); it stays on the card",
                       label, ids[i], st.last_status);
        } else if (r == CAIRN_INTAKE_ABORTED) {
            cairn_bundle_close(s_bundle);
            break;
        } else {
            out->failed++;
            CAIRN_LOGW(TAG, "[%s] %s: %s (HTTP %d)", label, ids[i],
                       cairn_intake_outcome_name(r), st.last_status);
        }

        cairn_bundle_close(s_bundle);
    }

    cairn_http_disconnect(&s_http);
    out->ms = millis() - t0;

    CAIRN_LOGI(TAG, "[%s] session: %u delivered, %u retained, %u refused, "
                    "%u unreadable, %u failed, %u up, %u down, %u ms",
               label, (unsigned)out->delivered, (unsigned)out->retained,
               (unsigned)out->refused, (unsigned)out->unreadable,
               (unsigned)out->failed, (unsigned)out->bytes_up,
               (unsigned)out->bytes_down, (unsigned)out->ms);
}

#endif /* CAIRN_WIFI_UPLINK || CAIRN_LTE_UPLINK */
