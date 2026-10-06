/* Private to lib/cairn_digest: the seam between the reducer and the (provisional)
 * serialiser. Not part of the API; the contract will replace the serialiser. */

#ifndef CAIRN_DIGEST_INTERNAL_H
#define CAIRN_DIGEST_INTERNAL_H

#include "cairn_digest.h"

/*
 * Serialise the current reduced state into `out`, which holds `budget` bytes.
 * Returns the length, or 0 when it would not fit (nothing past `budget` is ever
 * written). `with_events` false omits the event list.
 */
size_t cairn_digest_wire_encode(const cairn_digest_t *d, const cairn_digest_meta_t *meta,
                                uint32_t budget, bool with_events, uint8_t *out);

/* Reducer helpers the encoder and the budget loop both need. */
bool cairn_digest_drop_least_important(cairn_digest_t *d);

#endif
