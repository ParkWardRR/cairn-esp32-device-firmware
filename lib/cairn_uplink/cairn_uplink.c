#include "cairn_uplink.h"

#include <string.h>

/* Millisecond clocks wrap; every comparison is on the signed difference. */
static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

const char *cairn_path_name(cairn_path_t p)
{
    switch (p) {
    case CAIRN_PATH_BLE:  return "ble";
    case CAIRN_PATH_WIFI: return "wifi";
    case CAIRN_PATH_LTE:  return "lte";
    default:              return "?";
    }
}

const char *cairn_uplink_state_name(cairn_uplink_state_t s)
{
    switch (s) {
    case CAIRN_UL_BLE_HOME:    return "BLE_HOME";
    case CAIRN_UL_WIFI_SLOT:   return "WIFI_SLOT";
    case CAIRN_UL_WIFI_SCAN:   return "WIFI_SCAN";
    case CAIRN_UL_BLE_CHECKIN: return "BLE_CHECKIN";
    default:                   return "?";
    }
}

void cairn_uplink_config_defaults(cairn_uplink_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->slot_max_ms          = 90u * 1000u;
    c->slots_per_session    = 2;
    c->min_slot_gap_ms      = 30u * 1000u;
    c->checkin_wait_ms      = 10u * 1000u;
    c->abort_return_ms      = 2u * 1000u;
    c->backoff_initial_ms   = 10u * 60u * 1000u;
    c->backoff_max_ms       = 6u * 60u * 60u * 1000u;
    c->scan_min_interval_ms = 15u * 60u * 1000u;
    c->scan_max_ms          = 8u * 1000u;
    c->scan_hit_ttl_ms      = 30u * 1000u;
    c->lte_auto             = true;    /* the owner's proposed default; still open */
    c->lte_after_trip_ms    = 60u * 1000u;
    c->lte_retry_ms         = 15u * 60u * 1000u;
}

void cairn_uplink_init(cairn_uplink_t *u, const cairn_uplink_config_t *cfg)
{
    memset(u, 0, sizeof *u);
    u->cfg   = *cfg;
    u->state = CAIRN_UL_BLE_HOME;
}

bool cairn_uplink_ble_may_advertise(const cairn_uplink_t *u)
{
    return u->state == CAIRN_UL_BLE_HOME || u->state == CAIRN_UL_BLE_CHECKIN;
}

bool cairn_uplink_abort_pending(const cairn_uplink_t *u) { return u->abort_ordered; }

bool cairn_uplink_wifi_may_run(const cairn_uplink_t *u)
{
    return u->state == CAIRN_UL_WIFI_SLOT || u->state == CAIRN_UL_WIFI_SCAN;
}

/* ── the gates ────────────────────────────────────────────────────────────── */

static cairn_home_trigger_t effective_home(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                           uint32_t now)
{
    if (ev->home != CAIRN_HOME_NONE) return ev->home;
    if (u->scan_hit_valid && !reached(now, u->scan_hit_until_ms)) return CAIRN_HOME_SCAN_HIT;
    return CAIRN_HOME_NONE;
}

/* Common to a slot and a scan: both put the radio on Wi-Fi. */
static const char *wifi_radio_blocker(const cairn_uplink_evidence_t *ev)
{
    if (ev->trip_active)                                    return "trip active";
    if (!ev->parked)                                        return "not parked";
    if (!ev->battery_ok)                                    return "battery low";
    if (ev->bundles_waiting == 0)                           return "nothing to send";
    if (!ev->path[CAIRN_PATH_WIFI].configured)              return "wifi not configured";
    if (!ev->path[CAIRN_PATH_WIFI].allowed)                 return "wifi not allowed";
    return NULL;
}

static const char *slot_blocker(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                uint32_t now)
{
    const char *b = wifi_radio_blocker(ev);
    if (b) return b;
    if (effective_home(u, ev, now) == CAIRN_HOME_NONE)      return "not at a known network";
    /* A session that has used its slots was already closed by the tick, so only the gap
     * within an open session and the backoff between sessions are left to check. */
    if (u->session_open) {
        if (!reached(now, u->next_slot_ok_ms))              return "gap between slots";
    } else if (!reached(now, u->next_session_ok_ms)) {
        return "backing off";
    }
    return NULL;
}

const char *cairn_uplink_scan_blocker(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                      uint32_t now_ms)
{
    const char *b = wifi_radio_blocker(ev);
    if (b) return b;
    if (u->state != CAIRN_UL_BLE_HOME)                      return "radio busy";
    /* With a phone present the phone asserts home; scanning would only spend power. */
    if (ev->phone_connected)                                return "phone present";
    if (effective_home(u, ev, now_ms) != CAIRN_HOME_NONE)   return "home already known";
    if (!u->session_open && !reached(now_ms, u->next_session_ok_ms)) return "backing off";
    if (u->scanned_ever && !reached(now_ms, u->last_scan_ms + u->cfg.scan_min_interval_ms))
        return "scanned recently";
    return NULL;
}

bool cairn_uplink_scan_allowed(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                               uint32_t now_ms)
{
    return cairn_uplink_scan_blocker(u, ev, now_ms) == NULL;
}

static bool lte_may_send(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev, uint32_t now)
{
    const cairn_path_info_t *p = &ev->path[CAIRN_PATH_LTE];

    if (!u->cfg.lte_auto || u->lte_in_flight)               return false;
    if (!p->configured || !p->allowed || !p->available)     return false;
    if (ev->trip_active || ev->bundles_waiting == 0)        return false;
    if (!ev->battery_ok)                                    return false;
    /* Away from home and with no phone to do it for free. */
    if (ev->phone_connected)                                return false;
    if (effective_home(u, ev, now) != CAIRN_HOME_NONE)      return false;
    /* After a trip, not during one, and not before the car has settled. */
    if (!u->trip_ended_known)                               return false;
    if (!reached(now, u->trip_ended_ms + u->cfg.lte_after_trip_ms)) return false;
    if (!reached(now, u->lte_next_ok_ms))                   return false;
    return true;
}

/* ── session bookkeeping ──────────────────────────────────────────────────── */

static void end_session(cairn_uplink_t *u, const cairn_uplink_evidence_t *ev, uint32_t now)
{
    if (ev->bundles_waiting == 0) {
        u->backoff_ms = 0;
    } else if (u->progress_this_session) {
        u->backoff_ms = u->cfg.backoff_initial_ms;
    } else {
        u->backoff_ms = (u->backoff_ms == 0) ? u->cfg.backoff_initial_ms
                                             : u->backoff_ms * 2u;
        if (u->backoff_ms > u->cfg.backoff_max_ms || u->backoff_ms < u->cfg.backoff_initial_ms)
            u->backoff_ms = u->cfg.backoff_max_ms;
    }
    u->next_session_ok_ms = now + u->backoff_ms;
    u->session_open = false;
    u->slots_used = 0;
    u->progress_this_session = false;
    u->announced = false;
}

static void reset_session_for_trip(cairn_uplink_t *u)
{
    /* The backoff stays: a drive does not make a failing network better. */
    u->session_open = false;
    u->slots_used = 0;
    u->progress_this_session = false;
    u->announced = false;
}

/* ── tick ─────────────────────────────────────────────────────────────────── */

static cairn_uplink_action_t act(cairn_uplink_action_kind_t k, cairn_uplink_reason_t r,
                                 uint8_t slot, uint32_t max_ms)
{
    cairn_uplink_action_t a;
    a.kind = k; a.reason = r; a.slot_no = slot; a.max_ms = max_ms;
    return a;
}

static cairn_uplink_action_t order_abort(cairn_uplink_t *u, cairn_uplink_reason_t why, uint32_t now)
{
    u->abort_ordered = true;
    u->abort_ordered_ms = now;
    return act(CAIRN_UL_ACT_ABORT_SLOT, why, u->slots_used, 0);
}

cairn_uplink_action_t cairn_uplink_tick(cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                        uint32_t now)
{
    /* The trip edge: the start resets the session, the end starts the LTE clock. */
    if (ev->trip_active) {
        if (!u->trip_was_active) reset_session_for_trip(u);
        u->trip_was_active = true;
        u->trip_ended_known = false;
        u->lte_sent_since_trip = false;
    } else if (u->trip_was_active) {
        u->trip_was_active = false;
        u->trip_ended_known = true;
        u->trip_ended_ms = now;
    }

    switch (u->state) {
    case CAIRN_UL_WIFI_SLOT:
    case CAIRN_UL_WIFI_SCAN: {
        if (u->abort_ordered) {
            /* Told to stop and not yet back: the radio must not be left on Wi-Fi. */
            if (reached(now, u->abort_ordered_ms + u->cfg.abort_return_ms))
                return act(CAIRN_UL_ACT_FORCE_RADIO_OFF, CAIRN_UL_REASON_NONE, u->slots_used, 0);
            return act(CAIRN_UL_ACT_NONE, CAIRN_UL_REASON_NONE, 0, 0);
        }
        if (ev->trip_active)   return order_abort(u, CAIRN_UL_REASON_TRIP, now);
        if (!ev->battery_ok)   return order_abort(u, CAIRN_UL_REASON_BATTERY, now);

        uint32_t started = (u->state == CAIRN_UL_WIFI_SLOT) ? u->slot_started_ms : u->scan_started_ms;
        uint32_t limit   = (u->state == CAIRN_UL_WIFI_SLOT) ? u->cfg.slot_max_ms : u->cfg.scan_max_ms;
        if (reached(now, started + limit)) return order_abort(u, CAIRN_UL_REASON_SLOT_LIMIT, now);
        return act(CAIRN_UL_ACT_NONE, CAIRN_UL_REASON_NONE, 0, 0);
    }

    case CAIRN_UL_BLE_CHECKIN:
        if (ev->phone_connected) {
            if (!u->checkin_in_flight) {
                u->checkin_in_flight = true;
                return act(CAIRN_UL_ACT_CHECKIN, CAIRN_UL_REASON_NONE, u->slots_used, 0);
            }
            return act(CAIRN_UL_ACT_NONE, CAIRN_UL_REASON_NONE, 0, 0);
        }
        u->checkin_in_flight = false;
        if (reached(now, u->checkin_since_ms + u->cfg.checkin_wait_ms)) {
            /* No phone to tell. The report stays pending for the next one that connects;
             * the schedule must not stall on a phone that is not there. */
            u->state = CAIRN_UL_BLE_HOME;
        }
        return act(CAIRN_UL_ACT_NONE, CAIRN_UL_REASON_NONE, 0, 0);

    case CAIRN_UL_BLE_HOME:
    default:
        break;
    }

    /* BLE_HOME */

    if (u->checkin_due && ev->phone_connected) {
        u->state = CAIRN_UL_BLE_CHECKIN;
        u->checkin_since_ms = now;
        u->checkin_in_flight = true;
        return act(CAIRN_UL_ACT_CHECKIN, CAIRN_UL_REASON_NONE, u->slots_used, 0);
    }

    if (u->session_open && (ev->bundles_waiting == 0 || u->slots_used >= u->cfg.slots_per_session))
        end_session(u, ev, now);

    if (slot_blocker(u, ev, now) == NULL) {
        cairn_uplink_reason_t why = (effective_home(u, ev, now) == CAIRN_HOME_PHONE_ASSERTED)
                                        ? CAIRN_UL_REASON_HOME_PHONE : CAIRN_UL_REASON_HOME_SCAN;
        uint8_t n = (uint8_t)(u->slots_used + 1);

        /* Tell the phone first, so the disconnect is not read as a lost link. With no phone
         * connected there is nobody to tell. */
        if (ev->phone_connected && !u->announced)
            return act(CAIRN_UL_ACT_ANNOUNCE_SLOT, why, n, u->cfg.slot_max_ms);
        return act(CAIRN_UL_ACT_START_WIFI_SLOT, why, n, u->cfg.slot_max_ms);
    }

    /* Whatever was announced is moot if the conditions went away; the phone's own
     * timeout on the announced bound covers the gap. */
    u->announced = false;

    if (cairn_uplink_scan_allowed(u, ev, now))
        return act(CAIRN_UL_ACT_SCAN_HOME, CAIRN_UL_REASON_NONE, 0, u->cfg.scan_max_ms);

    if (lte_may_send(u, ev, now))
        return act(CAIRN_UL_ACT_LTE_SEND, CAIRN_UL_REASON_NONE, 0, 0);

    return act(CAIRN_UL_ACT_NONE, CAIRN_UL_REASON_NONE, 0, 0);
}

/* ── reports from the caller ──────────────────────────────────────────────── */

void cairn_uplink_slot_announced(cairn_uplink_t *u) { u->announced = true; }

void cairn_uplink_slot_started(cairn_uplink_t *u, uint32_t now)
{
    u->state = CAIRN_UL_WIFI_SLOT;
    u->slot_started_ms = now;
    u->session_open = true;
    u->slots_used++;
    u->slots_total++;
    u->announced = false;
    u->abort_ordered = false;
}

void cairn_uplink_slot_finished(cairn_uplink_t *u, const cairn_slot_result_t *r, uint32_t now)
{
    u->last_result = *r;
    if (r->aborted_by != CAIRN_UL_REASON_NONE) u->slots_aborted++;
    if (r->committed > 0) {
        u->progress_this_session = true;
        u->backoff_ms = 0;
    }
    u->abort_ordered = false;
    u->next_slot_ok_ms = now + u->cfg.min_slot_gap_ms;

    /* Always back on BLE, and always to say what happened. */
    u->state = CAIRN_UL_BLE_CHECKIN;
    u->checkin_due = true;
    u->checkin_since_ms = now;
    u->checkin_in_flight = false;
}

void cairn_uplink_checkin_done(cairn_uplink_t *u, uint32_t now)
{
    (void)now;
    u->checkin_due = false;
    u->checkin_in_flight = false;
    if (u->state == CAIRN_UL_BLE_CHECKIN) u->state = CAIRN_UL_BLE_HOME;
}

void cairn_uplink_scan_started(cairn_uplink_t *u, uint32_t now)
{
    u->state = CAIRN_UL_WIFI_SCAN;
    u->scan_started_ms = now;
    u->abort_ordered = false;
}

void cairn_uplink_scan_done(cairn_uplink_t *u, bool hit, uint32_t now)
{
    u->scanned_ever = true;
    u->last_scan_ms = now;
    u->abort_ordered = false;
    if (hit) {
        u->scan_hit_valid = true;
        u->scan_hit_until_ms = now + u->cfg.scan_hit_ttl_ms;
    }
    if (u->state == CAIRN_UL_WIFI_SCAN) u->state = CAIRN_UL_BLE_HOME;
}

void cairn_uplink_lte_started(cairn_uplink_t *u) { u->lte_in_flight = true; }

void cairn_uplink_lte_finished(cairn_uplink_t *u, bool ok, uint32_t now)
{
    u->lte_in_flight = false;
    u->lte_next_ok_ms = now + u->cfg.lte_retry_ms;
    if (ok) u->lte_sent_since_trip = true;
}

/* ── per-bundle delivery ──────────────────────────────────────────────────── */

size_t cairn_uplink_path_order(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                               cairn_path_t out[CAIRN_PATH_COUNT])
{
    size_t n = 0;
    bool wifi_active = (u->state == CAIRN_UL_WIFI_SLOT);

    if (wifi_active) {
        /* The radio is on Wi-Fi: BLE is down for the whole slot. */
        if (ev->path[CAIRN_PATH_WIFI].configured && ev->path[CAIRN_PATH_WIFI].allowed)
            out[n++] = CAIRN_PATH_WIFI;
    } else if (ev->phone_connected && ev->path[CAIRN_PATH_BLE].available &&
               ev->path[CAIRN_PATH_BLE].allowed) {
        out[n++] = CAIRN_PATH_BLE;
    }

    /* LTE last: it costs data. Whole bundles only when the user has allowed them. */
    const cairn_path_info_t *l = &ev->path[CAIRN_PATH_LTE];
    if (ev->lte_full_bundles_ok && l->configured && l->allowed && l->available)
        out[n++] = CAIRN_PATH_LTE;

    return n;
}

cairn_delivery_t cairn_uplink_deliver(cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                      const cairn_uplink_io_t *io, const cairn_bundle_ref_t *b,
                                      bool (*should_abort)(void *), void *abort_ctx)
{
    cairn_delivery_t d;
    cairn_path_t order[CAIRN_PATH_COUNT];
    uint8_t receipt[256];

    memset(&d, 0, sizeof d);
    size_t n = cairn_uplink_path_order(u, ev, order);

    for (size_t i = 0; i < n; i++) {
        const cairn_transport_t *t = &io->path[order[i]];
        if (t->transfer == NULL) continue;

        size_t receipt_len = 0;
        cairn_xfer_stats_t st;
        memset(&st, 0, sizeof st);

        d.paths_tried++;
        cairn_xfer_result_t r = t->transfer(t->ctx, b, should_abort, abort_ctx,
                                            receipt, sizeof receipt, &receipt_len, &st);
        d.bytes += st.bytes;

        if (r == CAIRN_XFER_ABORTED) { d.aborted = true; return d; }

        if (r == CAIRN_XFER_RECEIPT) {
            if (receipt_len <= sizeof receipt && io->prune_if_receipted != NULL &&
                io->prune_if_receipted(io->prune_ctx, b, receipt, receipt_len)) {
                d.delivered = true;
                return d;
            }
            /* The gate refused it. Nothing was deleted; treat the path as having failed. */
            d.failures++;
            continue;
        }

        d.failures++;   /* PARTIAL or FAILED: the next path resumes from what was accepted */
    }
    return d;
}
