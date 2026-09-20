/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - portable model of the ESP32 ToF safety gate (test only).
 *
 * Transcription of the decision logic in mibot_controller.cpp:
 *   - tof_all_valid_locked(): every reading must be valid and within timeout.
 *   - front_edge_locked():    either front sensor past the edge threshold.
 *   - apply_action_step_locked() gate order:
 *       1. motion == Fault || Braking            -> E_SAFETY_LOCK
 *       2. moving && !tof_all_valid_locked        -> E_TOF_INVALID
 *       3. moving && forward/turn && front_edge   -> E_SAFETY_LOCK
 *       else                                       -> allow
 *     where "moving" = (left_motor != 0 || right_motor != 0) and
 *     "forward/turn" = (left_motor > 0 || right_motor > 0).
 */

#include "tof_safety_model.h"

bool tof_all_valid(const tof_gate_input_t *in)
{
  int i;

  if (in == NULL)
    {
      return false;
    }

  for (i = 0; i < 4; i++)
    {
      const tof_reading_t *r = &in->tof[i];

      if (!r->valid)
        {
          return false;
        }

      if (in->now_ms - r->timestamp_ms > in->timeout_ms)
        {
          return false;
        }
    }

  return true;
}

bool tof_front_edge(const tof_gate_input_t *in)
{
  if (in == NULL)
    {
      return false;
    }

  return (in->tof[0].valid && in->tof[0].mm > in->edge_threshold_mm) ||
         (in->tof[1].valid && in->tof[1].mm > in->edge_threshold_mm);
}

tof_gate_result_t tof_gate_decide(const tof_gate_input_t *in)
{
  bool moving;
  bool forward_or_turn;

  if (in == NULL)
    {
      return TOF_GATE_DENY_SAFETY;
    }

  moving = (in->left_motor != 0 || in->right_motor != 0);
  forward_or_turn = (in->left_motor > 0 || in->right_motor > 0);

  /* 1. Hard safety lock: an active fault or an in-progress brake blocks any
   *    step regardless of ToF. */
  if (in->motion == TOF_MOTION_FAULT || in->motion == TOF_MOTION_BRAKING)
    {
      return TOF_GATE_DENY_SAFETY;
    }

  /* 2. A moving step requires all ToF valid+fresh.  This is the conservative
   *    default: with the stub disabled every reading is invalid, so any moving
   *    step is denied (需求 9.4, P14). */
  if (moving && !tof_all_valid(in))
    {
      return TOF_GATE_DENY_TOF;
    }

  /* 3. A forward/turn step at a front edge is blocked. */
  if (moving && forward_or_turn && tof_front_edge(in))
    {
      return TOF_GATE_DENY_SAFETY;
    }

  /* Non-moving (servo-only) steps, and moving steps with valid ToF and no
   * front edge, are allowed. */
  return TOF_GATE_ALLOW;
}
