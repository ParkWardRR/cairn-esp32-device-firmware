/*
 * A dongle you can talk to over a pipe.
 *
 * Runs lib/cairn_offload, the same code the device runs, over a card directory on
 * the host, with stdin and stdout standing in for the BLE link. It exists so the
 * phone side and the server's relay can be tested end to end against the real
 * firmware protocol code without a radio: server/internal/offloadclient drives it
 * from a Go test.
 *
 *   offload-sim <card-dir> <pinned-receipt-key-hex | none> [<trip-file>]
 *
 * If <trip-file> exists a trip is "in progress" and offload is refused.
 *
 * Framing, both directions: kind u8, length u16 little-endian, bytes.
 *   in : 'C' control write, 'D' data write, 'M' MTU (u16), 'X' disconnect, 'Q' quit
 *   out: 'I' indication, 'N' notification
 */

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "board_config.h"
#include "cairn_fs.h"
#include "cairn_log.h"
#include "cairn_offload.h"
#include "cairn_store.h"

static const char *g_trip_file;

static void emit(char kind, const uint8_t *b, size_t len)
{
    uint8_t h[3] = { (uint8_t)kind, (uint8_t)len, (uint8_t)(len >> 8) };
    fwrite(h, 1, 3, stdout);
    if (len > 0) fwrite(b, 1, len, stdout);
    fflush(stdout);
}

static bool io_indicate(void *c, const uint8_t *b, size_t len) { (void)c; emit('I', b, len); return true; }
static bool io_notify(void *c, const uint8_t *b, size_t len)   { (void)c; emit('N', b, len); return true; }

static uint32_t io_now(void *c)
{
    (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static bool io_trip(void *c) { (void)c; return g_trip_file != NULL && access(g_trip_file, F_OK) == 0; }

static bool unhex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static bool read_exact(uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(STDIN_FILENO, buf + got, n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: offload-sim <card-dir> <pinned-key-hex|none> [trip-file]\n");
        return 2;
    }
    g_trip_file = (argc > 3) ? argv[3] : NULL;

    cairn_log_init(0);
    if (!cairn_fs_begin(argv[1]) || !cairn_store_init()) {
        fprintf(stderr, "cannot open the card at %s\n", argv[1]);
        return 2;
    }

    uint8_t key[32];
    bool have_key = strcmp(argv[2], "none") != 0 && unhex32(argv[2], key);

    static cairn_offload_t o;
    cairn_offload_io_t io = { NULL, io_indicate, io_notify, io_now, io_trip, have_key ? key : NULL };
    cairn_offload_init(&o, &io);
    cairn_offload_set_mtu(&o, 23);

    for (;;) {
        struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
        int pr = poll(&p, 1, 1);
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP))) {
            uint8_t h[3];
            if (!read_exact(h, 3)) break;
            size_t len = (size_t)h[1] | ((size_t)h[2] << 8);
            static uint8_t buf[512];
            if (len > sizeof(buf) || !read_exact(buf, len)) break;
            switch (h[0]) {
            case 'C': cairn_offload_on_control_write(&o, buf, len); break;
            case 'D': cairn_offload_on_data_write(&o, buf, len); break;
            case 'M': if (len == 2) cairn_offload_set_mtu(&o, (uint16_t)(buf[0] | (buf[1] << 8))); break;
            case 'X': cairn_offload_on_disconnect(&o); break;
            case 'Q': return 0;
            default: break;
            }
        }
        cairn_offload_pump(&o);
    }
    return 0;
}
