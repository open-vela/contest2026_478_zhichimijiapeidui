/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - standalone hardware-test argument contract (impl).
 *
 * Pure logic only: no NuttX, no board, no UART.  This is the single source of
 * truth for how `va_test` classifies its sub-command and for the motor/servo
 * range rules the ESP32 enforces, so both the device dispatcher (va_test.c)
 * and the host property tests (P15/P16) agree by construction.
 */

#include "va_test_args.h"

#include <string.h>

/* Motor range rules, mirrored from esp32 mibot_controller.cpp
 * robot.test_motor_a: speed in [-50,50] and != 0, duration_ms in [50,30000].
 *
 * The upper bound used to be 1000 ms, which is too short to probe the driver
 * pins with a multimeter; the ESP32 now allows up to 30 s for that, still
 * self-stopping at the deadline. */
#define VA_MOTOR_SPEED_MIN   (-50)
#define VA_MOTOR_SPEED_MAX   (50)
#define VA_MOTOR_MS_MIN      (50)
#define VA_MOTOR_MS_MAX      (30000)

/* Servo range rule, mirrored from robot.set_arm_pose: angle in [0,180]. */
#define VA_SERVO_DEG_MIN     (0)
#define VA_SERVO_DEG_MAX     (180)

/* Action names accepted by robot.perform_action, mirrored from the ESP32's
 * ACTION_PLANS table.  The ESP32 owns the motor and servo values behind each
 * name; the SF32 only names the action.
 *
 * Note which of these actually move the wheels, because it decides whether the
 * command exercises the motor path at all: idle, listening and greeting hold
 * both motors at 0 and move only the servos, by design (they are auto-played on
 * state entry, so they must never be blocked by a safety lock).  cute and happy
 * drive the two motors in opposite directions, which spins in place; surprised
 * drives them together, so the robot travels. */
static const char *const VA_ACTION_NAMES[] =
{
  "idle", "listening", "thinking", "happy", "sad", "confused",
  "surprised", "cute", "greeting", "warning",
};

#define VA_ACTION_NAME_COUNT \
  ((int)(sizeof(VA_ACTION_NAMES) / sizeof(VA_ACTION_NAMES[0])))

va_test_kind_t va_test_classify(const char *sub)
{
  if (sub == NULL)
    {
      return VA_TEST_UNKNOWN;
    }

  if (strcmp(sub, "mic") == 0)
    {
      return VA_TEST_MIC;
    }

  if (strcmp(sub, "spk") == 0)
    {
      return VA_TEST_SPK;
    }

  if (strcmp(sub, "hw") == 0)
    {
      return VA_TEST_HW_TONE;
    }

  if (strcmp(sub, "spkmic") == 0)
    {
      return VA_TEST_SPKMIC;
    }

  if (strcmp(sub, "spkstream") == 0)
    {
      return VA_TEST_SPKSTREAM;
    }

  if (strcmp(sub, "motor") == 0)
    {
      return VA_TEST_MOTOR;
    }

  if (strcmp(sub, "servo") == 0)
    {
      return VA_TEST_SERVO;
    }

  if (strcmp(sub, "action") == 0)
    {
      return VA_TEST_ACTION;
    }

  return VA_TEST_UNKNOWN;
}

bool va_test_needs_cloud(va_test_kind_t kind)
{
  /* No standalone hardware test needs the cloud (需求 10.5). */
  (void)kind;
  return false;
}

bool va_test_uses_state_machine(va_test_kind_t kind)
{
  /* No standalone hardware test drives the dialog main state machine: the
   * entry is a flat command dispatch independent of va_context_t (需求 10.5). */
  (void)kind;
  return false;
}

bool va_test_motor_params_valid(int speed, int duration_ms)
{
  if (speed == 0)
    {
      return false;
    }

  if (speed < VA_MOTOR_SPEED_MIN || speed > VA_MOTOR_SPEED_MAX)
    {
      return false;
    }

  if (duration_ms < VA_MOTOR_MS_MIN || duration_ms > VA_MOTOR_MS_MAX)
    {
      return false;
    }

  return true;
}

bool va_test_servo_params_valid(int deg)
{
  return deg >= VA_SERVO_DEG_MIN && deg <= VA_SERVO_DEG_MAX;
}

bool va_test_motor_channel_valid(const char *channel)
{
  if (channel == NULL)
    {
      return false;
    }

  return strcmp(channel, "a") == 0 ||
         strcmp(channel, "b") == 0 ||
         strcmp(channel, "both") == 0;
}

bool va_test_action_name_valid(const char *name)
{
  int i;

  if (name == NULL)
    {
      return false;
    }

  for (i = 0; i < VA_ACTION_NAME_COUNT; i++)
    {
      if (strcmp(name, VA_ACTION_NAMES[i]) == 0)
        {
          return true;
        }
    }

  return false;
}

bool va_test_action_moves_motors(const char *name)
{
  if (!va_test_action_name_valid(name))
    {
      return false;
    }

  /* Mirrors the ESP32 ACTION_PLANS table: these three hold both motors at 0 for
   * their whole sequence, so they prove nothing about the motor path. */
  return strcmp(name, "idle") != 0 &&
         strcmp(name, "listening") != 0 &&
         strcmp(name, "greeting") != 0;
}


