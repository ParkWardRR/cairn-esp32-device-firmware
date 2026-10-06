#ifndef BOOT_TIMING_H
#define BOOT_TIMING_H

#include "cairn_boottime.h"

/*
 * The boot timing record (lib/cairn_boottime) wired to the real clock. Marks come from
 * several tasks, so the first-mark-wins check has a lock-free fast path: once a stage
 * is recorded, marking it again costs one load.
 */

/* First line of setup(): fixes T0 and reads the reset reason. */
void boot_timing_begin(void);

/* Record that a stage was reached; later calls for the same stage do nothing. */
void boot_timing_mark(cairn_boot_stage_t stage);

/* A copy of the record as it stands, for the console and (later) device info. */
void boot_timing_snapshot(cairn_boottime_t *out);

/* The chip's own reset code (esp_reset_reason()), as the device-info contract reports it. */
uint8_t boot_timing_reset_code(void);

/* Print the record on the log, once the stages of interest have been reached. */
void boot_timing_log(void);

#endif /* BOOT_TIMING_H */
