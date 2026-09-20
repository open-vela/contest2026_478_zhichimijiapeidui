/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - standalone hardware-test argument contract.
 *
 * This header holds the *pure-logic* contract behind the `va_test` NSH command
 * (mic/spk/hw/motor/servo).  It deliberately depends on nothing from NuttX or the
 * board so it can be compiled and property-tested on a host, the same way the
 * state-machine core is (mibot_voice_agent.h).
 *
 * Two things are captured here:
 *
 *   1. Sub-command classification (va_test_classify): which of the four
 *      standalone tests a token names, or "unknown".  Used by the device
 *      dispatcher and by property P15 to prove the test entry is a flat command
 *      dispatch that neither consults Cloud_Available nor enters the dialog
 *      main state machine.
 *
 *   2. Motor/servo parameter validity (va_test_motor_params_valid /
 *      va_test_servo_params_valid): a reference model of the range rules the
 *      ESP32 enforces for robot.test_motor_a and robot.set_arm_pose.  The SF32
 *      side forwards the command and the ESP32 rejects out-of-range values, so
 *      this model lets property P16 assert "out-of-range -> error, not
 *      execution" on host without the ESP32 in the loop.
 *
 * Requirements traceability: 10.5 (standalone, cloud-independent, outside the
 * main state machine) and 10.6 (out-of-range motor/servo params rejected).
 */

#ifndef VA_TEST_ARGS_H
#define VA_TEST_ARGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The five standalone hardware tests, plus an "unknown" sentinel. */
typedef enum
{
  VA_TEST_UNKNOWN = 0,
  VA_TEST_MIC,
  VA_TEST_SPK,
  VA_TEST_HW_TONE,
  VA_TEST_SPKMIC,
  VA_TEST_SPKSTREAM,
  VA_TEST_MOTOR,
  VA_TEST_SERVO,
  VA_TEST_ACTION,
} va_test_kind_t;

/* Classify a sub-command token ("mic"/"spk"/"hw"/"motor"/"servo"/"action").  A
 * NULL or unrecognised token classifies as VA_TEST_UNKNOWN.  Pure, no side
 * effects. */
va_test_kind_t va_test_classify(const char *sub);

/* True iff a standalone test kind requires the cloud to run.  None of them do:
 * mic/spk exercise local board audio and motor/servo go straight to the ESP32
 * over UART, so this is always false for known kinds (需求 10.5, P15). */
bool va_test_needs_cloud(va_test_kind_t kind);

/* True iff a standalone test kind drives the dialog main state machine.  None
 * of them do: the test entry is independent of va_context_t (需求 10.5, P15).
 * Always false for known kinds. */
bool va_test_uses_state_machine(va_test_kind_t kind);

/* Reference model of the ESP32 robot.test_motor_a range rules: speed must be
 * in [-50, 50] and non-zero, duration_ms in [50, 30000].  Out-of-range values
 * are rejected by the ESP32 rather than executed (需求 10.6, P16). */
bool va_test_motor_params_valid(int speed, int duration_ms);

/* True iff channel names a half-bridge the motor test can drive: "a", "b" or
 * "both".  The ESP32 defaults to "a" when the field is absent. */
bool va_test_motor_channel_valid(const char *channel);

/* Reference model of the ESP32 robot.set_arm_pose range rule: each servo angle
 * must be in [0, 180]; out-of-range returns E_SERVO_LIMIT (需求 10.6, P16). */
bool va_test_servo_params_valid(int deg);

/* True iff name is one of the bounded actions in the ESP32's ACTION_PLANS table,
 * i.e. something robot.perform_action will accept.  The ESP32 owns the motor and
 * servo values behind the name; only the name travels over the link. */
bool va_test_action_name_valid(const char *name);

/* True iff the named action actually drives the wheel motors.  idle, listening
 * and greeting hold both motors at 0 for their whole sequence, so they exercise
 * only the servos -- running one of those and seeing no wheel movement is
 * correct behaviour, not a motor fault. */
bool va_test_action_moves_motors(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* VA_TEST_ARGS_H */
