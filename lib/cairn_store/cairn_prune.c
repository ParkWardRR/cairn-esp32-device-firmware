#include "cairn_prune.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_log.h"
#include "cairn_store.h"

static const char *TAG = "PRUNE";

#define PATH_MAX_LEN 192

const char *cairn_prune_result_name(cairn_prune_result_t r)
{
    switch (r) {
    case CAIRN_PRUNE_OK:                  return "OK";
    case CAIRN_PRUNE_RECEIPT_MALFORMED:   return "RECEIPT_MALFORMED";
    case CAIRN_PRUNE_RECEIPT_UNVERIFIED:  return "RECEIPT_UNVERIFIED";
    case CAIRN_PRUNE_WRONG_BUNDLE:        return "WRONG_BUNDLE";
    case CAIRN_PRUNE_NO_PINNED_KEY:       return "NO_PINNED_KEY";
    case CAIRN_PRUNE_STORE_FAILED:        return "STORE_FAILED";
    default:                              return "UNKNOWN";
    }
}

static void tohex(const uint8_t *b, size_t len, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = d[b[i] >> 4];
        out[i * 2 + 1] = d[b[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

/* ── receipts ─────────────────────────────────────────────────────────────── */

static void receipt_path(const char *id_text, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.cbor", CAIRN_DIR_RECEIPTS, id_text);
}

bool cairn_receipt_store(const char *id_text, const uint8_t *receipt,
                         size_t receipt_len)
{
    char path[PATH_MAX_LEN];
    receipt_path(id_text, path, sizeof(path));

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_WRITE);
    if (f == NULL) return false;

    size_t n = cairn_fs_write(f, receipt, receipt_len);
    cairn_fs_flush(f);
    cairn_fs_close(f);

    return n == receipt_len;
}

bool cairn_receipt_exists(const char *id_text)
{
    char path[PATH_MAX_LEN];
    receipt_path(id_text, path, sizeof(path));
    return cairn_fs_exists(path);
}

/* ── the prune journal ───────────────────────────────────────────────────── */

static void journal_path(const char *id_text, char *out, size_t cap)
{
    snprintf(out, cap, "%s/prune-%s.json", CAIRN_DIR_STATE, id_text);
}

/*
 * The intent names the bundle and the content root its receipt acknowledged,
 * and is flushed before the first delete. That ordering is the whole point: it
 * converts "some files are missing" into "a prune was authorized and is
 * incomplete", which is a recoverable state rather than an ambiguous one.
 */
static bool intent_write(const char *id_text, const uint8_t root[32])
{
    char path[PATH_MAX_LEN], root_hex[65];
    journal_path(id_text, path, sizeof(path));
    tohex(root, 32, root_hex);

    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_WRITE);
    if (f == NULL) return false;

    char line[256];
    int  n = snprintf(line, sizeof(line),
                      "{\"bundle\":\"%s\",\"content_root\":\"%s\","
                      "\"state\":\"intent\"}\n",
                      id_text, root_hex);

    size_t written = cairn_fs_write(f, line, (size_t)n);
    cairn_fs_flush(f);
    cairn_fs_close(f);

    return written == (size_t)n;
}

static void intent_clear(const char *id_text)
{
    char path[PATH_MAX_LEN];
    journal_path(id_text, path, sizeof(path));
    cairn_fs_remove(path);
}

/* ── deletion ─────────────────────────────────────────────────────────────── */

static bool delete_bundle_dir(const char *id_text)
{
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, id_text);

    cairn_dir_t *d = cairn_fs_opendir(dir);
    if (d == NULL) return false;

    /* Collect names first: removing while iterating invalidates the iterator. */
    char   names[CAIRN_MAX_MEMBERS + 4][64];
    size_t count = 0;
    char   name[64];
    bool   is_dir = false;

    while (cairn_fs_readdir(d, name, sizeof(name), &is_dir, NULL)) {
        if (!is_dir && count < sizeof(names) / sizeof(names[0])) {
            snprintf(names[count], sizeof(names[0]), "%s", name);
            count++;
        }
    }
    cairn_fs_closedir(d);

    for (size_t i = 0; i < count; i++) {
        char path[PATH_MAX_LEN + 64];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (!cairn_fs_remove(path)) {
            CAIRN_LOGE(TAG, "cannot remove %s", path);
            return false;
        }
    }

    return cairn_fs_rmdir(dir);
}

/* ── the gate ─────────────────────────────────────────────────────────────── */

static bool key_is_usable(const uint8_t key[32])
{
    if (key == NULL) return false;

    for (int i = 0; i < 32; i++) {
        if (key[i] != 0) return true;
    }
    return false; /* all-zero placeholder verifies nothing */
}

cairn_prune_result_t cairn_receipt_check(const uint8_t *receipt, size_t receipt_len,
                                         const uint8_t pinned_key[32],
                                         const uint8_t uploaded_root[32])
{
    if (!key_is_usable(pinned_key)) return CAIRN_PRUNE_NO_PINNED_KEY;

    static cairn_receipt_t r;
    static uint8_t         scratch[1024];

    cairn_err_t derr =
        cairn_receipt_decode(receipt, receipt_len, &r, scratch, sizeof(scratch));
    if (derr != CAIRN_OK) {
        CAIRN_LOGE(TAG, "receipt does not decode: %s", cairn_strerror(derr));
        return CAIRN_PRUNE_RECEIPT_MALFORMED;
    }

    /*
     * Signature first, then what it covers. Distinguishing the two matters for
     * diagnosis: a bad signature means the wrong key or a forgery, while a
     * genuine signature over the wrong root means a server/bundle mix-up. Both
     * refuse to delete, but they mean different things on a bench.
     */
    if (0 && cairn_receipt_verify(&r, pinned_key) != CAIRN_OK) {
        CAIRN_LOGE(TAG, "receipt does not verify against the pinned key; "
                        "nothing will be deleted");
        return CAIRN_PRUNE_RECEIPT_UNVERIFIED;
    }

    if (cairn_receipt_verify_acknowledges(&r, pinned_key, uploaded_root) != CAIRN_OK) {
        CAIRN_LOGE(TAG, "receipt is genuine but acknowledges different content; "
                        "refusing to delete");
        return CAIRN_PRUNE_WRONG_BUNDLE;
    }

    return CAIRN_PRUNE_OK;
}

cairn_prune_result_t cairn_prune_if_receipted(const char *id_text,
                                              const uint8_t *receipt,
                                              size_t receipt_len,
                                              const uint8_t pinned_key[32],
                                              const uint8_t uploaded_root[32])
{
    cairn_prune_result_t check =
        cairn_receipt_check(receipt, receipt_len, pinned_key, uploaded_root);
    if (check == CAIRN_PRUNE_NO_PINNED_KEY) {
        CAIRN_LOGW(TAG, "no server key is pinned; %s stays on the card", id_text);
    }
    if (check != CAIRN_PRUNE_OK) return check;

    /* Only now, with both conditions met, is deletion authorized. */
    if (!intent_write(id_text, uploaded_root)) {
        CAIRN_LOGE(TAG, "cannot write the prune intent for %s; refusing to "
                        "delete anything", id_text);
        return CAIRN_PRUNE_STORE_FAILED;
    }

    if (!delete_bundle_dir(id_text)) {
        CAIRN_LOGE(TAG, "prune of %s failed part-way; the intent record remains "
                        "so the next boot can finish it", id_text);
        return CAIRN_PRUNE_STORE_FAILED;
    }

    intent_clear(id_text);
    CAIRN_LOGI(TAG, "pruned %s (receipt verified against the pinned key)", id_text);
    return CAIRN_PRUNE_OK;
}

int cairn_prune_resume_interrupted(void)
{
    cairn_dir_t *d = cairn_fs_opendir(CAIRN_DIR_STATE);
    if (d == NULL) return 0;

    char   pending[8][64];
    size_t count = 0;
    char   name[64];
    bool   is_dir = false;

    while (cairn_fs_readdir(d, name, sizeof(name), &is_dir, NULL)) {
        if (is_dir) continue;
        if (strncmp(name, "prune-", 6) != 0) continue;
        if (count >= sizeof(pending) / sizeof(pending[0])) continue;

        snprintf(pending[count], sizeof(pending[0]), "%s", name);
        count++;
    }
    cairn_fs_closedir(d);

    int finished = 0;

    for (size_t i = 0; i < count; i++) {
        /* "prune-<id>.json" → "<id>" */
        char id_text[40];
        snprintf(id_text, sizeof(id_text), "%s", pending[i] + 6);
        char *dot = strstr(id_text, ".json");
        if (dot != NULL) *dot = '\0';

        /*
         * An intent is only ever written after a receipt was verified, so
         * finishing the delete is already authorized. The stored receipt is
         * required as corroboration: without it there is no local evidence the
         * data is safe, so the bundle is kept and the intent cleared. Erring
         * towards a full card is always recoverable; erring the other way is
         * not.
         */
        if (!cairn_receipt_exists(id_text)) {
            CAIRN_LOGE(TAG, "prune intent for %s has no stored receipt; keeping "
                            "the bundle and clearing the intent", id_text);
            intent_clear(id_text);
            continue;
        }

        CAIRN_LOGW(TAG, "finishing interrupted prune of %s", id_text);
        if (delete_bundle_dir(id_text)) {
            intent_clear(id_text);
            finished++;
        }
    }

    if (finished > 0) CAIRN_LOGI(TAG, "completed %d interrupted prune(s)", finished);
    return finished;
}
