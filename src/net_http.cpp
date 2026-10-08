/* HTTP/1.1 over an Arduino Client. See net_http.h. */

#include "net_http.h"

#include <Arduino.h>
#include <string.h>

#include "cairn_log.h"

static const char *TAG = "HTTP";

/* ── reading ──────────────────────────────────────────────────────────────── */

/* One CRLF-terminated line, without the terminator. Returns false on timeout
 * or a closed socket. */
static bool read_line(Client *c, char *out, size_t cap, uint32_t deadline)
{
    size_t n = 0;

    for (;;) {
        if ((int32_t)(millis() - deadline) >= 0) return false;

        if (!c->connected() && c->available() == 0) return false;

        int ch = c->read();
        if (ch < 0) {
            delay(2);
            continue;
        }
        if (ch == '\n') {
            while (n > 0 && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return true;
        }
        if (n + 1 < cap) out[n++] = (char)ch;
    }
}

/* Read exactly `len` bytes into `out` (which may be NULL to discard). */
static bool read_exact(Client *c, uint8_t *out, size_t len, uint32_t deadline)
{
    size_t got = 0;

    while (got < len) {
        if ((int32_t)(millis() - deadline) >= 0) return false;

        int avail = c->available();
        if (avail <= 0) {
            if (!c->connected()) return false;
            delay(2);
            continue;
        }

        size_t want = len - got;
        if ((size_t)avail < want) want = (size_t)avail;

        if (out != NULL) {
            int n = c->read(out + got, want);
            if (n <= 0) return false;
            got += (size_t)n;
        } else {
            /* Discarding: a body larger than the caller's buffer still has to
             * leave the socket, or the next response would read its tail. */
            uint8_t sink[128];
            size_t  step = want > sizeof(sink) ? sizeof(sink) : want;
            int     n    = c->read(sink, step);
            if (n <= 0) return false;
            got += (size_t)n;
        }
    }
    return true;
}

static bool header_is(const char *line, const char *name)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0) return false;
    return line[n] == ':';
}

static const char *header_value(const char *line)
{
    const char *p = strchr(line, ':');
    if (p == NULL) return "";
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* ── the four callbacks ───────────────────────────────────────────────────── */

static bool ensure_connected(cairn_http_t *h)
{
    if (h->client->connected() && !h->must_reconnect) return true;

    if (h->client->connected()) h->client->stop();

    uint32_t t0 = millis();
    if (h->client->connect(h->host, h->port) != 1) {
        CAIRN_LOGW(TAG, "connect to %s:%u failed after %u ms", h->host,
                   (unsigned)h->port, (unsigned)(millis() - t0));
        return false;
    }

    h->connects++;
    h->must_reconnect = false;
    CAIRN_LOGI(TAG, "connected to %s:%u in %u ms (connection %u)", h->host,
               (unsigned)h->port, (unsigned)(millis() - t0),
               (unsigned)h->connects);
    return true;
}

static bool http_begin(void *ctx, const char *method, const char *path,
                       const char *extra_header, size_t content_length)
{
    cairn_http_t *h = (cairn_http_t *)ctx;

    if (!ensure_connected(h)) return false;

    /* Keep-alive is explicit. A bundle is around thirty requests and a
     * handshake each would dominate both the time and, on LTE, the bytes. */
    char head[512];
    int  n = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s:%u\r\n"
                      "Connection: keep-alive\r\n"
                      "Content-Length: %u\r\n"
                      "Content-Type: application/cbor\r\n",
                      method, path, h->host, (unsigned)h->port,
                      (unsigned)content_length);
    if (n <= 0 || (size_t)n >= sizeof(head)) return false;

    if (extra_header != NULL && extra_header[0] != '\0') {
        int m = snprintf(head + n, sizeof(head) - (size_t)n, "%s\r\n", extra_header);
        if (m <= 0 || (size_t)(n + m) >= sizeof(head)) return false;
        n += m;
    }

    int m = snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");
    if (m <= 0 || (size_t)(n + m) >= sizeof(head)) return false;
    n += m;

    if (h->client->write((const uint8_t *)head, (size_t)n) != (size_t)n) {
        CAIRN_LOGW(TAG, "writing the request head failed");
        h->must_reconnect = true;
        return false;
    }

    h->request_open = true;
    h->requests++;
    return true;
}

static bool http_write(void *ctx, const uint8_t *data, size_t len)
{
    cairn_http_t *h = (cairn_http_t *)ctx;

    size_t sent = 0;
    while (sent < len) {
        size_t n = h->client->write(data + sent, len - sent);
        if (n == 0) {
            if (!h->client->connected()) {
                CAIRN_LOGW(TAG, "the socket closed mid-body");
                h->must_reconnect = true;
                return false;
            }
            delay(2);
            continue;
        }
        sent += n;
    }
    return true;
}

static bool http_finish(void *ctx, int *status, uint8_t *resp, size_t resp_cap,
                        size_t *resp_len)
{
    cairn_http_t *h = (cairn_http_t *)ctx;
    *status   = 0;
    *resp_len = 0;

    uint32_t deadline = millis() + CAIRN_HTTP_REPLY_TIMEOUT_MS;
    char     line[256];

    if (!read_line(h->client, line, sizeof(line), deadline)) {
        CAIRN_LOGW(TAG, "no status line within %u ms",
                   (unsigned)CAIRN_HTTP_REPLY_TIMEOUT_MS);
        h->must_reconnect = true;
        return false;
    }

    /* "HTTP/1.1 200 OK" */
    if (strncmp(line, "HTTP/1.", 7) != 0) {
        CAIRN_LOGW(TAG, "not an HTTP response: %s", line);
        h->must_reconnect = true;
        return false;
    }
    const char *sp = strchr(line, ' ');
    if (sp == NULL) { h->must_reconnect = true; return false; }
    *status = atoi(sp + 1);

    long  content_length = -1;
    bool  chunked        = false;
    bool  close_after    = false;

    for (;;) {
        if (!read_line(h->client, line, sizeof(line), deadline)) {
            h->must_reconnect = true;
            return false;
        }
        if (line[0] == '\0') break;          /* end of headers */

        if (header_is(line, "Content-Length")) {
            content_length = atol(header_value(line));
        } else if (header_is(line, "Transfer-Encoding")) {
            if (strcasestr(header_value(line), "chunked") != NULL) chunked = true;
        } else if (header_is(line, "Connection")) {
            if (strcasestr(header_value(line), "close") != NULL) close_after = true;
        }
    }

    if (chunked) {
        /*
         * Go sets Content-Length for the small bodies this protocol uses, but
         * it falls back to chunked whenever it cannot buffer the response, and
         * a client that only understood Content-Length would mistake the size
         * line for the body.
         */
        for (;;) {
            if (!read_line(h->client, line, sizeof(line), deadline)) {
                h->must_reconnect = true;
                return false;
            }
            long size = strtol(line, NULL, 16);
            if (size <= 0) {
                /* Trailer section, then a blank line. */
                while (read_line(h->client, line, sizeof(line), deadline) &&
                       line[0] != '\0') {
                    /* discard trailers */
                }
                break;
            }

            size_t room = (*resp_len < resp_cap) ? (resp_cap - *resp_len) : 0;
            size_t take = ((size_t)size < room) ? (size_t)size : room;

            if (take > 0 && !read_exact(h->client, resp + *resp_len, take, deadline)) {
                h->must_reconnect = true;
                return false;
            }
            *resp_len += take;

            if ((size_t)size > take &&
                !read_exact(h->client, NULL, (size_t)size - take, deadline)) {
                h->must_reconnect = true;
                return false;
            }

            /* The CRLF after each chunk. */
            if (!read_line(h->client, line, sizeof(line), deadline)) {
                h->must_reconnect = true;
                return false;
            }
        }
    } else if (content_length > 0) {
        size_t take = ((size_t)content_length < resp_cap) ? (size_t)content_length
                                                          : resp_cap;
        if (take > 0 && !read_exact(h->client, resp, take, deadline)) {
            h->must_reconnect = true;
            return false;
        }
        *resp_len = take;

        if ((size_t)content_length > take) {
            CAIRN_LOGW(TAG, "response body is %ld bytes, buffer holds %u; "
                            "draining the rest",
                       content_length, (unsigned)resp_cap);
            if (!read_exact(h->client, NULL, (size_t)content_length - take, deadline)) {
                h->must_reconnect = true;
                return false;
            }
        }
    } else if (content_length < 0) {
        /*
         * Neither a length nor chunked: the body runs to end-of-connection, so
         * the socket cannot be reused afterwards.
         */
        close_after = true;
        while ((int32_t)(millis() - deadline) < 0) {
            int avail = h->client->available();
            if (avail <= 0) {
                if (!h->client->connected()) break;
                delay(2);
                continue;
            }
            size_t room = (*resp_len < resp_cap) ? (resp_cap - *resp_len) : 0;
            if (room == 0) { (void)read_exact(h->client, NULL, (size_t)avail, deadline); continue; }
            size_t want = ((size_t)avail < room) ? (size_t)avail : room;
            int    n    = h->client->read(resp + *resp_len, want);
            if (n <= 0) break;
            *resp_len += (size_t)n;
        }
    }

    if (close_after) h->must_reconnect = true;
    return true;
}

static void http_close(void *ctx)
{
    cairn_http_t *h = (cairn_http_t *)ctx;

    /*
     * Ends the request, NOT the connection. cairn_intake calls this after every
     * exchange; tearing the socket down here is what would cost a handshake per
     * chunk. cairn_http_disconnect closes it, once per transfer.
     */
    h->request_open = false;
}

/* ── setup ────────────────────────────────────────────────────────────────── */

void cairn_http_init(cairn_http_t *h, Client *client, const char *host, uint16_t port)
{
    memset(h, 0, sizeof(*h));
    h->client = client;
    h->host   = host;
    h->port   = port;
}

void cairn_http_as_intake(cairn_http_t *h, cairn_intake_http_t *out)
{
    out->ctx    = h;
    out->begin  = http_begin;
    out->write  = http_write;
    out->finish = http_finish;
    out->close  = http_close;
}

void cairn_http_disconnect(cairn_http_t *h)
{
    if (h->client != NULL && h->client->connected()) h->client->stop();

    CAIRN_LOGI(TAG, "disconnected: %u requests over %u connection(s)",
               (unsigned)h->requests, (unsigned)h->connects);
    h->must_reconnect = true;
}
