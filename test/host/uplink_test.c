/*
 * Uplink manager against a simulated world: a server that keeps the chunks it is given
 * and issues one receipt per bundle, paths that can be killed mid-bundle, a phone
 * that comes and goes, and a car that can start a trip at any moment.
 *
 * Two transports (the Wi-Fi slot machinery was deleted with the Wi-Fi stack): BLE to
 * the phone, which is free and preferred, and LTE for a car parked away. The
 * properties under test are the ones the owner's schedule (issue #17) states, as they
 * survive LTE-only scheduling:
 *
 *   - however a path dies, a bundle ends in exactly one commit, one receipt, one prune,
 *     and no chunk is ever sent twice (the next attempt resumes);
 *   - nothing is pruned on any receipt the gate does not accept;
 *   - LTE fires only parked, away from home, with no phone, a settle delay after the
 *     trip (or after boot, #36), within its retry spacing, and never during a trip;
 *   - the path order never offers LTE whole bundles the user has not allowed.
 */

#include <stdio.h>
#include <string.h>

#include "cairn_uplink.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [uplink] %s\n", name); }         \
        else      { g_fail++; printf("  FAIL  [uplink] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

/* ── the world ────────────────────────────────────────────────────────────── */

#define MAX_BUNDLES 3
#define MAX_CHUNKS  20000

typedef struct {
    uint8_t root[32];
    int     nchunks;
    bool    have[MAX_CHUNKS];   /* server side */
    int     commits;
    int     receipts_issued;
    bool    on_card;            /* dongle side: false once pruned */
    int     prunes;
} sim_bundle_t;

typedef struct {
    cairn_uplink_t u;
    uint32_t now;

    /* evidence */
    bool trip, parked, battery_ok, phone, lte_cfg, lte_allowed, lte_full;
    bool lte_avail;
    cairn_home_trigger_t home;

    /* the server */
    sim_bundle_t b[MAX_BUNDLES];
    int nb;
    int chunk_puts;
    bool forge_first;           /* the server's first receipt names the wrong root */

    /* per-path fault injection: kill after this many chunks in one transfer, once */
    int kill_after[CAIRN_PATH_COUNT];
    uint32_t ms_per_chunk[CAIRN_PATH_COUNT];
    uint32_t trip_at;           /* a trip begins at this time (0 = never) */

    /* what the caller observed */
    int lte_sends;
    int prune_refusals;
} world_t;

static void world_init(world_t *w, int nbundles, int nchunks)
{
    memset(w, 0, sizeof *w);
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);
    cairn_uplink_init(&w->u, &cfg);
    w->now = 1000000u;               /* away from zero so wrap arithmetic is exercised */
    w->parked = true;
    w->battery_ok = true;
    w->lte_allowed = true;
    for (int p = 0; p < CAIRN_PATH_COUNT; p++) { w->kill_after[p] = -1; w->ms_per_chunk[p] = 100; }
    w->nb = nbundles;
    for (int i = 0; i < nbundles; i++) {
        memset(w->b[i].root, 0x10 + i, 32);
        w->b[i].nchunks = nchunks;
        w->b[i].on_card = true;
    }
}

static uint32_t waiting(const world_t *w)
{
    uint32_t n = 0;
    for (int i = 0; i < w->nb; i++) if (w->b[i].on_card) n++;
    return n;
}

static void evidence(const world_t *w, cairn_uplink_evidence_t *ev)
{
    memset(ev, 0, sizeof *ev);
    ev->trip_active      = w->trip;
    ev->parked           = w->parked && !w->trip;
    ev->battery_ok       = w->battery_ok;
    ev->bundles_waiting  = waiting(w);
    ev->phone_connected  = w->phone;
    ev->home             = w->home;
    ev->lte_full_bundles_ok = w->lte_full;
    ev->path[CAIRN_PATH_BLE] = (cairn_path_info_t){ true, w->phone, true };
    ev->path[CAIRN_PATH_LTE] = (cairn_path_info_t){ w->lte_cfg, w->lte_avail || w->lte_cfg, w->lte_allowed };
}

/* ── the transports ───────────────────────────────────────────────────────── */

typedef struct { world_t *w; cairn_path_t path; } tctx_t;

static sim_bundle_t *find(world_t *w, const cairn_bundle_ref_t *b)
{
    for (int i = 0; i < w->nb; i++) if (memcmp(w->b[i].root, b->root, 32) == 0) return &w->b[i];
    return NULL;
}

static bool should_abort(void *ctx)
{
    world_t *w = ctx;
    if (w->trip_at && (int32_t)(w->now - w->trip_at) >= 0) w->trip = true;
    return w->trip;
}

static cairn_xfer_result_t sim_transfer(void *ctx, const cairn_bundle_ref_t *b,
                                        bool (*abort_fn)(void *), void *abort_ctx,
                                        uint8_t *receipt, size_t cap, size_t *rlen,
                                        cairn_xfer_stats_t *st)
{
    tctx_t *t = ctx;
    world_t *w = t->w;
    sim_bundle_t *sb = find(w, b);
    if (!sb || cap < 33) return CAIRN_XFER_FAILED;

    int sent = 0;
    for (int c = 0; c < sb->nchunks; c++) {
        if (sb->have[c]) continue;                     /* the offer returned the missing set */
        if (abort_fn(abort_ctx)) return CAIRN_XFER_ABORTED;
        if (w->kill_after[t->path] >= 0 && sent >= w->kill_after[t->path]) {
            w->kill_after[t->path] = -1;               /* the link dies once */
            return sent ? CAIRN_XFER_PARTIAL : CAIRN_XFER_FAILED;
        }
        w->now += w->ms_per_chunk[t->path];
        sb->have[c] = true;
        w->chunk_puts++;
        sent++;
        st->chunks_sent++;
        st->bytes += 1024;
    }

    if (sb->commits == 0) { sb->commits = 1; sb->receipts_issued = 1; }   /* idempotent commit */
    receipt[0] = 0xA5;
    memcpy(receipt + 1, sb->root, 32);
    if (w->forge_first) { receipt[1] ^= 0x01; w->forge_first = false; }
    *rlen = 33;
    return CAIRN_XFER_RECEIPT;
}

static bool sim_prune(void *ctx, const cairn_bundle_ref_t *b, const uint8_t *r, size_t len)
{
    world_t *w = ctx;
    sim_bundle_t *sb = find(w, b);
    if (!sb || len != 33 || r[0] != 0xA5 || memcmp(r + 1, b->root, 32) != 0) {
        w->prune_refusals++;
        return false;
    }
    if (sb->on_card) { sb->on_card = false; sb->prunes++; }
    return true;
}

static tctx_t g_tctx[CAIRN_PATH_COUNT];

static void make_io(world_t *w, cairn_uplink_io_t *io)
{
    memset(io, 0, sizeof *io);
    for (int p = 0; p < CAIRN_PATH_COUNT; p++) {
        g_tctx[p].w = w; g_tctx[p].path = (cairn_path_t)p;
        io->path[p].transfer = sim_transfer;
        io->path[p].ctx = &g_tctx[p];
    }
    io->prune_if_receipted = sim_prune;
    io->prune_ctx = w;
}

/* ── the caller: what the lifecycle does with each action ─────────────────── */

/* Advance the world in 100 ms ticks, doing what the manager asks: an LTE send
 * delivers every waiting bundle it can, like the runner's bounded session. */
static void drive(world_t *w, uint32_t ms)
{
    uint32_t end = w->now + ms;
    while ((int32_t)(w->now - end) < 0) {
        if (w->trip_at && (int32_t)(w->now - w->trip_at) >= 0) w->trip = true;

        cairn_uplink_evidence_t ev;
        evidence(w, &ev);
        cairn_uplink_action_t a = cairn_uplink_tick(&w->u, &ev, w->now);

        if (a.kind == CAIRN_UL_ACT_LTE_SEND) {
            w->lte_sends++;
            cairn_uplink_lte_started(&w->u);

            cairn_uplink_io_t io;
            make_io(w, &io);
            bool any = false;
            for (int i = 0; i < w->nb; i++) {
                if (!w->b[i].on_card) continue;
                cairn_bundle_ref_t ref;
                memcpy(ref.root, w->b[i].root, 32);
                evidence(w, &ev);
                cairn_delivery_t d = cairn_uplink_deliver(&w->u, &ev, &io, &ref, should_abort, w);
                if (d.delivered) any = true;
                if (d.aborted) break;
            }
            cairn_uplink_lte_finished(&w->u, any, w->now);
        }
        w->now += 100;
    }
}

static bool exactly_once(const world_t *w, int i)
{
    return w->b[i].commits == 1 && w->b[i].receipts_issued == 1 && w->b[i].prunes == 1 &&
           !w->b[i].on_card;
}

/* ── tests ────────────────────────────────────────────────────────────────── */

/* A trip ends away from home; LTE waits out the settle delay and then sends
 * everything, exactly once. */
static void test_schedule(void)
{
    world_t w;
    world_init(&w, 2, 10);
    w.lte_cfg = true; w.lte_avail = true; w.lte_full = true;

    w.trip = true; drive(&w, 1000); w.trip = false;   /* a trip has just ended */
    drive(&w, 30u * 1000u);
    CHECK("LTE does not fire before the settle delay", w.lte_sends == 0 && waiting(&w) == 2);

    drive(&w, 5u * 60u * 1000u);
    CHECK("one send carries both bundles", w.lte_sends >= 1);
    CHECK("bundle 0 ends in exactly one commit, one receipt, one prune", exactly_once(&w, 0));
    CHECK("bundle 1 too", exactly_once(&w, 1));
    CHECK("nothing was sent twice", w.chunk_puts == 20);

    int sends = w.lte_sends;
    drive(&w, 30u * 60u * 1000u);
    CHECK("with nothing waiting, LTE stays quiet", w.lte_sends == sends);
}

static void test_gates(void)
{
    static const char *why[] = { "a trip is active", "the car is parked at home (phone asserted)",
                                 "the battery is low", "a phone is connected",
                                 "LTE is not configured", "LTE is not allowed",
                                 "nothing is waiting", "auto is off" };
    for (int i = 0; i < 8; i++) {
        world_t w;
        world_init(&w, 1, 4);
        w.lte_cfg = true; w.lte_avail = true; w.lte_full = true;
        w.trip = true; drive(&w, 1000); w.trip = false;
        switch (i) {
        case 0: w.trip_at = w.now + 1; break;
        case 1: w.home = CAIRN_HOME_PHONE_ASSERTED; break;
        case 2: w.battery_ok = false; break;
        case 3: w.phone = true; break;
        case 4: w.lte_cfg = false; break;
        case 5: w.lte_allowed = false; break;
        case 6: w.b[0].on_card = false; break;
        case 7: w.u.cfg.lte_auto = false; break;
        }
        drive(&w, 15u * 60u * 1000u);
        char name[96];
        snprintf(name, sizeof name, "no LTE send when %s", why[i]);
        CHECK(name, w.lte_sends == 0);
    }
}

static void test_lte_trigger(void)
{
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);
    cairn_uplink_t u;
    cairn_uplink_init(&u, &cfg);

    cairn_uplink_evidence_t ev;
    memset(&ev, 0, sizeof ev);
    ev.parked = true; ev.battery_ok = true; ev.bundles_waiting = 1;
    ev.path[CAIRN_PATH_LTE] = (cairn_path_info_t){ true, true, true };

    uint32_t t = 10000;
    cairn_uplink_action_t a = cairn_uplink_tick(&u, &ev, t);
    /* Boot now counts as a trip end (#36), so what holds this back is the settle
     * delay, not the absence of an edge. test_lte_after_reboot covers that. */
    CHECK("LTE does not fire the instant the device boots", a.kind != CAIRN_UL_ACT_LTE_SEND);

    ev.trip_active = true; ev.parked = false;
    cairn_uplink_tick(&u, &ev, t);
    CHECK("not during a trip", cairn_uplink_tick(&u, &ev, t + 1000).kind != CAIRN_UL_ACT_LTE_SEND);

    ev.trip_active = false; ev.parked = true;
    uint32_t end = t + 2000;
    cairn_uplink_tick(&u, &ev, end);
    CHECK("not straight after the trip", cairn_uplink_tick(&u, &ev, end + 1000).kind != CAIRN_UL_ACT_LTE_SEND);
    a = cairn_uplink_tick(&u, &ev, end + cfg.lte_after_trip_ms);
    CHECK("after a trip, away, no phone, bundles waiting: a send", a.kind == CAIRN_UL_ACT_LTE_SEND);

    cairn_uplink_lte_started(&u);
    CHECK("not again while one is in flight",
          cairn_uplink_tick(&u, &ev, end + cfg.lte_after_trip_ms + 100).kind != CAIRN_UL_ACT_LTE_SEND);
    cairn_uplink_lte_finished(&u, false, end + cfg.lte_after_trip_ms + 200);
    CHECK("retries are spaced",
          cairn_uplink_tick(&u, &ev, end + cfg.lte_after_trip_ms + 300).kind != CAIRN_UL_ACT_LTE_SEND);
    CHECK("and allowed once the spacing has passed",
          cairn_uplink_tick(&u, &ev, end + cfg.lte_after_trip_ms + 200 + cfg.lte_retry_ms).kind == CAIRN_UL_ACT_LTE_SEND);

    /* Each gate on its own. */
    struct { const char *name; int id; } gates[] = {
        { "a phone is present", 0 }, { "LTE is not allowed (caps, pause)", 1 }, { "LTE is not configured", 2 },
        { "home is known", 3 }, { "nothing is waiting", 4 }, { "the battery is low", 5 }, { "auto is off", 6 },
    };
    for (unsigned g = 0; g < sizeof gates / sizeof gates[0]; g++) {
        cairn_uplink_t v;
        cairn_uplink_init(&v, &cfg);
        cairn_uplink_evidence_t e = ev;
        e.trip_active = true; cairn_uplink_tick(&v, &e, t);
        e.trip_active = false;
        cairn_uplink_tick(&v, &e, end);
        switch (gates[g].id) {
        case 0: e.phone_connected = true; break;
        case 1: e.path[CAIRN_PATH_LTE].allowed = false; break;
        case 2: e.path[CAIRN_PATH_LTE].configured = false; break;
        case 3: e.home = CAIRN_HOME_PHONE_ASSERTED; break;
        case 4: e.bundles_waiting = 0; break;
        case 5: e.battery_ok = false; break;
        case 6: v.cfg.lte_auto = false; break;
        }
        char name[96];
        snprintf(name, sizeof name, "LTE does not fire when %s", gates[g].name);
        CHECK(name, cairn_uplink_tick(&v, &e, end + cfg.lte_after_trip_ms).kind != CAIRN_UL_ACT_LTE_SEND);
    }
}

/*
 * A power cycle while parked, away, with bundles already sealed (#36).
 *
 * Before boot counted as a trip end this device could never send: trip_ended_known
 * is zeroed by init, so the manager had no edge to point at and the bundles waited
 * for some later drive to finish — in exactly the situation where cellular is the
 * only way they can leave.
 */
static void test_lte_after_reboot(void)
{
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);

    cairn_uplink_evidence_t ev;
    memset(&ev, 0, sizeof ev);
    ev.parked = true; ev.battery_ok = true; ev.bundles_waiting = 1;
    ev.path[CAIRN_PATH_LTE] = (cairn_path_info_t){ true, true, true };

    const uint32_t boot = 5000;

    cairn_uplink_t u;
    cairn_uplink_init(&u, &cfg);
    CHECK("a fresh boot does not send immediately",
          cairn_uplink_tick(&u, &ev, boot).kind != CAIRN_UL_ACT_LTE_SEND);
    CHECK("nor before the settle delay has elapsed, so the phone keeps first refusal",
          cairn_uplink_tick(&u, &ev, boot + cfg.lte_after_trip_ms - 1).kind
              != CAIRN_UL_ACT_LTE_SEND);
    CHECK("but a reboot parked with bundles waiting does send once it has",
          cairn_uplink_tick(&u, &ev, boot + cfg.lte_after_trip_ms).kind
              == CAIRN_UL_ACT_LTE_SEND);

    /*
     * The adopted end must not survive a reboot that lands mid-drive. Capture
     * takes a few seconds to arm, so the first tick legitimately sees no trip;
     * once it does arm, the real trip edge has to clear what boot assumed.
     */
    cairn_uplink_t v;
    cairn_uplink_init(&v, &cfg);
    cairn_uplink_evidence_t e = ev;
    cairn_uplink_tick(&v, &e, boot);
    e.trip_active = true; e.parked = false;
    cairn_uplink_tick(&v, &e, boot + 12000);
    CHECK("a reboot that lands mid-drive does not send on the adopted end",
          cairn_uplink_tick(&v, &e, boot + cfg.lte_after_trip_ms).kind
              != CAIRN_UL_ACT_LTE_SEND);

    /* And once that drive really ends, the normal edge takes over. */
    uint32_t end = boot + cfg.lte_after_trip_ms + 1000;
    e.trip_active = false; e.parked = true;
    cairn_uplink_tick(&v, &e, end);
    CHECK("and sends after the real trip end instead",
          cairn_uplink_tick(&v, &e, end + cfg.lte_after_trip_ms).kind
              == CAIRN_UL_ACT_LTE_SEND);
}

static void test_fallback_paths(void)
{
    /* For each path as the first choice: it dies at several points, the bundle falls back
     * to the next path (or a later attempt resumes), and ends exactly once with no chunk
     * sent twice. */
    static const int kills[] = { 0, 1, 3, 5, 8 };
    for (unsigned k = 0; k < sizeof kills / sizeof kills[0]; k++) {
        for (int first = 0; first < CAIRN_PATH_COUNT; first++) {
            world_t w;
            world_init(&w, 1, 9);
            w.lte_cfg = true; w.lte_avail = true; w.lte_full = true;
            if (first == CAIRN_PATH_BLE) w.phone = true;
            cairn_uplink_io_t io;
            make_io(&w, &io);

            w.kill_after[first] = kills[k];

            cairn_bundle_ref_t ref;
            memcpy(ref.root, w.b[0].root, 32);
            cairn_uplink_evidence_t ev;
            evidence(&w, &ev);

            cairn_delivery_t d = cairn_uplink_deliver(&w.u, &ev, &io, &ref, should_abort, &w);
            if (!d.delivered) {
                /* The only path died: the next attempt, later, resumes from what was accepted. */
                w.kill_after[first] = -1;
                evidence(&w, &ev);
                d = cairn_uplink_deliver(&w.u, &ev, &io, &ref, should_abort, &w);
            }

            char name[120];
            snprintf(name, sizeof name, "first path %s killed after %d chunk(s): one commit, one receipt, one prune",
                     cairn_path_name((cairn_path_t)first), kills[k]);
            CHECK(name, d.delivered && exactly_once(&w, 0));

            snprintf(name, sizeof name, "first path %s killed after %d chunk(s): nothing sent twice",
                     cairn_path_name((cairn_path_t)first), kills[k]);
            CHECK(name, w.chunk_puts == 9);
        }
    }
}

static void test_receipt_gate(void)
{
    world_t w;
    world_init(&w, 1, 4);
    w.lte_cfg = true; w.lte_avail = true; w.lte_full = true;
    w.phone = true;
    w.forge_first = true;                    /* BLE's receipt names the wrong bundle */

    cairn_uplink_io_t io;
    make_io(&w, &io);
    cairn_bundle_ref_t ref;
    memcpy(ref.root, w.b[0].root, 32);
    cairn_uplink_evidence_t ev;
    evidence(&w, &ev);

    cairn_delivery_t d = cairn_uplink_deliver(&w.u, &ev, &io, &ref, should_abort, &w);
    CHECK("a receipt the gate refuses deletes nothing on that path", w.prune_refusals == 1);
    CHECK("the next path's genuine receipt does", d.delivered && d.paths_tried == 2 && exactly_once(&w, 0));
    CHECK("the prune ran once", w.b[0].prunes == 1);

    /* Only a forged receipt exists: the bundle must stay. */
    world_t v;
    world_init(&v, 1, 4);
    v.phone = true;
    v.forge_first = true;
    make_io(&v, &io);
    memcpy(ref.root, v.b[0].root, 32);
    evidence(&v, &ev);
    d = cairn_uplink_deliver(&v.u, &ev, &io, &ref, should_abort, &v);
    CHECK("a forged receipt alone leaves the bundle on the card",
          !d.delivered && v.b[0].on_card && v.b[0].prunes == 0 && v.prune_refusals == 1);

    /* No gate at all: delivery cannot succeed, so nothing can be deleted. */
    world_t x;
    world_init(&x, 1, 4);
    x.phone = true;
    make_io(&x, &io);
    io.prune_if_receipted = NULL;
    memcpy(ref.root, x.b[0].root, 32);
    evidence(&x, &ev);
    d = cairn_uplink_deliver(&x.u, &ev, &io, &ref, should_abort, &x);
    CHECK("without a prune gate the module never reports delivery", !d.delivered && x.b[0].on_card);
}

static void test_path_order(void)
{
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);
    cairn_uplink_t u;
    cairn_uplink_init(&u, &cfg);

    cairn_uplink_evidence_t ev;
    memset(&ev, 0, sizeof ev);
    ev.phone_connected = true;
    ev.path[CAIRN_PATH_BLE] = (cairn_path_info_t){ true, true, true };
    ev.path[CAIRN_PATH_LTE] = (cairn_path_info_t){ true, true, true };

    cairn_path_t order[CAIRN_PATH_COUNT];
    size_t n = cairn_uplink_path_order(&u, &ev, order);
    CHECK("with a phone and no LTE permission the order is BLE only", n == 1 && order[0] == CAIRN_PATH_BLE);

    ev.lte_full_bundles_ok = true;
    n = cairn_uplink_path_order(&u, &ev, order);
    CHECK("with LTE allowed it is BLE first, LTE as the fallback",
          n == 2 && order[0] == CAIRN_PATH_BLE && order[1] == CAIRN_PATH_LTE);

    ev.phone_connected = false;
    ev.path[CAIRN_PATH_BLE].available = false;
    n = cairn_uplink_path_order(&u, &ev, order);
    CHECK("with no phone it is LTE only", n == 1 && order[0] == CAIRN_PATH_LTE);

    ev.lte_full_bundles_ok = false;
    n = cairn_uplink_path_order(&u, &ev, order);
    CHECK("digests-only LTE is never offered a whole bundle", n == 0);
}

static void test_trip_aborts_send(void)
{
    world_t w;
    world_init(&w, 1, 100);
    w.lte_cfg = true; w.lte_avail = true; w.lte_full = true;
    w.trip = true; drive(&w, 1000); w.trip = false;
    w.trip_at = w.now + w.u.cfg.lte_after_trip_ms + 3000;   /* mid-send */
    w.ms_per_chunk[CAIRN_PATH_LTE] = 500;

    drive(&w, 10u * 60u * 1000u);
    CHECK("a trip starting mid-send aborts it",
          w.lte_sends == 1 && w.b[0].on_card && w.b[0].prunes == 0);
    CHECK("only part of the bundle moved", w.chunk_puts > 0 && w.chunk_puts < 100);

    /* The drive ends; the next send resumes from the accepted chunks. */
    int before = w.chunk_puts;
    w.trip_at = 0; w.trip = false;
    drive(&w, 60u * 60u * 1000u);
    CHECK("the next send resumes rather than starting over",
          exactly_once(&w, 0) && w.chunk_puts == 100 && w.chunk_puts > before);
}

static void test_wrap(void)
{
    /* The same schedule with the clock about to wrap. */
    world_t w;
    world_init(&w, 1, 10);
    w.lte_cfg = true; w.lte_avail = true; w.lte_full = true;
    w.now = 0xFFFFFFFFu - 30u * 1000u;
    w.trip = true; drive(&w, 1000); w.trip = false;
    drive(&w, 10u * 60u * 1000u);
    CHECK("the schedule still completes across a 32-bit clock wrap",
          exactly_once(&w, 0) && w.lte_sends >= 1);
}

int main(void)
{
    test_schedule();
    test_gates();
    test_lte_trigger();
    test_lte_after_reboot();
    test_fallback_paths();
    test_receipt_gate();
    test_path_order();
    test_trip_aborts_send();
    test_wrap();

    printf("uplink manager: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
