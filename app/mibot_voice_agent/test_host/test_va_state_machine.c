/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side tests for the Mibot Voice Agent pure-logic core.
 *
 * Covers task 1.1 (init/state names) plus task 1.2 (the 6-state main state
 * machine and the single transition entry point):
 *   - legal transitions succeed and move state along design edges
 *   - illegal transitions are rejected and leave state unchanged
 *   - the cloud_available gate on IDLE -> LISTENING (需求 3.3)
 *   - va_transition runs stop-old -> change -> start-new via injected hooks
 *   - the va_handle_event event mapping drives a full dialog turn
 *
 * The full property tests for P1/P2/P9 etc. land in later (optional) tasks.
 * This uses a tiny dependency-free assertion harness so the host build needs
 * nothing but a C compiler.
 */

#include <stdio.h>
#include <string.h>

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
      else                                                                   \
        {                                                                    \
          printf("ok: %s\n", (msg));                                         \
        }                                                                    \
    }                                                                        \
  while (0)

/* va_init() must leave the machine in a defined IDLE resting state. */
static void test_init_state_is_idle(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);

  CHECK(ctx.state == VA_IDLE, "va_init sets state to VA_IDLE");
  CHECK(ctx.cloud_available == false, "va_init clears cloud_available");
  CHECK(ctx.fault == false, "va_init clears fault");
  CHECK(ctx.safety_pending == false, "va_init clears safety_pending");
  CHECK(ctx.pending_action[0] == '\0', "va_init clears pending_action");
  CHECK(ctx.pending_answer[0] == '\0', "va_init clears pending_answer");
}

/* va_state_name must map VA_IDLE to the string "IDLE". */
static void test_state_name_idle(void)
{
  CHECK(strcmp(va_state_name(VA_IDLE), "IDLE") == 0,
        "va_state_name(VA_IDLE) == \"IDLE\"");
}

/* Sanity: every legal state maps to a stable non-empty name. */
static void test_state_names_all(void)
{
  const va_state_t states[VA_STATE_COUNT] =
    {
      VA_IDLE, VA_LISTENING, VA_THINKING,
      VA_ACTING, VA_SPEAKING, VA_SAFE_STOP,
    };
  const char *expected[VA_STATE_COUNT] =
    {
      "IDLE", "LISTENING", "THINKING",
      "ACTING", "SPEAKING", "SAFE_STOP",
    };
  int i;

  for (i = 0; i < VA_STATE_COUNT; i++)
    {
      CHECK(strcmp(va_state_name(states[i]), expected[i]) == 0,
            expected[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* Task 1.2: transition legality.                                            */
/* ------------------------------------------------------------------------- */

/* va_transition_allowed must accept exactly the design's legal edges. */
static void test_transition_legality_matrix(void)
{
  /* Legal edges from design.md's state diagram. */
  CHECK(va_transition_allowed(VA_IDLE, VA_LISTENING),
        "IDLE -> LISTENING legal");
  CHECK(va_transition_allowed(VA_LISTENING, VA_THINKING),
        "LISTENING -> THINKING legal");
  CHECK(va_transition_allowed(VA_LISTENING, VA_IDLE),
        "LISTENING -> IDLE legal (cloud drop)");
  CHECK(va_transition_allowed(VA_THINKING, VA_ACTING),
        "THINKING -> ACTING legal");
  CHECK(va_transition_allowed(VA_THINKING, VA_SPEAKING),
        "THINKING -> SPEAKING legal");
  CHECK(va_transition_allowed(VA_THINKING, VA_IDLE),
        "THINKING -> IDLE legal");
  CHECK(va_transition_allowed(VA_ACTING, VA_SPEAKING),
        "ACTING -> SPEAKING legal");
  CHECK(va_transition_allowed(VA_ACTING, VA_IDLE),
        "ACTING -> IDLE legal");
  CHECK(va_transition_allowed(VA_SPEAKING, VA_IDLE),
        "SPEAKING -> IDLE legal");
  CHECK(va_transition_allowed(VA_SAFE_STOP, VA_IDLE),
        "SAFE_STOP -> IDLE legal");

  /* Any normal state -> SAFE_STOP is legal (需求 2.1). */
  CHECK(va_transition_allowed(VA_IDLE, VA_SAFE_STOP),
        "IDLE -> SAFE_STOP legal");
  CHECK(va_transition_allowed(VA_LISTENING, VA_SAFE_STOP),
        "LISTENING -> SAFE_STOP legal");
  CHECK(va_transition_allowed(VA_THINKING, VA_SAFE_STOP),
        "THINKING -> SAFE_STOP legal");
  CHECK(va_transition_allowed(VA_ACTING, VA_SAFE_STOP),
        "ACTING -> SAFE_STOP legal");
  CHECK(va_transition_allowed(VA_SPEAKING, VA_SAFE_STOP),
        "SPEAKING -> SAFE_STOP legal");

  /* A representative set of illegal edges must be rejected (需求 1.11). */
  CHECK(!va_transition_allowed(VA_IDLE, VA_THINKING),
        "IDLE -> THINKING illegal");
  CHECK(!va_transition_allowed(VA_IDLE, VA_ACTING),
        "IDLE -> ACTING illegal");
  CHECK(!va_transition_allowed(VA_IDLE, VA_SPEAKING),
        "IDLE -> SPEAKING illegal");
  CHECK(!va_transition_allowed(VA_LISTENING, VA_ACTING),
        "LISTENING -> ACTING illegal");
  CHECK(!va_transition_allowed(VA_LISTENING, VA_SPEAKING),
        "LISTENING -> SPEAKING illegal");
  CHECK(!va_transition_allowed(VA_THINKING, VA_LISTENING),
        "THINKING -> LISTENING illegal");
  CHECK(!va_transition_allowed(VA_SPEAKING, VA_THINKING),
        "SPEAKING -> THINKING illegal");
  CHECK(!va_transition_allowed(VA_SAFE_STOP, VA_LISTENING),
        "SAFE_STOP -> LISTENING illegal");
  CHECK(!va_transition_allowed(VA_SAFE_STOP, VA_SAFE_STOP),
        "SAFE_STOP -> SAFE_STOP illegal (no re-entry)");

  /* No-op self-transitions are rejected so hardware effects are not re-fired. */
  CHECK(!va_transition_allowed(VA_IDLE, VA_IDLE),
        "IDLE -> IDLE illegal (self-transition)");
  CHECK(!va_transition_allowed(VA_THINKING, VA_THINKING),
        "THINKING -> THINKING illegal (self-transition)");
}

/* va_transition must reject illegal moves and leave state unchanged. */
static void test_transition_rejects_illegal(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;

  CHECK(va_transition(&ctx, VA_THINKING) == false,
        "va_transition rejects IDLE -> THINKING");
  CHECK(ctx.state == VA_IDLE,
        "state unchanged after rejected transition");

  CHECK(va_transition(&ctx, VA_LISTENING) == true,
        "va_transition accepts IDLE -> LISTENING");
  CHECK(ctx.state == VA_LISTENING,
        "state moved to LISTENING after legal transition");

  CHECK(va_transition(&ctx, VA_SPEAKING) == false,
        "va_transition rejects LISTENING -> SPEAKING");
  CHECK(ctx.state == VA_LISTENING,
        "state unchanged after rejected LISTENING -> SPEAKING");
}

/* ------------------------------------------------------------------------- */
/* Task 1.2: cloud_available gate on IDLE -> LISTENING (需求 3.3).           */
/* ------------------------------------------------------------------------- */

static void test_wake_gated_by_cloud(void)
{
  va_context_t ctx;

  /* Cloud down: WAKE must NOT move to LISTENING. */
  va_init(&ctx, NULL);
  ctx.cloud_available = false;
  CHECK(va_handle_event(&ctx, VA_EVENT_WAKE) == false,
        "WAKE ignored when cloud unavailable");
  CHECK(ctx.state == VA_IDLE,
        "state stays IDLE when cloud unavailable");

  /* Cloud up: WAKE moves to LISTENING. */
  ctx.cloud_available = true;
  CHECK(va_handle_event(&ctx, VA_EVENT_WAKE) == true,
        "WAKE moves to LISTENING when cloud available");
  CHECK(ctx.state == VA_LISTENING,
        "state is LISTENING after WAKE with cloud available");
}

/* ------------------------------------------------------------------------- */
/* Task 1.2: enter/exit hardware sequencing via injected hooks (需求 1.12).  */
/* ------------------------------------------------------------------------- */

/* Recording hook state: append a short token per hook invocation so we can
 * assert ordering (stop-old before start-new). */
#define TRACE_SIZE 512
static char g_trace[TRACE_SIZE];

static void trace_append(const char *tok)
{
  size_t used = strlen(g_trace);
  size_t room = TRACE_SIZE - used - 1;
  strncat(g_trace, tok, room);
}

static void hook_lcd_show(va_context_t *ctx, va_state_t state, void *user)
{
  (void)ctx; (void)user;
  trace_append("lcd(");
  trace_append(va_state_name(state));
  trace_append(")");
}

static void hook_action_start(va_context_t *ctx, const char *action, void *user)
{
  (void)ctx; (void)user;
  trace_append("act_start(");
  trace_append(action);
  trace_append(")");
}

static void hook_action_stop(va_context_t *ctx, void *user)
{
  (void)ctx; (void)user;
  trace_append("act_stop");
}

static void hook_audio_up_start(va_context_t *ctx, void *user)
{
  (void)ctx; (void)user;
  trace_append("up_start");
}

static void hook_audio_up_stop(va_context_t *ctx, void *user)
{
  (void)ctx; (void)user;
  trace_append("up_stop");
}

static void hook_audio_down_start(va_context_t *ctx, void *user)
{
  (void)ctx; (void)user;
  trace_append("down_start");
}

static void hook_audio_down_stop(va_context_t *ctx, void *user)
{
  (void)ctx; (void)user;
  trace_append("down_stop");
}

static va_hooks_t make_recording_hooks(void)
{
  va_hooks_t h;
  memset(&h, 0, sizeof(h));
  h.lcd_show          = hook_lcd_show;
  h.action_start      = hook_action_start;
  h.action_stop       = hook_action_stop;
  h.audio_up_start    = hook_audio_up_start;
  h.audio_up_stop     = hook_audio_up_stop;
  h.audio_down_start  = hook_audio_down_start;
  h.audio_down_stop   = hook_audio_down_stop;
  h.user              = NULL;
  return h;
}

/* Entering LISTENING must render LCD, start the listening action and open
 * AUDIO_UP capture. */
static void test_enter_listening_effects(void)
{
  va_context_t ctx;
  va_hooks_t hooks = make_recording_hooks();

  va_init(&ctx, &hooks);
  ctx.cloud_available = true;

  g_trace[0] = '\0';
  CHECK(va_transition(&ctx, VA_LISTENING) == true,
        "transition to LISTENING accepted");
  CHECK(strcmp(g_trace, "lcd(LISTENING)act_start(listening)up_start") == 0,
        "LISTENING enter effects: lcd + action + audio_up_start");
}

/* Leaving LISTENING must stop AUDIO_UP before the new state starts. */
static void test_exit_listening_stops_audio_up(void)
{
  va_context_t ctx;
  va_hooks_t hooks = make_recording_hooks();

  va_init(&ctx, &hooks);
  ctx.cloud_available = true;
  va_transition(&ctx, VA_LISTENING);

  g_trace[0] = '\0';
  CHECK(va_transition(&ctx, VA_THINKING) == true,
        "transition LISTENING -> THINKING accepted");
  /* up_stop (exit) must come before lcd/action_start (enter). */
  CHECK(strcmp(g_trace, "up_stoplcd(THINKING)act_start(thinking)") == 0,
        "exit stops AUDIO_UP before entering THINKING");
}

/* Entering SPEAKING opens AUDIO_DOWN; leaving it closes AUDIO_DOWN. */
static void test_speaking_audio_down_lifecycle(void)
{
  va_context_t ctx;
  va_hooks_t hooks = make_recording_hooks();

  va_init(&ctx, &hooks);
  ctx.cloud_available = true;
  va_transition(&ctx, VA_LISTENING);
  va_transition(&ctx, VA_THINKING);

  g_trace[0] = '\0';
  va_transition(&ctx, VA_SPEAKING);
  CHECK(strcmp(g_trace, "lcd(SPEAKING)down_start") == 0,
        "SPEAKING enter effects: lcd + audio_down_start");

  g_trace[0] = '\0';
  va_transition(&ctx, VA_IDLE);
  CHECK(strcmp(g_trace, "down_stoplcd(IDLE)act_start(idle)") == 0,
        "exit stops AUDIO_DOWN before entering IDLE");
}

/* ACTING must start the staged pending_action. */
static void test_acting_uses_pending_action(void)
{
  va_context_t ctx;
  va_hooks_t hooks = make_recording_hooks();

  va_init(&ctx, &hooks);
  ctx.cloud_available = true;
  va_transition(&ctx, VA_LISTENING);
  va_transition(&ctx, VA_THINKING);
  strcpy(ctx.pending_action, "happy");

  g_trace[0] = '\0';
  va_transition(&ctx, VA_ACTING);
  CHECK(strcmp(g_trace, "lcd(ACTING)act_start(happy)") == 0,
        "ACTING starts the staged pending_action");
}

/* SAFE_STOP must cut all continuous effects. */
static void test_safe_stop_cuts_all(void)
{
  va_context_t ctx;
  va_hooks_t hooks = make_recording_hooks();

  va_init(&ctx, &hooks);
  ctx.cloud_available = true;
  va_transition(&ctx, VA_LISTENING);

  g_trace[0] = '\0';
  CHECK(va_transition(&ctx, VA_SAFE_STOP) == true,
        "transition to SAFE_STOP accepted");
  /* exit LISTENING stops AUDIO_UP, then SAFE_STOP cuts action + both audio. */
  CHECK(strcmp(g_trace,
               "up_stoplcd(SAFE_STOP)act_stopup_stopdown_stop") == 0,
        "SAFE_STOP cuts action and audio");
}

/* ------------------------------------------------------------------------- */
/* Task 1.2: event-driven full dialog turn.                                  */
/* ------------------------------------------------------------------------- */

static void test_event_driven_dialog_turn(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;

  CHECK(va_handle_event(&ctx, VA_EVENT_WAKE) && ctx.state == VA_LISTENING,
        "WAKE: IDLE -> LISTENING");
  CHECK(va_handle_event(&ctx, VA_EVENT_ASR_DONE) && ctx.state == VA_THINKING,
        "ASR_DONE: LISTENING -> THINKING");

  strcpy(ctx.pending_action, "greeting");
  CHECK(va_handle_event(&ctx, VA_EVENT_TOOL_CALLS) && ctx.state == VA_ACTING,
        "TOOL_CALLS: THINKING -> ACTING");

  /* Final text present -> ACTING done goes to SPEAKING. */
  strcpy(ctx.pending_answer, "hello there");
  CHECK(va_handle_event(&ctx, VA_EVENT_ACTION_DONE) && ctx.state == VA_SPEAKING,
        "ACTION_DONE w/ text: ACTING -> SPEAKING");
  CHECK(va_handle_event(&ctx, VA_EVENT_SPEAK_DONE) && ctx.state == VA_IDLE,
        "SPEAK_DONE: SPEAKING -> IDLE");
}

/* ACTING with no final text goes straight back to IDLE (需求 1.9). */
static void test_event_acting_no_text_to_idle(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_handle_event(&ctx, VA_EVENT_WAKE);
  va_handle_event(&ctx, VA_EVENT_ASR_DONE);
  strcpy(ctx.pending_action, "happy");   /* stage a whitelisted action */
  va_handle_event(&ctx, VA_EVENT_TOOL_CALLS);

  ctx.pending_answer[0] = '\0';   /* no final text */
  CHECK(va_handle_event(&ctx, VA_EVENT_ACTION_DONE) && ctx.state == VA_IDLE,
        "ACTION_DONE w/o text: ACTING -> IDLE");
}

/* THINKING text-only path and cloud-fail path (需求 1.6, 1.7). */
static void test_event_thinking_branches(void)
{
  va_context_t ctx;

  /* text-only -> SPEAKING */
  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_handle_event(&ctx, VA_EVENT_WAKE);
  va_handle_event(&ctx, VA_EVENT_ASR_DONE);
  CHECK(va_handle_event(&ctx, VA_EVENT_TEXT_ONLY) && ctx.state == VA_SPEAKING,
        "TEXT_ONLY: THINKING -> SPEAKING");

  /* cloud fail -> IDLE */
  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_handle_event(&ctx, VA_EVENT_WAKE);
  va_handle_event(&ctx, VA_EVENT_ASR_DONE);
  CHECK(va_handle_event(&ctx, VA_EVENT_CLOUD_FAIL) && ctx.state == VA_IDLE,
        "CLOUD_FAIL: THINKING -> IDLE");
}

/* CLOUD_FAIL during LISTENING ends the turn back to IDLE (需求 3.4). */
static void test_event_listening_cloud_fail(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_handle_event(&ctx, VA_EVENT_WAKE);
  CHECK(va_handle_event(&ctx, VA_EVENT_CLOUD_FAIL) && ctx.state == VA_IDLE,
        "CLOUD_FAIL: LISTENING -> IDLE");
}

/* VA_EVENT_SAFETY latches safety_pending without an immediate transition. */
static void test_event_safety_latches_pending(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_handle_event(&ctx, VA_EVENT_WAKE);   /* now LISTENING */

  CHECK(va_handle_event(&ctx, VA_EVENT_SAFETY) == false,
        "SAFETY does not itself transition");
  CHECK(ctx.safety_pending == true,
        "SAFETY latches safety_pending");
  CHECK(ctx.state == VA_LISTENING,
        "state unchanged by SAFETY (preemption is va_tick's job)");
}

/* SAFE_STOP recovery via SAFE_CLEAR (需求 3.5). */
static void test_event_safe_clear_recovers(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  va_transition(&ctx, VA_SAFE_STOP);
  CHECK(ctx.state == VA_SAFE_STOP, "reached SAFE_STOP");
  CHECK(va_handle_event(&ctx, VA_EVENT_SAFE_CLEAR) && ctx.state == VA_IDLE,
        "SAFE_CLEAR: SAFE_STOP -> IDLE");
}

/* ------------------------------------------------------------------------- */
/* Task 1.4: safety preemption via va_tick.                                  */
/* ------------------------------------------------------------------------- */

/* Drive ctx into a specific normal state from a fresh init, using the event
 * API so we exercise the real transition path.  Cloud is available. */
static void drive_to_state(va_context_t *ctx, va_state_t target)
{
  va_init(ctx, NULL);
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
      va_handle_event(ctx, VA_EVENT_TOOL_CALLS);   /* THINKING -> ACTING */
      return;
    }

  if (target == VA_SPEAKING)
    {
      va_handle_event(ctx, VA_EVENT_TEXT_ONLY);    /* THINKING -> SPEAKING */
      return;
    }
}

/* From every normal state, a latched safety_pending must move to SAFE_STOP on
 * the next va_tick (需求 2.1, 2.7, P3). */
static void test_tick_preempts_from_every_state(void)
{
  const va_state_t normal[] =
    {
      VA_IDLE, VA_LISTENING, VA_THINKING, VA_ACTING, VA_SPEAKING,
    };
  size_t i;

  for (i = 0; i < sizeof(normal) / sizeof(normal[0]); i++)
    {
      va_context_t ctx;

      drive_to_state(&ctx, normal[i]);
      CHECK(ctx.state == normal[i],
            "precondition: reached the target normal state");

      /* Latch a safety event (parser context does not transition). */
      va_handle_event(&ctx, VA_EVENT_SAFETY);
      CHECK(ctx.safety_pending == true,
            "safety latched before tick");

      va_tick(&ctx);
      CHECK(ctx.state == VA_SAFE_STOP,
            "va_tick preempts to SAFE_STOP from normal state");
      CHECK(ctx.safety_pending == false,
            "va_tick clears safety_pending after preemption");
    }
}

/* va_tick without a pending safety must not spuriously move to SAFE_STOP. */
static void test_tick_noop_without_pending(void)
{
  va_context_t ctx;

  drive_to_state(&ctx, VA_THINKING);
  va_tick(&ctx);
  CHECK(ctx.state == VA_THINKING,
        "va_tick is a no-op without safety_pending");
}

/* va_on_frame must latch safety_pending for an ESP32 safety EVENT frame and
 * must NOT transition in the parser context (需求 2.1, 2.7). */
static void test_on_frame_latches_safety(void)
{
  va_context_t ctx;
  const char *estop = "{\"schema\":\"mibot.evt.v1\",\"event\":\"estop\"}";

  drive_to_state(&ctx, VA_ACTING);

  va_on_frame(&ctx, VA_FRAME_EVENT,
              (const uint8_t *)estop, (uint16_t)strlen(estop));
  CHECK(ctx.safety_pending == true,
        "safety EVENT frame latches safety_pending");
  CHECK(ctx.state == VA_ACTING,
        "safety EVENT frame does not transition in parser context");
  CHECK(ctx.fault == false,
        "ordinary estop is a recoverable stop (fault=false)");

  va_tick(&ctx);
  CHECK(ctx.state == VA_SAFE_STOP,
        "tick after safety frame moves to SAFE_STOP");
}

/* A non-safety EVENT frame must not latch a safety preemption. */
static void test_on_frame_ignores_non_safety(void)
{
  va_context_t ctx;
  const char *action = "{\"schema\":\"mibot.evt.v1\",\"event\":\"action_update\"}";

  drive_to_state(&ctx, VA_ACTING);
  va_on_frame(&ctx, VA_FRAME_EVENT,
              (const uint8_t *)action, (uint16_t)strlen(action));
  CHECK(ctx.safety_pending == false,
        "non-safety EVENT frame does not latch safety_pending");
  CHECK(ctx.state == VA_ACTING,
        "non-safety EVENT frame leaves state unchanged");
}

/* A non-EVENT frame type must be ignored by the safety path. */
static void test_on_frame_ignores_other_types(void)
{
  va_context_t ctx;
  const char *body = "estop";   /* even a safety marker on a non-EVENT type */

  drive_to_state(&ctx, VA_LISTENING);
  va_on_frame(&ctx, 0x11 /* ACK */,
              (const uint8_t *)body, (uint16_t)strlen(body));
  CHECK(ctx.safety_pending == false,
        "safety marker on non-EVENT frame is ignored");
}

/* Low-voltage class is treated as an unrecoverable fault (需求 2.8). */
static void test_on_frame_lowvolt_sets_fault(void)
{
  va_context_t ctx;
  const char *lv = "{\"event\":\"lowvolt\"}";

  drive_to_state(&ctx, VA_IDLE);
  va_on_frame(&ctx, VA_FRAME_EVENT,
              (const uint8_t *)lv, (uint16_t)strlen(lv));
  va_tick(&ctx);
  CHECK(ctx.state == VA_SAFE_STOP,
        "lowvolt EVENT reaches SAFE_STOP");
  CHECK(ctx.fault == true,
        "lowvolt sets the unrecoverable fault flag");
}

/* While in SAFE_STOP, WAKE and TOOL_CALLS must be ignored (需求 2.6, P4). */
static void test_safe_stop_ignores_dialog_events(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_transition(&ctx, VA_SAFE_STOP);

  CHECK(va_handle_event(&ctx, VA_EVENT_WAKE) == false,
        "SAFE_STOP ignores WAKE");
  CHECK(ctx.state == VA_SAFE_STOP,
        "state stays SAFE_STOP after ignored WAKE");

  CHECK(va_handle_event(&ctx, VA_EVENT_TOOL_CALLS) == false,
        "SAFE_STOP ignores TOOL_CALLS");
  CHECK(ctx.state == VA_SAFE_STOP,
        "state stays SAFE_STOP after ignored TOOL_CALLS");

  /* Other stray events must not move it either. */
  CHECK(va_handle_event(&ctx, VA_EVENT_ASR_DONE) == false,
        "SAFE_STOP ignores ASR_DONE");
  CHECK(va_handle_event(&ctx, VA_EVENT_SPEAK_DONE) == false,
        "SAFE_STOP ignores SPEAK_DONE");
  CHECK(ctx.state == VA_SAFE_STOP,
        "state stays SAFE_STOP after other stray events");
}

/* SAFE_CLEAR recovers to IDLE and clears the fault flag (需求 2.5, 2.8). */
static void test_safe_clear_recovers_and_clears_fault(void)
{
  va_context_t ctx;
  const char *lv = "{\"event\":\"lowvolt\"}";

  drive_to_state(&ctx, VA_ACTING);
  va_on_frame(&ctx, VA_FRAME_EVENT,
              (const uint8_t *)lv, (uint16_t)strlen(lv));
  va_tick(&ctx);
  CHECK(ctx.state == VA_SAFE_STOP && ctx.fault == true,
        "in SAFE_STOP with fault set");

  CHECK(va_handle_event(&ctx, VA_EVENT_SAFE_CLEAR) && ctx.state == VA_IDLE,
        "SAFE_CLEAR: SAFE_STOP -> IDLE");
  CHECK(ctx.fault == false,
        "recovery clears the fault flag");
}

/* ------------------------------------------------------------------------- */
/* Task 2.1: enter-action table (需求 4.1, 8.2, 8.3, 8.4; P6, P11).          */
/* ------------------------------------------------------------------------- */

/* Each Main_State renders exactly one LCD expression on entry (P6): the trace
 * for every legal transition contains exactly one "lcd(<state>)" token for the
 * state being entered. */
static void test_enter_table_one_expression_per_state(void)
{
  va_context_t ctx;
  va_hooks_t hooks = make_recording_hooks();

  va_init(&ctx, &hooks);
  ctx.cloud_available = true;

  /* IDLE auto-triggers idle; LCD shows IDLE. */
  g_trace[0] = '\0';
  va_transition(&ctx, VA_LISTENING);
  va_transition(&ctx, VA_THINKING);
  va_transition(&ctx, VA_IDLE);
  CHECK(strcmp(g_trace,
               "lcd(LISTENING)act_start(listening)up_start"
               "up_stoplcd(THINKING)act_start(thinking)"
               "lcd(IDLE)act_start(idle)") == 0,
        "enter table: IDLE/LISTENING/THINKING auto-trigger their bound actions");
}

/* idle/listening/thinking are triggered only on their own state entry, and
 * never as ACTING dialog actions (P11): entering ACTING starts pending_action,
 * not one of the state-bound names. */
static void test_state_bound_actions_only_on_their_state(void)
{
  va_context_t ctx;
  va_hooks_t hooks = make_recording_hooks();

  va_init(&ctx, &hooks);
  ctx.cloud_available = true;
  va_transition(&ctx, VA_LISTENING);
  va_transition(&ctx, VA_THINKING);
  strcpy(ctx.pending_action, "cute");

  g_trace[0] = '\0';
  va_transition(&ctx, VA_ACTING);
  CHECK(strcmp(g_trace, "lcd(ACTING)act_start(cute)") == 0,
        "ACTING triggers the dialog action, not a state-bound action");
}

/* ------------------------------------------------------------------------- */
/* Task 2.2: dialog-driven action whitelist (需求 8.5, 8.6, 5.3; P12).       */
/* ------------------------------------------------------------------------- */

/* The six emotion actions are on the whitelist. */
static void test_whitelist_accepts_six_emotions(void)
{
  CHECK(va_action_allowed("happy"),     "whitelist accepts happy");
  CHECK(va_action_allowed("sad"),       "whitelist accepts sad");
  CHECK(va_action_allowed("confused"),  "whitelist accepts confused");
  CHECK(va_action_allowed("surprised"), "whitelist accepts surprised");
  CHECK(va_action_allowed("cute"),      "whitelist accepts cute");
  CHECK(va_action_allowed("greeting"),  "whitelist accepts greeting");
}

/* State-bound actions and junk are rejected by the whitelist (P11, P12). */
static void test_whitelist_rejects_state_bound_and_junk(void)
{
  CHECK(!va_action_allowed("idle"),
        "whitelist rejects idle (state-bound, not dialog-driven)");
  CHECK(!va_action_allowed("listening"),
        "whitelist rejects listening (state-bound)");
  CHECK(!va_action_allowed("thinking"),
        "whitelist rejects thinking (state-bound)");
  CHECK(!va_action_allowed("dance"),   "whitelist rejects unknown action");
  CHECK(!va_action_allowed(""),        "whitelist rejects empty string");
  CHECK(!va_action_allowed(NULL),      "whitelist rejects NULL");
  CHECK(!va_action_allowed("Happy"),   "whitelist is case-sensitive");
}

/* va_request_acting with a legal action enters ACTING and stages the action. */
static void test_request_acting_legal(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_handle_event(&ctx, VA_EVENT_WAKE);
  va_handle_event(&ctx, VA_EVENT_ASR_DONE);   /* now THINKING */

  CHECK(va_request_acting(&ctx, "surprised") == true,
        "va_request_acting accepts a whitelisted action");
  CHECK(ctx.state == VA_ACTING,
        "va_request_acting enters ACTING for a legal action");
  CHECK(strcmp(ctx.pending_action, "surprised") == 0,
        "va_request_acting stages the action into pending_action");
}

/* va_request_acting with an illegal action returns false, does not enter
 * ACTING and does not change state or stage anything (需求 8.6, 5.3, P12). */
static void test_request_acting_illegal_rejected(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_handle_event(&ctx, VA_EVENT_WAKE);
  va_handle_event(&ctx, VA_EVENT_ASR_DONE);   /* now THINKING */

  CHECK(va_request_acting(&ctx, "idle") == false,
        "va_request_acting rejects a state-bound action");
  CHECK(ctx.state == VA_THINKING,
        "state stays THINKING after a rejected action");
  CHECK(ctx.pending_action[0] == '\0',
        "pending_action untouched after a rejected action");

  CHECK(va_request_acting(&ctx, "dance") == false,
        "va_request_acting rejects an unknown action");
  CHECK(ctx.state == VA_THINKING,
        "state stays THINKING after a rejected unknown action");
  CHECK(va_request_acting(&ctx, NULL) == false,
        "va_request_acting rejects NULL action");
  CHECK(ctx.state == VA_THINKING,
        "state stays THINKING after a NULL action");
}

/* The TOOL_CALLS event path also refuses to enter ACTING with an illegal
 * staged action (defence in depth, P12). */
static void test_tool_calls_guards_illegal_pending(void)
{
  va_context_t ctx;

  va_init(&ctx, NULL);
  ctx.cloud_available = true;
  va_handle_event(&ctx, VA_EVENT_WAKE);
  va_handle_event(&ctx, VA_EVENT_ASR_DONE);   /* now THINKING */

  /* Illegal staged action: TOOL_CALLS must not enter ACTING. */
  strcpy(ctx.pending_action, "thinking");
  CHECK(va_handle_event(&ctx, VA_EVENT_TOOL_CALLS) == false,
        "TOOL_CALLS refused with an illegal staged action");
  CHECK(ctx.state == VA_THINKING,
        "state stays THINKING when TOOL_CALLS has an illegal action");

  /* Legal staged action: TOOL_CALLS enters ACTING. */
  strcpy(ctx.pending_action, "happy");
  CHECK(va_handle_event(&ctx, VA_EVENT_TOOL_CALLS) == true,
        "TOOL_CALLS enters ACTING with a legal staged action");
  CHECK(ctx.state == VA_ACTING,
        "state is ACTING after TOOL_CALLS with a legal action");
}

int main(void)
{
  test_init_state_is_idle();
  test_state_name_idle();
  test_state_names_all();

  test_transition_legality_matrix();
  test_transition_rejects_illegal();
  test_wake_gated_by_cloud();
  test_enter_listening_effects();
  test_exit_listening_stops_audio_up();
  test_speaking_audio_down_lifecycle();
  test_acting_uses_pending_action();
  test_safe_stop_cuts_all();
  test_event_driven_dialog_turn();
  test_event_acting_no_text_to_idle();
  test_event_thinking_branches();
  test_event_listening_cloud_fail();
  test_event_safety_latches_pending();
  test_event_safe_clear_recovers();

  test_tick_preempts_from_every_state();
  test_tick_noop_without_pending();
  test_on_frame_latches_safety();
  test_on_frame_ignores_non_safety();
  test_on_frame_ignores_other_types();
  test_on_frame_lowvolt_sets_fault();
  test_safe_stop_ignores_dialog_events();
  test_safe_clear_recovers_and_clears_fault();

  test_enter_table_one_expression_per_state();
  test_state_bound_actions_only_on_their_state();
  test_whitelist_accepts_six_emotions();
  test_whitelist_rejects_state_bound_and_junk();
  test_request_acting_legal();
  test_request_acting_illegal_rejected();
  test_tool_calls_guards_illegal_pending();

  if (g_failures == 0)
    {
      printf("\nAll tests passed.\n");
      return 0;
    }

  printf("\n%d test(s) failed.\n", g_failures);
  return 1;
}
