#include "cairn_log.h"

#include <Arduino.h>
#include <SD.h>
#include <ff.h>

#include <stdio.h>
#include <string.h>

#include "board_config.h"

/* ── state ────────────────────────────────────────────────────────────────── */

/*
 * Lines written before the card mounts land here. 8 KB covers boot through SD
 * init with room to spare; overflow is counted rather than allowed to overwrite,
 * because the first lines of a failing boot are the informative ones and the
 * later ones are usually repetition.
 */
#define PREMOUNT_BUFFER_BYTES 8192

#define CAIRN_LINE_MAX 320

/* Lines are flushed on a timer unless the level demands immediacy. 2 s bounds
 * what a pulled fuse can cost to a few INFO lines. */
#define FLUSH_INTERVAL_MS 2000

static SemaphoreHandle_t   s_mutex;
static cairn_log_level_t   s_level = (cairn_log_level_t)CAIRN_LOG_LEVEL;
static bool                s_initialized;

static File     s_file;
static bool     s_sd_attached;
static bool     s_sd_suspended;
static uint32_t s_file_bytes;
static uint32_t s_file_index;
static uint32_t s_boot_count;
static uint32_t s_last_flush_ms;
static bool     s_dirty;

static char     s_premount[PREMOUNT_BUFFER_BYTES];
static size_t   s_premount_len;
static bool     s_premount_overflowed;

static char     s_boot_id_hex[9]; /* first 4 bytes of the boot id, for grepping */

static cairn_log_stats_t s_stats;

/* ── helpers ──────────────────────────────────────────────────────────────── */

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

static void lock(void)
{
    if (s_mutex != nullptr) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_mutex != nullptr) xSemaphoreGive(s_mutex);
}

static void log_path(uint32_t boot, uint32_t index, char *out, size_t cap)
{
    snprintf(out, cap, "%s/boot-%06u-%03u.log", CAIRN_DIR_LOGS,
             (unsigned)boot, (unsigned)index);
}

/*
 * One pass over the log directory through FatFs: how many files, how many bytes, and
 * the oldest one by name.
 *
 * The Arduino directory iterator (File::openNextFile) opens and stats every entry, and a
 * FAT stat is itself a directory search, so a pass over N files costs O(N squared)
 * directory reads. Measured on the dongle's card: 267 files took 23 s per pass, and the
 * pass ran on every boot and on every rotation, with the log lock held and capture
 * running. f_readdir returns each entry's size as it goes: one sequential read of the
 * directory, no per-file open.
 *
 * Returns false if FatFs cannot open the directory (drive number not found, no card), in
 * which case the caller falls back to the slow path rather than skipping enforcement.
 */
struct LogScan {
    uint32_t total;
    int      files;
    char     oldest[96];
    uint32_t oldest_rank;
};

static bool scan_logs_fast(LogScan *out)
{
    memset(out, 0, sizeof(*out));
    out->oldest_rank = UINT32_MAX;

    /* The SD library registers its volume with the first free FatFs drive; there is
     * only one card, so it is drive 0, but look rather than assume. */
    for (int drv = 0; drv < FF_VOLUMES; drv++) {
        char dirpath[40];
        snprintf(dirpath, sizeof(dirpath), "%d:%s", drv, CAIRN_DIR_LOGS);

        FF_DIR dir;
        if (f_opendir(&dir, dirpath) != FR_OK) continue;

        FILINFO fno;
        for (;;) {
            if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
            if (fno.fattrib & AM_DIR) continue;

            out->total += (uint32_t)fno.fsize;
            out->files++;

            unsigned b = 0, i = 0;
            if (sscanf(fno.fname, "boot-%6u-%3u.log", &b, &i) == 2) {
                uint32_t rank = b * 1000u + i;
                if (rank < out->oldest_rank) {
                    out->oldest_rank = rank;
                    snprintf(out->oldest, sizeof(out->oldest), "%s/%s", CAIRN_DIR_LOGS, fno.fname);
                }
            }
        }
        f_closedir(&dir);
        return true;
    }
    return false;
}

/*
 * Enforce the total log budget oldest-first.
 *
 * Deleting log files needs no receipt, and must not be confused with pruning
 * bundles, which does: this only ever walks CAIRN_DIR_LOGS.
 */
static void enforce_log_budget_slow(void);

static void enforce_log_budget(void)
{
    /* Each pass is one directory read, so the bound is on deletions, not on scans; the
     * time cap stops a card with thousands of old files from stalling the boot. */
    uint32_t started = millis();
    for (int pass = 0; pass < 256; pass++) {
        LogScan scan;
        if (!scan_logs_fast(&scan)) {
            enforce_log_budget_slow();
            return;
        }

        /* Keep at least one file: deleting the only log to satisfy a budget
         * would destroy the very thing the budget exists to preserve. */
        if (scan.total <= CAIRN_LOG_TOTAL_BUDGET_BYTES || scan.files <= 1) return;
        if (scan.oldest[0] == '\0') return; /* nothing recognizable to drop */

        SD.remove(scan.oldest);
        if (millis() - started > 3000) return; /* the rest goes at the next attach or rotation */
    }
}

/* The original slow walk, kept for a card where FatFs cannot be reached directly. */
static void enforce_log_budget_slow(void)
{
    /*
     * Delete oldest-first until the tree fits. Iterative rather than recursive:
     * the number of deletions is bounded only by how many files are on the card,
     * and recursion that deep would overflow an embedded stack.
     *
     * Each pass re-opens the directory because removing an entry invalidates the
     * iterator. The bound keeps a pathological card from looping forever.
     */
    for (int pass = 0; pass < 256; pass++) {
        File dir = SD.open(CAIRN_DIR_LOGS);
        if (!dir) return;

        uint32_t total = 0;
        int      file_count = 0;
        char     oldest[96] = { 0 };
        uint32_t oldest_rank = UINT32_MAX;

        for (;;) {
            File entry = dir.openNextFile();
            if (!entry) break;

            if (!entry.isDirectory()) {
                total += (uint32_t)entry.size();
                file_count++;

                /*
                 * Names sort by boot then index, so lexicographic order is
                 * chronological. Wall-clock mtime is not usable here: the device
                 * often has no valid UTC at boot, which is the same reason
                 * ordering truth in the format is (boot_id, seq).
                 */
                const char *name = entry.name();
                const char *base = strrchr(name, '/');
                base = (base != nullptr) ? base + 1 : name;

                unsigned b = 0, i = 0;
                if (sscanf(base, "boot-%6u-%3u.log", &b, &i) == 2) {
                    uint32_t rank = b * 1000u + i;
                    if (rank < oldest_rank) {
                        oldest_rank = rank;
                        snprintf(oldest, sizeof(oldest), "%s/%s", CAIRN_DIR_LOGS,
                                 base);
                    }
                }
            }
            entry.close();
        }
        dir.close();

        /* Keep at least one file: deleting the only log to satisfy a budget
         * would destroy the very thing the budget exists to preserve. */
        if (total <= CAIRN_LOG_TOTAL_BUDGET_BYTES || file_count <= 1) return;
        if (oldest[0] == '\0') return; /* nothing recognizable to drop */

        SD.remove(oldest);
    }
}

static bool open_next_file(void)
{
    char path[96];
    log_path(s_boot_count, s_file_index, path, sizeof(path));

    s_file = SD.open(path, FILE_APPEND);
    if (!s_file) {
        s_stats.sd_write_errors++;
        return false;
    }

    /*
     * The file's current length, for the rotation check. Not File::size(): for a file
     * that did not exist before this open, the Arduino core never fills in the stat buffer
     * size() reads (VFSFileImpl: stat() fails, _stat stays uninitialised, and size() only
     * refreshes it after a write). It returned whatever the heap held, here the bytes
     * "/cai" of the path string, 1767990063, which is far past the 2 MiB rotation limit,
     * so the first write rotated, the next file did the same, and every log line rotated
     * the file and re-scanned the directory. Seeking to the end and asking for the
     * position is the real length for both a new and an existing file.
     */
    s_file.seek(0, SeekEnd);
    s_file_bytes = (uint32_t)s_file.position();
    return true;
}

/*
 * Logging yields to capture data. Below the free-space floor the SD sink shuts
 * itself off; UART logging is unaffected, and the condition is reported in
 * DEVICE_HEALTH so it shows up in the data rather than only here.
 */
static bool check_free_space(void)
{
    uint64_t total = SD.totalBytes();
    uint64_t used  = SD.usedBytes();
    uint64_t free_mib = (total > used) ? (total - used) / (1024 * 1024) : 0;

    if (free_mib < CAIRN_LOG_FREE_SPACE_FLOOR_MIB) {
        if (!s_sd_suspended) {
            s_sd_suspended = true;
            s_stats.sd_suspended = true;
            Serial.printf(
                "[LOG ] SD logging suspended: %llu MiB free is below the "
                "%d MiB floor reserved for capture data\n",
                (unsigned long long)free_mib, CAIRN_LOG_FREE_SPACE_FLOOR_MIB);
            if (s_file) {
                s_file.flush();
                s_file.close();
            }
        }
        return false;
    }

    return true;
}

static void write_to_sd(const char *line, size_t len)
{
    if (!s_sd_attached || s_sd_suspended) return;

    if (s_file_bytes >= CAIRN_LOG_ROTATE_BYTES) {
        s_file.flush();
        s_file.close();
        s_file_index++;
        enforce_log_budget();
        if (!open_next_file()) {
            s_sd_attached = false;
            s_stats.sd_attached = false;
            return;
        }
    }

    size_t written = s_file.write((const uint8_t *)line, len);
    if (written != len) {
        s_stats.sd_write_errors++;
        /*
         * A short write usually means the card is full or gone. Re-check free
         * space, which either suspends the sink or leaves the error counted and
         * visible in DEVICE_HEALTH.
         */
        check_free_space();
        return;
    }

    s_file_bytes += (uint32_t)written;
    s_stats.bytes_written += (uint32_t)written;
    s_dirty = true;
}

static void emit(cairn_log_level_t level, const char *line, size_t len)
{
    Serial.write((const uint8_t *)line, len);

    if (s_sd_attached) {
        write_to_sd(line, len);

        /* WARN and ERROR reach the card before the next statement runs: these
         * are the lines that explain a failure, and the failure may be the
         * thing that stops the next flush from happening. */
        if (level <= CAIRN_LOG_WARN) {
            s_file.flush();
            s_dirty = false;
            s_last_flush_ms = millis();
        }
    } else if (!s_premount_overflowed) {
        if (s_premount_len + len < sizeof(s_premount)) {
            memcpy(s_premount + s_premount_len, line, len);
            s_premount_len += len;
        } else {
            s_premount_overflowed = true;
            s_stats.lines_dropped++;
        }
    } else {
        s_stats.lines_dropped++;
    }

    s_stats.lines_emitted++;
}

/* ── public API ───────────────────────────────────────────────────────────── */

void cairn_log_init(uint32_t baud)
{
    if (s_initialized) return;

    s_mutex = xSemaphoreCreateMutex();

    /*
     * The console carries provisioning lines of up to ~4 KB (a PEM key in
     * base64). Arduino's default UART receive buffer is 256 bytes, so a long
     * line arriving while this task is busy elsewhere silently loses bytes and
     * the base64 no longer decodes — which looks exactly like a bad credential.
     * The size must be set BEFORE begin().
     */
    Serial.setRxBufferSize(8192);
    Serial.begin(baud);

    memset(&s_stats, 0, sizeof(s_stats));
    s_premount_len = 0;
    s_premount_overflowed = false;
    s_sd_attached = false;
    s_sd_suspended = false;
    s_last_flush_ms = millis();
    snprintf(s_boot_id_hex, sizeof(s_boot_id_hex), "--------");

    s_initialized = true;
}

bool cairn_log_attach_sd(uint32_t boot_count)
{
    lock();

    s_boot_count = boot_count;
    s_file_index = 0;

    if (!SD.exists(CAIRN_DIR_ROOT)) SD.mkdir(CAIRN_DIR_ROOT);
    if (!SD.exists(CAIRN_DIR_LOGS)) SD.mkdir(CAIRN_DIR_LOGS);

    if (!check_free_space()) {
        unlock();
        return false;
    }

    enforce_log_budget();

    /* Never append to a previous boot's file: a fresh file per boot makes the
     * reboot boundary unambiguous when reading logs after the fact. */
    char path[96];
    while (s_file_index < 999) {
        log_path(s_boot_count, s_file_index, path, sizeof(path));
        if (!SD.exists(path)) break;
        s_file_index++;
    }

    if (!open_next_file()) {
        unlock();
        return false;
    }

    s_sd_attached = true;
    s_stats.sd_attached = true;

    /* Drain whatever accumulated before the card was available. */
    if (s_premount_len > 0) {
        write_to_sd(s_premount, s_premount_len);
        s_premount_len = 0;
    }
    if (s_premount_overflowed) {
        const char *note =
            "[LOG ] pre-mount buffer overflowed; some early lines were dropped\n";
        write_to_sd(note, strlen(note));
    }

    s_file.flush();
    s_dirty = false;

    unlock();

    CAIRN_LOGI("LOG", "SD sink attached: %s (boot %u, %u MiB free)", path,
               (unsigned)boot_count,
               (unsigned)((SD.totalBytes() - SD.usedBytes()) / (1024 * 1024)));
    return true;
}

void cairn_log_detach_sd(void)
{
    lock();
    if (s_sd_attached) {
        if (s_file) {
            s_file.flush();
            s_file.close();
        }
        s_sd_attached = false;
        s_stats.sd_attached = false;
        s_dirty = false;
    }
    unlock();
}

void cairn_log_set_level(cairn_log_level_t level)
{
    lock();
    s_level = level;
    unlock();
}

cairn_log_level_t cairn_log_get_level(void)
{
    return s_level;
}

void cairn_log_set_context(const uint8_t boot_id[16])
{
    lock();
    snprintf(s_boot_id_hex, sizeof(s_boot_id_hex), "%02x%02x%02x%02x",
             boot_id[0], boot_id[1], boot_id[2], boot_id[3]);
    unlock();
}

void cairn_log_line(cairn_log_level_t level, const char *tag,
                    const char *fmt, ...)
{
    if (!s_initialized || level > s_level) return;

    char line[CAIRN_LINE_MAX];

    /*
     * Monotonic milliseconds, not UTC. UTC may be absent or may jump, and a log
     * whose timestamps go backwards is worse than one with no timestamps; the
     * boot id makes lines attributable to a specific boot.
     */
    int n = snprintf(line, sizeof(line), "%10lu %s [%s] %-5s ",
                     (unsigned long)millis(), s_boot_id_hex, tag,
                     level_name(level));
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
    va_end(ap);

    size_t len = (size_t)n + ((m > 0) ? (size_t)m : 0);
    if (len > sizeof(line) - 2) len = sizeof(line) - 2;

    line[len++] = '\n';
    line[len] = '\0';

    lock();
    emit(level, line, len);
    unlock();
}

void cairn_log_hexdump(cairn_log_level_t level, const char *tag,
                       const char *what, const uint8_t *data, size_t len)
{
    if (!s_initialized || level > s_level) return;

    cairn_log_line(level, tag, "%s (%u bytes)", what, (unsigned)len);

    for (size_t off = 0; off < len; off += 16) {
        char hex[16 * 3 + 1];
        char ascii[17];
        size_t n = (len - off < 16) ? len - off : 16;

        for (size_t i = 0; i < n; i++) {
            snprintf(hex + i * 3, 4, "%02x ", data[off + i]);
            uint8_t c = data[off + i];
            ascii[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        hex[n * 3] = '\0';
        ascii[n] = '\0';

        cairn_log_line(level, tag, "  %04x  %-48s |%s|", (unsigned)off, hex, ascii);
    }
}

void cairn_log_flush(void)
{
    lock();
    if (s_sd_attached && s_dirty && s_file) {
        s_file.flush();
        s_dirty = false;
        s_last_flush_ms = millis();
    }
    unlock();
}

void cairn_log_tick(void)
{
    if (!s_sd_attached || !s_dirty) return;

    uint32_t now = millis();
    if (now - s_last_flush_ms < FLUSH_INTERVAL_MS) return;

    cairn_log_flush();
}

void cairn_log_get_stats(cairn_log_stats_t *out)
{
    lock();
    *out = s_stats;
    unlock();
}
