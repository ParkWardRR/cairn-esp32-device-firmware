/*
 * Uploading the pending bundles over whichever link is up.
 *
 * Wi-Fi and LTE differ in how a byte leaves the dongle and in nothing else, so
 * this owns everything above that: walking the card, reading each bundle,
 * driving lib/cairn_intake, and putting the receipt through the prune gate. The
 * two link files bring a Client up and hand it over.
 *
 * It also owns the buffers, and that is not incidental. Each transport used to
 * hold its own cairn_bundle_t, HTTP state, scratch and receipt buffer, and once
 * both were in one image the static DRAM segment overflowed by 65 KB — the
 * build failed at link time. The schedule in lib/cairn_uplink never runs a
 * Wi-Fi slot and an LTE send at once (one radio, time-sliced; LTE is a separate
 * modem but still a separate action), so one set of buffers is not a compromise
 * but the accurate expression of that: the sharing is enforced by there being
 * only one, rather than asserted in a comment.
 */

#ifndef CAIRN_NET_UPLOAD_H
#define CAIRN_NET_UPLOAD_H

#include <Client.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t considered;  /* sealed bundles seen on the card */
    uint32_t delivered;   /* receipted AND pruned */
    uint32_t retained;    /* receipted but the prune did not complete */
    uint32_t refused;     /* the server will never accept these bytes */
    uint32_t unreadable;  /* the card or the format, not the network */
    uint32_t failed;      /* link trouble; worth another attempt */
    uint32_t bytes_up;
    uint32_t bytes_down;
    uint32_t ms;
} net_upload_result_t;

/*
 * Upload up to `max_bundles` bundles through `client`, which must already be
 * connected-capable (the HTTP layer connects it). `label` names the path in the
 * log so a line is attributable to Wi-Fi or LTE without guessing.
 *
 * A bundle is pruned only when the server's receipt verifies against the key
 * pinned in firmware AND names that bundle's own content root. Every other
 * outcome leaves the card untouched.
 *
 * A bundle that cannot be opened does not count against `max_bundles`: nothing
 * went over the air, and unreadable bundles sort first on this card, so
 * charging them would exhaust the budget before a sendable one was reached.
 */
void net_upload_pending(Client *client, const char *host, uint16_t port,
                        const char *label, uint32_t max_bundles,
                        bool (*should_abort)(void *), void *abort_ctx,
                        net_upload_result_t *out);

#endif /* CAIRN_NET_UPLOAD_H */
