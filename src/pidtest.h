/*
 * PID validation pass. Only compiled when CAIRN_PIDTEST is defined; see
 * src/pidtest.cpp for why it exists.
 */

#ifndef CAIRN_PIDTEST_H
#define CAIRN_PIDTEST_H

#include "config.h"

#if CAIRN_PIDTEST
/* Prints one probe block per CAIRN_PIDTEST_PERIOD_MS. Safe to call every tick. */
void pidtest_tick(void);
#endif

#endif /* CAIRN_PIDTEST_H */
