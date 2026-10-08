/*
 * Uplink manager against a simulated world: a server that keeps the chunks it is given
 * and issues one receipt per bundle, three paths that can be killed mid-bundle, a phone
 * that comes and goes, and a car that can start a trip at any moment.
 *
 * The properties under test are the ones the owner's schedule (issue #17) states:
 *
 *   - however a path dies, a bundle ends in exactly one commit, one receipt, one prune;
 *   - BLE and Wi-Fi are never both up; a slot is announced before the radio leaves BLE
 *     and reported to the phone when it returns;
 *   - a trip start returns the radio to capture/BLE within a bounded time, and a caller
 *     that does not return is cut off;
 *   - nothing is pruned on any receipt the gate does not accept;
 *   - a home scan with no phone is bounded and rate limited, and runs only when parked
 *     with bundles waiting.
 *
 * The delivery loop is driven through the real tick(): should_abort() calls it, so the
 * abort path under test is the one the lifecycle will use.
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
#define MAX_LOG     64

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
    bool trip, parked, battery_ok, phone, wifi_cfg, wifi_allowed, lte_cfg, lte_allowed, lte_full;
    bool lte_avail;
    cairn_home_trigger_t home;
    bool network_in_range;      /* what a scan would find */

    /* the radios */
    bool ble_up, wifi_up;
    int  concurrent_radio_violations;

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
    cairn_uplink_action_kind_t log[MAX_LOG];
    int nlog;
    int announces, announced_before_leave_ok, leaves, checkins, scans, lte_sends, force_offs;
    uint32_t last_abort_ordered_at, last_returned_at;
    uint32_t slot_start_at[8], slot_end_at[8];
    cairn_uplink_reason_t last_abort_reason;
    int prune_refusals;
    bool in_slot;
    bool announced_flag;
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
    w->wifi_cfg = w->wifi_allowed = true;
    w->lte_allowed = true;
    for (int p = 0; p < CAIRN_PATH_COUNT; p++) { w->kill_after[p] = -1; w->ms_per_chunk[p] = 100; }
    w->ms_per_chunk[CAIRN_PATH_WIFI] = 500;
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
    ev->phone_connected  = w->phone && w->ble_up;
    ev->home             = w->home;
    ev->lte_full_bundles_ok = w->lte_full;
    ev->path[CAIRN_PATH_BLE]  = (cairn_path_info_t){ true, w->phone && w->ble_up, true };
    ev->path[CAIRN_PATH_WIFI] = (cairn_path_info_t){ w->wifi_cfg, true, w->wifi_allowed };
    ev->path[CAIRN_PATH_LTE]  = (cairn_path_info_t){ w->lte_cfg, w->lte_avail || w->lte_cfg, w->lte_allowed };
}

static void note(world_t *w, cairn_uplink_action_kind_t k)
{
    if (w->nlog < MAX_LOG) w->log[w->nlog++] = k;
}

static int count_kind(const world_t *w, cairn_uplink_action_kind_t k)
{
    int n = 0;
    for (int i = 0; i < w->nlog; i++) if (w->log[i] == k) n++;
    return n;
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

    cairn_uplink_evidence_t ev;
    evidence(w, &ev);
    cairn_uplink_action_t a = cairn_uplink_tick(&w->u, &ev, w->now);
    if (a.kind == CAIRN_UL_ACT_ABORT_SLOT) {
        w->last_abort_ordered_at = w->now;
        w->last_abort_reason = a.reason;
    }
    if (a.kind == CAIRN_UL_ACT_FORCE_RADIO_OFF) w->force_offs++;
    return a.kind == CAIRN_UL_ACT_ABORT_SLOT || a.kind == CAIRN_UL_ACT_FORCE_RADIO_OFF ||
           cairn_uplink_abort_pending(&w->u);
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

    /* The invariant the radio rule exists for. */
    if (t->path == CAIRN_PATH_BLE && w->wifi_up) w->concurrent_radio_violations++;
    if (t->path == CAIRN_PATH_WIFI && w->ble_up) w->concurrent_radio_violations++;

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

static void run_slot(world_t *w, cairn_uplink_action_t a)
{
    (void)a;
    if (w->leaves < 8) w->slot_start_at[w->leaves] = w->now;
    cairn_uplink_slot_started(&w->u, w->now);
    w->ble_up = false;          /* the radio is on Wi-Fi now */
    w->wifi_up = true;
    w->in_slot = true;
    w->leaves++;

    cairn_uplink_io_t io;
    make_io(w, &io);

    cairn_slot_result_t r;
    memset(&r, 0, sizeof r);
    uint32_t t0 = w->now;

    for (int i = 0; i < w->nb; i++) {
        if (!w->b[i].on_card) continue;
        cairn_bundle_ref_t ref;
        memcpy(ref.root, w->b[i].root, 32);

        cairn_uplink_evidence_t ev;
        evidence(w, &ev);
        cairn_delivery_t d = cairn_uplink_deliver(&w->u, &ev, &io, &ref, should_abort, w);
        r.bytes += d.bytes;
        if (d.delivered) r.committed++;
        else r.failed++;
        if (d.aborted) { r.aborted_by = w->last_abort_reason; break; }
    }
    r.ms = w->now - t0;

    w->wifi_up = false;
    w->ble_up = true;           /* back on BLE, always */
    w->in_slot = false;
    w->last_returned_at = w->now;
    if (w->leaves >= 1 && w->leaves <= 8) w->slot_end_at[w->leaves - 1] = w->now;
    cairn_uplink_slot_finished(&w->u, &r, w->now);
}

/* Advance the world in 100 ms ticks, doing what the manager asks. */
static void drive(world_t *w, uint32_t ms)
{
    uint32_t end = w->now + ms;
    while ((int32_t)(w->now - end) < 0) {
        if (w->trip_at && (int32_t)(w->now - w->trip_at) >= 0) w->trip = true;

        cairn_uplink_evidence_t ev;
        evidence(w, &ev);
        cairn_uplink_action_t a = cairn_uplink_tick(&w->u, &ev, w->now);
        if (a.kind != CAIRN_UL_ACT_NONE) note(w, a.kind);

        switch (a.kind) {
        case CAIRN_UL_ACT_ANNOUNCE_SLOT:
            w->announces++;
            w->announced_flag = true;
            cairn_uplink_slot_announced(&w->u);
            break;
        case CAIRN_UL_ACT_START_WIFI_SLOT:
            /* A phone that is connected must have been told before the radio leaves. */
            if (!w->phone || w->announced_flag) w->announced_before_leave_ok++;
            w->announced_flag = false;
            run_slot(w, a);
            break;
        case CAIRN_UL_ACT_CHECKIN:
            w->checkins++;
            cairn_uplink_checkin_done(&w->u, w->now);
            break;
        case CAIRN_UL_ACT_SCAN_HOME:
            w->scans++;
            cairn_uplink_scan_started(&w->u, w->now);
            w->ble_up = false;
            w->wifi_up = true;
            w->now += 2000;     /* a short scan */
            w->wifi_up = false;
            w->ble_up = true;
            cairn_uplink_scan_done(&w->u, w->network_in_range, w->now);
            break;
        case CAIRN_UL_ACT_LTE_SEND:
            w->lte_sends++;
            cairn_uplink_lte_started(&w->u);
            cairn_uplink_lte_finished(&w->u, true, w->now);
            break;
        default: break;
        }
        w->now += 100;
    }
}

static void begin_parked_at_home(world_t *w)
{
    w->ble_up = true;
    w->phone = true;
    w->home = CAIRN_HOME_PHONE_ASSERTED;
}

static bool exactly_once(const world_t *w, int i)
{
    return w->b[i].commits == 1 && w->b[i].receipts_issued == 1 && w->b[i].prunes == 1 &&
           !w->b[i].on_card;
}

/* ── tests ────────────────────────────────────────────────────────────────── */

/* The owner's whole sequence: BLE, slot 1 fails halfway, BLE report, slot 2 completes,
 * BLE check-in, one commit, one receipt, one prune. */
static void test_schedule(void)
{
    world_t w;
    world_init(&w, 1, 10);
    begin_parked_at_home(&w);
    w.kill_after[CAIRN_PATH_WIFI] = 4;       /* slot 1 dies after four chunks */

    drive(&w, 5u * 60u * 1000u);

    CHECK("the bundle ends in exactly one commit, one receipt, one prune", exactly_once(&w, 0));
    CHECK("two slots were used", w.u.slots_total == 2 && w.leaves == 2);
    CHECK("slot 2 sent only the chunks slot 1 left, nothing twice", w.chunk_puts == 10);
    CHECK("each slot was announced to the phone before the radio left BLE",
          w.announces == 2 && w.announced_before_leave_ok == 2);
    CHECK("the phone was told after each slot", w.checkins == 2);
    CHECK("the order is announce, slot, check-in, announce, slot, check-in",
          w.nlog >= 6 &&
          w.log[0] == CAIRN_UL_ACT_ANNOUNCE_SLOT && w.log[1] == CAIRN_UL_ACT_START_WIFI_SLOT &&
          w.log[2] == CAIRN_UL_ACT_CHECKIN && w.log[3] == CAIRN_UL_ACT_ANNOUNCE_SLOT &&
          w.log[4] == CAIRN_UL_ACT_START_WIFI_SLOT && w.log[5] == CAIRN_UL_ACT_CHECKIN);
    CHECK("slot 2 waited out the gap after slot 1",
          w.leaves == 2 && w.slot_start_at[1] - w.slot_end_at[0] >= w.u.cfg.min_slot_gap_ms);
    CHECK("it stays on BLE once everything is committed",
          count_kind(&w, CAIRN_UL_ACT_START_WIFI_SLOT) == 2 && w.u.state == CAIRN_UL_BLE_HOME);
    CHECK("BLE and Wi-Fi were never both up", w.concurrent_radio_violations == 0);
    CHECK("no slot was cut short", w.u.slots_aborted == 0);
}

static void test_one_slot_when_it_works(void)
{
    world_t w;
    world_init(&w, 2, 5);
    begin_parked_at_home(&w);
    drive(&w, 2u * 60u * 1000u);

    CHECK("two bundles in one slot, each once", exactly_once(&w, 0) && exactly_once(&w, 1));
    CHECK("a successful first slot does not start a second", w.u.slots_total == 1);
    CHECK("and does not back off", w.u.backoff_ms == 0);
}

static void test_gates(void)
{
    for (int i = 0; i < 7; i++) {
        world_t w;
        world_init(&w, 1, 4);
        begin_parked_at_home(&w);
        switch (i) {
        case 0: w.trip = true; break;
        case 1: w.parked = false; break;
        case 2: w.battery_ok = false; break;
        case 3: w.home = CAIRN_HOME_NONE; break;
        case 4: w.wifi_cfg = false; break;
        case 5: w.wifi_allowed = false; break;
        case 6: w.b[0].on_card = false; break;
        }
        drive(&w, 60u * 1000u);
        char name[96];
        static const char *why[] = { "a trip is active", "the car is not parked", "the battery is low",
                                     "there is no home trigger", "wifi is not configured",
                                     "wifi is not allowed", "nothing is waiting" };
        snprintf(name, sizeof name, "no Wi-Fi slot starts when %s", why[i]);
        CHECK(name, w.leaves == 0 && w.chunk_puts == 0);
    }
}

static void test_no_announce_without_phone(void)
{
    world_t w;
    world_init(&w, 1, 4);
    w.ble_up = true;
    w.phone = false;
    w.home = CAIRN_HOME_SCAN_HIT;            /* a scan found the network */
    drive(&w, 2u * 60u * 1000u);
    CHECK("with no phone there is nobody to announce to, and the slot still runs",
          w.announces == 0 && w.leaves >= 1 && exactly_once(&w, 0));
    CHECK("the report waits for a phone that is not there", w.u.checkin_due);

    w.phone = true;
    drive(&w, 1000);
    CHECK("it is delivered when a phone connects", !w.u.checkin_due && w.checkins == 1);
}

static void test_trip_aborts_slot(void)
{
    world_t w;
    world_init(&w, 1, 100);                  /* 100 chunks at 500 ms: far more than a slot */
    begin_parked_at_home(&w);
    w.trip_at = w.now + 20u * 1000u;
    uint32_t t_trip = w.trip_at;

    drive(&w, 3u * 60u * 1000u);

    CHECK("the slot was aborted for the trip", w.u.slots_aborted == 1 &&
                                               w.u.last_result.aborted_by == CAIRN_UL_REASON_TRIP);
    CHECK("the radio returned to BLE within the bound",
          (int32_t)(w.last_returned_at - t_trip) >= 0 &&
          w.last_returned_at - t_trip <= w.u.cfg.abort_return_ms);
    CHECK("only part of the bundle moved, and it was not pruned",
          w.chunk_puts > 0 && w.chunk_puts < 100 && w.b[0].on_card && w.b[0].prunes == 0);
    CHECK("no new slot starts while the trip lasts", w.leaves == 1);
    CHECK("the abort was reported to the phone", w.checkins >= 1);
}

static void test_force_radio_off(void)
{
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);
    cairn_uplink_t u;
    cairn_uplink_init(&u, &cfg);

    cairn_uplink_evidence_t ev;
    memset(&ev, 0, sizeof ev);
    ev.parked = true; ev.battery_ok = true; ev.bundles_waiting = 1; ev.home = CAIRN_HOME_PHONE_ASSERTED;
    ev.path[CAIRN_PATH_WIFI] = (cairn_path_info_t){ true, true, true };

    uint32_t now = 5000;
    cairn_uplink_slot_started(&u, now);
    cairn_uplink_action_t a = cairn_uplink_tick(&u, &ev, now + 1000);
    CHECK("a healthy slot is left alone", a.kind == CAIRN_UL_ACT_NONE);

    ev.trip_active = true;
    a = cairn_uplink_tick(&u, &ev, now + 2000);
    CHECK("a trip orders the abort", a.kind == CAIRN_UL_ACT_ABORT_SLOT && a.reason == CAIRN_UL_REASON_TRIP);
    a = cairn_uplink_tick(&u, &ev, now + 2500);
    CHECK("the abort is not re-ordered while it is being honoured", a.kind == CAIRN_UL_ACT_NONE &&
                                                                    cairn_uplink_abort_pending(&u));
    a = cairn_uplink_tick(&u, &ev, now + 2000 + cfg.abort_return_ms);
    CHECK("a caller that does not return is cut off", a.kind == CAIRN_UL_ACT_FORCE_RADIO_OFF);

    cairn_slot_result_t r;
    memset(&r, 0, sizeof r);
    r.aborted_by = CAIRN_UL_REASON_TRIP;
    cairn_uplink_slot_finished(&u, &r, now + 6000);
    CHECK("once it returns the radio is BLE's again", cairn_uplink_ble_may_advertise(&u) &&
                                                       !cairn_uplink_wifi_may_run(&u) &&
                                                       !cairn_uplink_abort_pending(&u));
}

static void test_battery_and_slot_limit(void)
{
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);
    cairn_uplink_t u;
    cairn_uplink_init(&u, &cfg);

    cairn_uplink_evidence_t ev;
    memset(&ev, 0, sizeof ev);
    ev.parked = true; ev.battery_ok = true; ev.bundles_waiting = 1; ev.home = CAIRN_HOME_PHONE_ASSERTED;
    ev.path[CAIRN_PATH_WIFI] = (cairn_path_info_t){ true, true, true };

    cairn_uplink_slot_started(&u, 0);
    ev.battery_ok = false;
    cairn_uplink_action_t a = cairn_uplink_tick(&u, &ev, 100);
    CHECK("a low battery aborts the slot", a.kind == CAIRN_UL_ACT_ABORT_SLOT && a.reason == CAIRN_UL_REASON_BATTERY);

    cairn_uplink_init(&u, &cfg);
    ev.battery_ok = true;
    cairn_uplink_slot_started(&u, 0);
    a = cairn_uplink_tick(&u, &ev, cfg.slot_max_ms - 1);
    CHECK("a slot runs to its limit", a.kind == CAIRN_UL_ACT_NONE);
    a = cairn_uplink_tick(&u, &ev, cfg.slot_max_ms);
    CHECK("and not past it", a.kind == CAIRN_UL_ACT_ABORT_SLOT && a.reason == CAIRN_UL_REASON_SLOT_LIMIT);
}

static void test_backoff(void)
{
    world_t w;
    world_init(&w, 1, 20000);                /* never finishes inside a slot, or a session */
    begin_parked_at_home(&w);

    drive(&w, 10u * 60u * 1000u);
    CHECK("a session is bounded to its two slots", w.u.slots_total == 2);
    CHECK("a session that got nowhere backs off", w.u.backoff_ms == w.u.cfg.backoff_initial_ms);
    CHECK("slots stopped at the limit with reason slot-limit",
          w.u.slots_aborted == 2 && w.u.last_result.aborted_by == CAIRN_UL_REASON_SLOT_LIMIT);

    uint32_t before = w.u.slots_total;
    drive(&w, (w.u.next_session_ok_ms - w.now) - 1000u);   /* to just before it ends */
    CHECK("no new session during the backoff", w.u.slots_total == before);

    drive(&w, 5000u);
    CHECK("a new session starts after it", w.u.slots_total > before);
    drive(&w, 10u * 60u * 1000u);
    CHECK("a second fruitless session doubles it", w.u.backoff_ms == 2u * w.u.cfg.backoff_initial_ms);

    for (int i = 0; i < 12; i++) drive(&w, w.u.cfg.backoff_max_ms + 10u * 60u * 1000u);
    CHECK("the backoff is capped", w.u.backoff_ms == w.u.cfg.backoff_max_ms);
}

static void test_backoff_resets_on_progress(void)
{
    world_t w;
    world_init(&w, 1, 20);
    begin_parked_at_home(&w);
    w.u.backoff_ms = 4u * w.u.cfg.backoff_initial_ms;   /* from earlier failures */
    drive(&w, 5u * 60u * 1000u);
    CHECK("a delivered bundle clears the backoff", exactly_once(&w, 0) && w.u.backoff_ms == 0);
}

static void test_fallback_paths(void)
{
    /* For each path as the first choice: it dies at several points, the bundle falls back
     * to the next path, and ends exactly once with no chunk sent twice. */
    static const int kills[] = { 0, 1, 3, 5, 8 };
    for (unsigned k = 0; k < sizeof kills / sizeof kills[0]; k++) {
        for (int first = 0; first < CAIRN_PATH_COUNT; first++) {
            world_t w;
            world_init(&w, 1, 9);
            w.lte_cfg = true; w.lte_avail = true; w.lte_full = true;
            cairn_uplink_io_t io;
            make_io(&w, &io);

            cairn_path_t second;
            if (first == CAIRN_PATH_BLE) {
                w.ble_up = true; w.phone = true;
                second = CAIRN_PATH_LTE;
            } else if (first == CAIRN_PATH_WIFI) {
                w.ble_up = false;
                cairn_uplink_slot_started(&w.u, w.now);
                second = CAIRN_PATH_LTE;
            } else {
                /* LTE as the only way: no phone, not in a slot; a second attempt follows. */
                second = CAIRN_PATH_LTE;
            }
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
            (void)second;

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
    w.ble_up = true; w.phone = true;
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
    v.ble_up = true; v.phone = true;
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
    x.ble_up = true; x.phone = true;
    make_io(&x, &io);
    io.prune_if_receipted = NULL;
    memcpy(ref.root, x.b[0].root, 32);
    evidence(&x, &ev);
    d = cairn_uplink_deliver(&x.u, &ev, &io, &ref, should_abort, &x);
    CHECK("without a prune gate the module never reports delivery", !d.delivered && x.b[0].on_card);
}

static void test_radio_exclusivity(void)
{
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);
    cairn_uplink_t u;
    cairn_uplink_init(&u, &cfg);

    CHECK("at home: BLE yes, Wi-Fi no", cairn_uplink_ble_may_advertise(&u) && !cairn_uplink_wifi_may_run(&u));
    cairn_uplink_slot_started(&u, 0);
    CHECK("in a slot: Wi-Fi yes, BLE no", !cairn_uplink_ble_may_advertise(&u) && cairn_uplink_wifi_may_run(&u));
    cairn_uplink_slot_finished(&u, &(cairn_slot_result_t){ 0 }, 10);
    CHECK("checking in: BLE yes, Wi-Fi no", cairn_uplink_ble_may_advertise(&u) && !cairn_uplink_wifi_may_run(&u));
    cairn_uplink_checkin_done(&u, 11);
    cairn_uplink_scan_started(&u, 12);
    CHECK("a scan is Wi-Fi use too: BLE is down for it", !cairn_uplink_ble_may_advertise(&u) && cairn_uplink_wifi_may_run(&u));
    cairn_uplink_scan_done(&u, false, 13);
    CHECK("and back", cairn_uplink_ble_may_advertise(&u) && !cairn_uplink_wifi_may_run(&u));

    /* The path order never mixes the two. */
    cairn_uplink_evidence_t ev;
    memset(&ev, 0, sizeof ev);
    ev.phone_connected = true;
    ev.path[CAIRN_PATH_BLE]  = (cairn_path_info_t){ true, true, true };
    ev.path[CAIRN_PATH_WIFI] = (cairn_path_info_t){ true, true, true };
    cairn_path_t order[CAIRN_PATH_COUNT];
    size_t n = cairn_uplink_path_order(&u, &ev, order);
    CHECK("at home the order is BLE only", n == 1 && order[0] == CAIRN_PATH_BLE);
    cairn_uplink_slot_started(&u, 20);
    n = cairn_uplink_path_order(&u, &ev, order);
    CHECK("in a slot the order is Wi-Fi only", n == 1 && order[0] == CAIRN_PATH_WIFI);
}

static void test_home_scan(void)
{
    world_t w;
    world_init(&w, 1, 4);
    w.ble_up = true;
    w.phone = false;
    w.network_in_range = true;

    drive(&w, 3u * 60u * 1000u);
    CHECK("with no phone a short scan finds the network and a slot follows",
          w.scans >= 1 && w.leaves >= 1 && exactly_once(&w, 0));
    CHECK("the scan came before the slot", count_kind(&w, CAIRN_UL_ACT_SCAN_HOME) >= 1 &&
          w.log[0] == CAIRN_UL_ACT_SCAN_HOME);
}

static void test_scan_conditions(void)
{
    cairn_uplink_config_t cfg;
    cairn_uplink_config_defaults(&cfg);
    cairn_uplink_t u;
    cairn_uplink_evidence_t ev;

    memset(&ev, 0, sizeof ev);
    ev.parked = true; ev.battery_ok = true; ev.bundles_waiting = 1;
    ev.path[CAIRN_PATH_WIFI] = (cairn_path_info_t){ true, true, true };

    cairn_uplink_init(&u, &cfg);
    CHECK("scans when parked, with bundles waiting, no phone, no home known",
          cairn_uplink_scan_allowed(&u, &ev, 1000));

    cairn_uplink_evidence_t e = ev;
    e.trip_active = true;                     CHECK("not during a trip", !cairn_uplink_scan_allowed(&u, &e, 1000));
    e = ev; e.parked = false;                 CHECK("not when not parked", !cairn_uplink_scan_allowed(&u, &e, 1000));
    e = ev; e.battery_ok = false;             CHECK("not on a low battery", !cairn_uplink_scan_allowed(&u, &e, 1000));
    e = ev; e.bundles_waiting = 0;            CHECK("not with nothing to send", !cairn_uplink_scan_allowed(&u, &e, 1000));
    e = ev; e.phone_connected = true;         CHECK("not when a phone is present (it asserts home itself)",
                                                    !cairn_uplink_scan_allowed(&u, &e, 1000));
    e = ev; e.home = CAIRN_HOME_PHONE_ASSERTED; CHECK("not when home is already known",
                                                      !cairn_uplink_scan_allowed(&u, &e, 1000));
    e = ev; e.path[CAIRN_PATH_WIFI].configured = false;
                                              CHECK("not with no network provisioned", !cairn_uplink_scan_allowed(&u, &e, 1000));

    cairn_uplink_scan_started(&u, 1000);
    CHECK("not while the radio is busy", !cairn_uplink_scan_allowed(&u, &ev, 1100));
    cairn_uplink_scan_done(&u, false, 3000);
    CHECK("rate limited: not again straight away", !cairn_uplink_scan_allowed(&u, &ev, 3000 + 1000));
    CHECK("rate limited: not just before the interval",
          !cairn_uplink_scan_allowed(&u, &ev, 3000 + cfg.scan_min_interval_ms - 1));
    CHECK("again once the interval has passed", cairn_uplink_scan_allowed(&u, &ev, 3000 + cfg.scan_min_interval_ms));

    /* A hit is a valid trigger only for a while. */
    cairn_uplink_init(&u, &cfg);
    cairn_uplink_scan_started(&u, 100);
    cairn_uplink_scan_done(&u, true, 200);
    cairn_uplink_action_t a = cairn_uplink_tick(&u, &ev, 300);
    CHECK("a scan hit starts a slot", a.kind == CAIRN_UL_ACT_START_WIFI_SLOT && a.reason == CAIRN_UL_REASON_HOME_SCAN);
    a = cairn_uplink_tick(&u, &ev, 200 + cfg.scan_hit_ttl_ms + 1);
    CHECK("a stale hit does not", a.kind != CAIRN_UL_ACT_START_WIFI_SLOT);

    /* The scan is bounded. */
    cairn_uplink_init(&u, &cfg);
    cairn_uplink_scan_started(&u, 0);
    a = cairn_uplink_tick(&u, &ev, cfg.scan_max_ms);
    CHECK("a scan that overruns is aborted", a.kind == CAIRN_UL_ACT_ABORT_SLOT && a.reason == CAIRN_UL_REASON_SLOT_LIMIT);
    cairn_uplink_init(&u, &cfg);
    cairn_uplink_scan_started(&u, 0);
    ev.trip_active = true;
    a = cairn_uplink_tick(&u, &ev, 100);
    CHECK("a trip during a scan aborts it", a.kind == CAIRN_UL_ACT_ABORT_SLOT && a.reason == CAIRN_UL_REASON_TRIP);
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

static void test_wrap(void)
{
    /* The same schedule with the clock about to wrap. */
    world_t w;
    world_init(&w, 1, 10);
    begin_parked_at_home(&w);
    w.now = 0xFFFFFFFFu - 30u * 1000u;
    w.kill_after[CAIRN_PATH_WIFI] = 4;
    drive(&w, 5u * 60u * 1000u);
    CHECK("the schedule still completes across a 32-bit clock wrap", exactly_once(&w, 0) && w.u.slots_total == 2);
}

int main(void)
{
    test_schedule();
    test_one_slot_when_it_works();
    test_gates();
    test_no_announce_without_phone();
    test_trip_aborts_slot();
    test_force_radio_off();
    test_battery_and_slot_limit();
    test_backoff();
    test_backoff_resets_on_progress();
    test_fallback_paths();
    test_receipt_gate();
    test_radio_exclusivity();
    test_home_scan();
    test_scan_conditions();
    test_lte_trigger();
    test_lte_after_reboot();
    test_wrap();

    printf("uplink manager: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
