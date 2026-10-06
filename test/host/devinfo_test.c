/*
 * DEVICE_INFO and UPLINK_EVENT against the contract's own vectors (contracts/ble/v1/
 * vectors/device-info/vectors.json, contracts-v0.2.0).
 *
 * Both directions are checked: the struct the vector's "decoded" block describes must encode
 * to exactly the vector's bytes, and the vector's bytes must decode to that struct. The
 * malformed values must be refused, the unknown record type skipped. Synthetic rows add what
 * a fixed vector set cannot: the 512-byte cut at its exact edge, every truncation, and
 * every single-byte mutation of a good value (each must either decode to something valid or
 * be refused, never read out of bounds, which ASan checks).
 *
 *   devinfo_test <path to vectors.json>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_devinfo.h"
#include "minijson.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [devinfo] %s\n", name); }        \
        else      { g_fail++; printf("  FAIL  [devinfo] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

static int hex_decode(const char *hex, uint8_t *out, size_t max)
{
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned b;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1) return -1;
        out[i] = (uint8_t)b;
    }
    return (int)(n / 2);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ── building the expected struct from a vector's "decoded" block ─────────── */

static void bytes_from(const mj_doc_t *d, const mj_node_t *arr, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)mj_num_or(mj_at(d, arr, i), 0);
}

static uint32_t unum(const mj_doc_t *d, const mj_node_t *o, const char *k)
{
    return (uint32_t)mj_num_or(mj_get(d, o, k), 0);
}

static void build_expected(const mj_doc_t *d, const mj_node_t *dec, cairn_devinfo_t *e)
{
    memset(e, 0, sizeof *e);
    e->version = (uint8_t)unum(d, dec, "Version");
    e->minor = (uint8_t)unum(d, dec, "Minor");
    e->capabilities = unum(d, dec, "Capabilities");
    e->truncated = mj_bool_or(mj_get(d, dec, "Truncated"), false);

    const mj_node_t *fw = mj_get(d, dec, "Firmware");
    if (fw && !mj_is_null(fw)) {
        e->has_firmware = true;
        e->firmware.major = (uint8_t)unum(d, fw, "Major");
        e->firmware.minor = (uint8_t)unum(d, fw, "Minor");
        e->firmware.patch = (uint8_t)unum(d, fw, "Patch");
        e->firmware.flags = (uint8_t)unum(d, fw, "Flags");
        bytes_from(d, mj_get(d, fw, "Commit"), e->firmware.commit, 8);
        e->firmware.build_unix = unum(d, fw, "BuildUnix");
    }
    const mj_node_t *id = mj_get(d, dec, "Identity");
    if (id && !mj_is_null(id)) {
        e->has_identity = true;
        bytes_from(d, mj_get(d, id, "DeviceID"), e->identity.device_id, 16);
        bytes_from(d, mj_get(d, id, "Fingerprint"), e->identity.fingerprint, 4);
        e->identity.enrol_state = (uint8_t)unum(d, id, "EnrolState");
        e->identity.storage_key_version = unum(d, id, "StorageKeyVersion");
    }
    const mj_node_t *st = mj_get(d, dec, "Storage");
    if (st && !mj_is_null(st)) {
        e->has_storage = true;
        e->storage.state = (uint8_t)unum(d, st, "State");
        e->storage.pending_bundles = (uint16_t)unum(d, st, "PendingBundles");
        e->storage.free_mib = unum(d, st, "FreeMiB");
    }
    const mj_node_t *tr = mj_get(d, dec, "Transports");
    if (tr && !mj_is_null(tr)) {
        for (size_t i = 0; i < mj_len(d, tr) && e->n_transports < CAIRN_DI_MAX_TRANSPORTS; i++) {
            const mj_node_t *t = mj_at(d, tr, i);
            e->transports[e->n_transports].kind = (uint8_t)unum(d, t, "Kind");
            e->transports[e->n_transports].state = (uint8_t)unum(d, t, "State");
            e->transports[e->n_transports].last_error = (uint16_t)unum(d, t, "LastError");
            e->n_transports++;
        }
    }
    const mj_node_t *en = mj_get(d, dec, "Engines");
    if (en && !mj_is_null(en)) {
        for (size_t i = 0; i < mj_len(d, en) && e->n_engines < CAIRN_DI_MAX_ENGINES; i++) {
            const mj_node_t *x = mj_at(d, en, i);
            cairn_di_engine_t *o = &e->engines[e->n_engines++];
            o->profile_version = (uint16_t)unum(d, x, "ProfileVersion");
            bytes_from(d, mj_get(d, x, "Hash"), o->hash, 8);
            const char *s = mj_str_or(mj_get(d, x, "ID"), "");
            o->id_len = (uint8_t)strlen(s);
            memcpy(o->id, s, o->id_len);
        }
    }
    const mj_node_t *bt = mj_get(d, dec, "Boot");
    if (bt && !mj_is_null(bt)) {
        e->has_boot = true;
        e->boot.to_ble_ms = unum(d, bt, "ToBLEms");
        e->boot.to_ready_ms = unum(d, bt, "ToReadyMs");
        e->boot.to_first_fix_ms = unum(d, bt, "ToFirstFixMs");
        e->boot.reset_reason = (uint8_t)unum(d, bt, "ResetReason");
    }
    const mj_node_t *un = mj_get(d, dec, "Unknown");
    if (un && !mj_is_null(un)) e->unknown_records = (uint8_t)mj_len(d, un);
}

static bool same(const cairn_devinfo_t *a, const cairn_devinfo_t *b)
{
    if (a->version != b->version || a->minor != b->minor || a->capabilities != b->capabilities) return false;
    if (a->truncated != b->truncated || a->unknown_records != b->unknown_records) return false;
    if (a->has_firmware != b->has_firmware || a->has_identity != b->has_identity ||
        a->has_storage != b->has_storage || a->has_boot != b->has_boot) return false;
    if (a->has_firmware && memcmp(&a->firmware, &b->firmware, sizeof a->firmware) != 0) return false;
    if (a->has_identity && memcmp(&a->identity, &b->identity, sizeof a->identity) != 0) return false;
    if (a->has_storage && memcmp(&a->storage, &b->storage, sizeof a->storage) != 0) return false;
    if (a->has_boot && memcmp(&a->boot, &b->boot, sizeof a->boot) != 0) return false;
    if (a->n_transports != b->n_transports || a->n_engines != b->n_engines) return false;
    for (uint8_t i = 0; i < a->n_transports; i++)
        if (memcmp(&a->transports[i], &b->transports[i], sizeof a->transports[i]) != 0) return false;
    for (uint8_t i = 0; i < a->n_engines; i++) {
        const cairn_di_engine_t *x = &a->engines[i], *y = &b->engines[i];
        if (x->profile_version != y->profile_version || x->id_len != y->id_len ||
            memcmp(x->hash, y->hash, 8) != 0 || memcmp(x->id, y->id, x->id_len) != 0) return false;
    }
    return true;
}

/* ── the vectors ──────────────────────────────────────────────────────────── */

static void test_device_info_vectors(const mj_doc_t *d, const mj_node_t *list)
{
    int accepted = 0, rejected = 0;
    for (size_t i = 0; i < mj_len(d, list); i++) {
        const mj_node_t *v = mj_at(d, list, i);
        const char *name = mj_str_or(mj_get(d, v, "name"), "?");
        const char *hex = mj_str_or(mj_get(d, v, "hex"), "");
        bool must_reject = mj_bool_or(mj_get(d, v, "must_be_rejected"), false);

        uint8_t bytes[1024];
        int n = hex_decode(hex, bytes, sizeof bytes);
        char label[200];

        if (must_reject) {
            cairn_devinfo_t got;
            snprintf(label, sizeof label, "refused: %s", name);
            CHECK(label, n >= 0 && cairn_devinfo_decode(&got, bytes, (size_t)n) != CAIRN_DI_OK);
            rejected++;
            continue;
        }

        cairn_devinfo_t want, got;
        build_expected(d, mj_get(d, v, "decoded"), &want);

        snprintf(label, sizeof label, "decodes to the vector's fields: %s", name);
        cairn_di_err_t e = (n >= 0) ? cairn_devinfo_decode(&got, bytes, (size_t)n) : CAIRN_DI_ERR_SHORT;
        CHECK(label, e == CAIRN_DI_OK && same(&got, &want));

        /* The encoder never writes a minor it does not know or a record it skips, so a vector
         * from a newer minor (with unknown records) is read-only. */
        if (want.minor == CAIRN_DI_MINOR && want.unknown_records == 0) {
            uint8_t out[CAIRN_DI_MAX_LEN];
            size_t m = cairn_devinfo_encode(&want, out, sizeof out);
            snprintf(label, sizeof label, "encodes to the vector's bytes: %s", name);
            CHECK(label, (int)m == n && memcmp(out, bytes, m) == 0);
        }
        accepted++;
    }
    CHECK("the vector file has accepted and rejected values", accepted >= 4 && rejected >= 3);
}

static void test_uplink_event_vectors(const mj_doc_t *d, const mj_node_t *list)
{
    int good = 0;
    for (size_t i = 0; i < mj_len(d, list); i++) {
        const mj_node_t *v = mj_at(d, list, i);
        const char *name = mj_str_or(mj_get(d, v, "name"), "?");
        const char *note = mj_str_or(mj_get(d, v, "note"), "");
        uint8_t bytes[64];
        int n = hex_decode(mj_str_or(mj_get(d, v, "hex"), ""), bytes, sizeof bytes);
        char label[200];
        cairn_uplink_event_t e;

        if (strstr(note, "must be rejected")) {
            snprintf(label, sizeof label, "refused: %s", name);
            CHECK(label, n >= 0 && !cairn_uplink_event_decode(&e, bytes, (size_t)n));
            continue;
        }
        snprintf(label, sizeof label, "decodes and re-encodes to the same 24 bytes: %s", name);
        uint8_t out[CAIRN_UE_LEN];
        CHECK(label, n == CAIRN_UE_LEN && cairn_uplink_event_decode(&e, bytes, (size_t)n) &&
                     cairn_uplink_event_encode(&e, out, sizeof out) == CAIRN_UE_LEN &&
                     memcmp(out, bytes, CAIRN_UE_LEN) == 0);
        good++;
    }
    CHECK("the vector file has uplink events", good >= 3);
}

/* The first vector's named facts, so a transcription error in the runner cannot hide. */
static void test_known_facts(const uint8_t *full, int n)
{
    cairn_devinfo_t i;
    CHECK("the full vector decodes", cairn_devinfo_decode(&i, full, (size_t)n) == CAIRN_DI_OK);
    CHECK("two engines, bmw-n20 then bmw-b58, in the dongle's order",
          i.n_engines == 2 && strcmp(i.engines[0].id, "bmw-n20") == 0 && strcmp(i.engines[1].id, "bmw-b58") == 0);
    CHECK("three transports: BLE, Wi-Fi, LTE", i.n_transports == 3 && i.transports[0].kind == 1 &&
                                               i.transports[1].kind == 2 && i.transports[2].kind == 3);
    CHECK("boot to BLE 820 ms, no fix yet", i.boot.to_ble_ms == 820 && i.boot.to_first_fix_ms == CAIRN_DI_UNKNOWN_MS);
    CHECK("capabilities 0xFF", i.capabilities == 0xFF);
}

/* ── synthetic rows ───────────────────────────────────────────────────────── */

static void fill_engine(cairn_di_engine_t *e, int i, int id_len)
{
    memset(e, 0, sizeof *e);
    e->profile_version = (uint16_t)(1 + i);
    for (int k = 0; k < 8; k++) e->hash[k] = (uint8_t)(i * 8 + k);
    e->id_len = (uint8_t)id_len;
    for (int k = 0; k < id_len; k++) e->id[k] = (char)('a' + (i + k) % 26);
}

static void test_cut_at_512(void)
{
    cairn_devinfo_t in;
    memset(&in, 0, sizeof in);
    in.capabilities = CAIRN_CAP_DEVICE_INFO | CAIRN_CAP_OFFLOAD;
    in.has_firmware = in.has_identity = in.has_storage = in.has_boot = true;
    in.n_transports = 3;
    for (int k = 0; k < 3; k++) { in.transports[k].kind = (uint8_t)(k + 1); in.transports[k].state = 31; }
    in.n_engines = 30;
    for (int i = 0; i < 30; i++) fill_engine(&in.engines[i], i, 32);   /* 45 bytes each */

    uint8_t out[CAIRN_DI_MAX_LEN + 64];
    size_t n = cairn_devinfo_encode(&in, out, sizeof out);
    CHECK("thirty long engine ids are cut to fit, never past 512", n > 0 && n <= CAIRN_DI_MAX_LEN);
    CHECK("total_len says so", n > 4 && (out[2] | (out[3] << 8)) == (int)n);

    cairn_devinfo_t back;
    CHECK("it decodes, with the truncated marker", cairn_devinfo_decode(&back, out, n) == CAIRN_DI_OK && back.truncated);
    CHECK("everything but engines survived", back.has_firmware && back.has_identity && back.has_storage &&
                                              back.has_boot && back.n_transports == 3);
    CHECK("the kept engines are the first ones, in order, intact", back.n_engines > 0 && back.n_engines < 30 &&
          ({ bool ok = true; for (uint8_t i = 0; i < back.n_engines; i++) ok = ok && back.engines[i].profile_version == (uint16_t)(1 + i) &&
                                                 memcmp(back.engines[i].id, in.engines[i].id, 32) == 0; ok; }));

    /* The edge: one more engine would not have fit. */
    size_t kept = back.n_engines;
    cairn_devinfo_t one_more = in;
    one_more.n_engines = (uint8_t)(kept + 1);
    size_t m = cairn_devinfo_encode(&one_more, out, sizeof out);
    cairn_devinfo_t chk;
    CHECK("the cut is maximal: a list of kept+1 is still cut back to kept",
          m > 0 && cairn_devinfo_decode(&chk, out, m) == CAIRN_DI_OK && chk.n_engines == kept && chk.truncated);

    /* And a list that fits exactly is not marked. */
    cairn_devinfo_t fits = in;
    fits.n_engines = (uint8_t)kept;
    fits.truncated = false;
    size_t f = cairn_devinfo_encode(&fits, out, sizeof out);
    CHECK("kept engines without a marker still fit", f > 0 && f <= CAIRN_DI_MAX_LEN);
    CHECK("and carry no marker", cairn_devinfo_decode(&chk, out, f) == CAIRN_DI_OK && !chk.truncated && chk.n_engines == kept);
}

static void test_encoder_refusals(void)
{
    cairn_devinfo_t in;
    uint8_t out[CAIRN_DI_MAX_LEN];
    memset(&in, 0, sizeof in);
    in.capabilities = CAIRN_CAP_DEVICE_INFO;

    CHECK("the minimal value is the 8-byte header", cairn_devinfo_encode(&in, out, sizeof out) == 8 && out[0] == 1 &&
                                                    out[1] == 0 && out[2] == 8 && out[3] == 0);
    CHECK("a short buffer is refused", cairn_devinfo_encode(&in, out, 7) == 0);

    in.capabilities = 1u << 11;
    CHECK("a reserved capability bit is refused", cairn_devinfo_encode(&in, out, sizeof out) == 0);
    in.capabilities = 1u << 31;
    CHECK("including the top bit", cairn_devinfo_encode(&in, out, sizeof out) == 0);
    in.capabilities = 0;

    in.n_engines = 1;
    fill_engine(&in.engines[0], 0, 7);
    in.engines[0].id_len = 0;
    CHECK("an empty engine id is refused", cairn_devinfo_encode(&in, out, sizeof out) == 0);
    in.engines[0].id_len = 33;
    CHECK("a 33-byte engine id is refused", cairn_devinfo_encode(&in, out, sizeof out) == 0);
    fill_engine(&in.engines[0], 0, 7);
    in.engines[0].id[3] = ' ';
    CHECK("a space in an engine id is refused", cairn_devinfo_encode(&in, out, sizeof out) == 0);
    in.engines[0].id[3] = (char)0xC3;
    CHECK("a non-ASCII engine id is refused", cairn_devinfo_encode(&in, out, sizeof out) == 0);
    fill_engine(&in.engines[0], 0, 7);
    in.n_transports = CAIRN_DI_MAX_TRANSPORTS + 1;
    CHECK("too many transports is refused", cairn_devinfo_encode(&in, out, sizeof out) == 0);
}

static void test_decoder_rules(void)
{
    cairn_devinfo_t good, got;
    uint8_t buf[CAIRN_DI_MAX_LEN], bad[CAIRN_DI_MAX_LEN];
    memset(&good, 0, sizeof good);
    good.version = CAIRN_DI_VERSION;   /* what a decode reports */
    good.capabilities = CAIRN_CAP_DEVICE_INFO;
    good.has_storage = true;
    good.storage.state = 1;
    good.n_engines = 1;
    fill_engine(&good.engines[0], 0, 7);
    size_t n = cairn_devinfo_encode(&good, buf, sizeof buf);
    CHECK("a small value round trips", n > 0 && cairn_devinfo_decode(&got, buf, n) == CAIRN_DI_OK && same(&got, &good));

    /* a known type repeated that must not repeat */
    memcpy(bad, buf, n);
    memcpy(bad + n, buf + 8, 10);     /* a second storage record */
    bad[2] = (uint8_t)(n + 10);
    CHECK("a second storage record is refused", cairn_devinfo_decode(&got, bad, n + 10) == CAIRN_DI_ERR_RECORD);

    /* known types out of order: engine (5) before storage (3) */
    uint8_t ooo[64];
    size_t k = 0;
    ooo[k++] = 1; ooo[k++] = 0; ooo[k++] = 0; ooo[k++] = 0; ooo[k++] = 8; ooo[k++] = 0; ooo[k++] = 0; ooo[k++] = 0;
    ooo[k++] = CAIRN_DI_REC_BOOT; ooo[k++] = 16; for (int i = 0; i < 16; i++) ooo[k++] = 0;
    ooo[k++] = CAIRN_DI_REC_STORAGE; ooo[k++] = 8; for (int i = 0; i < 8; i++) ooo[k++] = 0;
    ooo[2] = (uint8_t)k;
    CHECK("records out of ascending order are refused", cairn_devinfo_decode(&got, ooo, k) == CAIRN_DI_ERR_RECORD);

    /* an unknown record, anywhere, is skipped by its length */
    uint8_t unk[32];
    k = 0;
    unk[k++] = 1; unk[k++] = 0; unk[k++] = 0; unk[k++] = 0; unk[k++] = 8; unk[k++] = 0; unk[k++] = 0; unk[k++] = 0;
    unk[k++] = 0x30; unk[k++] = 3; unk[k++] = 9; unk[k++] = 9; unk[k++] = 9;
    unk[k++] = CAIRN_DI_REC_STORAGE; unk[k++] = 8; for (int i = 0; i < 8; i++) unk[k++] = (uint8_t)(i == 0);
    unk[2] = (uint8_t)k;
    CHECK("an unknown record type is skipped and the next one read",
          cairn_devinfo_decode(&got, unk, k) == CAIRN_DI_OK && got.has_storage && got.storage.state == 1 &&
          got.unknown_records == 1);

    /* an unknown record that runs past the end */
    unk[9] = 200;                      /* the unknown record's length byte */
    CHECK("an unknown record whose length runs off the end is refused",
          cairn_devinfo_decode(&got, unk, k) == CAIRN_DI_ERR_SHORT);

    /* a minor version the decoder has not seen is accepted */
    memcpy(bad, buf, n);
    bad[1] = 9;
    CHECK("any minor of a known major is accepted", cairn_devinfo_decode(&got, bad, n) == CAIRN_DI_OK && got.minor == 9);

    /* A known record with the wrong length is refused, for every known type and both
     * directions (one byte short, one byte long). */
    static const struct { uint8_t type; uint8_t len; const char *name; } known[] = {
        { CAIRN_DI_REC_FIRMWARE, 16, "firmware" }, { CAIRN_DI_REC_IDENTITY, 25, "identity" },
        { CAIRN_DI_REC_STORAGE, 8, "storage" },    { CAIRN_DI_REC_TRANSPORT, 4, "transport" },
        { CAIRN_DI_REC_BOOT, 16, "boot timing" },
    };
    for (unsigned t = 0; t < sizeof known / sizeof known[0]; t++) {
        for (int delta = -1; delta <= 1; delta += 2) {
            uint8_t rec[64] = { 1, 0, 0, 0, 0, 0, 0, 0 };
            size_t m = 8;
            uint8_t rl = (uint8_t)(known[t].len + delta);
            rec[m++] = known[t].type; rec[m++] = rl;
            for (uint8_t i = 0; i < rl; i++) rec[m++] = 0;
            rec[2] = (uint8_t)m;
            char label[96];
            snprintf(label, sizeof label, "a %s record %d byte(s) off its length is refused", known[t].name, delta);
            CHECK(label, cairn_devinfo_decode(&got, rec, m) == CAIRN_DI_ERR_RECORD);
        }
    }
    {
        /* engine: id_len says 3 but the record carries 4 id bytes, then 2 carried for id_len 3 */
        uint8_t rec[64] = { 1, 0, 0, 0, 0, 0, 0, 0, CAIRN_DI_REC_ENGINE, 0 };
        size_t m = 10;
        for (int i = 0; i < 10; i++) rec[m++] = 0;        /* version, hash */
        rec[m++] = 3;                                      /* id_len */
        rec[m++] = 'a'; rec[m++] = 'b'; rec[m++] = 'c'; rec[m++] = 'd';   /* four bytes */
        rec[9] = (uint8_t)(m - 10);
        rec[2] = (uint8_t)m;
        CHECK("an engine whose id_len disagrees with its record length is refused",
              cairn_devinfo_decode(&got, rec, m) == CAIRN_DI_ERR_RECORD);
        m = 10 + 10;
        rec[m++] = 0;                                      /* id_len 0 */
        rec[9] = (uint8_t)(m - 10);
        rec[2] = (uint8_t)m;
        CHECK("an engine with id_len 0 is refused", cairn_devinfo_decode(&got, rec, m) == CAIRN_DI_ERR_RECORD);
    }

    /* a 513-byte claim */
    memcpy(bad, buf, n);
    bad[2] = 0x01; bad[3] = 0x02;
    CHECK("a total_len above 512 is refused", cairn_devinfo_decode(&got, bad, n) == CAIRN_DI_ERR_LENGTH);

    /* a truncated marker with a body */
    uint8_t tr[16] = { 1, 0, 10, 0, 0, 0, 0, 0, CAIRN_DI_REC_TRUNCATED, 0 };
    CHECK("the truncated marker alone decodes", cairn_devinfo_decode(&got, tr, 10) == CAIRN_DI_OK && got.truncated);
    tr[9] = 1; tr[2] = 11; tr[10] = 0;
    CHECK("a truncated marker with a body is refused", cairn_devinfo_decode(&got, tr, 11) == CAIRN_DI_ERR_RECORD);

    /* every truncation and every single-byte change: never accepted wrongly, never out of bounds */
    int sloppy = 0;
    for (size_t cut = 0; cut < n; cut++)
        if (cairn_devinfo_decode(&got, buf, cut) == CAIRN_DI_OK) sloppy++;
    CHECK("every proper prefix of a value is refused", sloppy == 0);

    int crashes = 0;
    for (size_t i = 0; i < n; i++)
        for (int bit = 0; bit < 8; bit++) {
            memcpy(bad, buf, n);
            bad[i] ^= (uint8_t)(1 << bit);
            cairn_di_err_t e = cairn_devinfo_decode(&got, bad, n);
            if (e == CAIRN_DI_OK && got.n_engines > CAIRN_DI_MAX_ENGINES) crashes++;
        }
    CHECK("every single-bit change decodes or is refused, never out of range", crashes == 0);
}

static void test_uplink_event_rules(void)
{
    cairn_uplink_event_t e, back;
    uint8_t out[32];
    memset(&e, 0, sizeof e);
    e.kind = CAIRN_UE_RETURNED; e.path = CAIRN_UE_PATH_WIFI; e.reason = CAIRN_UE_REASON_HOME_PHONE;
    e.outcome = CAIRN_UE_OUTCOME_PARTIAL; e.slot = 70000; e.committed = 2; e.failed = 1;
    e.bytes_sent = 1u << 31; e.duration_ms = 61234;

    CHECK("an event is exactly 24 bytes", cairn_uplink_event_encode(&e, out, sizeof out) == 24);
    CHECK("and round trips", cairn_uplink_event_decode(&back, out, 24) && memcmp(&back, &e, sizeof e) == 0);
    CHECK("the reserved bytes are zero", out[14] == 0 && out[15] == 0);
    CHECK("a short buffer is refused", cairn_uplink_event_encode(&e, out, 23) == 0);

    cairn_uplink_event_t k = e;
    k.kind = 0;
    CHECK("kind 0 is refused when encoding", cairn_uplink_event_encode(&k, out, sizeof out) == 0);
    k.kind = 4;
    CHECK("kind 4 is refused when encoding", cairn_uplink_event_encode(&k, out, sizeof out) == 0);

    cairn_uplink_event_encode(&e, out, sizeof out);
    out[15] = 1;
    CHECK("a non-zero reserved field makes the frame invalid", !cairn_uplink_event_decode(&back, out, 24));
    out[15] = 0;
    CHECK("a 25-byte frame is refused", !cairn_uplink_event_decode(&back, out, 25));
    CHECK("a 23-byte frame is refused", !cairn_uplink_event_decode(&back, out, 23));
    out[0] = 0;
    CHECK("kind 0 is refused when decoding", !cairn_uplink_event_decode(&back, out, 24));
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: devinfo_test <vectors.json>\n"); return 2; }

    char *text = slurp(argv[1]);
    if (!text) { printf("  FAIL  [devinfo] cannot read %s\n", argv[1]); return 1; }
    mj_doc_t *doc = malloc(sizeof *doc);
    if (!doc || !mj_parse(doc, text)) {
        printf("  FAIL  [devinfo] cannot parse %s: %s\n", argv[1], doc ? doc->error : "out of memory");
        return 1;
    }

    const mj_node_t *root = mj_root(doc);
    const mj_node_t *di = mj_get(doc, root, "device_info");
    const mj_node_t *ue = mj_get(doc, root, "uplink_event");
    CHECK("the vector file has device_info and uplink_event", di && ue && mj_len(doc, di) >= 7 && mj_len(doc, ue) >= 4);
    if (di && ue) {
        test_device_info_vectors(doc, di);
        test_uplink_event_vectors(doc, ue);

        uint8_t full[CAIRN_DI_MAX_LEN];
        int n = hex_decode(mj_str_or(mj_get(doc, mj_at(doc, di, 0), "hex"), ""), full, sizeof full);
        if (n > 0) test_known_facts(full, n);
    }

    test_cut_at_512();
    test_encoder_refusals();
    test_decoder_rules();
    test_uplink_event_rules();

    free(doc);
    free(text);
    printf("device info: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
