/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Feature: mibot-voice-agent - property tests for the standalone hardware-test
 * argument contract (optional task 5.3).
 *
 * Covers the two correctness properties assigned to the `va_test` entry:
 *
 *   P15 - the standalone test entry (mic/spk/motor/servo) neither depends on
 *         the cloud nor enters the dialog main state machine.
 *         Validates: 需求 10.5
 *   P16 - out-of-range motor/servo parameters are reported as errors rather
 *         than executed.
 *         Validates: 需求 10.6
 *
 * These are pure-logic properties: P15 is proved against va_test_args (which
 * models the flat, cloud-free, state-machine-free dispatch contract) and P16
 * against the motor/servo range reference model that mirrors the ESP32 rules.
 * No third-party PBT library: a fixed-seed LCG drives >= 100 iterations per
 * randomised property, and the small spaces are also swept exhaustively.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "va_test_args.h"

static int g_failures;

#define CHECK(cond, msg)                                                     \
  do                                                                         \
    {                                                                        \
      if (!(cond))                                                           \
        {                                                                    \
          printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);           \
          g_failures++;                                                      \
        }                                                                    \
    }                                                                        \
  while (0)

/* ------------------------------------------------------------------------- */
/* Deterministic RNG (fixed-seed LCG) for reproducible random sequences.     */
/* ------------------------------------------------------------------------- */

static uint64_t g_rng = 0x0f1e2d3c4b5a6978ULL;

static void rng_seed(uint64_t seed)
{
  g_rng = seed;
}

static uint32_t rng_next(void)
{
  g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return (uint32_t)(g_rng >> 32);
}

/* Uniform int in [lo, hi]. */
static int rng_range(int lo, int hi)
{
  uint32_t span = (uint32_t)(hi - lo + 1);
  return lo + (int)(rng_next() % span);
}

/* ------------------------------------------------------------------------- */
/* Fixtures.                                                                  */
/* ------------------------------------------------------------------------- */

static const char *const g_known_subs[] =
{
  "mic", "spk", "motor", "servo", "action",
};
#define KNOWN_SUB_COUNT ((int)(sizeof(g_known_subs) / sizeof(g_known_subs[0])))

static const va_test_kind_t g_known_kinds[] =
{
  VA_TEST_MIC, VA_TEST_SPK, VA_TEST_MOTOR, VA_TEST_SERVO, VA_TEST_ACTION,
};

static const char *const g_unknown_subs[] =
{
  "", "wake", "dance", "MIC", "Motor", "help", "state", "listen", "spin",
};
#define UNKNOWN_SUB_COUNT ((int)(sizeof(g_unknown_subs) / sizeof(g_unknown_subs[0])))

/* ========================================================================= */
/* Task 5.3 - P15.                                                            */
/* ========================================================================= */

/* Property P15: the standalone test entry does not depend on the cloud and
 * does not enter the dialog main state machine, for every sub-command kind.
 * Exhaustive over the four known kinds plus the UNKNOWN sentinel.
 * Validates: 需求 10.5 */
static void prop_p15_standalone_independent(void)
{
  int i;

  /* Known kinds: never need cloud, never touch the state machine. */
  for (i = 0; i < KNOWN_SUB_COUNT; i++)
    {
      va_test_kind_t kind = va_test_classify(g_known_subs[i]);

      CHECK(kind == g_known_kinds[i],
            "P15: sub-command classifies to its own kind");
      CHECK(va_test_needs_cloud(kind) == false,
            "P15: standalone test does not depend on the cloud");
      CHECK(va_test_uses_state_machine(kind) == false,
            "P15: standalone test does not enter the main state machine");
    }

  /* The UNKNOWN sentinel is equally cloud-free / state-machine-free. */
  CHECK(va_test_needs_cloud(VA_TEST_UNKNOWN) == false,
        "P15: UNKNOWN kind does not depend on the cloud");
  CHECK(va_test_uses_state_machine(VA_TEST_UNKNOWN) == false,
        "P15: UNKNOWN kind does not enter the main state machine");
}

/* Property P15 (classification form): known tokens map to their kind and any
 * other token is UNKNOWN, so the dispatch is a flat, closed command table with
 * no hidden state.  Validates: 需求 10.5 */
static void prop_p15_classification_total(void)
{
  int i;

  for (i = 0; i < KNOWN_SUB_COUNT; i++)
    {
      CHECK(va_test_classify(g_known_subs[i]) == g_known_kinds[i],
            "P15: known sub-command maps to its kind");
    }

  for (i = 0; i < UNKNOWN_SUB_COUNT; i++)
    {
      CHECK(va_test_classify(g_unknown_subs[i]) == VA_TEST_UNKNOWN,
            "P15: unrecognised token classifies as UNKNOWN");
    }

  /* NULL is safe and classifies as UNKNOWN. */
  CHECK(va_test_classify(NULL) == VA_TEST_UNKNOWN,
        "P15: NULL token classifies as UNKNOWN");
}

/* ========================================================================= */
/* Task 5.3 - P16.                                                            */
/* ========================================================================= */

/* Reference bounds, independent of va_test_args.c internals, taken straight
 * from the ESP32 rules in mibot_controller.cpp. */
static bool ref_motor_valid(int speed, int ms)
{
  return speed != 0 && speed >= -50 && speed <= 50 && ms >= 50 && ms <= 30000;
}

static bool ref_servo_valid(int deg)
{
  return deg >= 0 && deg <= 180;
}

/* Property P16 (motor): the motor parameter validator accepts exactly the
 * in-range, non-zero-speed combinations and rejects everything else, so an
 * out-of-range request is flagged as an error rather than executed.
 * Validates: 需求 10.6
 *
 * 400 random (speed, ms) pairs spanning well beyond the legal window, plus an
 * exhaustive sweep of the boundary values. */
static void prop_p16_motor_range(void)
{
  const int trials = 400;
  int t;

  rng_seed(0x16A0716A0716A071ULL);

  for (t = 0; t < trials; t++)
    {
      int speed = rng_range(-120, 120);
      int ms    = rng_range(-200, 40000);

      CHECK(va_test_motor_params_valid(speed, ms) == ref_motor_valid(speed, ms),
            "P16: motor validity matches the ESP32 range reference");
    }

  /* Boundary sweep: just inside / on / just outside every edge.  1000 is kept
   * as an interior point because it used to be the upper bound. */
  {
    const int speeds[] = { -51, -50, -1, 0, 1, 50, 51 };
    const int mss[]     = { 49, 50, 500, 1000, 29999, 30000, 30001 };
    int a;
    int b;

    for (a = 0; a < (int)(sizeof(speeds) / sizeof(speeds[0])); a++)
      {
        for (b = 0; b < (int)(sizeof(mss) / sizeof(mss[0])); b++)
          {
            int speed = speeds[a];
            int ms    = mss[b];

            CHECK(va_test_motor_params_valid(speed, ms) ==
                  ref_motor_valid(speed, ms),
                  "P16: motor boundary value matches reference");
          }
      }
  }

  /* Zero speed is always rejected even with a legal duration. */
  CHECK(va_test_motor_params_valid(0, 500) == false,
        "P16: zero motor speed is rejected");
}

/* Property P16 (servo): the servo validator accepts exactly [0,180] and
 * rejects everything else (the ESP32 returns E_SERVO_LIMIT out of range).
 * Validates: 需求 10.6
 *
 * 400 random angles well outside the window, plus a boundary sweep. */
static void prop_p16_servo_range(void)
{
  const int trials = 400;
  int t;

  rng_seed(0x53E7005EA1053E70ULL);

  for (t = 0; t < trials; t++)
    {
      int deg = rng_range(-90, 360);

      CHECK(va_test_servo_params_valid(deg) == ref_servo_valid(deg),
            "P16: servo validity matches the ESP32 range reference");
    }

  {
    const int degs[] = { -1, 0, 1, 90, 179, 180, 181 };
    int i;

    for (i = 0; i < (int)(sizeof(degs) / sizeof(degs[0])); i++)
      {
        CHECK(va_test_servo_params_valid(degs[i]) == ref_servo_valid(degs[i]),
              "P16: servo boundary value matches reference");
      }
  }
}

/* ========================================================================= */

/* The action names must match the ESP32's ACTION_PLANS table exactly: an
 * accepted-here-but-rejected-there name turns a hardware test into a confusing
 * E_INVALID_ARG, and a rejected-here-but-valid-there name hides a usable
 * action.  The motor/servo split matters just as much, because running an
 * action that holds both motors at 0 and seeing still wheels looks identical to
 * a dead motor. */
static void prop_action_names(void)
{
  static const char *const moves[] =
  {
    "thinking", "happy", "sad", "confused", "surprised", "cute", "warning",
  };
  static const char *const servos_only[] = { "idle", "listening", "greeting" };
  static const char *const invalid[] =
  {
    "", "Happy", "dance", "spin", "wave", "idle ", "angry",
  };
  int i;

  for (i = 0; i < (int)(sizeof(moves) / sizeof(moves[0])); i++)
    {
      CHECK(va_test_action_name_valid(moves[i]) == true,
            "action name accepted");
      CHECK(va_test_action_moves_motors(moves[i]) == true,
            "action drives both motors");
    }

  for (i = 0; i < (int)(sizeof(servos_only) / sizeof(servos_only[0])); i++)
    {
      CHECK(va_test_action_name_valid(servos_only[i]) == true,
            "servo-only action name accepted");
      CHECK(va_test_action_moves_motors(servos_only[i]) == false,
            "servo-only action reported as not driving motors");
    }

  for (i = 0; i < (int)(sizeof(invalid) / sizeof(invalid[0])); i++)
    {
      CHECK(va_test_action_name_valid(invalid[i]) == false,
            "unknown action name rejected");
      CHECK(va_test_action_moves_motors(invalid[i]) == false,
            "unknown action reported as not driving motors");
    }

  CHECK(va_test_action_name_valid(NULL) == false,
        "NULL action name rejected");
  CHECK(va_test_action_moves_motors(NULL) == false,
        "NULL action reported as not driving motors");
}

/* The channel names must match what the ESP32 accepts, or a bench probe aimed
 * at motor B silently drives motor A instead (absent/unknown defaults to "a"
 * there), which would read as "B works" when B was never energised. */
static void prop_motor_channels(void)
{
  static const char *const valid[] = { "a", "b", "both" };
  static const char *const invalid[] = { "", "A", "B", "Both", "ab", "left", "0" };
  int i;

  for (i = 0; i < (int)(sizeof(valid) / sizeof(valid[0])); i++)
    {
      CHECK(va_test_motor_channel_valid(valid[i]) == true,
            "motor channel accepted");
    }

  for (i = 0; i < (int)(sizeof(invalid) / sizeof(invalid[0])); i++)
    {
      CHECK(va_test_motor_channel_valid(invalid[i]) == false,
            "unknown motor channel rejected");
    }

  CHECK(va_test_motor_channel_valid(NULL) == false,
        "NULL motor channel rejected");
}

int main(void)
{
  /* Task 5.3 - P15 */
  prop_p15_standalone_independent();
  prop_p15_classification_total();

  /* Task 5.3 - P16 */
  prop_p16_motor_range();
  prop_p16_servo_range();

  prop_action_names();
  prop_motor_channels();

  if (g_failures == 0)
    {
      printf("All va_test_args property tests passed.\n");
      return 0;
    }

  printf("\n%d property check(s) failed.\n", g_failures);
  return 1;
}
