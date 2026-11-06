/*
 * The retention card glue (cairn_prune_retention_with_policy): the decision
 * logic is tested in retention_test.c against fakes; this drives the same
 * policy through the real cairn_fs POSIX backend — real bundle directories,
 * real segment headers for the age, real receipt files, a real log.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* mkdtemp lives here on macOS; stdlib.h covers glibc */

#include "board_config.h"
#include "cairn_format.h"
#include "cairn_fs.h"
#include "cairn_prune.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [retention-card] %s\n", name); } \
        else      { g_fail++; printf("  FAIL  [retention-card] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* A sealed-looking bundle: a directory with one segment whose header carries
 * `counter`, and some ballast so sizes differ. */
static void make_bundle(const char *id, uint64_t counter, size_t ballast)
{
    char dir[160], path[200];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, id);
    cairn_fs_mkdir(dir);

    cairn_segment_header_t h;
    memset(&h, 0, sizeof(h));
    h.format_version = CAIRN_FORMAT_VERSION;
    h.device_counter = counter;

    uint8_t buf[CAIRN_SEGMENT_HEADER_SIZE];
    if (cairn_encode_segment_header(&h, buf, sizeof(buf)) != CAIRN_OK) {
        printf("  FAIL  [retention-card] cannot encode a header\n");
        exit(1);
    }

    snprintf(path, sizeof(path), "%s/seg-00000000.seg", dir);
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_WRITE);
    cairn_fs_write(f, buf, sizeof(buf));
    static const uint8_t zeros[256];
    for (size_t put = 0; put < ballast; put += sizeof(zeros))
        cairn_fs_write(f, zeros, sizeof(zeros));
    cairn_fs_close(f);
}

static void make_receipt(const char *id)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.cbor", CAIRN_DIR_RECEIPTS, id);
    cairn_file_t *f = cairn_fs_open(path, CAIRN_FS_WRITE);
    cairn_fs_write(f, "stub", 4);
    cairn_fs_close(f);
}

static bool bundle_exists(const char *id)
{
    char dir[160];
    snprintf(dir, sizeof(dir), "%s/%s", CAIRN_DIR_BUNDLES, id);
    return cairn_fs_exists(dir);
}

int main(void)
{
    char root[] = "/tmp/cairn-retention-XXXXXX";
    if (mkdtemp(root) == NULL) { printf("no tmpdir\n"); return 1; }
    cairn_fs_begin(root);
    cairn_fs_mkdir("/cairn");
    cairn_fs_mkdir(CAIRN_DIR_BUNDLES);
    cairn_fs_mkdir(CAIRN_DIR_RECEIPTS);

    /* Three bundles: the middle-aged one receipted, the oldest and newest not. */
    make_bundle("OLDUNRECEIPTED00000000000", 3, 4096);
    make_bundle("RECEIPTEDMIDDLE0000000000", 7, 4096);
    make_bundle("NEWUNRECEIPTED00000000000", 9, 4096);
    make_receipt("RECEIPTEDMIDDLE0000000000");

    cairn_retention_stats_t st;

    /* A healthy policy deletes nothing, and does not even need the listing. */
    cairn_retention_policy_t healthy = { 0, 0, true };
    CHECK("healthy run ok", cairn_prune_retention_with_policy(&healthy, &st));
    CHECK("healthy run deleted nothing",
          st.bytes_freed == 0 && bundle_exists("OLDUNRECEIPTED00000000000") &&
          bundle_exists("RECEIPTEDMIDDLE0000000000") &&
          bundle_exists("NEWUNRECEIPTED00000000000"));

    /*
     * Unquenchable pressure: low and high water above any real disk's free
     * space. The receipted bundle must go despite not being oldest; then the
     * unreceipted ones oldest-first. This proves the glue reads the counter
     * out of the real header and the receipt off the real card.
     */
    cairn_retention_policy_t drain = { UINT64_MAX, UINT64_MAX, false };
    CHECK("receipted-only run ok", cairn_prune_retention_with_policy(&drain, &st));
    CHECK("only the receipted bundle went",
          st.receipted_dropped == 1 && st.unreceipted_dropped == 0 &&
          !bundle_exists("RECEIPTEDMIDDLE0000000000") &&
          bundle_exists("OLDUNRECEIPTED00000000000") &&
          bundle_exists("NEWUNRECEIPTED00000000000"));
    CHECK("its receipt outlives it (format v3 section 2)",
          cairn_fs_exists(CAIRN_DIR_RECEIPTS "/RECEIPTEDMIDDLE0000000000.cbor"));
    CHECK("exhaustion is reported", st.still_under_low);

    drain.allow_unreceipted = true;
    CHECK("last-resort run ok", cairn_prune_retention_with_policy(&drain, &st));
    CHECK("both unreceipted went", st.unreceipted_dropped == 2 &&
          !bundle_exists("OLDUNRECEIPTED00000000000") &&
          !bundle_exists("NEWUNRECEIPTED00000000000"));
    CHECK("the drops were logged to the card",
          cairn_fs_exists("/cairn/retention.log"));

    /* The log names the oldest unreceipted bundle before the newer one. */
    cairn_file_t *f = cairn_fs_open("/cairn/retention.log", CAIRN_FS_READ);
    char log[1024] = {0};
    size_t n = f ? cairn_fs_read(f, log, sizeof(log) - 1) : 0;
    if (f) cairn_fs_close(f);
    const char *oldp = strstr(log, "OLDUNRECEIPTED");
    const char *newp = strstr(log, "NEWUNRECEIPTED");
    CHECK("log order is oldest-first", n > 0 && oldp && newp && oldp < newp);

    cairn_fs_end();
    printf("[retention-card] %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
