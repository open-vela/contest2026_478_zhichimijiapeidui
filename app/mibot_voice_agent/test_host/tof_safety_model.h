/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - portable model of the ESP32 ToF safety gate (test only).
 *
 * The real safety gate lives on the ESP32 in mibot_controller.cpp
 * (apply_action_step_locked + tof_all_valid_locked + front_edge_locked) and is
 * built against ESP-IDF, so it cannot be compiled on a host.  This header is a
 * faithful, dependency-free transcription of that gate's *decision logic* so
 * the conservative-default property (P14: stubbed/invalid ToF data never lets a
 * dangerous action through) can be exercised off-target.
 *
 * This is a TEST artifact, not production code: it is not linked into any
 * device build.  Its only job is to encode the ESP32 rules as executable spec
 * and be checked against an independent reference in the P14 property test, the
 * same way va_test_args.c is checked for P16.
 *
 * Requirements traceability: 9.4 (ToF stub data must not allow dangerous
 * actions to be executed).
 */

#ifndef TOF_SAFETY_MODEL_H
#define TOF_SAFETY_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Mirror of the ESP32 RobotState motion classes relevant to the gate. */
typedef enum
{
  TOF_MOTION_STANDBY = 0,
  TOF_MOTION_RUNNING,
  TOF_MOTION_HOLD,
  TOF_MOTION_BRAKING,
  TOF_MOTION_FAULT,
} tof_motion_t;

/* One ToF reading, mirroring g_state.tof[] entries. */
typedef struct
{
  bool     valid;         /* reading was produced by a working sensor       */
  uint16_t mm;            /* measured distance in millimetres               */
  int64_t  timestamp_ms;  /* when the reading was taken                     */
} tof_reading_t;

/* All the inputs the ESP32 gate consults for a single action step. */
typedef struct
{
  tof_reading_t tof[4];        /* front_left, front_right, rear_left, rear_right */
  int64_t       now_ms;        /* current time                                   */
  int64_t       timeout_ms;    /* MIBOT_TOF_TIMEOUT_MS                            */
  uint16_t      edge_threshold_mm;  /* MIBOT_EDGE_THRESHOLD_MM                    */
  tof_motion_t  motion;        /* current motion class                           */
  int           left_motor;    /* commanded left motor (post-intensity)          */
  int           right_motor;   /* commanded right motor                          */
} tof_gate_input_t;

/* Result of the gate decision, mirroring the ESP32 error codes. */
typedef enum
{
  TOF_GATE_ALLOW = 0,       /* step may execute                              */
  TOF_GATE_DENY_SAFETY,     /* E_SAFETY_LOCK (fault/braking or front edge)   */
  TOF_GATE_DENY_TOF,        /* E_TOF_INVALID (no valid/fresh ToF while moving)*/
} tof_gate_result_t;

/* True iff every ToF reading is valid and fresh (mirror of
 * tof_all_valid_locked): any invalid or stale reading fails the whole set,
 * which is what makes the stub-disabled / unimplemented default conservative. */
bool tof_all_valid(const tof_gate_input_t *in);

/* True iff either front sensor reports a drop-off past the edge threshold
 * (mirror of front_edge_locked). */
bool tof_front_edge(const tof_gate_input_t *in);

/* The gate decision for one action step, transcribed from
 * apply_action_step_locked: a "moving" step (non-zero motor) requires all ToF
 * valid+fresh, and a forward/turn (positive motor) step is additionally
 * blocked at a front edge; fault/braking blocks everything.  A non-moving
 * (servo-only) step is always allowed. */
tof_gate_result_t tof_gate_decide(const tof_gate_input_t *in);

#ifdef __cplusplus
}
#endif

#endif /* TOF_SAFETY_MODEL_H */
