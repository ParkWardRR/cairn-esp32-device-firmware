/*
 * Host implementation of the cairn_log API.
 *
 * The device logger is Arduino C++ over UART and SD; this provides the same C
 * entry points over stderr so cairn_store.c links unchanged under test. Quiet
 * by default because the fault tests deliberately provoke errors, and a passing
 * run should not look like a disaster; set CAIRN_TEST_VERBOSE=1 to see them.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cairn_log.h"

static cairn_log_level_t s_level = CAIRN_LOG_TRACE;
static int               s_verbose = -1;
static cairn_log_stats_t s_stats;

static bool verbose(void)
{
    if (s_verbose < 0) {
        const char *v = getenv("CAIRN_TEST_VERBOSE");
        s_verbose = (v != NULL && v[0] == '1') ? 1 : 0;
    }
    return s_verbose == 1;
}

static const char *level_name(cairn_log_level_t level)
{
    switch (level) {
    case CAIRN_LOG_ERROR: return "ERROR";
    case CAIRN_LOG_WARN:  return "WARN ";
    case CAIRN_LOG_INFO:  return "INFO ";
    case CAIRN_LOG_DEBUG: return "DEBUG";
    case CAIRN_LOG_TRACE: return "TRACE";
    default:              return "?????";
    }
}

void cairn_log_init(uint32_t baud)
{
    (void)baud;
    memset(&s_stats, 0, sizeof(s_stats));
}

bool cairn_log_attach_sd(uint32_t boot_count)
{
    (void)boot_count;
    return true;
}

void cairn_log_detach_sd(void) { }

void cairn_log_set_level(cairn_log_level_t level) { s_level = level; }
cairn_log_level_t cairn_log_get_level(void) { return s_level; }

void cairn_log_set_context(const uint8_t boot_id[16]) { (void)boot_id; }

void cairn_log_line(cairn_log_level_t level, const char *tag,
                    const char *fmt, ...)
{
    s_stats.lines_emitted++;
    if (!verbose() || level > s_level) return;

    fprintf(stderr, "    %-5s [%s] ", level_name(level), tag);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

void cairn_log_hexdump(cairn_log_level_t level, const char *tag,
                       const char *what, const uint8_t *data, size_t len)
{
    if (!verbose() || level > s_level) return;

    cairn_log_line(level, tag, "%s (%u bytes)", what, (unsigned)len);
    for (size_t off = 0; off < len; off += 16) {
        char hex[16 * 3 + 1];
        size_t n = (len - off < 16) ? len - off : 16;
        for (size_t i = 0; i < n; i++) snprintf(hex + i * 3, 4, "%02x ", data[off + i]);
        hex[n * 3] = '\0';
        cairn_log_line(level, tag, "  %04x  %s", (unsigned)off, hex);
    }
}

void cairn_log_flush(void) { fflush(stderr); }
void cairn_log_tick(void) { }

void cairn_log_get_stats(cairn_log_stats_t *out) { *out = s_stats; }
