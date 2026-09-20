/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Feature: mibot-voice-agent - property-based tests for the pure-logic core.
 *
 * Model-based / randomised property tests for the design.md correctness
 * property table (P1-P16).  These cover the properties assigned to the
 * optional test tasks 1.3, 1.5, 1.6, 2.3 and 3.2:
 *
 *   Task 1.3: P1  - Main_State is always one of the 6 legal values
 *             P2  - transitions only ever follow legal edges
 *             P9  - any Dialog_Turn ends back in IDLE or SAFE_STOP
 *   Task 1.5: P3  - injecting a Safety_Event in any state -> SAFE_STOP next tick
 *             P4  - SAFE_STOP ignores WAKE/TOOL_CALLS etc. (no transition)
 *   Task 1.6: P5  - while Cloud_Available is false there is no edge to LISTENING
 *   Task 2.3: P6  - each Main_State maps to exactly one Expression
 *             P11 - idle/listening/thinking actions only fire in their state
 *             P12 - ACTING dialog action is always one of the 6 allowed values;
 *                   an illegal action never enters ACTING
 *   Task 3.2: P13 - stubbed ASR/TTS do not block LISTENING->THINKING->
 *                   SPEAKING->IDLE
 *
 * No third-party PBT library is used: randomness comes from a fixed-seed LCG
 * so every run is reproducible, and every property is also checked
 * exhaustively over the (state x event) space where that is small enough.
 * Each randomised property runs >= 100 iterations.  Uses the same tiny
 * dependency-free assertion harness as the other host tests.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mibot_voice_agent.h"
#include "va_stubs.h"

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

static uint64_t g_rng = 0x123456789abcdef0ULL;

static void rng_seed(uint64_t seed)
{
  g_rng = seed;
}

static uint32_t rng_next(void)
{
  /* Numerical Recipes LCG constants; return the high 32 bits. */
  g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return (uint32_t)(g_rng >> 32);
}

/* Uniform in [0, n). */
static uint32_t rng_below(uint32_t n)
{
  return rng_next() % n;
}

static bool rng_bool(void)
{
  return (rng_next() & 1u) != 0;
}

/* ------------------------------------------------------------------------- */
/* Helpers.                                                                   */
/* ------------------------------------------------------------------------- */

/* All nine abstract events, for random selection. */
static const va_event_t g_all_events[] =
{
  VA_EVENT_WAKE, VA_EVENT_ASR_DONE, VA_EVENT_TOOL_CALLS, VA_EVENT_TEXT_ONLY,
  VA_EVENT_ACTION_DONE, VA_EVENT_SPEAK_DONE, VA_EVENT_CLOUD_FAIL,
  VA_EVENT_SAFETY, VA_EVENT_SAFE_CLEAR,
};
#define EVENT_COUNT ((int)(sizeof(g_all_events) / sizeof(g_all_events[0])))

/* The six normal (non-SAFE_STOP) states, for exhaustive drives. */
static const va_state_t g_normal_states[] =
{
  VA_IDLE, VA_LISTENING, VA_THINKING, VA_ACTING, VA_SPEAKING,
};
#define NORMAL_STATE_COUNT ((int)(sizeof(g_normal_states) / sizeof(g_normal_states[0])))

/* The six legal dialog-driven ACTING actions, plus some illegal names. */
static const char *const g_legal_actions[] =
{
  "happy", "sad", "confused", "surprised", "cute", "greeting",
};
#define LEGAL_ACTION_COUNT ((int)(sizeof(g_legal_actions) / sizeof(g_legal_actions[0])))

static const char *const g_illegal_actions[] =
{
  "idle", "listening", "thinking", "dance", "Happy", "", "spin", "warning",
};
#define ILLEGAL_ACTION_COUNT ((int)(sizeof(g_illegal_actions) / sizeof(g_illegal_actions[0])))

static bool state_is_legal(va_state_t s)
{
  return s == VA_IDLE || s == VA_LISTENING || s == VA_THINKING ||
         s == VA_ACTING || s == VA_SPEAKING || s == VA_SAFE_STOP;
}

/* Independent reference model of the legal transition edges, derived straight
 * from design.md's state diagram (NOT from the implementation).  Used to both
 * validate va_transition_allowed itself and to check observed transitions. */
static bool ref_edge_legal(va_state_t from, va_state_t to)
{
  if (to == VA_SAFE_STOP)
    {
      return from != VA_SAFE_STOP;   /* any normal state may be preempted */
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

/* Drive a fresh context to a target normal state via the real event API.
 * Cloud is available so WAKE is accepted. */
static void drive_to_state(va_context_t *ctx, const va_hooks_t *hooks,
                           va_state_t target)
{
  va_init(ctx, hooks);
  ctx->cloud_available = true;

  if (target == VA_IDLE)
    {
      return;
    }

  va_handle_event(ctx, VA_EVENT_WAKE);         /* IDLE -> LISTENING */
  if (target == VA_LISTENING)
    {
      return;
    }

  va_handle_event(ctx, VA_EVENT_ASR_DONE);     /* LISTENING -> THINKING */
  if (target == VA_THINKING)
    {
      return;
    }

  if (target == VA_ACTING)
    {
      strcpy(ctx->pending_action, "happy");
      va_handle_event(ctx, VA_EVENT_TOOL_CALLS);
      return;
    }

  if (target == VA_SPEAKING)
    {
      va_handle_event(ctx, VA_EVENT_TEXT_ONLY);
      return;
    }
}

/* ------------------------------------------------------------------------- */
/* Recording hooks: capture LCD expression + action_start names per enter.   */
/* Used by P6/P11/P12 to assert exactly-one-expression and action bindings.  */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int  lcd_count;                 /* lcd_show calls this transition           */
  va_state_t lcd_last_state;      /* last state passed to lcd_show            */
  int  action_start_count;        /* action_start calls this transition       */
  char action_last[VA_PENDING_ACTION_SIZE];  /* last action name started     */
} rec_t;

static rec_t g_rec;

static void rec_reset(void)
{
  memset(&g_rec, 0, sizeof(g_rec));
}

static void rec_lcd_show(va_context_t *ctx, va_state_t state, void *user)
{
  (void)ctx; (void)user;
  g_rec.lcd_count++;
  g_rec.lcd_last_state = state;
}

static void rec_action_start(va_context_t *ctx, const char *action, void *user)
{
  (void)ctx; (void)user;
  g_rec.action_start_count++;
  strncpy(g_rec.action_last, action ? action : "", VA_PENDING_ACTION_SIZE - 1);
  g_rec.action_last[VA_PENDING_ACTION_SIZE - 1] = '\0';
}

static va_hooks_t make_recording_hooks(void)
{
  va_hooks_t h;
  memset(&h, 0, sizeof(h));
  h.lcd_show     = rec_lcd_show;
  h.action_start = rec_action_start;
  return h;
}

/* The action a state auto-plays on entry, or NULL if it plays none. */
static const char *state_bound_action(va_state_t s)
{
  switch (s)
    {
      case VA_IDLE:      return "idle";
      case VA_LISTENING: return "listening";
      case VA_THINKING:  return "thinking";
      default:           return NULL;   /* ACTING uses pending_action; others none */
    }
}

/* ========================================================================= */
/* Task 1.3 - P1, P2, P9.                                                     */
/* ========================================================================= */

/* Property P1: for any event sequence, Main_State is always one of the 6
 * legal values.  Property P2: every observed state change follows a legal
 * transition edge (checked against the independent reference model).
 * Validates: Requirements 1.1, 1.2, 1.11
 *
 * 200 random sequences of 40 events each, plus randomised cloud toggles and
 * staged actions, driven through va_handle_event + va_tick. */
static void prop_p1_p2_state_and_edges(void)
{
  const int trials = 200;
  const int seq_len = 40;
  int t;

  rng_seed(0x00A1B2C3D4E5F601ULL);

  for (t = 0; t < trials; t++)
    {
      va_context_t ctx;
      int i;

      va_init(&ctx, NULL);
      ctx.cloud_available = rng_bool();

      CHECK(state_is_legal(ctx.state), "P1: initial state is legal");

      for (i = 0; i < seq_len; i++)
        {
          va_state_t before = ctx.state;
          va_event_t ev = g_all_events[rng_below(EVENT_COUNT)];

          /* Randomly perturb the orthogonal condition inputs so we explore
           * cloud-gated and action-gated paths too. */
          if (rng_below(4) == 0)
            {
              ctx.cloud_available = rng_bool();
            }
          if (rng_below(4) == 0)
            {
              /* Stage either a legal or an obviously illegal action. */
              if (rng_bool())
                {
                  strcpy(ctx.pending_action,
                         g_legal_actions[rng_below(LEGAL_ACTION_COUNT)]);
                }
              else
                {
                  strcpy(ctx.pending_action, "bogus");
                }
            }
          if (rng_below(4) == 0)
            {
              /* Toggle presence of a final answer for ACTING branching. */
              if (rng_bool())
                {
                  strcpy(ctx.pending_answer, "answer");
                }
              else
                {
                  ctx.pending_answer[0] = '\0';
                }
            }

          va_handle_event(&ctx, ev);

          /* A latched safety event is consumed by the next tick. */
          va_tick(&ctx);

          CHECK(state_is_legal(ctx.state), "P1: state stays legal after event");

          /* P2: any change from `before` must be a legal edge.  Staying put is
           * always fine (rejected/non-applicable events). */
          if (ctx.state != before)
            {
              CHECK(ref_edge_legal(before, ctx.state),
                    "P2: state change follows a legal transition edge");
            }
        }
    }
}

/* Property P2 (predicate form): va_transition_allowed exactly matches the
 * independent reference model over the full 6x6 state grid.
 * Validates: Requirements 1.11 */
static void prop_p2_allowed_matches_reference(void)
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
                "P2: va_transition_allowed matches reference model");
        }
    }
}

/* Property P9: any Dialog_Turn, driven by an arbitrary event sequence, always
 * ends with Main_State back in IDLE or SAFE_STOP once the turn is closed out.
 * We model "closing out" by feeding a terminating sequence that exercises the
 * normal completion and safety-recovery edges, then asserting the resting
 * state is IDLE or SAFE_STOP.
 * Validates: Requirements 1.9, 1.10, 3.4
 *
 * 150 random turns: random prefix of events, then a deterministic drain that
 * pushes any live turn to completion (SPEAK_DONE/ACTION_DONE/CLOUD_FAIL) and
 * ticks out any latched safety. */
static void prop_p9_turn_ends_in_idle_or_safe(void)
{
  const int trials = 150;
  int t;

  rng_seed(0x0FEEDFACEC0FFEEULL);

  for (t = 0; t < trials; t++)
    {
      va_context_t ctx;
      int i;
      int prefix = 1 + (int)rng_below(20);

      va_init(&ctx, NULL);
      ctx.cloud_available = true;

      /* Random prefix of events (may or may not open/advance a turn). */
      for (i = 0; i < prefix; i++)
        {
          if (rng_below(3) == 0)
            {
              strcpy(ctx.pending_action,
                     g_legal_actions[rng_below(LEGAL_ACTION_COUNT)]);
            }
          if (rng_bool())
            {
              strcpy(ctx.pending_answer, "answer");
            }
          va_handle_event(&ctx, g_all_events[rng_below(EVENT_COUNT)]);
          va_tick(&ctx);
        }

      /* Deterministic drain: whatever state the turn is in, drive it to a
       * resting state.  Repeatedly apply the completion event valid for the
       * current state until it settles at IDLE or SAFE_STOP. */
      for (i = 0; i < 12; i++)
        {
          va_tick(&ctx);   /* consume any latched safety first */

          switch (ctx.state)
            {
              case VA_IDLE:
              case VA_SAFE_STOP:
                break;   /* resting states */
              case VA_LISTENING:
                va_handle_event(&ctx, VA_EVENT_ASR_DONE);
                break;
              case VA_THINKING:
                va_handle_event(&ctx, VA_EVENT_TEXT_ONLY);
                break;
              case VA_ACTING:
                ctx.pending_answer[0] = '\0';   /* force ACTING -> IDLE */
                va_handle_event(&ctx, VA_EVENT_ACTION_DONE);
                break;
              case VA_SPEAKING:
                va_handle_event(&ctx, VA_EVENT_SPEAK_DONE);
                break;
              default:
                break;
            }
        }

      CHECK(ctx.state == VA_IDLE || ctx.state == VA_SAFE_STOP,
            "P9: Dialog_Turn settles in IDLE or SAFE_STOP");
    }
}

/* ========================================================================= */
/* Task 1.5 - P3, P4.                                                         */
/* ========================================================================= */

/* Property P3: injecting a Safety_Event in ANY normal state results in
 * SAFE_STOP after the safety preemption is consumed by va_tick, regardless of
 * what audio/cloud/action work was in flight.
 * Validates: Requirements 2.1, 2.6, 2.7
 *
 * Exhaustive over the 5 normal states x {VA_EVENT_SAFETY, safety EVENT frame},
 * repeated with random pending action/answer/cloud state (>= 100 iterations). */
static void prop_p3_safety_forces_safe_stop(void)
{
  const int reps = 30;                 /* 5 states x 2 sources x 30 = 300 */
  int r;
  int s;

  rng_seed(0x5AFE57015AFE5701ULL);

  for (r = 0; r < reps; r++)
    {
      for (s = 0; s < NORMAL_STATE_COUNT; s++)
        {
          va_state_t target = g_normal_states[s];
          int source;

          for (source = 0; source < 2; source++)
            {
              va_context_t ctx;

              drive_to_state(&ctx, NULL, target);
              CHECK(ctx.state == target,
                    "P3: precondition reached target normal state");

              /* Randomise orthogonal state to prove it can't block the stop. */
              ctx.cloud_available = rng_bool();
              if (rng_bool())
                {
                  strcpy(ctx.pending_answer, "answer");
                }

              if (source == 0)
                {
                  va_handle_event(&ctx, VA_EVENT_SAFETY);
                }
              else
                {
                  const char *estop = "{\"event\":\"estop\"}";
                  va_on_frame(&ctx, VA_FRAME_EVENT,
                              (const uint8_t *)estop,
                              (uint16_t)strlen(estop));
                }

              CHECK(ctx.safety_pending == true,
                    "P3: safety latched before tick");

              va_tick(&ctx);
              CHECK(ctx.state == VA_SAFE_STOP,
                    "P3: any state + Safety_Event -> SAFE_STOP");
              CHECK(ctx.safety_pending == false,
                    "P3: safety_pending cleared after preemption");
            }
        }
    }
}

/* Property P4: while in SAFE_STOP, no event other than SAFE_CLEAR produces a
 * transition (WAKE/TOOL_CALLS and every other dialog event are ignored).
 * Validates: Requirements 2.6
 *
 * Exhaustive over all 9 events x random orthogonal state (>= 100 iterations). */
static void prop_p4_safe_stop_ignores_events(void)
{
  const int reps = 20;                 /* 9 events x 20 = 180 */
  int r;
  int e;

  rng_seed(0xDEAD5709DEAD5709ULL);

  for (r = 0; r < reps; r++)
    {
      for (e = 0; e < EVENT_COUNT; e++)
        {
          va_context_t ctx;
          va_event_t ev = g_all_events[e];
          bool moved;

          va_init(&ctx, NULL);
          ctx.cloud_available = rng_bool();
          va_transition(&ctx, VA_SAFE_STOP);
          CHECK(ctx.state == VA_SAFE_STOP, "P4: precondition in SAFE_STOP");

          if (rng_bool())
            {
              strcpy(ctx.pending_action,
                     g_legal_actions[rng_below(LEGAL_ACTION_COUNT)]);
            }

          moved = va_handle_event(&ctx, ev);

          if (ev == VA_EVENT_SAFE_CLEAR)
            {
              /* SAFE_CLEAR is the one legal recovery edge. */
              CHECK(moved && ctx.state == VA_IDLE,
                    "P4: SAFE_CLEAR is the only recovery edge");
            }
          else
            {
              /* VA_EVENT_SAFETY re-latches safety_pending but must not move
               * the state; all other dialog events are ignored outright. */
              CHECK(moved == false,
                    "P4: SAFE_STOP ignores non-recovery events (no transition)");
              CHECK(ctx.state == VA_SAFE_STOP,
                    "P4: state stays SAFE_STOP for non-recovery events");
            }
        }
    }
}

/* ========================================================================= */
/* Task 1.6 - P5.                                                             */
/* ========================================================================= */

/* Property P5: while Cloud_Available is false, there is no transition into
 * LISTENING.  The only edge into LISTENING is IDLE -> LISTENING on WAKE, which
 * is cloud-gated (需求 3.3).  We prove that from EVERY state, with cloud down,
 * no event sequence lands in LISTENING.
 * Validates: Requirements 3.3, 3.4
 *
 * 200 random sequences with cloud held false throughout, from random start
 * states, asserting LISTENING is never observed. */
static void prop_p5_no_listening_without_cloud(void)
{
  const int trials = 200;
  const int seq_len = 30;
  int t;

  rng_seed(0xC10DBAD0C10DBAD0ULL);

  for (t = 0; t < trials; t++)
    {
      va_context_t ctx;
      int i;
      /* Random start among the normal states (cloud was up to get there). */
      va_state_t start = g_normal_states[rng_below(NORMAL_STATE_COUNT)];

      drive_to_state(&ctx, NULL, start);

      /* From here on, cloud is DOWN and stays down. */
      ctx.cloud_available = false;

      /* If we started in LISTENING, first leave it (that entry predates the
       * cloud-down window and is not what P5 constrains). */
      if (ctx.state == VA_LISTENING)
        {
          va_handle_event(&ctx, VA_EVENT_ASR_DONE);
        }

      for (i = 0; i < seq_len; i++)
        {
          va_event_t ev = g_all_events[rng_below(EVENT_COUNT)];

          /* Keep cloud pinned false; only re-stage actions/answers. */
          ctx.cloud_available = false;
          if (rng_below(3) == 0)
            {
              strcpy(ctx.pending_action,
                     g_legal_actions[rng_below(LEGAL_ACTION_COUNT)]);
            }

          va_handle_event(&ctx, ev);
          va_tick(&ctx);

          CHECK(ctx.state != VA_LISTENING,
                "P5: no transition into LISTENING while cloud unavailable");
        }
    }
}

/* Property P5 (direct edge form): from IDLE with cloud down, WAKE never enters
 * LISTENING; with cloud up it does.  Validates: Requirements 3.3 */
static void prop_p5_wake_gate_direct(void)
{
  int i;

  rng_seed(0x11FEED9911FEED99ULL);

  for (i = 0; i < 100; i++)
    {
      va_context_t ctx;
      bool cloud = rng_bool();
      bool moved;

      va_init(&ctx, NULL);
      ctx.cloud_available = cloud;

      moved = va_handle_event(&ctx, VA_EVENT_WAKE);

      if (cloud)
        {
          CHECK(moved && ctx.state == VA_LISTENING,
                "P5: WAKE enters LISTENING when cloud up");
        }
      else
        {
          CHECK(!moved && ctx.state == VA_IDLE,
                "P5: WAKE stays IDLE when cloud down");
        }
    }
}

/* ========================================================================= */
/* Task 2.3 - P6, P11, P12.                                                   */
/* ========================================================================= */

/* Property P6: each Main_State maps to exactly one Expression.  On every legal
 * transition, lcd_show is called exactly once and with the state being
 * entered, so the (state -> expression) mapping is total and single-valued.
 * Validates: Requirements 4.1
 *
 * Exhaustive over every legal edge, repeated across random turns (>= 100). */
static void prop_p6_one_expression_per_state(void)
{
  const int reps = 40;
  int r;
  int f;
  int to;

  rng_seed(0x6E6E6E6E6E6E6E6EULL);

  for (r = 0; r < reps; r++)
    {
      for (f = 0; f < VA_STATE_COUNT; f++)
        {
          for (to = 0; to < VA_STATE_COUNT; to++)
            {
              va_state_t from = (va_state_t)f;
              va_state_t dst  = (va_state_t)to;
              va_context_t ctx;
              va_hooks_t hooks = make_recording_hooks();

              if (!va_transition_allowed(from, dst))
                {
                  continue;
                }

              /* Put ctx into `from` cleanly, then clear the recorder so we
               * only observe the from -> dst transition's LCD calls. */
              drive_to_state(&ctx, &hooks, from);
              if (ctx.state != from)
                {
                  continue;   /* SAFE_STOP isn't reached via drive_to_state */
                }

              if (dst == VA_ACTING)
                {
                  strcpy(ctx.pending_action,
                         g_legal_actions[rng_below(LEGAL_ACTION_COUNT)]);
                }

              rec_reset();
              CHECK(va_transition(&ctx, dst) == true,
                    "P6: legal edge accepted");
              CHECK(g_rec.lcd_count == 1,
                    "P6: exactly one Expression rendered per state entry");
              CHECK(g_rec.lcd_last_state == dst,
                    "P6: Expression matches the entered state");
            }
        }
    }
}

/* P6 also requires the mapping to be defined for SAFE_STOP entry. */
static void prop_p6_safe_stop_expression(void)
{
  int s;

  for (s = 0; s < NORMAL_STATE_COUNT; s++)
    {
      va_context_t ctx;
      va_hooks_t hooks = make_recording_hooks();

      drive_to_state(&ctx, &hooks, g_normal_states[s]);
      rec_reset();
      va_transition(&ctx, VA_SAFE_STOP);
      CHECK(g_rec.lcd_count == 1 && g_rec.lcd_last_state == VA_SAFE_STOP,
            "P6: SAFE_STOP renders exactly one Expression");
    }
}

/* Property P11: idle/listening/thinking actions are triggered only on entering
 * their own state, never as an ACTING dialog action.  On entering IDLE/
 * LISTENING/THINKING the started action is exactly the bound name; on entering
 * ACTING the started action is the (whitelisted) pending_action, never a
 * state-bound name.
 * Validates: Requirements 8.2, 8.3, 8.4
 *
 * Exhaustive over every legal edge, repeated (>= 100 iterations). */
static void prop_p11_state_bound_actions(void)
{
  const int reps = 40;
  int r;
  int f;
  int to;

  rng_seed(0x11B011B011B011B0ULL);

  for (r = 0; r < reps; r++)
    {
      for (f = 0; f < VA_STATE_COUNT; f++)
        {
          for (to = 0; to < VA_STATE_COUNT; to++)
            {
              va_state_t from = (va_state_t)f;
              va_state_t dst  = (va_state_t)to;
              va_context_t ctx;
              va_hooks_t hooks = make_recording_hooks();
              const char *bound;

              if (!va_transition_allowed(from, dst))
                {
                  continue;
                }

              drive_to_state(&ctx, &hooks, from);
              if (ctx.state != from)
                {
                  continue;
                }

              if (dst == VA_ACTING)
                {
                  strcpy(ctx.pending_action,
                         g_legal_actions[rng_below(LEGAL_ACTION_COUNT)]);
                }

              rec_reset();
              va_transition(&ctx, dst);

              bound = state_bound_action(dst);
              if (bound != NULL)
                {
                  /* IDLE/LISTENING/THINKING: exactly the bound action fires. */
                  CHECK(g_rec.action_start_count == 1 &&
                        strcmp(g_rec.action_last, bound) == 0,
                        "P11: state-bound action fires only on its state");
                }
              else if (dst == VA_ACTING)
                {
                  /* ACTING fires pending_action, which is a dialog action and
                   * never one of the state-bound names (P11 + P12). */
                  CHECK(g_rec.action_start_count == 1 &&
                        va_action_allowed(g_rec.action_last),
                        "P11: ACTING fires a whitelisted dialog action, "
                        "never idle/listening/thinking");
                  CHECK(strcmp(g_rec.action_last, "idle") != 0 &&
                        strcmp(g_rec.action_last, "listening") != 0 &&
                        strcmp(g_rec.action_last, "thinking") != 0,
                        "P11: ACTING action is not a state-bound name");
                }
              else
                {
                  /* SPEAKING/SAFE_STOP start no bounded action_start. */
                  CHECK(g_rec.action_start_count == 0,
                        "P11: SPEAKING/SAFE_STOP start no bound action");
                }
            }
        }
    }
}

/* Property P12: the ACTING dialog action is always one of the 6 allowed
 * values; an illegal action never enters ACTING and never changes state.
 * Validates: Requirements 8.5, 8.6
 *
 * Legal actions (6) and illegal actions (8) each tried many times from
 * THINKING via both va_request_acting and the TOOL_CALLS event (>= 100). */
static void prop_p12_acting_whitelist(void)
{
  const int reps = 12;                 /* (6+8) x 2 paths x 12 = 336 */
  int r;
  int a;

  rng_seed(0xAC71A6AC71A6AC71ULL);

  for (r = 0; r < reps; r++)
    {
      /* Legal actions: both paths must enter ACTING and stage the action. */
      for (a = 0; a < LEGAL_ACTION_COUNT; a++)
        {
          const char *action = g_legal_actions[a];
          va_context_t ctx;

          /* Path 1: va_request_acting. */
          drive_to_state(&ctx, NULL, VA_THINKING);
          CHECK(va_request_acting(&ctx, action) == true &&
                ctx.state == VA_ACTING,
                "P12: legal action enters ACTING via va_request_acting");
          CHECK(va_action_allowed(ctx.pending_action),
                "P12: staged ACTING action is whitelisted");

          /* Path 2: TOOL_CALLS event with the action staged. */
          drive_to_state(&ctx, NULL, VA_THINKING);
          strcpy(ctx.pending_action, action);
          CHECK(va_handle_event(&ctx, VA_EVENT_TOOL_CALLS) == true &&
                ctx.state == VA_ACTING,
                "P12: legal staged action enters ACTING via TOOL_CALLS");
        }

      /* Illegal actions: neither path may enter ACTING; state stays THINKING. */
      for (a = 0; a < ILLEGAL_ACTION_COUNT; a++)
        {
          const char *action = g_illegal_actions[a];
          va_context_t ctx;

          /* Path 1: va_request_acting must reject and not touch state. */
          drive_to_state(&ctx, NULL, VA_THINKING);
          CHECK(va_request_acting(&ctx, action) == false &&
                ctx.state == VA_THINKING,
                "P12: illegal action rejected by va_request_acting");
          CHECK(ctx.pending_action[0] == '\0',
                "P12: illegal action not staged by va_request_acting");

          /* Path 2: TOOL_CALLS with an illegal staged action must not enter. */
          drive_to_state(&ctx, NULL, VA_THINKING);
          strcpy(ctx.pending_action, action);
          CHECK(va_handle_event(&ctx, VA_EVENT_TOOL_CALLS) == false &&
                ctx.state == VA_THINKING,
                "P12: illegal staged action does not enter ACTING via TOOL_CALLS");
        }
    }
}

/* ========================================================================= */
/* Task 3.2 - P13.                                                            */
/* ========================================================================= */

/* Property P13: with ASR/TTS in stub mode, the missing real capabilities do
 * not block the LISTENING->THINKING->SPEAKING->IDLE flow.  For many randomised
 * turns, the stub ASR always yields a non-empty transcript, the stub TTS
 * always accepts the request, and the turn always completes to IDLE.
 * Validates: Requirements 9.1, 9.2
 *
 * 150 randomised turns (random PCM lengths / answer text) end at IDLE. */
static void prop_p13_stub_pipeline_never_blocks(void)
{
  const int trials = 150;
  int t;

  rng_seed(0x13B0B13B0B13B0B1ULL);

  /* Precondition: the host build must actually be in stub mode. */
  CHECK(va_asr_stub_active() && va_tts_stub_active(),
        "P13: host build has ASR and TTS stubs active");

  for (t = 0; t < trials; t++)
    {
      va_context_t ctx;
      char transcript[64];
      unsigned char pcm[32];
      size_t pcm_len = rng_below(sizeof(pcm) + 1);
      size_t k;
      int prev_tts;

      for (k = 0; k < pcm_len; k++)
        {
          pcm[k] = (unsigned char)rng_next();
        }

      va_stub_reset_observability();
      va_init(&ctx, NULL);
      ctx.cloud_available = true;

      /* IDLE -> LISTENING */
      CHECK(va_handle_event(&ctx, VA_EVENT_WAKE) && ctx.state == VA_LISTENING,
            "P13: WAKE -> LISTENING");

      /* Stub ASR must yield a non-empty transcript regardless of PCM. */
      CHECK(asr_transcribe(pcm, pcm_len, transcript, sizeof(transcript))
            == VA_STUB_OK && transcript[0] != '\0',
            "P13: stub ASR yields non-empty transcript, unblocking THINKING");

      /* LISTENING -> THINKING */
      CHECK(va_handle_event(&ctx, VA_EVENT_ASR_DONE) && ctx.state == VA_THINKING,
            "P13: ASR_DONE -> THINKING");

      /* THINKING -> SPEAKING (text-only reply) */
      strcpy(ctx.pending_answer, "stub answer");
      CHECK(va_handle_event(&ctx, VA_EVENT_TEXT_ONLY) &&
            ctx.state == VA_SPEAKING,
            "P13: TEXT_ONLY -> SPEAKING");

      /* Stub TTS must accept the request without blocking. */
      prev_tts = va_stub_tts_request_count();
      CHECK(tts_request(ctx.pending_answer, &ctx) == VA_STUB_OK,
            "P13: stub TTS accepts the SPEAKING request");
      CHECK(va_stub_tts_request_count() == prev_tts + 1,
            "P13: stub TTS records the request");

      /* SPEAKING -> IDLE (playback eos) */
      CHECK(va_handle_event(&ctx, VA_EVENT_SPEAK_DONE) && ctx.state == VA_IDLE,
            "P13: SPEAK_DONE -> IDLE, full stubbed turn completed");
    }
}

/* ========================================================================= */

int main(void)
{
  /* Task 1.3 */
  prop_p1_p2_state_and_edges();
  prop_p2_allowed_matches_reference();
  prop_p9_turn_ends_in_idle_or_safe();

  /* Task 1.5 */
  prop_p3_safety_forces_safe_stop();
  prop_p4_safe_stop_ignores_events();

  /* Task 1.6 */
  prop_p5_no_listening_without_cloud();
  prop_p5_wake_gate_direct();

  /* Task 2.3 */
  prop_p6_one_expression_per_state();
  prop_p6_safe_stop_expression();
  prop_p11_state_bound_actions();
  prop_p12_acting_whitelist();

  /* Task 3.2 */
  prop_p13_stub_pipeline_never_blocks();

  if (g_failures == 0)
    {
      printf("All property tests passed.\n");
      return 0;
    }

  printf("\n%d property check(s) failed.\n", g_failures);
  return 1;
}
