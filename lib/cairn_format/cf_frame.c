/*
 * Segment headers, frame encoding and the recovery scan.
 *
 * The scan is the algorithm that decides what survives a power cut, so it
 * follows the specification literally and is checked against the committed
 * vectors. Its behaviour is stop-at-first-invalid: every frame before the
 * failure is retained, everything after it is reported as a discarded tail
 * with an exact byte count and a reason.
 */

#include <string.h>

#include "cairn_format.h"

static const uint8_t SEGMENT_MAGIC[4] = { 'C', 'R', 'N', '2' };

/* ── little-endian helpers ────────────────────────────────────────────────── */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* ── record types ─────────────────────────────────────────────────────────── */

bool cairn_record_type_known(uint8_t t)
{
    return t >= CAIRN_REC_GNSS_SAMPLE && t <= CAIRN_REC_POLICY_SNAPSHOT;
}

const char *cairn_record_type_name(uint8_t t)
{
    switch (t) {
    case CAIRN_REC_GNSS_SAMPLE:      return "GNSS_SAMPLE";
    case CAIRN_REC_IMU_SUMMARY:      return "IMU_SUMMARY";
    case CAIRN_REC_IMU_RAW_WINDOW:   return "IMU_RAW_WINDOW";
    case CAIRN_REC_OBD_SNAPSHOT:     return "OBD_SNAPSHOT";
    case CAIRN_REC_DEVICE_HEALTH:    return "DEVICE_HEALTH";
    case CAIRN_REC_TRIP_EVENT:       return "TRIP_EVENT";
    case CAIRN_REC_STATE_TRANSITION: return "STATE_TRANSITION";
    case CAIRN_REC_GNSS_GAP:         return "GNSS_GAP";
    case CAIRN_REC_POLICY_SNAPSHOT:  return "POLICY_SNAPSHOT";
    default:                         return "UNKNOWN";
    }
}

const char *cairn_stop_reason_name(cairn_stop_reason_t r)
{
    switch (r) {
    case CAIRN_STOP_EOF:           return "EOF";
    case CAIRN_STOP_TORN_TAIL:     return "TORN_TAIL";
    case CAIRN_STOP_CORRUPT_FRAME: return "CORRUPT_FRAME";
    case CAIRN_STOP_CHAIN_BREAK:   return "CHAIN_BREAK";
    case CAIRN_STOP_SEQ_GAP:       return "SEQ_GAP";
    default:                       return "?";
    }
}

const char *cairn_strerror(cairn_err_t e)
{
    switch (e) {
    case CAIRN_OK:                        return "ok";
    case CAIRN_ERR_BAD_MAGIC:             return "bad segment magic";
    case CAIRN_ERR_BAD_HEADER_CRC:        return "segment header CRC mismatch";
    case CAIRN_ERR_SHORT_HEADER:          return "segment shorter than header";
    case CAIRN_ERR_UNSUPPORTED_VERSION:   return "unsupported format version";
    case CAIRN_ERR_BAD_HEADER_LEN:        return "header_len below the minimum";
    case CAIRN_ERR_PAYLOAD_TOO_LARGE:     return "payload exceeds the maximum";
    case CAIRN_ERR_BUFFER_TOO_SMALL:      return "output buffer too small";
    case CAIRN_ERR_NON_CANONICAL:         return "non-canonical encoding";
    case CAIRN_ERR_TRUNCATED:             return "truncated input";
    case CAIRN_ERR_UNSUPPORTED_CBOR:      return "unsupported CBOR feature";
    case CAIRN_ERR_MALFORMED:             return "malformed";
    case CAIRN_ERR_BAD_SIGNATURE:         return "signature verification failed";
    case CAIRN_ERR_CONTENT_ROOT_MISMATCH: return "content root does not match members";
    case CAIRN_ERR_RECEIPT_ROOT_MISMATCH: return "receipt acknowledges different content";
    case CAIRN_ERR_TOO_MANY_MEMBERS:      return "too many members";
    case CAIRN_ERR_READ_FAILED:           return "segment read failed";
    default:                              return "unknown error";
    }
}

/* ── segment header ───────────────────────────────────────────────────────── */

cairn_err_t cairn_encode_segment_header(const cairn_segment_header_t *h,
                                        uint8_t *out, size_t out_cap)
{
    if (out_cap < CAIRN_SEGMENT_HEADER_SIZE) return CAIRN_ERR_BUFFER_TOO_SMALL;

    memset(out, 0, CAIRN_SEGMENT_HEADER_SIZE);
    memcpy(out, SEGMENT_MAGIC, 4);
    put_u16(out + 4, CAIRN_FORMAT_VERSION);
    put_u16(out + 6, CAIRN_SEGMENT_HEADER_SIZE);
    memcpy(out + 8, h->device_id, 16);
    memcpy(out + 24, h->boot_id, 16);
    put_u32(out + 40, h->segment_index);
    put_u32(out + 44, h->first_seq);
    put_u64(out + 48, h->opened_monotonic_us);
    put_u32(out + 56, 0); /* reserved */
    put_u32(out + 60, cairn_crc32(out, 60));

    return CAIRN_OK;
}

cairn_err_t cairn_parse_segment_header(const uint8_t *buf, size_t len,
                                       cairn_segment_header_t *out,
                                       size_t *header_len)
{
    if (len < CAIRN_SEGMENT_HEADER_SIZE) return CAIRN_ERR_SHORT_HEADER;
    if (memcmp(buf, SEGMENT_MAGIC, 4) != 0) return CAIRN_ERR_BAD_MAGIC;

    uint32_t stored = get_u32(buf + 60);
    if (cairn_crc32(buf, 60) != stored) return CAIRN_ERR_BAD_HEADER_CRC;

    out->format_version = get_u16(buf + 4);
    size_t hlen = get_u16(buf + 6);

    memcpy(out->device_id, buf + 8, 16);
    memcpy(out->boot_id, buf + 24, 16);
    out->segment_index       = get_u32(buf + 40);
    out->first_seq           = get_u32(buf + 44);
    out->opened_monotonic_us = get_u64(buf + 48);

    if (header_len) *header_len = hlen;

    if (out->format_version != CAIRN_FORMAT_VERSION) return CAIRN_ERR_UNSUPPORTED_VERSION;
    if (hlen < CAIRN_SEGMENT_HEADER_SIZE) return CAIRN_ERR_BAD_HEADER_LEN;

    return CAIRN_OK;
}

/* ── frames ───────────────────────────────────────────────────────────────── */

cairn_err_t cairn_encode_frame(uint8_t record_type, uint8_t schema_version,
                               uint16_t flags, uint32_t seq,
                               uint32_t monotonic_ms, uint32_t prev_crc32,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t *out, size_t out_cap,
                               size_t *written, uint32_t *crc_out)
{
    if (payload_len > CAIRN_MAX_PAYLOAD_SIZE) return CAIRN_ERR_PAYLOAD_TOO_LARGE;

    size_t frame_len = CAIRN_FRAME_OVERHEAD + payload_len;
    if (out_cap < frame_len) return CAIRN_ERR_BUFFER_TOO_SMALL;

    put_u16(out + 0, (uint16_t)frame_len);
    out[2] = record_type;
    out[3] = schema_version;
    put_u16(out + 4, flags);
    put_u16(out + 6, 0); /* reserved */
    put_u32(out + 8, seq);
    put_u32(out + 12, monotonic_ms);
    put_u32(out + 16, prev_crc32);
    put_u32(out + 20, 0); /* reserved2, aligns the payload to 4 bytes */

    if (payload_len > 0) memcpy(out + CAIRN_FRAME_HEADER_SIZE, payload, payload_len);

    uint32_t crc = cairn_crc32(out, CAIRN_FRAME_HEADER_SIZE + payload_len);
    put_u32(out + CAIRN_FRAME_HEADER_SIZE + payload_len, crc);

    if (written) *written = frame_len;
    if (crc_out) *crc_out = crc;

    return CAIRN_OK;
}

/* ── the recovery scan ────────────────────────────────────────────────────── */

cairn_err_t cairn_scan_segment_stream(cairn_read_fn read, void *read_user,
                                      uint64_t len,
                                      cairn_scan_state_t state,
                                      cairn_scan_result_t *out,
                                      cairn_frame_cb cb, void *user)
{
    size_t  header_len = 0;
    uint8_t head[CAIRN_SEGMENT_HEADER_SIZE];

    /*
     * One frame is resident at a time. A segment on the card is far larger than
     * ESP32 DRAM, and the recovery scan has to work without PSRAM being
     * present, so nothing here scales with segment size.
     */
    static uint8_t stage[CAIRN_MAX_FRAME_LEN];

    memset(out, 0, sizeof(*out));

    if (len < CAIRN_SEGMENT_HEADER_SIZE) return CAIRN_ERR_SHORT_HEADER;
    if (!read(read_user, 0, head, sizeof(head))) return CAIRN_ERR_READ_FAILED;

    cairn_err_t err =
        cairn_parse_segment_header(head, sizeof(head), &out->header, &header_len);
    if (err != CAIRN_OK) return err;

    /*
     * For a chain's first segment the caller has no prior state, so the
     * header's own first_seq establishes the expectation.
     */
    uint32_t expected_seq = state.expected_seq;
    if (out->header.segment_index == 0 &&
        state.expected_seq == 0 && state.expected_prev == 0) {
        expected_seq = out->header.first_seq;
    }
    uint32_t expected_prev = state.expected_prev;

    uint64_t offset = header_len;
    bool     have_first = false;

    for (;;) {
        uint64_t remaining = len - offset;

        if (remaining == 0) {
            out->stop = CAIRN_STOP_EOF;
            break;
        }
        if (remaining < CAIRN_MIN_FRAME_LEN) {
            out->stop = CAIRN_STOP_TORN_TAIL;
            break;
        }

        /* The length prefix comes first so a truncated frame is recognizable
         * without trusting anything inside it. */
        uint8_t lenbuf[2];
        if (!read(read_user, offset, lenbuf, sizeof(lenbuf))) {
            return CAIRN_ERR_READ_FAILED;
        }

        size_t frame_len = get_u16(lenbuf);
        if (frame_len < CAIRN_MIN_FRAME_LEN || frame_len > CAIRN_MAX_FRAME_LEN) {
            out->stop = CAIRN_STOP_TORN_TAIL;
            break;
        }
        if (offset + frame_len > len) {
            out->stop = CAIRN_STOP_TORN_TAIL;
            break;
        }

        if (!read(read_user, offset, stage, frame_len)) {
            return CAIRN_ERR_READ_FAILED;
        }

        const uint8_t *body = stage;
        uint32_t stored_crc = get_u32(body + frame_len - CAIRN_FRAME_TRAILER_SIZE);
        uint32_t computed   = cairn_crc32(body, frame_len - CAIRN_FRAME_TRAILER_SIZE);

        if (computed != stored_crc) {
            out->stop = CAIRN_STOP_CORRUPT_FRAME;
            break;
        }

        cairn_frame_t f;
        f.record_type    = body[2];
        f.schema_version = body[3];
        f.flags          = get_u16(body + 4);
        f.seq            = get_u32(body + 8);
        f.monotonic_ms   = get_u32(body + 12);
        f.prev_crc32     = get_u32(body + 16);
        f.payload        = body + CAIRN_FRAME_HEADER_SIZE;
        f.payload_len    = frame_len - CAIRN_FRAME_OVERHEAD;
        f.crc32          = stored_crc;

        /*
         * The chain detects a record that was removed, reordered or spliced —
         * each remaining frame would still be individually CRC-valid, so only
         * prev_crc32 can catch it.
         */
        if (f.prev_crc32 != expected_prev) {
            out->stop = CAIRN_STOP_CHAIN_BREAK;
            break;
        }
        if (f.seq != expected_seq) {
            out->stop = CAIRN_STOP_SEQ_GAP;
            break;
        }

        /*
         * An unknown record type is not an error: it is skipped via frame_len,
         * counted and reported. That is how a newer device stays partially
         * readable by an older decoder, and the frame CRC still applies so the
         * skipped record remains integrity-checked.
         */
        if (!cairn_record_type_known(f.record_type)) out->unknown_type_count++;
        out->record_counts[f.record_type]++;

        if (!have_first) {
            out->first_seq = f.seq;
            have_first = true;
        }
        out->last_seq = f.seq;
        out->frames++;

        if (cb && !cb(&f, user)) {
            out->stop = CAIRN_STOP_EOF;
            offset += frame_len;
            expected_seq = f.seq + 1;
            expected_prev = stored_crc;
            break;
        }

        expected_seq  = f.seq + 1;
        expected_prev = stored_crc;
        offset       += frame_len;
    }

    out->stop_offset          = (size_t)offset;
    out->discarded_tail_bytes = (uint32_t)(len - offset);
    out->next.expected_seq    = expected_seq;
    out->next.expected_prev   = expected_prev;

    return CAIRN_OK;
}

/* ── the buffer entry point ───────────────────────────────────────────────── */

typedef struct {
    const uint8_t *buf;
    size_t         len;
} mem_reader_t;

static bool mem_read(void *user, uint64_t offset, uint8_t *dst, size_t len)
{
    const mem_reader_t *m = user;

    if (offset > m->len || len > m->len - offset) return false;
    memcpy(dst, m->buf + offset, len);
    return true;
}

/*
 * Thin wrapper over the streaming scan, so the in-memory path the conformance
 * vectors drive and the on-card path the firmware recovers through are the same
 * body of code.
 */
cairn_err_t cairn_scan_segment(const uint8_t *buf, size_t len,
                               cairn_scan_state_t state,
                               cairn_scan_result_t *out,
                               cairn_frame_cb cb, void *user)
{
    mem_reader_t m = { buf, len };
    return cairn_scan_segment_stream(mem_read, &m, len, state, out, cb, user);
}

/* ── payload builders ─────────────────────────────────────────────────────── */

void cairn_encode_gnss_sample(const cairn_gnss_sample_t *s, uint8_t out[32])
{
    memset(out, 0, 32);
    put_u32(out + 0, (uint32_t)s->lat_e7);
    put_u32(out + 4, (uint32_t)s->lon_e7);
    put_u32(out + 8, (uint32_t)s->alt_cm);
    put_u16(out + 12, s->speed_cmps);
    put_u16(out + 14, s->heading_cdeg);
    put_u16(out + 16, s->hdop_e2);
    put_u16(out + 18, s->h_acc_cm);
    put_u16(out + 20, s->v_acc_cm);
    out[22] = s->fix_type;
    out[23] = s->sats_used;
    out[24] = s->sats_visible;
    out[25] = s->source_flags;
    put_u32(out + 26, (uint32_t)s->utc_offset_ms);
    put_u16(out + 30, s->utc_acc_ms);
}

void cairn_encode_imu_summary(const cairn_imu_summary_t *s, uint8_t out[20])
{
    memset(out, 0, 20);
    put_u16(out + 0, s->window_ms);
    put_u16(out + 2, s->accel_rms_mg);
    put_u16(out + 4, (uint16_t)s->accel_peak_x_mg);
    put_u16(out + 6, (uint16_t)s->accel_peak_y_mg);
    put_u16(out + 8, (uint16_t)s->accel_peak_z_mg);
    put_u16(out + 10, (uint16_t)s->gyro_peak_dps_e1);
    put_u16(out + 12, s->variance);
    put_u16(out + 14, s->sample_count);
    out[16] = s->event_flags;
}

void cairn_encode_obd_snapshot(const cairn_obd_snapshot_t *s, uint8_t out[24])
{
    memset(out, 0, 24);
    put_u16(out + 0, (uint16_t)s->speed_kph);
    put_u16(out + 2, (uint16_t)s->rpm);
    put_u16(out + 4, s->fuel_pressure_kpa);
    out[6]  = s->throttle_pct;
    out[7]  = s->engine_load_pct;
    out[8]  = (uint8_t)s->coolant_temp_c;
    out[9]  = (uint8_t)s->intake_temp_c;
    out[10] = (uint8_t)s->timing_advance_deg;
    out[11] = s->pid_error_count;
    put_u32(out + 12, s->pids_requested);
    put_u32(out + 16, s->pids_answered);
    put_u16(out + 20, s->poll_cadence_ms);
}

void cairn_encode_device_health(const cairn_device_health_t *s, uint8_t out[16])
{
    memset(out, 0, 16);
    put_u16(out + 0, s->battery_mv);
    put_u16(out + 2, s->sd_write_errors);
    put_u16(out + 4, s->sd_free_mib);
    out[6] = (uint8_t)s->device_temp_c;
    out[7] = (uint8_t)s->rssi_dbm;
    put_u16(out + 8, s->ext_sensor_1);
    put_u16(out + 10, s->ext_sensor_2);
    out[12] = s->health_state;
    out[13] = s->reboot_count;
}

void cairn_encode_gnss_gap(const cairn_gnss_gap_t *s, uint8_t out[12])
{
    memset(out, 0, 12);
    put_u32(out + 0, s->duration_ms);
    put_u16(out + 4, s->expected_samples);
    out[6] = s->cause;
}

void cairn_encode_state_transition(const cairn_state_transition_t *s, uint8_t out[20])
{
    memset(out, 0, 20);
    out[0] = s->region;
    out[1] = s->from_state;
    out[2] = s->to_state;
    out[3] = s->trigger_event;
    out[4] = s->reason_code;
    out[5] = s->policy_version;
    put_u16(out + 8, s->start_score_e2);
    put_u16(out + 10, s->stop_score_e2);
    put_u32(out + 12, s->wake_cause);
}
