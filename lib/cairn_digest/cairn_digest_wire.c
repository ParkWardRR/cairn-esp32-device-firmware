/*
 * PROVISIONAL serialiser for the digest. NOT the contract format.
 *
 * contracts/digest/v1 is unreleased, so this layout is a placeholder chosen to
 * make the byte budget real and measurable: byte-oriented, little machinery, easy
 * to decode in a test. Its first byte after "CDG" is 0xD0, a DRAFT marker; a
 * consumer must treat anything starting that way as unreleased. When the contract
 * lands this file is replaced and the reducer (cairn_digest.c) is untouched.
 *
 *   magic      "CDG" 0xD0                              4
 *   flags      u8  b0 route times, b1 events dropped, b2 route reduced to fit
 *   trip_root[32] vehicle[16] assignment[16]
 *   varint profile_id, profile_version
 *   varint start_ms, duration_s
 *   varint distance_m, max_speed_cmps, moving_time_s, fixes_used, fixes_rejected
 *   u8 channel mask; per present channel: varint count, zigzag min, max, mean
 *   varint health_flags
 *   u8 event count; per event: u8 type, varint t_s
 *   u8 coord exponent (5: coordinates are 1e-5 degree, about 1.1 m)
 *   varint point count
 *   point 0: zigzag lat, zigzag lon [, varint t_s]   (absolute, quantised)
 *   point i: zigzag dlat, zigzag dlon [, varint dt_s] (against the previous point)
 *
 * Every write goes through a bounded writer: past the budget it records overflow
 * and writes nothing, so no code path here can emit a byte beyond the budget.
 */

#include <string.h>

#include "cairn_digest.h"
#include "cairn_digest_internal.h"

#define NONE 0xFFFFu

#define FLAG_ROUTE_TIMES   0x01
#define FLAG_EVENTS_DROP   0x02
#define FLAG_ROUTE_REDUCED 0x04

#define COORD_EXP 5
#define COORD_DIV 100   /* 1e-7 -> 1e-5 degrees */

typedef struct {
    uint8_t *out;
    uint32_t cap;
    uint32_t len;
    bool     overflow;
} writer_t;

static void put_u8(writer_t *w, uint8_t b)
{
    if (w->overflow || w->len >= w->cap) {
        w->overflow = true;
        return;
    }
    w->out[w->len++] = b;
}

static void put_bytes(writer_t *w, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) put_u8(w, p[i]);
}

static void put_varint(writer_t *w, uint64_t v)
{
    while (v >= 0x80) {
        put_u8(w, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    put_u8(w, (uint8_t)v);
}

static uint64_t zigzag(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }

static void put_zigzag(writer_t *w, int64_t v) { put_varint(w, zigzag(v)); }

static int32_t quant(int32_t e7)
{
    return (e7 >= 0) ? (e7 + COORD_DIV / 2) / COORD_DIV : (e7 - COORD_DIV / 2) / COORD_DIV;
}

size_t cairn_digest_wire_encode(const cairn_digest_t *d, const cairn_digest_meta_t *meta,
                                uint32_t budget, bool with_events, uint8_t *out)
{
    writer_t w = { out, budget, 0, false };
    const cairn_digest_stats_t *st = &d->st;

    put_u8(&w, 'C');
    put_u8(&w, 'D');
    put_u8(&w, 'G');
    put_u8(&w, 0xD0);

    uint8_t flags = 0;
    if (d->cfg.route_times) flags |= FLAG_ROUTE_TIMES;
    if (d->events_dropped_by_budget || st->events_dropped > 0 || !with_events) {
        flags |= FLAG_EVENTS_DROP;
    }
    if (d->route_reduced_by_budget) flags |= FLAG_ROUTE_REDUCED;
    put_u8(&w, flags);

    put_bytes(&w, meta->trip_root, 32);
    put_bytes(&w, meta->vehicle_id, 16);
    put_bytes(&w, meta->assignment_id, 16);
    put_varint(&w, meta->engine_profile_id);
    put_varint(&w, meta->engine_profile_version);

    put_varint(&w, st->started ? st->first_t_ms : 0);
    put_varint(&w, st->started ? (st->last_t_ms - st->first_t_ms + 500) / 1000 : 0);
    put_varint(&w, (st->distance_mm + 500) / 1000);
    put_varint(&w, st->max_speed_cmps);
    put_varint(&w, (st->moving_time_ms + 500) / 1000);
    put_varint(&w, st->fixes_used);
    put_varint(&w, st->fixes_rejected);

    uint8_t mask = 0;
    for (uint32_t i = 0; i < CAIRN_DIGEST_NCHAN; i++) {
        if (st->ch[i].count > 0) mask |= (uint8_t)(1u << i);
    }
    put_u8(&w, mask);
    for (uint32_t i = 0; i < CAIRN_DIGEST_NCHAN; i++) {
        if (!(mask & (1u << i))) continue;
        put_varint(&w, st->ch[i].count);
        put_zigzag(&w, st->ch[i].min);
        put_zigzag(&w, st->ch[i].max);
        put_zigzag(&w, st->ch[i].sum / (int64_t)st->ch[i].count);
    }

    put_varint(&w, st->health_flags);

    uint8_t nev = with_events ? d->ev_count : 0;
    put_u8(&w, nev);
    for (uint8_t i = 0; i < nev; i++) {
        put_u8(&w, d->ev[i].type);
        put_varint(&w, d->ev[i].t_s);
    }

    put_u8(&w, COORD_EXP);
    put_varint(&w, d->count);

    int32_t  pl = 0, po = 0;
    uint32_t pt = 0;
    bool     first = true;
    for (uint16_t i = d->head; i != NONE; i = d->pt[i].next) {
        int32_t  ql = quant(d->pt[i].lat_e7), qo = quant(d->pt[i].lon_e7);
        uint32_t ts = d->pt[i].t_s;
        if (first) {
            put_zigzag(&w, ql);
            put_zigzag(&w, qo);
            if (d->cfg.route_times) put_varint(&w, ts);
            first = false;
        } else {
            put_zigzag(&w, (int64_t)ql - pl);
            put_zigzag(&w, (int64_t)qo - po);
            if (d->cfg.route_times) put_varint(&w, ts - pt);
        }
        pl = ql;
        po = qo;
        pt = ts;
    }

    return w.overflow ? 0 : w.len;
}
