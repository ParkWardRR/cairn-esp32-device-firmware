/*
 * Whether a phone's cached fix should be handed to the receiver, and how honest to be
 * about it.
 *
 * The receiver sits low under the dash behind the Freematics co-processor and starts
 * cold. At each start it is told where the car was last fixed, at +/- 10 km
 * (sensors.cpp), and then, about 14 s in, where the Wi-Fi anchors put it if they can
 * (gnss_hint.cpp). A phone that has just been in the owner's pocket knows better than
 * either, usually to within tens of metres -- and it is connected over BLE before the
 * first scan has even run.
 *
 * So: a `GNSS_FIX` write with `companion_flags` bit 1 (`HINT`, ble/v1) is not a
 * measurement and is never stored as one. It is a position for `UBX-MGA-INI-POS_LLH`,
 * and nothing else.
 *
 * Two rules decide, and both are here rather than in the Arduino source so the host
 * tests exercise exactly what the device runs (the pattern cairn_blesec set).
 *
 * **The accuracy floor.** A phone's own `h_acc` is honest about the phone, which was in
 * a pocket a minute ago and is now somewhere in a car. The fix is minutes old by
 * construction -- that is what makes it a hint -- so its accuracy understates how far
 * the car may have moved since. An over-confident hint is the one way a hint can hurt:
 * it can pull a receiver's first solutions off and cost more than it saves. Floor it at
 * 300 m, which covers a car that has been rolled out of a driveway and still leaves the
 * hint two orders of magnitude better than the +/- 10 km last-fix one.
 *
 * **The best hint wins.** Three sources can place the car in one receiver start, and the
 * receiver keeps whichever arrived last. Ordering them by arrival would make the answer
 * depend on when a phone happened to connect. Ordering them by stated accuracy does not,
 * and it is the ordering that is actually meant: a hint is only ever sent when it is
 * strictly better than the best one already sent this start.
 *
 * Nothing here is acknowledged -- the co-processor returns no UBX replies (bench,
 * 2026-10-26) -- so the measure is the time-to-first-fix line, with the phone and
 * without it.
 */
#ifndef CAIRN_PHONEHINT_H
#define CAIRN_PHONEHINT_H

#include <stdbool.h>
#include <stdint.h>

/* contracts/ble/v1/spec.md, GNSS_FIX byte 26. */
#define CAIRN_COMPANION_FLAG_CHARGING 0x01u
#define CAIRN_COMPANION_FLAG_HINT     0x02u

/* GNSS_FIX byte 21, bit 0. A hint with no position is nothing. */
#define CAIRN_GNSS_VALID_POSITION 0x01u

/* The floor, in centimetres. See the header comment: 300 m. */
#define CAIRN_PHONEHINT_FLOOR_CM 30000u

/* h_acc_cm == 0xFFFF means the phone called its own accuracy invalid. */
#define CAIRN_PHONEHINT_ACC_UNKNOWN 0xFFFFu

/* What the receiver has already been told this start. */
typedef struct {
    /* Non-zero once the receiver has accepted a fix of its own: hints stop mattering. */
    bool     receiver_fixed;
    /* Accuracy of the best hint sent this receiver start, in cm; 0 when none has gone. */
    uint32_t best_acc_cm;
    /* False when there is no internal receiver to send to (external GNSS, or none). */
    bool     receiver_assistable;
} cairn_phonehint_state_t;

/* One `GNSS_FIX` write, as it arrived. */
typedef struct {
    uint8_t  companion_flags; /* byte 26 */
    uint8_t  validity_flags;  /* byte 21 */
    uint16_t h_acc_cm;        /* bytes 16..17 */
} cairn_phonehint_frame_t;

typedef enum {
    CAIRN_PHONEHINT_SEND = 0,
    /* An ordinary live fix. Not a hint; the staleness rules apply to it as always. */
    CAIRN_PHONEHINT_NOT_A_HINT,
    CAIRN_PHONEHINT_NO_POSITION,
    CAIRN_PHONEHINT_RECEIVER_FIXED,
    CAIRN_PHONEHINT_NO_RECEIVER,
    /* A hint already went that was at least as good. */
    CAIRN_PHONEHINT_NOT_BETTER,
} cairn_phonehint_verdict_t;

/* Short tokens, meant to be grepped and compared across drives. */
static inline const char *cairn_phonehint_verdict_name(cairn_phonehint_verdict_t v)
{
    switch (v) {
    case CAIRN_PHONEHINT_SEND:           return "sent";
    case CAIRN_PHONEHINT_NOT_A_HINT:     return "not-a-hint";
    case CAIRN_PHONEHINT_NO_POSITION:    return "no-position";
    case CAIRN_PHONEHINT_RECEIVER_FIXED: return "already-fixed";
    case CAIRN_PHONEHINT_NO_RECEIVER:    return "no-receiver";
    case CAIRN_PHONEHINT_NOT_BETTER:     return "not-better";
    default:                             return "unknown";
    }
}

static inline bool cairn_phonehint_is_hint(uint8_t companion_flags)
{
    return (companion_flags & CAIRN_COMPANION_FLAG_HINT) != 0u;
}

/* What to tell the receiver this hint is worth, in centimetres. Never below the floor,
 * and an unknown accuracy is simply the floor rather than a refusal: the position is
 * still better than 10 km. */
static inline uint32_t cairn_phonehint_accuracy_cm(uint16_t h_acc_cm)
{
    uint32_t acc = (h_acc_cm == CAIRN_PHONEHINT_ACC_UNKNOWN) ? 0u : (uint32_t)h_acc_cm;
    return acc < CAIRN_PHONEHINT_FLOOR_CM ? CAIRN_PHONEHINT_FLOOR_CM : acc;
}

/* The decision. `acc_cm_out` is filled on SEND and left alone otherwise. */
static inline cairn_phonehint_verdict_t cairn_phonehint_decide(
    const cairn_phonehint_state_t *s, const cairn_phonehint_frame_t *f, uint32_t *acc_cm_out)
{
    if (s == NULL || f == NULL) return CAIRN_PHONEHINT_NOT_A_HINT;
    if (!cairn_phonehint_is_hint(f->companion_flags)) return CAIRN_PHONEHINT_NOT_A_HINT;
    if ((f->validity_flags & CAIRN_GNSS_VALID_POSITION) == 0u) return CAIRN_PHONEHINT_NO_POSITION;
    if (s->receiver_fixed) return CAIRN_PHONEHINT_RECEIVER_FIXED;
    if (!s->receiver_assistable) return CAIRN_PHONEHINT_NO_RECEIVER;

    uint32_t acc = cairn_phonehint_accuracy_cm(f->h_acc_cm);
    if (s->best_acc_cm != 0u && acc >= s->best_acc_cm) return CAIRN_PHONEHINT_NOT_BETTER;

    if (acc_cm_out != NULL) *acc_cm_out = acc;
    return CAIRN_PHONEHINT_SEND;
}

/* Whether a hint frame may be stored as a position sample. It may not, ever: it is a
 * cached fix, deliberately sent past the staleness rule, and ble/v1 is explicit that it
 * never becomes `last_gnss`, never clears DEGRADED_GNSS and never influences trip
 * scoring. Spelled out as a function so the host test can assert it and so the one call
 * site reads as the rule rather than as a negation. */
static inline bool cairn_phonehint_is_recordable(uint8_t companion_flags)
{
    return !cairn_phonehint_is_hint(companion_flags);
}

#endif /* CAIRN_PHONEHINT_H */
