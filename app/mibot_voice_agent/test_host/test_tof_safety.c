/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Feature: mibot-voice-agent - property test for the ToF stub conservative
 * safety default (optional task 7.3).
 *
 * Covers the correctness property assigned to the ToF stub:
 *
 *   P14 - stubbed / invalid ToF data does not let a dangerous action through.
 *         Validates: 需求 9.4
 *
 * The real gate lives on the ESP32 (mibot_controller.cpp) and cannot compile on
 * host, so this exercises tof_safety_model, a faithful transcription of that
 * gate, checked against an independent inline reference.  A moving step with
 * any invalid/stale ToF reading (the exact situation when the ToF stub is
 * disabled and tof_read_mm() returns NOT_SUPPORTED, so readings are invalid)
 * must be DENIED, never allowed.
 *
 * No third-party PBT library: a fixed-seed LCG drives >= 100 iterations, and
 * the key "all invalid -> deny" case is also swept exhaustively.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "tof_safety_model.h"

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
/* Deterministic RNG (fixed-seed LCG).                                        */
/* ------------------------------------------------------------------------- */

static uint64_t g_rng = 0x70F5AFE70F5AFE07ULL;

static void rng_seed(uint64_t seed)
{
  g_rng = seed;
}

static uint32_t rng_next(void)
{
  g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return (uint32_t)(g_rng >> 32);
}

static int rng_range(int lo, int hi)
{
  uint32_t span = (uint32_t)(hi - lo + 1);
  return lo + (int)(rng_next() % span);
}

static bool rng_bool(void)
{
  return (rng_next() & 1u) != 0;
}

/* Constants mirroring the ESP32 config used in the model. */
#define TOF_TIMEOUT_MS      200
#define TOF_EDGE_THRESH_MM  120

/* Independent reference of the gate decision, written separately from
 * tof_safety_model.c so the two must agree. */
static tof_gate_result_t ref_decide(const tof_gate_input_t *in)
{
  bool moving = (in->left_motor != 0 || in->right_motor != 0);
  bool fwd    = (in->left_motor > 0 || in->right_motor > 0);
  bool all_valid = true;
  bool front_edge;
  int i;

  for (i = 0; i < 4; i++)
    {
      if (!in->tof[i].valid ||
          (in->now_ms - in->tof[i].timestamp_ms) > in->timeout_ms)
        {
          all_valid = false;
        }
    }

  front_edge =
      (in->tof[0].valid && in->tof[0].mm > in->edge_threshold_mm) ||
      (in->tof[1].valid && in->tof[1].mm > in->edge_threshold_mm);

  if (in->motion == TOF_MOTION_FAULT || in->motion == TOF_MOTION_BRAKING)
    {
      return TOF_GATE_DENY_SAFETY;
    }
  if (moving && !all_valid)
    {
      return TOF_GATE_DENY_TOF;
    }
  if (moving && fwd && front_edge)
    {
      return TOF_GATE_DENY_SAFETY;
    }
  return TOF_GATE_ALLOW;
}

/* Fill a context with a fully-valid, fresh, obstacle-free ToF set. */
static void make_safe_ctx(tof_gate_input_t *in)
{
  int i;

  in->now_ms = 100000;
  in->timeout_ms = TOF_TIMEOUT_MS;
  in->edge_threshold_mm = TOF_EDGE_THRESH_MM;
  in->motion = TOF_MOTION_STANDBY;
  in->left_motor = 0;
  in->right_motor = 0;

  for (i = 0; i < 4; i++)
    {
      in->tof[i].valid = true;
      in->tof[i].mm = 30;                 /* well under the edge threshold */
      in->tof[i].timestamp_ms = in->now_ms;   /* fresh */
    }
}

/* ========================================================================= */
/* Task 7.3 - P14.                                                            */
/* ========================================================================= */

/* Property P14 (core): whenever a step commands motion and ANY ToF reading is
 * invalid or stale, the gate must DENY the step (never allow it).  This is the
 * exact conservative-default situation the ToF stub protects: with the real
 * sensor unimplemented, readings are invalid, so no dangerous (moving) action
 * may execute.
 * Validates: 需求 9.4
 *
 * 500 random contexts with at least one bad reading and a moving command. */
static void prop_p14_invalid_tof_denies_motion(void)
{
  const int trials = 500;
  int t;

  rng_seed(0x14D0714D0714D071ULL);

  for (t = 0; t < trials; t++)
    {
      tof_gate_input_t in;
      int bad;
      int i;
      tof_gate_result_t got;

      make_safe_ctx(&in);

      /* Command some motion (moving step): non-zero motor, random direction. */
      do
        {
          in.left_motor = rng_range(-50, 50);
          in.right_motor = rng_range(-50, 50);
        }
      while (in.left_motor == 0 && in.right_motor == 0);

      /* Corrupt at least one reading: either mark it invalid or make it
       * stale.  Randomise the others too so we cover mixed sets. */
      bad = rng_range(0, 3);
      for (i = 0; i < 4; i++)
        {
          if (i == bad || rng_bool())
            {
              if (rng_bool())
                {
                  in.tof[i].valid = false;              /* stub-off default */
                }
              else
                {
                  in.tof[i].timestamp_ms =
                      in.now_ms - (in.timeout_ms + rng_range(1, 5000));
                }
            }
        }

      /* Ensure the chosen `bad` reading is definitely bad. */
      in.tof[bad].valid = false;

      got = tof_gate_decide(&in);

      CHECK(got != TOF_GATE_ALLOW,
            "P14: moving step with invalid/stale ToF is never allowed");
      CHECK(got == ref_decide(&in),
            "P14: gate decision matches the independent reference");
    }
}

/* Property P14 (all-invalid default): the exact stub-disabled state - every
 * reading invalid - denies motion for every non-zero motor command, while a
 * servo-only (non-moving) step is still allowed (idle/listening remain safe).
 * Validates: 需求 9.4 */
static void prop_p14_all_invalid_default(void)
{
  const int speeds[] = { -50, -25, -1, 0, 1, 25, 50 };
  int a;
  int b;

  for (a = 0; a < (int)(sizeof(speeds) / sizeof(speeds[0])); a++)
    {
      for (b = 0; b < (int)(sizeof(speeds) / sizeof(speeds[0])); b++)
        {
          tof_gate_input_t in;
          int i;
          bool moving;
          tof_gate_result_t got;

          make_safe_ctx(&in);

          /* Stub-disabled default: tof_read_mm() -> NOT_SUPPORTED, so every
           * reading is invalid. */
          for (i = 0; i < 4; i++)
            {
              in.tof[i].valid = false;
            }

          in.left_motor = speeds[a];
          in.right_motor = speeds[b];
          moving = (in.left_motor != 0 || in.right_motor != 0);

          got = tof_gate_decide(&in);

          if (moving)
            {
              CHECK(got == TOF_GATE_DENY_TOF,
                    "P14: all-invalid ToF denies any moving step (E_TOF_INVALID)");
            }
          else
            {
              CHECK(got == TOF_GATE_ALLOW,
                    "P14: servo-only step still allowed with invalid ToF");
            }

          CHECK(got == ref_decide(&in),
                "P14: all-invalid decision matches reference");
        }
    }
}

/* Property P14 (fuzz agreement): across fully random contexts (valid/invalid
 * mixes, edges, motion classes, motor commands), the model and the independent
 * reference always agree, and a dangerous (moving) step is never allowed while
 * ToF is not fully valid.
 * Validates: 需求 9.4 */
static void prop_p14_model_matches_reference(void)
{
  const int trials = 500;
  int t;

  rng_seed(0xF0FF0FF0FF0FF0FFULL);

  for (t = 0; t < trials; t++)
    {
      tof_gate_input_t in;
      int i;
      tof_gate_result_t got;

      make_safe_ctx(&in);

      in.motion = (tof_motion_t)rng_range(0, 4);
      in.left_motor = rng_range(-60, 60);
      in.right_motor = rng_range(-60, 60);

      for (i = 0; i < 4; i++)
        {
          in.tof[i].valid = rng_bool();
          in.tof[i].mm = (uint16_t)rng_range(0, 300);
          in.tof[i].timestamp_ms =
              in.now_ms - (int64_t)rng_range(0, 400);
        }

      got = tof_gate_decide(&in);

      CHECK(got == ref_decide(&in),
            "P14: model agrees with reference on random context");

      /* Safety invariant restated: if the step moves and ToF is not fully
       * valid, it must not be allowed. */
      {
        bool moving = (in.left_motor != 0 || in.right_motor != 0);
        if (moving && !tof_all_valid(&in) && got == TOF_GATE_ALLOW)
          {
            CHECK(false,
                  "P14: moving step allowed despite non-valid ToF");
          }
      }
    }
}

/* ========================================================================= */

int main(void)
{
  prop_p14_invalid_tof_denies_motion();
  prop_p14_all_invalid_default();
  prop_p14_model_matches_reference();

  if (g_failures == 0)
    {
      printf("All ToF safety property tests passed.\n");
      return 0;
    }

  printf("\n%d property check(s) failed.\n", g_failures);
  return 1;
}
