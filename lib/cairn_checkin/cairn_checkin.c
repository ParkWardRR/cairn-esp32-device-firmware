#include "cairn_checkin.h"

#include <string.h>

#include "cairn_format.h"

static const char CONTEXT[] = "CAIRN-INSTR-V1";   /* 14 bytes, then a 0x00 */

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Millisecond clocks wrap; compare the signed difference. */
static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

const char *cairn_ci_status_name(cairn_ci_status_t s)
{
    switch (s) {
    case CAIRN_CI_APPLIED:             return "applied";
    case CAIRN_CI_BAD_SIGNATURE:       return "bad_signature";
    case CAIRN_CI_REPLAY:              return "replay";
    case CAIRN_CI_UNKNOWN_TYPE:        return "unknown_type";
    case CAIRN_CI_BAD_LENGTH:          return "bad_length";
    case CAIRN_CI_RATE_LIMITED:        return "rate_limited";
    case CAIRN_CI_TRIP_ACTIVE:         return "trip_active";
    case CAIRN_CI_ENCRYPTION_REQUIRED: return "encryption_required";
    case CAIRN_CI_OUT_OF_RANGE:        return "out_of_range";
    case CAIRN_CI_STORE_FAILED:        return "store_failed";
    default:                           return "?";
    }
}

void cairn_checkin_init(cairn_checkin_t *c, const uint8_t instruction_key[32],
                        const uint8_t device_id[16], uint32_t counter_floor)
{
    memset(c, 0, sizeof *c);
    memcpy(c->key, instruction_key, 32);
    memcpy(c->device_id, device_id, 16);
    c->counter_floor = counter_floor;
}

/* The answer for the phone: status, the frame's type byte, two reserved bytes, the floor. */
static cairn_ci_status_t answer(const cairn_checkin_t *c, cairn_ci_status_t st, uint8_t type,
                                uint8_t result[CAIRN_CI_RESULT_LEN])
{
    result[0] = (uint8_t)st;
    result[1] = type;
    result[2] = 0;
    result[3] = 0;
    result[4] = (uint8_t)c->counter_floor;
    result[5] = (uint8_t)(c->counter_floor >> 8);
    result[6] = (uint8_t)(c->counter_floor >> 16);
    result[7] = (uint8_t)(c->counter_floor >> 24);
    return st;
}

/* 1 per 2 s and 30 per hour, counted for frames that reach this check. A refused frame does
 * not move the window, so a flood cannot lock out the next genuine one for longer than its
 * own rate. */
static bool rate_ok(cairn_checkin_t *c, uint32_t now)
{
    if (c->have_last && !reached(now, c->last_ms + 2000u)) return false;

    uint32_t hour_start = c->hour_start_ms;
    uint16_t hour_count = c->hour_count;
    if (!c->have_last || reached(now, hour_start + 3600u * 1000u)) {
        hour_start = now;
        hour_count = 0;
    }
    if (hour_count >= 30) return false;

    c->have_last = true;
    c->last_ms = now;
    c->hour_start_ms = hour_start;
    c->hour_count = (uint16_t)(hour_count + 1);
    return true;
}

cairn_ci_status_t cairn_checkin_instruction(cairn_checkin_t *c, const cairn_checkin_ops_t *ops,
                                            const uint8_t *frame, size_t len, uint32_t now_ms,
                                            uint8_t result[CAIRN_CI_RESULT_LEN])
{
    uint8_t type = (len >= 2) ? frame[1] : 0;

    /* 1. version, size and the length the header claims. */
    if (len < CAIRN_CI_MIN_FRAME || len > CAIRN_CI_MAX_FRAME || frame[0] != 1)
        return answer(c, CAIRN_CI_BAD_LENGTH, type, result);
    size_t body_len = get16(frame + 2);
    if (len != 8 + body_len + 64) return answer(c, CAIRN_CI_BAD_LENGTH, type, result);

    /* 2. the signature, over a context, this device and the frame up to the signature.
     * Nothing after this is looked at before it is good, so an unauthenticated writer learns
     * nothing but "rejected". */
    size_t signed_len = 8 + body_len;
    memcpy(c->sig_msg, CONTEXT, sizeof CONTEXT - 1);
    c->sig_msg[sizeof CONTEXT - 1] = 0;
    memcpy(c->sig_msg + sizeof CONTEXT, c->device_id, 16);
    memcpy(c->sig_msg + sizeof CONTEXT + 16, frame, signed_len);
    if (!cairn_ed25519_verify(c->sig_msg, sizeof CONTEXT + 16 + signed_len, frame + signed_len, c->key))
        return answer(c, CAIRN_CI_BAD_SIGNATURE, type, result);

    /* 3. the type is one of the closed set, and the body is the size the type requires. */
    switch (type) {
    case CAIRN_CI_UPLOAD_NOW:
    case CAIRN_CI_CLEAR_STOP:
        if (body_len != 0) return answer(c, CAIRN_CI_BAD_LENGTH, type, result);
        break;
    case CAIRN_CI_STOP_TRYING:
        if (body_len != 2) return answer(c, CAIRN_CI_BAD_LENGTH, type, result);
        break;
    case CAIRN_CI_CONFIG:
        if (ops->config == NULL) return answer(c, CAIRN_CI_UNKNOWN_TYPE, type, result);
        if (body_len < 1 || body_len > CAIRN_CI_MAX_CONFIG) return answer(c, CAIRN_CI_BAD_LENGTH, type, result);
        break;
    default:
        /* However well signed: the set is closed. */
        return answer(c, CAIRN_CI_UNKNOWN_TYPE, type, result);
    }

    /* 4. strictly greater than the floor: an equal counter is a replay. */
    uint32_t counter = get32(frame + 4);
    if (counter <= c->counter_floor) return answer(c, CAIRN_CI_REPLAY, type, result);

    /* 5. the body is in range. */
    uint16_t hours = 0;
    if (type == CAIRN_CI_STOP_TRYING) {
        hours = get16(frame + 8);
        if (hours < 1 || hours > 168) return answer(c, CAIRN_CI_OUT_OF_RANGE, type, result);
    }

    /* 6. not during a trip. (A CONFIG that carries a credential is refused by the
     * configuration verifier below with ENCRYPTION_REQUIRED; the contract lists that here,
     * ahead of the rate limit, but it cannot be known without running the verifier, which
     * must not run for a frame the rate limit would refuse. Only the order of two refusals
     * differs, and only when both apply.) */
    if (ops->trip_active && ops->trip_active(ops->ctx)) return answer(c, CAIRN_CI_TRIP_ACTIVE, type, result);

    /* 7. rate limit. */
    if (!rate_ok(c, now_ms)) return answer(c, CAIRN_CI_RATE_LIMITED, type, result);

    /* 8. apply, and make the floor durable before answering, so a power cut cannot make the
     * same frame valid again. A CONFIG is applied first, because the verifier may refuse it and
     * a refused instruction must not move the floor. The others take effect only after the
     * floor is durable: losing one to a power cut is harmless, replaying one is not. */
    if (type == CAIRN_CI_CONFIG) {
        cairn_ci_status_t st = ops->config(ops->ctx, frame + 8, body_len);
        if (st != CAIRN_CI_APPLIED) return answer(c, st, type, result);
        if (!ops->persist_floor || !ops->persist_floor(ops->ctx, counter))
            return answer(c, CAIRN_CI_STORE_FAILED, type, result);
        c->counter_floor = counter;
        return answer(c, CAIRN_CI_APPLIED, type, result);
    }

    if (!ops->persist_floor || !ops->persist_floor(ops->ctx, counter))
        return answer(c, CAIRN_CI_STORE_FAILED, type, result);
    c->counter_floor = counter;

    switch (type) {
    case CAIRN_CI_UPLOAD_NOW:  if (ops->upload_now)  ops->upload_now(ops->ctx);        break;
    case CAIRN_CI_STOP_TRYING: if (ops->stop_trying) ops->stop_trying(ops->ctx, hours); break;
    case CAIRN_CI_CLEAR_STOP:  if (ops->clear_stop)  ops->clear_stop(ops->ctx);        break;
    default: break;
    }
    return answer(c, CAIRN_CI_APPLIED, type, result);
}

/* ── HOME_TRIGGER ─────────────────────────────────────────────────────────── */

cairn_ci_home_result_t cairn_checkin_home_trigger(cairn_checkin_t *c, const uint8_t *frame, size_t len,
                                                  uint32_t now_ms, bool trip_active)
{
    if (len != CAIRN_CI_HOME_LEN) return CAIRN_CI_HOME_REFUSED;
    uint8_t flags = frame[0];
    uint16_t valid_s = get16(frame + 2);
    uint16_t seq = get16(frame + 4);
    if ((flags & ~1u) != 0 || valid_s > CAIRN_CI_HOME_MAX_SECONDS) return CAIRN_CI_HOME_REFUSED;

    if (trip_active) return CAIRN_CI_HOME_IGNORED;
    if (c->have_trigger_ms && !reached(now_ms, c->last_trigger_ms + 10u * 1000u)) return CAIRN_CI_HOME_IGNORED;
    /* A repeat or an older value, with the sequence number allowed to wrap. */
    if (c->have_seq && (int16_t)(uint16_t)(seq - c->last_seq) <= 0) return CAIRN_CI_HOME_IGNORED;

    c->have_seq = true;
    c->last_seq = seq;
    c->have_trigger_ms = true;
    c->last_trigger_ms = now_ms;

    if (flags & 1u) {
        c->home = true;
        c->home_until_ms = now_ms + (uint32_t)valid_s * 1000u;
    } else {
        c->home = false;           /* withdrawn at once */
    }
    return CAIRN_CI_HOME_ACCEPTED;
}

bool cairn_checkin_home_asserted(cairn_checkin_t *c, uint32_t now_ms)
{
    if (!c->home) return false;
    if (reached(now_ms, c->home_until_ms)) { c->home = false; return false; }
    return true;
}
