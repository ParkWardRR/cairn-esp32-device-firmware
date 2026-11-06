/*
 * Retention (lib/cairn_retention): the card deletes nothing until it must,
 * then receipted bundles oldest-first, then — owner's policy — unreceipted
 * oldest-first, each logged before the delete.
 */

#include <stdio.h>
#include <string.h>

#include "cairn_retention.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [retention] %s\n", name); }      \
        else      { g_fail++; printf("  FAIL  [retention] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

#define MIB (1024ull * 1024ull)

/* A fake card: a fixed set of bundles, free space that grows as they go. */
#define MAX_B 16
typedef struct {
    cairn_retention_entry_t bundle[MAX_B];
    int      n;
    uint64_t free_bytes;
    int      list_fails;      /* make list_sealed return -1 */
    int      delete_fails_id; /* index whose delete returns false, -1 none */
    char     dropped[MAX_B][27];
    int      drops;
    char     logged[MAX_B][27];
    int      logs;
} fake_card_t;

static uint64_t f_free(void *ctx) { return ((fake_card_t *)ctx)->free_bytes; }

static int f_list(void *ctx, cairn_retention_entry_t *out, int max)
{
    fake_card_t *c = ctx;
    if (c->list_fails) return -1;
    int n = c->n < max ? c->n : max;
    memcpy(out, c->bundle, (size_t)n * sizeof(*out));
    return c->n;
}

static bool f_delete(void *ctx, const cairn_retention_entry_t *e)
{
    fake_card_t *c = ctx;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->bundle[i].id, e->id) != 0) continue;
        if (c->delete_fails_id == i) return false;
        snprintf(c->dropped[c->drops++], 27, "%s", e->id);
        c->free_bytes += e->bytes;
        return true;
    }
    return false;
}

static void f_log(void *ctx, const cairn_retention_entry_t *e)
{
    fake_card_t *c = ctx;
    snprintf(c->logged[c->logs++], 27, "%s", e->id);
}

static void card_init(fake_card_t *c, uint64_t free_bytes)
{
    memset(c, 0, sizeof(*c));
    c->free_bytes = free_bytes;
    c->delete_fails_id = -1;
}

static void add(fake_card_t *c, const char *id, uint64_t counter,
                uint64_t bytes, bool receipted)
{
    cairn_retention_entry_t *e = &c->bundle[c->n++];
    snprintf(e->id, sizeof(e->id), "%s", id);
    e->counter = counter;
    e->bytes = bytes;
    e->receipted = receipted;
}

static const cairn_retention_policy_t POLICY = {
    .low_water_bytes = 64 * MIB,
    .high_water_bytes = 128 * MIB,
    .allow_unreceipted = true,
};

static cairn_retention_entry_t g_scratch[MAX_B];

static bool run(fake_card_t *c, const cairn_retention_policy_t *p,
                cairn_retention_stats_t *st)
{
    cairn_retention_io_t io = { f_free, f_list, f_delete, f_log, c };
    return cairn_retention_run(&io, p, g_scratch, MAX_B, st);
}

static void test_healthy_card_deletes_nothing(void)
{
    fake_card_t c; cairn_retention_stats_t st;
    card_init(&c, 64 * MIB); /* exactly at low water counts as healthy */
    add(&c, "A", 1, 10 * MIB, true);
    CHECK("run ok", run(&c, &POLICY, &st));
    CHECK("nothing dropped", c.drops == 0 && st.bytes_freed == 0);
    CHECK("not reported full", !st.still_under_low);
}

static void test_receipted_oldest_first_stops_at_high(void)
{
    fake_card_t c; cairn_retention_stats_t st;
    card_init(&c, 30 * MIB);
    /* Deliberately out of order; counter decides, not position or name. */
    add(&c, "NEWER", 7, 60 * MIB, true);
    add(&c, "OLDEST", 2, 50 * MIB, true);
    add(&c, "UNRECEIPTED", 1, 40 * MIB, false); /* oldest of all, but safe */
    CHECK("run ok", run(&c, &POLICY, &st));
    /* 30+50=80 < 128, so both receipted go: 80+60=140 >= 128. */
    CHECK("two receipted dropped", st.receipted_dropped == 2);
    CHECK("oldest receipted went first", strcmp(c.dropped[0], "OLDEST") == 0);
    CHECK("unreceipted untouched", st.unreceipted_dropped == 0 && c.n == 3 &&
          strcmp(c.dropped[1], "NEWER") == 0);
    CHECK("receipted drops logged", c.logs == 2);
}

static void test_unreceipted_last_resort_to_low_only(void)
{
    fake_card_t c; cairn_retention_stats_t st;
    card_init(&c, 10 * MIB);
    add(&c, "R", 5, 20 * MIB, true);
    add(&c, "U-OLD", 1, 40 * MIB, false);
    add(&c, "U-NEW", 9, 40 * MIB, false);
    CHECK("run ok", run(&c, &POLICY, &st));
    /* 10+20=30 under high AND low; then U-OLD: 70 >= 64 low -> stop. */
    CHECK("one receipted then one unreceipted",
          st.receipted_dropped == 1 && st.unreceipted_dropped == 1);
    CHECK("the old unreceipted went, the new survived",
          strcmp(c.dropped[1], "U-OLD") == 0 && c.drops == 2);
    CHECK("stops at LOW water, not high", c.free_bytes == 70 * MIB);
    CHECK("unreceipted drop was logged", c.logs == 2);
}

static void test_unreceipted_refused_when_disallowed(void)
{
    fake_card_t c; cairn_retention_stats_t st;
    card_init(&c, 10 * MIB);
    add(&c, "U", 1, 100 * MIB, false);
    cairn_retention_policy_t p = POLICY;
    p.allow_unreceipted = false;
    CHECK("run ok", run(&c, &p, &st));
    CHECK("nothing dropped", c.drops == 0);
    CHECK("pressure reported", st.still_under_low);
}

static void test_unreadable_card_is_a_fault_not_a_license(void)
{
    fake_card_t c; cairn_retention_stats_t st;
    card_init(&c, 10 * MIB);
    c.list_fails = 1;
    CHECK("run reports failure", !run(&c, &POLICY, &st));
    CHECK("nothing dropped", c.drops == 0 && st.bytes_freed == 0);
}

static void test_delete_failure_skips_and_continues(void)
{
    fake_card_t c; cairn_retention_stats_t st;
    card_init(&c, 10 * MIB);
    add(&c, "STUCK", 1, 200 * MIB, true);
    add(&c, "OK", 2, 200 * MIB, true);
    c.delete_fails_id = 0;
    CHECK("run ok", run(&c, &POLICY, &st));
    CHECK("error counted, next bundle freed the card",
          st.delete_errors == 1 && st.receipted_dropped == 1 &&
          strcmp(c.dropped[0], "OK") == 0);
}

static void test_exhausted_policy_reports_full(void)
{
    fake_card_t c; cairn_retention_stats_t st;
    card_init(&c, 1 * MIB);
    add(&c, "TINY", 1, 2 * MIB, false);
    CHECK("run ok", run(&c, &POLICY, &st));
    CHECK("dropped what it could", st.unreceipted_dropped == 1);
    CHECK("still reports the card full", st.still_under_low);
}

static void test_default_policy(void)
{
    cairn_retention_policy_t p;
    cairn_retention_default_policy(8ull * 1024 * MIB, &p); /* 8 GiB card */
    CHECK("8GiB: 5% wins over 64MiB",
          p.low_water_bytes == (8ull * 1024 * MIB) / 20 &&
          p.high_water_bytes == p.low_water_bytes + 64 * MIB);
    cairn_retention_default_policy(512ull * MIB, &p);
    CHECK("small card: the 64MiB floor wins", p.low_water_bytes == 64 * MIB);
    CHECK("owner default allows the last resort", p.allow_unreceipted);
}

int main(void)
{
    test_healthy_card_deletes_nothing();
    test_receipted_oldest_first_stops_at_high();
    test_unreceipted_last_resort_to_low_only();
    test_unreceipted_refused_when_disallowed();
    test_unreadable_card_is_a_fault_not_a_license();
    test_delete_failure_skips_and_continues();
    test_exhausted_policy_reports_full();
    test_default_policy();
    printf("[retention] %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
