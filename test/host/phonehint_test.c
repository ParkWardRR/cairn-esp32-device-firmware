/*
 * The phone's position hint: when it is sent, how honest it is, and the one thing it may
 * never do.
 *
 * A `GNSS_FIX` write with companion_flags bit 1 is a cached position, deliberately sent
 * past the staleness rule so a cold receiver has somewhere to start. Everything else in
 * ble/v1 exists to stop an old fix being read as a current one, so the rules around this
 * one exception are worth testing by themselves.
 */

#include <stdio.h>

#include "cairn_phonehint.h"

static int g_pass, g_fail;

#define CHECK(name, cond)                                                      \
    do {                                                                       \
        if (cond) { g_pass++; printf("  pass  [phonehint] %s\n", name); }      \
        else      { g_fail++; printf("  FAIL  [phonehint] %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

static cairn_phonehint_state_t fresh_start(void)
{
    cairn_phonehint_state_t s;
    s.receiver_fixed      = false;
    s.best_acc_cm         = 0u;
    s.receiver_assistable = true;
    return s;
}

static cairn_phonehint_frame_t a_hint(uint16_t h_acc_cm)
{
    cairn_phonehint_frame_t f;
    f.companion_flags = CAIRN_COMPANION_FLAG_HINT;
    f.validity_flags  = CAIRN_GNSS_VALID_POSITION;
    f.h_acc_cm        = h_acc_cm;
    return f;
}

int main(void)
{
    uint32_t acc = 0;
    cairn_phonehint_state_t s;
    cairn_phonehint_frame_t f;

    /* ── what counts as a hint ───────────────────────────────────────────── */

    s = fresh_start();
    f = a_hint(800); /* 8 m: a good urban fix */
    CHECK("a frame flagged HINT with a position is sent",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_SEND);

    f.companion_flags = 0u;
    CHECK("an ordinary live fix is not a hint and takes the usual path",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_NOT_A_HINT);

    f.companion_flags = CAIRN_COMPANION_FLAG_CHARGING;
    CHECK("the charging bit alone is not a hint",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_NOT_A_HINT);

    f.companion_flags = CAIRN_COMPANION_FLAG_HINT | CAIRN_COMPANION_FLAG_CHARGING;
    CHECK("a hint from a charging phone is still a hint",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_SEND);

    f = a_hint(800);
    f.validity_flags = 0u;
    CHECK("a hint with no position is nothing",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_NO_POSITION);

    /* ── the one thing it may never do ───────────────────────────────────── */

    CHECK("a hint frame is never stored as a position sample",
          !cairn_phonehint_is_recordable(CAIRN_COMPANION_FLAG_HINT));
    CHECK("a hint from a charging phone is still never stored",
          !cairn_phonehint_is_recordable(CAIRN_COMPANION_FLAG_HINT | CAIRN_COMPANION_FLAG_CHARGING));
    CHECK("an ordinary fix is stored as always",
          cairn_phonehint_is_recordable(0u));
    CHECK("so is one from a charging phone",
          cairn_phonehint_is_recordable(CAIRN_COMPANION_FLAG_CHARGING));

    /* ── the accuracy floor ──────────────────────────────────────────────── */

    CHECK("a 5 m phone fix is still told to the receiver as 300 m",
          cairn_phonehint_accuracy_cm(500u) == CAIRN_PHONEHINT_FLOOR_CM);
    CHECK("exactly at the floor is the floor",
          cairn_phonehint_accuracy_cm(30000u) == CAIRN_PHONEHINT_FLOOR_CM);
    CHECK("a worse fix is reported as it is, not flattered",
          cairn_phonehint_accuracy_cm(45000u) == 45000u);
    CHECK("an accuracy the phone called invalid is the floor, not a refusal",
          cairn_phonehint_accuracy_cm(CAIRN_PHONEHINT_ACC_UNKNOWN) == CAIRN_PHONEHINT_FLOOR_CM);

    s = fresh_start();
    f = a_hint(500);
    acc = 0;
    (void)cairn_phonehint_decide(&s, &f, &acc);
    CHECK("the decision hands back the floored accuracy, not the phone's own",
          acc == CAIRN_PHONEHINT_FLOOR_CM);

    /* ── the best hint wins ──────────────────────────────────────────────── */

    s = fresh_start();
    s.best_acc_cm = 1000000u; /* the last saved fix: +/- 10 km */
    f = a_hint(1200);
    acc = 0;
    CHECK("a phone fix beats the +/- 10 km last-fix hint",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_SEND && acc == CAIRN_PHONEHINT_FLOOR_CM);

    s.best_acc_cm = 15000u; /* a Wi-Fi anchor fix that placed the car to 150 m */
    CHECK("a phone fix does not override a better hint already sent",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_NOT_BETTER);

    s.best_acc_cm = CAIRN_PHONEHINT_FLOOR_CM;
    CHECK("nor an equally good one: the receiver keeps what it has",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_NOT_BETTER);

    s.best_acc_cm = 60000u;
    CHECK("but it does override a worse one",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_SEND);

    s = fresh_start();
    f = a_hint(60000u); /* 600 m: a phone with only a cell-tower fix. The contract clamps
                           h_acc_cm at 65534, so this is near the worst it can state. */
    s.best_acc_cm = 15000u;
    CHECK("a vague phone fix loses to a sharp Wi-Fi one, which is the point of ordering by accuracy",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_NOT_BETTER);

    /* ── when there is nothing to hint ───────────────────────────────────── */

    s = fresh_start();
    s.receiver_fixed = true;
    f = a_hint(500);
    CHECK("a receiver that has found the sky needs no help",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_RECEIVER_FIXED);

    s = fresh_start();
    s.receiver_assistable = false;
    CHECK("an external receiver takes no hints through the co-processor",
          cairn_phonehint_decide(&s, &f, &acc) == CAIRN_PHONEHINT_NO_RECEIVER);

    /* ── it survives being asked nothing ─────────────────────────────────── */

    CHECK("a null state is not a hint", cairn_phonehint_decide(NULL, &f, &acc) == CAIRN_PHONEHINT_NOT_A_HINT);
    s = fresh_start();
    CHECK("a null frame is not a hint", cairn_phonehint_decide(&s, NULL, &acc) == CAIRN_PHONEHINT_NOT_A_HINT);
    f = a_hint(500);
    CHECK("no output pointer is fine", cairn_phonehint_decide(&s, &f, NULL) == CAIRN_PHONEHINT_SEND);

    /* ── every verdict has a name, because they end up in the SD log ─────── */

    int unnamed = 0;
    for (int v = CAIRN_PHONEHINT_SEND; v <= CAIRN_PHONEHINT_NOT_BETTER; v++) {
        const char *n = cairn_phonehint_verdict_name((cairn_phonehint_verdict_t)v);
        if (n == NULL || n[0] == '\0') unnamed = 1;
    }
    CHECK("every verdict has a token", !unnamed);

    printf("\nphonehint: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
