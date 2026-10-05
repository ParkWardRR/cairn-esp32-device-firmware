#include "mtprobe_parse.h"

#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * SAE J1979 data-byte counts. Only PIDs the probe requests are listed: the
 * three it anchors every request with, the support bitmaps, and the candidates.
 * Adding a candidate to mtprobe.cpp without a line here makes its reply
 * unwalkable, which shows up as "ans=" missing it rather than as bad data.
 */
uint8_t mtprobe_pid_len(uint8_t pid)
{
    switch (pid) {
    /* Support bitmaps. */
    case 0x00: case 0x20: case 0x40: case 0x60:
    case 0x80: case 0xA0: case 0xC0:
        return 4;

    /* The anchors carried on every request, so each log line is self-contained. */
    case 0x0C: return 2;   /* RPM */
    case 0x0D: return 1;   /* vehicle speed */
    case 0x11: return 1;   /* throttle */

    /* Load, fuel and engine state. */
    case 0x03: return 2;   /* fuel system status */
    case 0x04: return 1;   /* calculated load */
    case 0x1F: return 2;   /* run time since start */
    case 0x21: return 2;   /* distance with MIL on */
    case 0x22: return 2;   /* fuel rail pressure, relative to manifold */
    case 0x23: return 2;   /* fuel rail gauge pressure */
    case 0x2C: return 1;   /* commanded EGR */
    case 0x2E: return 1;   /* commanded evap purge */
    case 0x31: return 2;   /* distance since codes cleared */
    case 0x3C: return 2;   /* catalyst temperature B1S1 */
    case 0x42: return 2;   /* control module voltage */
    case 0x4D: return 2;   /* time run with MIL on */
    case 0x5C: return 1;   /* oil temperature */
    case 0x5D: return 2;   /* fuel injection timing */
    case 0x5E: return 2;   /* engine fuel rate */

    /* Pedal and throttle: the driver's input, which a shift is read against. */
    case 0x45: return 1;   /* relative throttle position */
    case 0x47: return 1;   /* absolute throttle position B */
    case 0x49: return 1;   /* accelerator pedal D */
    case 0x4A: return 1;   /* accelerator pedal E */
    case 0x4B: return 1;   /* accelerator pedal F */
    case 0x4C: return 1;   /* commanded throttle actuator */
    case 0x5A: return 1;   /* relative accelerator pedal position */

    /* Torque. */
    case 0x61: return 1;   /* driver's demand engine torque, % */
    case 0x62: return 1;   /* actual engine torque, % */
    case 0x63: return 2;   /* engine reference torque, Nm */
    case 0x64: return 5;   /* engine percent torque data */
    case 0x8E: return 1;   /* engine friction, % torque */

    /* Transmission actual gear: J1979-2, so unlikely on a 2014 DME, but free to ask. */
    case 0xA4: return 4;

    default:   return 0;
    }
}

int mtprobe_reply_bytes(const char *reply, uint8_t *out, int cap)
{
    if (reply == NULL || out == NULL || cap <= 0) return -1;

    const char *p = strstr(reply, "41 ");
    if (p == NULL) return -1;
    p += 3;

    int n = 0;
    while (*p != '\0' && n < cap) {
        while (*p == ' ' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;

        /* "0:" / "1:" line prefix on a multi-frame reply. */
        if (p[0] >= '0' && p[0] <= '9' && p[1] == ':') {
            p += 2;
            continue;
        }

        int hi = hexval(p[0]);
        int lo = (p[0] != '\0') ? hexval(p[1]) : -1;
        if (hi < 0 || lo < 0) return -1;

        out[n++] = (uint8_t)(hi << 4 | lo);
        p += 2;
    }
    return n;
}

int mtprobe_split(const uint8_t *bytes, int nbytes, mtprobe_hit_t *hits, int cap)
{
    int pos = 0;
    int h   = 0;

    while (pos < nbytes && h < cap) {
        uint8_t pid = bytes[pos];
        uint8_t len = mtprobe_pid_len(pid);
        if (len == 0 || pos + 1 + len > nbytes) break;

        hits[h].pid = pid;
        hits[h].len = len;
        memset(hits[h].data, 0, sizeof(hits[h].data));
        memcpy(hits[h].data, bytes + pos + 1, len);
        h++;

        pos += 1 + len;
    }
    return h;
}
