/*
 * The sensing task. Reads hardware, reports facts, owns no lifecycle state and
 * never touches the card.
 *
 * Pinned to the core the controller does not use, so a card write cannot stall
 * accelerometer sampling. That isolation is the reason this task exists.
 */

#ifndef CAIRN_SENSOR_TASK_H
#define CAIRN_SENSOR_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "facts.h"
#include "policy.h"
#include "sensors.h"

/*
 * Start sensing. `status` must outlive the task: it is shared, written only
 * here, and read by the controller for degraded-state reporting. The fields are
 * independent booleans, so a torn read costs a stale flag for one tick rather
 * than an inconsistent view.
 */
bool sensor_task_start(SensorStatus *status);

/* Take the next fact, if one is waiting. Non-blocking. */
bool sensor_task_poll(fact_t *out);

/* Facts dropped because the controller could not keep up. Reported in
 * DEVICE_HEALTH so the loss appears in the data, not only in the log. */
uint32_t sensor_task_dropped(void);

/* Ask the task to re-attempt subsystems that failed at boot. GNSS in particular
 * often appears only once the vehicle has been powered for a while. */
void sensor_task_request_retry(void);

/*
 * Set the sampling periods.
 *
 * The controller decides these, because classifying what the vehicle is doing
 * needs the whole picture — speed from whichever source answered, the trip
 * state, the policy — and the sensing task deliberately knows none of that. It
 * samples at the rate it is told.
 *
 * Written without a lock: three independent 16-bit periods, where a torn update
 * costs one interval at a stale rate. A mutex on the sampling path would be a
 * worse trade than that.
 */
void sensor_task_set_rates(const cairn_rates_t *r);

#endif /* CAIRN_SENSOR_TASK_H */
