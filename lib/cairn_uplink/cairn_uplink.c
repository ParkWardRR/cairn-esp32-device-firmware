#include "cairn_uplink.h"

#include <string.h>

/* Millisecond clocks wrap; every comparison is on the signed difference. */
static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

const char *cairn_path_name(cairn_path_t p)
{
    switch (p) {
    case CAIRN_PATH_BLE: return "ble";
    case CAIRN_PATH_LTE: return "lte";
    default:             return "?";
    }
}

void cairn_uplink_config_defaults(cairn_uplink_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->lte_auto          = true;    /* the owner's proposed default; still open */
    c->lte_after_trip_ms = 60u * 1000u;
    c->lte_retry_ms      = 15u * 60u * 1000u;
}

void cairn_uplink_init(cairn_uplink_t *u, const cairn_uplink_config_t *cfg)
{
    memset(u, 0, sizeof *u);
    u->cfg = *cfg;
}

/* ── the gate ─────────────────────────────────────────────────────────────── */

static bool lte_may_send(const cairn_uplink_t *u, const cairn_uplink_evidence_t *ev, uint32_t now)
{
    const cairn_path_info_t *p = &ev->path[CAIRN_PATH_LTE];

    if (!u->cfg.lte_auto || u->lte_in_flight)               return false;
    if (!p->configured || !p->allowed || !p->available)     return false;
    if (ev->trip_active || ev->bundles_waiting == 0)        return false;
    if (!ev->battery_ok)                                    return false;
    /* Away from home and with no phone to do it for free. */
    if (ev->phone_connected)                                return false;
    if (ev->home != CAIRN_HOME_NONE)                        return false;
    /* After a trip, not during one, and not before the car has settled. */
    if (!u->trip_ended_known)                               return false;
    if (!reached(now, u->trip_ended_ms + u->cfg.lte_after_trip_ms)) return false;
    if (!reached(now, u->lte_next_ok_ms))                   return false;
    return true;
}

/* ── tick ─────────────────────────────────────────────────────────────────── */

cairn_uplink_action_t cairn_uplink_tick(cairn_uplink_t *u, const cairn_uplink_evidence_t *ev,
                                        uint32_t now)
{
    cairn_uplink_action_t a = { CAIRN_UL_ACT_NONE };

    /* The trip edge: the end starts the LTE clock. */
    if (ev->trip_active) {
        u->trip_was_active = true;
        u->trip_ended_known = false;
        u->lte_sent_since_trip = false;
    } else if (u->trip_was_active) {
        u->trip_was_active = false;
        u->trip_ended_known = true;
        u->trip_ended_ms = now;
    } else if (!u->trip_ended_known) {
        /*
         * Cold start with no trip running: adopt boot as the trip end (#36).
         *
         * trip_ended_known is zeroed by init, so before this branch existed a
         * power cycle meant the manager had no trip edge to point at and LTE
         * would not fire at all. Bundles already sealed on the card sat there
         * until some *later* drive happened to end — which is the one situation
         * where the car is parked, away, holding data, and cellular is the only
         * way it can leave.
         *
         * Taking boot as the edge is not a guess about the past. Bundles on the
         * card are themselves the evidence that a trip happened, and
         * ev->trip_active already distinguishes parked from driving, so the
         * refusal was never protecting against a wrong answer — only against an
         * unobserved one.
         *
         * Anchoring at `now` rather than zero keeps the settle delay honest:
         * lte_after_trip_ms still has to elapse from boot, so the phone keeps
         * its free first attempt. And a reboot that lands mid-drive corrects
         * itself — capture arms a few seconds later, trip_active goes true, and
         * the branch above clears this again.
         */
        u->trip_ended_known = true;
        u->trip_ended_ms = now;
    }

    if (lte_may_send(u, ev, now)) a.kind = CAIRN_UL_ACT_LTE_SEND;
    return a;
}

/* ── reports from the caller ──────────────────────────────────────────────── */

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
    (void)u;

    if (ev->phone_connected && ev->path[CAIRN_PATH_BLE].available &&
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
