/*
 * Manual-transmission probe. Only compiled when CAIRN_MTPROBE is defined; see
 * src/mtprobe.cpp for what it asks and why.
 */

#ifndef CAIRN_MTPROBE_H
#define CAIRN_MTPROBE_H

#include "config.h"

#if CAIRN_MTPROBE
/*
 * Issues at most one probe request per CAIRN_MTPROBE_PERIOD_MS. Call from the
 * sensing task, with the bus open and the ECU connected: it talks to the OBD
 * link, which only that task may do.
 */
void mtprobe_tick(void);
#endif

#endif /* CAIRN_MTPROBE_H */
