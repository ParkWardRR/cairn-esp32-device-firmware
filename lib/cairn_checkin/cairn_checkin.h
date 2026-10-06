/*
 * BLE check-in: signed instructions and the home trigger (contracts/ble/v1/checkin.md,
 * contracts-v0.2.0, draft).
 *
 * When the dongle is back on BLE after a Wi-Fi slot the phone may deliver a small, closed set
 * of instructions. This is a remote-control surface, so it is deliberately boring: four types,
 * no code, no free-form parameters, and every one signed by the SERVER with an instruction key
 * pinned in firmware. The phone only carries them; it cannot forge, alter or replay one, and
 * the dongle never needs to know who the phone is.
 *
 * Portable C. The effects (start an upload, stop trying, hand a body to the configuration
 * receiver) and the clock, the trip state and the durable counter floor are passed in, so the
 * whole thing runs on the host against the contract's vectors.
 *
 * Not wired into the firmware yet, on purpose: it needs the pinned instruction public key
 * (a build-time trust anchor, like the receipt key) and the GATT characteristics 0042 to 0044,
 * and a remote-control surface should not go live untested on a car. Until it is wired, the
 * capability bits for instructions and the home trigger stay clear in DEVICE_INFO.
 */

#ifndef CAIRN_CHECKIN_H
#define CAIRN_CHECKIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAIRN_CI_MAX_FRAME   512
#define CAIRN_CI_MIN_FRAME   72      /* 8 header + 0 body + 64 signature */
#define CAIRN_CI_MAX_CONFIG  440
#define CAIRN_CI_RESULT_LEN  8

typedef enum {
    CAIRN_CI_UPLOAD_NOW  = 0x01,
    CAIRN_CI_STOP_TRYING = 0x02,
    CAIRN_CI_CONFIG      = 0x03,
    CAIRN_CI_CLEAR_STOP  = 0x04
} cairn_ci_type_t;

/* INSTRUCTION_RESULT status, exactly the contract's table. */
typedef enum {
    CAIRN_CI_APPLIED = 0,
    CAIRN_CI_BAD_SIGNATURE,
    CAIRN_CI_REPLAY,
    CAIRN_CI_UNKNOWN_TYPE,
    CAIRN_CI_BAD_LENGTH,
    CAIRN_CI_RATE_LIMITED,
    CAIRN_CI_TRIP_ACTIVE,
    CAIRN_CI_ENCRYPTION_REQUIRED,
    CAIRN_CI_OUT_OF_RANGE,
    /*
     * NOT in the contract: the counter floor could not be made durable. The contract's table
     * has no row for it. It is returned so the phone gets an answer other than "applied", and
     * the phone treats an unknown status as "retry later". Raise it with the contract before
     * relying on it.
     */
    CAIRN_CI_STORE_FAILED = 9
} cairn_ci_status_t;

const char *cairn_ci_status_name(cairn_ci_status_t s);

typedef struct {
    void *ctx;

    bool (*trip_active)(void *ctx);                 /* never apply during a trip */

    /* Effects. Called only for an instruction that passed every check. */
    void (*upload_now)(void *ctx);
    void (*stop_trying)(void *ctx, uint16_t hours); /* network uplink only; BLE offload unaffected */
    void (*clear_stop)(void *ctx);

    /*
     * Hand the body of a CONFIG instruction to the configuration verifier (config/v1), opaque
     * to this layer. Return APPLIED, ENCRYPTION_REQUIRED (it carried a credential the dongle
     * may not hold yet), or another status that says why not. NULL means no configuration
     * verifier exists: the instruction is refused as UNKNOWN_TYPE.
     */
    cairn_ci_status_t (*config)(void *ctx, const uint8_t *body, size_t len);

    /* Make the counter floor durable. Must not return true until it is. */
    bool (*persist_floor)(void *ctx, uint32_t counter);
} cairn_checkin_ops_t;

typedef struct {
    uint8_t  key[32];            /* the pinned instruction public key (Ed25519) */
    uint8_t  device_id[16];
    uint32_t counter_floor;      /* the highest applied; loaded from durable storage at boot */

    /* rate limit: 1 per 2 s, 30 per hour */
    bool     have_last;
    uint32_t last_ms;
    uint32_t hour_start_ms;
    uint16_t hour_count;

    /* home trigger */
    bool     home;
    uint32_t home_until_ms;
    bool     have_seq;
    uint16_t last_seq;
    bool     have_trigger_ms;
    uint32_t last_trigger_ms;

    uint8_t  sig_msg[15 + 16 + CAIRN_CI_MAX_FRAME];   /* the signed bytes, contiguous */
} cairn_checkin_t;

void cairn_checkin_init(cairn_checkin_t *c, const uint8_t instruction_key[32],
                        const uint8_t device_id[16], uint32_t counter_floor);

/*
 * Process one INSTRUCTION write. Always fills `result` (8 bytes: status, type, 0, 0, floor LE)
 * with the answer to indicate, and returns the status. The floor in the answer is the floor
 * AFTER this frame: it moves only when the instruction was applied.
 */
cairn_ci_status_t cairn_checkin_instruction(cairn_checkin_t *c, const cairn_checkin_ops_t *ops,
                                            const uint8_t *frame, size_t len, uint32_t now_ms,
                                            uint8_t result[CAIRN_CI_RESULT_LEN]);

/* ── HOME_TRIGGER ─────────────────────────────────────────────────────────── */

#define CAIRN_CI_HOME_LEN         6
#define CAIRN_CI_HOME_MAX_SECONDS 900

typedef enum {
    CAIRN_CI_HOME_ACCEPTED = 0,
    CAIRN_CI_HOME_REFUSED,       /* not 6 bytes, a reserved bit, or valid_seconds above 900: an ATT error */
    CAIRN_CI_HOME_IGNORED        /* a stale or repeated seq, too soon after the last, or during a trip */
} cairn_ci_home_result_t;

/* Unsigned by design: it can only let the dongle try networks it already holds. */
cairn_ci_home_result_t cairn_checkin_home_trigger(cairn_checkin_t *c, const uint8_t *frame, size_t len,
                                                  uint32_t now_ms, bool trip_active);

/* Whether the phone has said "you may use Wi-Fi now" and it has not yet expired. RAM only:
 * it is not held across a reboot. */
bool cairn_checkin_home_asserted(cairn_checkin_t *c, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_CHECKIN_H */
