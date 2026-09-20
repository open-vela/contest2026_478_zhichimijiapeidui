/*
 * Four-channel VL6180X (TOF050C) ranging front end for the motion safety gate.
 *
 * Why this is a separate module: the safety gate in mibot_controller.cpp only
 * needs "distance + is this sample usable", while getting there involves an
 * awkward bring-up (four identical sensors on one bus, volatile address
 * reassignment, a 38-register init blob).  Keeping the two apart means the gate
 * stays readable and the driver can be swapped for a different sensor family
 * without touching safety logic.
 *
 * Read path timing is the constraint that shapes this API.  safety_task runs on
 * a fixed 50 ms phase at priority 12, so a read must not block: the sensors are
 * put in continuous ranging mode and mibot_tof_read() only ever does a few
 * register reads against whatever sample is already latched.
 */

#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* front_left, front_right, rear_left, rear_right — same order as
 * mibot_controller.cpp's TOF_NAMES and g_state.tof[]. */
#define MIBOT_TOF_COUNT 4

/* Bring up every sensor and leave it ranging continuously.
 *
 * Safe to call once, after I2C is initialised.  Never returns an error for a
 * missing or wrong sensor: a channel that fails to appear is simply left marked
 * unusable, which keeps motion locked instead of preventing boot.  Check
 * mibot_tof_channel_ready() or the logs to see what actually came up.
 *
 * Returns ESP_OK when at least the GPIO/bus setup succeeded, ESP_ERR_* when the
 * XSHUT lines could not be configured at all.
 */
esp_err_t mibot_tof_init(void);

/* Latest usable sample for one channel.
 *
 * Non-blocking.  Returns ESP_OK with a decoded distance, or:
 *   ESP_ERR_INVALID_ARG   index out of range / null out params
 *   ESP_ERR_INVALID_STATE channel never came up, or its address was lost
 *   ESP_ERR_TIMEOUT       no fresh sample within the staleness budget
 *   ESP_FAIL              the sensor reported an ambiguous measurement
 *
 * `quality` is 0 whenever the sample must not be trusted, matching what
 * safety_task already keys off.  A saturated "no target in range" reading is
 * reported as ESP_OK with a maximum distance so the edge threshold — not the
 * sensor-fault path — is what reacts to a drop-off.
 */
esp_err_t mibot_tof_read(uint8_t index, uint16_t *mm, uint8_t *quality);

/* Attempt to recover channels that stopped responding.
 *
 * Reassigning a VL6180X address is volatile, so a sensor that browns out
 * reappears at the factory address and stops answering on its assigned one.
 * Without this the robot would stay locked until a reboot.
 *
 * Costs a full re-init (about 40 register writes) for at most one channel per
 * call and self-throttles, so it is safe to call from the safety loop.  Cheap
 * no-op when every channel is healthy.
 */
void mibot_tof_maintain(void);

/* True when the channel completed bring-up and holds its assigned address. */
bool mibot_tof_channel_ready(uint8_t index);

/* Number of channels that completed bring-up. Intended for boot logging. */
uint8_t mibot_tof_ready_count(void);

#ifdef __cplusplus
}
#endif
