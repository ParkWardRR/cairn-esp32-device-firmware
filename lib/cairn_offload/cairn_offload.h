/*
 * BLE bundle offload: the dongle's half of docs/ble-offload.md.
 *
 * The dongle has no network. The enrolled phone pulls sealed bundles over BLE,
 * uploads them, and hands the server's signed receipt back. This module is the
 * whole protocol, as portable C with no Arduino or NimBLE dependency, so the
 * framing, the refusals and above all the receipt gate are exercised on the host
 * against exactly the code the device runs. A thin NimBLE shim (src/ble_offload.cpp)
 * feeds it bytes and gives it a way to send.
 *
 * What the phone is trusted with, and what it is not:
 *
 *   - It can pull ciphertext (frames are AEAD-sealed; it never holds a key).
 *   - It can NOT make this module delete anything. A bundle is pruned only
 *     through cairn_prune_if_receipted, and only on a receipt that verifies
 *     against the key pinned in firmware and names this bundle's content root.
 *     A bad receipt is rejected before anything is written to the card.
 *
 * Decisions where the spec left room (kept here so they are not rediscovered):
 *
 *   - One operation at a time. A request while one is in progress gets BUSY.
 *     A request that arrives while a previous response is still waiting for the
 *     stack to take it is dropped (the phone times out and asks again); the
 *     stack takes an indication in the write callback in every normal case.
 *   - LIST/GET_MANIFEST/READ/PUT_RECEIPT are all refused while a trip is active
 *     (TRIP_ACTIVE). Pruning competes with capture for the card just as reading
 *     does. ABORT is always honoured.
 *   - The MTU must allow at least one LIST entry (payload >= 40 bytes). Below
 *     that every request is answered IO_ERROR rather than half working.
 *   - A failed PUT_RECEIPT (bad sequence, too many bytes, stall) is answered with
 *     the 0x84 indication and a non-zero status and NO outcome byte.
 *   - PUT_RECEIPT outcome 4 (not in the original table): no key is pinned, so the
 *     receipt could not be verified; nothing was stored or deleted.
 */

#ifndef CAIRN_OFFLOAD_H
#define CAIRN_OFFLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cairn_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Control opcodes. A response carries the opcode with the high bit set. */
#define CAIRN_OFFLOAD_OP_LIST          0x01
#define CAIRN_OFFLOAD_OP_GET_MANIFEST  0x02
#define CAIRN_OFFLOAD_OP_READ          0x03
#define CAIRN_OFFLOAD_OP_PUT_RECEIPT   0x04
#define CAIRN_OFFLOAD_OP_ABORT         0x05

/* Transfer-done indication. */
#define CAIRN_OFFLOAD_IND_DONE         0x86

typedef enum {
    CAIRN_OFFLOAD_OK                 = 0,
    CAIRN_OFFLOAD_BUSY               = 1,
    CAIRN_OFFLOAD_UNKNOWN_BUNDLE     = 2,
    CAIRN_OFFLOAD_BAD_ARGUMENT       = 3,
    CAIRN_OFFLOAD_TRIP_ACTIVE        = 4,
    CAIRN_OFFLOAD_IO_ERROR           = 5,
    CAIRN_OFFLOAD_BAD_RECEIPT_LENGTH = 6,
    CAIRN_OFFLOAD_NO_TRANSFER        = 7,
} cairn_offload_status_t;

typedef enum {
    CAIRN_OFFLOAD_OUTCOME_PRUNED      = 0, /* verified, stored, pruned */
    CAIRN_OFFLOAD_OUTCOME_RETAINED    = 1, /* verified and stored; prune did not complete */
    CAIRN_OFFLOAD_OUTCOME_BAD_SIG     = 2, /* malformed, or signature does not verify */
    CAIRN_OFFLOAD_OUTCOME_WRONG_ROOT  = 3, /* genuine, but names different content */
    CAIRN_OFFLOAD_OUTCOME_NO_KEY      = 4, /* no key pinned: not verified, nothing done */
} cairn_offload_outcome_t;

#define CAIRN_OFFLOAD_MAX_READ        65536u
#define CAIRN_OFFLOAD_MAX_RECEIPT     1024u
#define CAIRN_OFFLOAD_LIST_ENTRY      29u
#define CAIRN_OFFLOAD_MAX_PAYLOAD     244u   /* ATT MTU 247 minus the 3-byte header */
#define CAIRN_OFFLOAD_MIN_PAYLOAD     40u
#define CAIRN_OFFLOAD_MAX_BUNDLES     16u
#define CAIRN_OFFLOAD_STALL_MS        8000u

/* What the shim gives the module. Every callback may be called from the shim's
 * task or from a BLE callback; none may block for long. */
typedef struct {
    void *ctx;

    /* Send one indication on OFFLOAD_CONTROL. Return false when the stack cannot
     * take it right now (the previous indication is unconfirmed); the module
     * keeps the message and retries from cairn_offload_pump(). */
    bool (*indicate)(void *ctx, const uint8_t *buf, size_t len);

    /* Send one notification on OFFLOAD_DATA. Same contract. */
    bool (*notify)(void *ctx, const uint8_t *buf, size_t len);

    uint32_t (*now_ms)(void *ctx);
    bool     (*trip_active)(void *ctx);

    /* The server's receipt public key, pinned in firmware: 32 bytes, or NULL /
     * all-zero when none is configured. */
    const uint8_t *pinned_key;
} cairn_offload_io_t;

typedef enum {
    CAIRN_OFFLOAD_IDLE = 0,
    CAIRN_OFFLOAD_STREAM,          /* data notifications (the response indication goes first) */
    CAIRN_OFFLOAD_RECEIPT_RX,
} cairn_offload_state_t;

typedef struct {
    cairn_offload_io_t io;
    uint16_t           payload;      /* bytes per indication / notification */

    cairn_offload_state_t state;
    uint32_t              last_progress_ms;

    /* The indication waiting for the stack, if any. */
    uint8_t ind[256];
    size_t  ind_len;
    bool    ind_pending;

    /* The request in progress. */
    uint8_t  op;
    uint8_t  request_id;
    char     id_text[40];

    /* A stream (GET_MANIFEST or READ). */
    bool           from_memory;       /* manifest bytes, not the bundle byte stream */
    uint64_t       remaining;
    uint64_t       sent;
    uint16_t       seq;
    uint32_t       crc;
    uint8_t        tx[2 + CAIRN_OFFLOAD_MAX_PAYLOAD];
    size_t         tx_len;            /* a prepared frame awaiting the stack */
    size_t         mem_pos;
    size_t         member;            /* READ: current member and position in it */
    uint64_t       member_pos;
    void          *file;              /* cairn_file_t * */

    /* A receipt being collected. */
    uint16_t receipt_len;
    uint16_t receipt_got;
    uint16_t receipt_seq;
    uint8_t  receipt[CAIRN_OFFLOAD_MAX_RECEIPT];
    uint8_t  content_root[32];
} cairn_offload_t;

void cairn_offload_init(cairn_offload_t *o, const cairn_offload_io_t *io);

/* Set the negotiated ATT MTU. Payload per message is mtu - 3. */
void cairn_offload_set_mtu(cairn_offload_t *o, uint16_t att_mtu);

/* A write to OFFLOAD_CONTROL. */
void cairn_offload_on_control_write(cairn_offload_t *o, const uint8_t *data, size_t len);

/* A write to OFFLOAD_DATA (receipt frames). */
void cairn_offload_on_data_write(cairn_offload_t *o, const uint8_t *data, size_t len);

/* Move things along: send a waiting indication, a few data frames, and notice
 * stalls. Call often while connected (every few ms); each call does a bounded
 * amount of work. */
void cairn_offload_pump(cairn_offload_t *o);

/* The link dropped: abandon any operation, close files, scrub the receipt buffer. */
void cairn_offload_on_disconnect(cairn_offload_t *o);

/* True while an operation or an unsent response is outstanding. The lifecycle
 * uses it to hold standby. */
bool cairn_offload_busy(const cairn_offload_t *o);

/* IEEE CRC-32, streaming. Start with 0xFFFFFFFF, finish by inverting. Exposed so
 * the tests can check it against cairn_crc32. */
uint32_t cairn_offload_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_OFFLOAD_H */
