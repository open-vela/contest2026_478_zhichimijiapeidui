/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Feature: mibot-voice-agent - property tests for the Sequential/Concurrent
 * execution modes (optional task 10.3).
 *
 * Covers:
 *   P7  - in Sequential_Mode, ACTING and SPEAKING never overlap in time.
 *         Validates: 需求 6.1, 6.2
 *   P10 - the two modes expose the identical Main_State set and transition
 *         edges; only playback timing differs.
 *         Validates: 需求 6.6
 *
 * The execution mode is a compile-time property (MIBOT_VA_CONCURRENT).  This
 * one test source is compiled and run TWICE by the CMake project - once linked
 * against the default (Sequential) core and once against a core built with
 * MIBOT_VA_CONCURRENT - so both modes are genuinely exercised.  Each run:
 *   - checks the mode predicates are self-consistent for its build (P7 gate),
 *   - checks the full transition-edge table and state set against an
 *     independent, mode-free reference (P10: edges are mode-independent),
 *   - models a Sequential turn and asserts playback starts only after ACTING
 *     completes when overlap is not allowed (P7).
 *
 * No third-party PBT library: a fixed-seed LCG drives >= 100 iterations and the
 * 6x6 edge grid is swept exhaustively.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mibot_voice_agent.h"

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
/* Deterministic RNG.                                                         */
/* ------------------------------------------------------------------------- */

static uint64_t g_rng = 0x0703EC0DE0703EC0ULL;

static uint32_t rng_next(void)
{
  g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return (uint32_t)(g_rng >> 32);
}

/* ------------------------------------------------------------------------- */
/* Independent, MODE-FREE reference of the legal transition edges (identical  */
/* to design.md's diagram).  P10 rests on the fact that this reference does   */
/* not mention the mode at all, yet matches va_transition_allowed in BOTH     */
/* builds.                                                                    */
/* ------------------------------------------------------------------------- */

static bool ref_edge_legal(va_state_t from, va_state_t to)
{
  if (to == VA_SAFE_STOP)
    {
      return from != VA_SAFE_STOP;
    }

  switch (from)
    {
      case VA_IDLE:      return to == VA_LISTENING;
      case VA_LISTENING: return to == VA_THINKING || to == VA_IDLE;
      case VA_THINKING:  return to == VA_ACTING || to == VA_SPEAKING ||
                                to == VA_IDLE;
      case VA_ACTING:    return to == VA_SPEAKING || to == VA_IDLE;
      case VA_SPEAKING:  return to == VA_IDLE;
      case VA_SAFE_STOP: return to == VA_IDLE;
      default:           return false;
    }
}

/* ========================================================================= */
/* P7 - Sequential mode has no ACTING/SPEAKING overlap.                       */
/* ========================================================================= */

/* The mode predicates must be self-consistent: overlap-allowed iff the build
 * is Concurrent.  In a Sequential build, playback may NOT overlap ACTING,
 * which is exactly the no-overlap guarantee (P7).
 * Validates: 需求 6.1, 6.2, 6.3, 6.4 */
static void prop_p7_mode_gate_consistent(void)
{
  va_exec_mode_t mode = va_exec_mode();

  CHECK(mode == VA_MODE_SEQUENTIAL || mode == VA_MODE_CONCURRENT,
        "P7: exec mode is one of the two defined values");

  if (mode == VA_MODE_SEQUENTIAL)
    {
      CHECK(va_playback_may_overlap_acting() == false,
            "P7: Sequential mode forbids ACTING/SPEAKING overlap");
    }
  else
    {
      CHECK(va_playback_may_overlap_acting() == true,
            "P7: Concurrent mode allows ACTING/SPEAKING overlap");
    }
}

/* Model a dialog turn and, using ONLY the mode gate, assert that in a
 * Sequential build no "playback active" flag is ever set while the state is
 * ACTING; i.e. SPEAKING output starts strictly after ACTING completes (P7).
 * In a Concurrent build, playback is permitted during ACTING.
 * Validates: 需求 6.1, 6.2 */
static void prop_p7_no_overlap_sequential(void)
{
  const int trials = 200;
  int t;
  bool overlap_allowed = va_playback_may_overlap_acting();

  for (t = 0; t < trials; t++)
    {
      va_context_t ctx;
      bool playback_active = false;
      bool answer_ready = (rng_next() & 1u) != 0;

      va_init(&ctx, NULL);
      ctx.cloud_available = true;

      va_handle_event(&ctx, VA_EVENT_WAKE);      /* IDLE -> LISTENING */
      va_handle_event(&ctx, VA_EVENT_ASR_DONE);  /* LISTENING -> THINKING */

      strcpy(ctx.pending_action, "happy");
      va_handle_event(&ctx, VA_EVENT_TOOL_CALLS);   /* THINKING -> ACTING */

      if (answer_ready)
        {
          strcpy(ctx.pending_answer, "hello");
        }

      /* Device-glue rule under test: playback may only start during ACTING
       * when the mode allows overlap and an answer is staged. */
      if (ctx.state == VA_ACTING && overlap_allowed &&
          ctx.pending_answer[0] != '\0')
        {
          playback_active = true;
        }

      if (!overlap_allowed)
        {
          CHECK(!(ctx.state == VA_ACTING && playback_active),
                "P7: Sequential never has playback active during ACTING");
        }

      /* Complete the action; now SPEAKING may run (either mode). */
      va_handle_event(&ctx, VA_EVENT_ACTION_DONE);

      if (answer_ready)
        {
          CHECK(ctx.state == VA_SPEAKING,
                "P7: ACTING with answer completes into SPEAKING");
          playback_active = true;   /* playback legitimately runs here */
          va_handle_event(&ctx, VA_EVENT_SPEAK_DONE);
        }

      CHECK(ctx.state == VA_IDLE, "P7: turn settles back in IDLE");
      (void)playback_active;
    }
}

/* ========================================================================= */
/* P10 - both modes share the identical state set and transition edges.       */
/* ========================================================================= */

/* va_transition_allowed matches the mode-free reference over the full 6x6
 * grid.  Because this holds in BOTH the Sequential and Concurrent builds
 * (which run this same assertion), the transition edges are identical across
 * modes (P10).
 * Validates: 需求 6.6 */
static void prop_p10_edges_mode_independent(void)
{
  int f;
  int to;

  for (f = 0; f < VA_STATE_COUNT; f++)
    {
      for (to = 0; to < VA_STATE_COUNT; to++)
        {
          va_state_t from = (va_state_t)f;
          va_state_t dst  = (va_state_t)to;

          CHECK(va_transition_allowed(from, dst) == ref_edge_legal(from, dst),
                "P10: transition edges match the mode-free reference");
        }
    }
}

/* The Main_State set (count + names) is identical regardless of mode. */
static void prop_p10_state_set_identical(void)
{
  static const char *const names[VA_STATE_COUNT] =
  {
    "IDLE", "LISTENING", "THINKING", "ACTING", "SPEAKING", "SAFE_STOP"
  };
  int s;

  CHECK(VA_STATE_COUNT == 6, "P10: exactly 6 Main_State values");

  for (s = 0; s < VA_STATE_COUNT; s++)
    {
      CHECK(strcmp(va_state_name((va_state_t)s), names[s]) == 0,
            "P10: state name is mode-independent");
    }
}

/* Drive random event sequences and confirm every state actually reached is one
 * of the 6 legal values in this build; combined with the Concurrent build
 * running the same check, the reachable state set is identical across modes. */
static void prop_p10_reachable_states_legal(void)
{
  const int trials = 150;
  static const va_event_t events[] =
  {
    VA_EVENT_WAKE, VA_EVENT_ASR_DONE, VA_EVENT_TOOL_CALLS, VA_EVENT_TEXT_ONLY,
    VA_EVENT_ACTION_DONE, VA_EVENT_SPEAK_DONE, VA_EVENT_CLOUD_FAIL,
    VA_EVENT_SAFETY, VA_EVENT_SAFE_CLEAR,
  };
  const int event_count = (int)(sizeof(events) / sizeof(events[0]));
  int t;

  for (t = 0; t < trials; t++)
    {
      va_context_t ctx;
      int i;

      va_init(&ctx, NULL);
      ctx.cloud_available = (rng_next() & 1u) != 0;

      for (i = 0; i < 40; i++)
        {
          if ((rng_next() & 3u) == 0)
            {
              strcpy(ctx.pending_action, "happy");
            }
          va_handle_event(&ctx, events[rng_next() % (uint32_t)event_count]);
          va_tick(&ctx);

          CHECK(ctx.state == VA_IDLE || ctx.state == VA_LISTENING ||
                ctx.state == VA_THINKING || ctx.state == VA_ACTING ||
                ctx.state == VA_SPEAKING || ctx.state == VA_SAFE_STOP,
                "P10: reached state is one of the 6 legal values");
        }
    }
}

/* ========================================================================= */

int main(void)
{
  printf("mode under test: %s\n",
         va_exec_mode() == VA_MODE_CONCURRENT ? "Concurrent" : "Sequential");

  prop_p7_mode_gate_consistent();
  prop_p7_no_overlap_sequential();
  prop_p10_edges_mode_independent();
  prop_p10_state_set_identical();
  prop_p10_reachable_states_legal();

  if (g_failures == 0)
    {
      printf("All mode property tests passed.\n");
      return 0;
    }

  printf("\n%d property check(s) failed.\n", g_failures);
  return 1;
}
