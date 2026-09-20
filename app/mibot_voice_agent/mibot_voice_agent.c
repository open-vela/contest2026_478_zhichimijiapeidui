/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - dialog main state machine core (SF32 side).
 *
 * This file holds the hardware-agnostic state machine core.  It compiles on a
 * host (gcc/clang) with no NuttX/board dependencies so the state logic can be
 * unit/property tested off-target.  NuttX/board-specific glue (UART parser,
 * LCD driver, board_audio_*, NSH entry point) is added in later tasks and is
 * guarded with `#if defined(__NuttX__)` so it never leaks into the host build.
 *
 * Task 1.1 scope: skeleton + minimal compilable implementation only.
 *  - va_state_name() returns the correct state names.
 *  - va_init() initialises the context to a defined IDLE resting state.
 *  - va_transition()/va_on_frame()/va_tick() are placeholders; the full
 *    transition logic is task 1.2, safety preemption is task 1.4.
 */

#include "mibot_voice_agent.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* State names (需求 1.1).                                                    */
/* ------------------------------------------------------------------------- */

const char *va_state_name(va_state_t s)
{
  switch (s)
    {
      case VA_IDLE:      return "IDLE";
      case VA_LISTENING: return "LISTENING";
      case VA_THINKING:  return "THINKING";
      case VA_ACTING:    return "ACTING";
      case VA_SPEAKING:  return "SPEAKING";
      case VA_SAFE_STOP: return "SAFE_STOP";
      default:           return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------------- */
/* Execution mode (需求 6.1-6.6).                                             */
/*                                                                            */
/* The mode is a compile-time property, chosen by MIBOT_VA_CONCURRENT.  It is  */
/* deliberately NOT part of va_context_t and NOT consulted by                 */
/* va_transition_allowed, so both modes expose the identical Main_State set    */
/* and transition edges (P10).  The only observable difference is whether      */
/* SPEAKING playback may overlap ACTING (P7), exposed as a pure predicate the  */
/* device glue consults; the pure state machine itself is mode-agnostic.       */
/* ------------------------------------------------------------------------- */

va_exec_mode_t va_exec_mode(void)
{
#if defined(MIBOT_VA_CONCURRENT)
  return VA_MODE_CONCURRENT;
#else
  return VA_MODE_SEQUENTIAL;
#endif
}

bool va_playback_may_overlap_acting(void)
{
  /* Sequential (default): playback waits for ACTING to finish, so ACTING and
   * SPEAKING never overlap (P7).  Concurrent: playback may start during
   * ACTING. */
  return va_exec_mode() == VA_MODE_CONCURRENT;
}

/* ------------------------------------------------------------------------- */
/* Dialog-driven action whitelist (需求 8.5, 8.6, 5.2, 5.3, P12).             */
/*                                                                            */
/* Exactly the six emotion actions may be dialog-driven into ACTING.  The     */
/* state-bound actions idle/listening/thinking are deliberately absent: they  */
/* are auto-played on entering IDLE/LISTENING/THINKING and must never be       */
/* accepted as a Tool_Call action (P11).                                      */
/* ------------------------------------------------------------------------- */

static const char *const g_va_acting_actions[VA_ACTING_ACTION_COUNT] =
{
  "happy",
  "sad",
  "confused",
  "surprised",
  "cute",
  "greeting",
};

bool va_action_allowed(const char *action)
{
  int i;

  if (action == NULL || action[0] == '\0')
    {
      return false;
    }

  for (i = 0; i < VA_ACTING_ACTION_COUNT; i++)
    {
      if (strcmp(action, g_va_acting_actions[i]) == 0)
        {
          return true;
        }
    }

  return false;
}

bool va_request_acting(va_context_t *ctx, const char *action)
{
  if (ctx == NULL)
    {
      return false;
    }

  /* Reject non-whitelisted actions before touching any state so a bad
   * Tool_Call produces no motion command and no state change (需求 8.6, 5.3,
   * P8, P12). */
  if (!va_action_allowed(action))
    {
      return false;
    }

  /* Stage the validated action, then drive THINKING -> ACTING.  The staged
   * name must be in place before va_transition runs va_enter_actions, which
   * fires action_start(ctx->pending_action). */
  strncpy(ctx->pending_action, action, VA_PENDING_ACTION_SIZE - 1);
  ctx->pending_action[VA_PENDING_ACTION_SIZE - 1] = '\0';

  return va_transition(ctx, VA_ACTING);
}

/* ------------------------------------------------------------------------- */
/* Initialisation (需求 1.2).                                                 */
/* ------------------------------------------------------------------------- */

void va_init(va_context_t *ctx, const va_hooks_t *hooks)
{
  if (ctx == NULL)
    {
      return;
    }

  memset(ctx, 0, sizeof(*ctx));

  ctx->state           = VA_IDLE;
  ctx->cloud_available = false;
  ctx->fault           = false;
  ctx->safety_pending  = false;
  ctx->pending_action[0] = '\0';
  ctx->pending_answer[0] = '\0';

  if (hooks != NULL)
    {
      ctx->hooks = *hooks;
    }
}

/* ------------------------------------------------------------------------- */
/* Transition legality (需求 1.11).                                           */
/*                                                                            */
/* Only the edges drawn in design.md's state diagram are legal.  Everything   */
/* else (including no-op self-transitions) is rejected.  Note that any        */
/* state -> SAFE_STOP is legal here so the safety edge is walkable; the       */
/* unconditional preemption that guarantees it is task 1.4.                   */
/* ------------------------------------------------------------------------- */

bool va_transition_allowed(va_state_t from, va_state_t to)
{
  /* Any state may be preempted into SAFE_STOP (需求 2.1, task 1.4 hardens it). */
  if (to == VA_SAFE_STOP)
    {
      return from != VA_SAFE_STOP;   /* already stopped: no re-entry */
    }

  switch (from)
    {
      case VA_IDLE:
        /* 触发对话 (cloud gating is enforced in va_handle_event, 需求 3.3). */
        return to == VA_LISTENING;

      case VA_LISTENING:
        /* 录音结束/静音, or cloud drop back to IDLE (需求 3.4). */
        return to == VA_THINKING || to == VA_IDLE;

      case VA_THINKING:
        /* tool_calls -> ACTING, text -> SPEAKING, fail/cloud -> IDLE. */
        return to == VA_ACTING || to == VA_SPEAKING || to == VA_IDLE;

      case VA_ACTING:
        /* action done w/ text -> SPEAKING, w/o text -> IDLE. */
        return to == VA_SPEAKING || to == VA_IDLE;

      case VA_SPEAKING:
        /* eos -> IDLE. */
        return to == VA_IDLE;

      case VA_SAFE_STOP:
        /* 危险解除 -> IDLE. */
        return to == VA_IDLE;

      default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* Enter/exit hardware effects (需求 1.12, 进入动作表).                       */
/* ------------------------------------------------------------------------- */

/* Stop the continuous hardware effects the *old* state started, before the
 * state variable changes.  Only effects a state actually starts need undoing:
 *  - LISTENING started AUDIO_UP capture   -> audio_up_stop
 *  - SPEAKING  started AUDIO_DOWN playback -> audio_down_stop
 * The per-state bounded action is superseded by the new state's action_start
 * (or action_stop for SAFE_STOP), so no generic action_stop is needed here. */
static void va_exit_actions(va_context_t *ctx, va_state_t from)
{
  const va_hooks_t *h = &ctx->hooks;

  switch (from)
    {
      case VA_LISTENING:
        if (h->audio_up_stop != NULL)
          {
            h->audio_up_stop(ctx, h->user);
          }
        break;

      case VA_SPEAKING:
        if (h->audio_down_stop != NULL)
          {
            h->audio_down_stop(ctx, h->user);
          }
        break;

      default:
        break;
    }
}

/* Start the hardware effects for the *new* state, after the state variable
 * changed.  Mirrors design.md's "va_transition 内的进入动作表". */
static void va_enter_actions(va_context_t *ctx, va_state_t to)
{
  const va_hooks_t *h = &ctx->hooks;

  /* Every state updates the LCD to its expression first (需求 4.1, 4.2). */
  if (h->lcd_show != NULL)
    {
      h->lcd_show(ctx, to, h->user);
    }

  switch (to)
    {
      case VA_IDLE:
        if (h->action_start != NULL)
          {
            h->action_start(ctx, "idle", h->user);
          }
        break;

      case VA_LISTENING:
        if (h->action_start != NULL)
          {
            h->action_start(ctx, "listening", h->user);
          }
        if (h->audio_up_start != NULL)
          {
            h->audio_up_start(ctx, h->user);
          }
        break;

      case VA_THINKING:
        if (h->action_start != NULL)
          {
            h->action_start(ctx, "thinking", h->user);
          }
        break;

      case VA_ACTING:
        /* The dialog-driven action name is staged in pending_action. */
        if (h->action_start != NULL)
          {
            h->action_start(ctx, ctx->pending_action, h->user);
          }
        break;

      case VA_SPEAKING:
        if (h->audio_down_start != NULL)
          {
            h->audio_down_start(ctx, h->user);
          }
        break;

      case VA_SAFE_STOP:
        /* Cut all continuous effects immediately (需求 2.2). */
        if (h->action_stop != NULL)
          {
            h->action_stop(ctx, h->user);
          }
        if (h->audio_up_stop != NULL)
          {
            h->audio_up_stop(ctx, h->user);
          }
        if (h->audio_down_stop != NULL)
          {
            h->audio_down_stop(ctx, h->user);
          }
        break;

      default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* va_transition - the single Main_State write point (需求 1.11, 1.12).       */
/* ------------------------------------------------------------------------- */

bool va_transition(va_context_t *ctx, va_state_t next)
{
  va_state_t prev;

  if (ctx == NULL)
    {
      return false;
    }

  if (!va_transition_allowed(ctx->state, next))
    {
      return false;   /* illegal transition: state unchanged (需求 1.11) */
    }

  prev = ctx->state;

  /* 1. Stop old-state hardware effects. */
  va_exit_actions(ctx, prev);

  /* 2. Change the state (only mutation of ctx->state in the code base). */
  ctx->state = next;

  /* 3. Start new-state hardware effects. */
  va_enter_actions(ctx, next);

  return true;
}

/* ------------------------------------------------------------------------- */
/* va_handle_event - event-driven transition entry (需求 1.3-1.10, 3.3, 3.4).*/
/* ------------------------------------------------------------------------- */

bool va_handle_event(va_context_t *ctx, va_event_t event)
{
  if (ctx == NULL)
    {
      return false;
    }

  /* Safety events never transition directly: they latch a high-priority
   * pending flag that va_tick consumes next round (需求 2.7, task 1.4).  A
   * direct VA_EVENT_SAFETY is a recoverable stop by default; the fault-bearing
   * classes come in through va_on_frame. */
  if (event == VA_EVENT_SAFETY)
    {
      va_signal_safety(ctx, false);
      return false;
    }

  switch (ctx->state)
    {
      case VA_IDLE:
        if (event == VA_EVENT_WAKE)
          {
            /* IDLE -> LISTENING only when the cloud path is up (需求 3.3). */
            if (ctx->cloud_available)
              {
                return va_transition(ctx, VA_LISTENING);
              }
            /* Cloud down: stay IDLE (degraded-hint expression handled by the
             * LCD layer); no transition. */
            return false;
          }
        break;

      case VA_LISTENING:
        if (event == VA_EVENT_ASR_DONE)
          {
            return va_transition(ctx, VA_THINKING);
          }
        if (event == VA_EVENT_CLOUD_FAIL)
          {
            /* Cloud dropped mid-turn: end the turn, back to IDLE (需求 3.4). */
            return va_transition(ctx, VA_IDLE);
          }
        break;

      case VA_THINKING:
        if (event == VA_EVENT_TOOL_CALLS)
          {
            /* The dialog-driven action must already be staged in
             * pending_action (via va_request_acting or the parser).  Guard the
             * transition with the whitelist so an illegal action can never
             * enter ACTING through this path either (需求 8.6, P12). */
            if (!va_action_allowed(ctx->pending_action))
              {
                return false;
              }
            return va_transition(ctx, VA_ACTING);
          }
        if (event == VA_EVENT_TEXT_ONLY)
          {
            return va_transition(ctx, VA_SPEAKING);
          }
        if (event == VA_EVENT_CLOUD_FAIL)
          {
            return va_transition(ctx, VA_IDLE);   /* 需求 1.7, 3.4 */
          }
        break;

      case VA_ACTING:
        if (event == VA_EVENT_ACTION_DONE)
          {
            /* Final text present -> SPEAKING, else -> IDLE (需求 1.8, 1.9). */
            if (ctx->pending_answer[0] != '\0')
              {
                return va_transition(ctx, VA_SPEAKING);
              }
            return va_transition(ctx, VA_IDLE);
          }
        break;

      case VA_SPEAKING:
        if (event == VA_EVENT_SPEAK_DONE)
          {
            return va_transition(ctx, VA_IDLE);   /* 需求 1.10 */
          }
        break;

      case VA_SAFE_STOP:
        if (event == VA_EVENT_SAFE_CLEAR)
          {
            /* Recovery clears the fault flag (需求 2.5, 2.8). */
            if (va_transition(ctx, VA_IDLE))   /* 需求 3.5 (also 2.5) */
              {
                ctx->fault = false;
                return true;
              }
            return false;
          }
        /* WAKE / TOOL_CALLS etc. ignored while stopped (task 1.4, 需求 2.6). */
        break;

      default:
        break;
    }

  return false;   /* event not applicable in this state: no transition */
}

/* ------------------------------------------------------------------------- */
/* Safety preemption (需求 2.1, 2.2, 2.7, 2.8, task 1.4).                      */
/* ------------------------------------------------------------------------- */

/* Case-insensitive substring search over a possibly non-terminated buffer.
 * `needle` is a normal NUL-terminated literal (lowercase).  Used to sniff
 * safety markers out of an EVENT frame body without a full JSON parser (the
 * real parser is task 9.1). */
static bool payload_contains(const uint8_t *payload, uint16_t len,
                             const char *needle)
{
  size_t nlen;
  size_t i;

  if (payload == NULL || needle == NULL)
    {
      return false;
    }

  nlen = strlen(needle);
  if (nlen == 0 || nlen > len)
    {
      return false;
    }

  for (i = 0; i + nlen <= (size_t)len; i++)
    {
      size_t j;

      for (j = 0; j < nlen; j++)
        {
          int c = (int)payload[i + j];

          /* ASCII lower-case fold; leaves non-letters untouched. */
          if (c >= 'A' && c <= 'Z')
            {
              c += 'a' - 'A';
            }

          if (c != (int)needle[j])
            {
              break;
            }
        }

      if (j == nlen)
        {
          return true;
        }
    }

  return false;
}

void va_signal_safety(va_context_t *ctx, bool unrecoverable)
{
  if (ctx == NULL)
    {
      return;
    }

  /* Latch only; the transition is va_tick's job so it can never be delayed
   * by audio/cloud/action work in the current context (需求 2.7). */
  ctx->safety_pending = true;

  /* fault distinguishes an unrecoverable fault from a recoverable stop, but
   * introduces no new Main_State (需求 2.8).  Once latched, fault stays set
   * until a successful SAFE_STOP -> IDLE recovery clears it. */
  if (unrecoverable)
    {
      ctx->fault = true;
    }
}

void va_on_frame(va_context_t *ctx, uint8_t type,
                 const uint8_t *payload, uint16_t len)
{
  if (ctx == NULL)
    {
      return;
    }

  /* Only EVENT frames can carry a physical Safety_Event.  Full decoding of
   * ACK/NACK/AUDIO_DOWN/AI_RESPONSE is task 9.1. */
  if (type != VA_FRAME_EVENT)
    {
      return;
    }

  /* Sniff the ESP32-reported safety classes: 急停 (estop/emergency), 边缘
   * (edge), 堵转过流 (stall/overcurrent), 低压 (lowvolt/undervolt).  A generic
   * "safety" marker also latches.  This mirrors VA_EVENT_SAFETY: latch the
   * pending flag, never transition in the parser context (需求 2.1, 2.7). */
  bool is_safety =
      payload_contains(payload, len, "safety")     ||
      payload_contains(payload, len, "estop")       ||
      payload_contains(payload, len, "emergency")   ||
      payload_contains(payload, len, "edge")        ||
      payload_contains(payload, len, "stall")       ||
      payload_contains(payload, len, "overcurrent") ||
      payload_contains(payload, len, "lowvolt")     ||
      payload_contains(payload, len, "undervolt");

  if (!is_safety)
    {
      return;   /* non-safety EVENT (e.g. action_update): task 9.1 */
    }

  /* First-version fault policy (需求 2.8): ordinary safety stops are
   * recoverable (fault=false).  Classes flagged unrecoverable, or events that
   * explicitly carry a fault/unrecoverable marker, set fault=true.  Low
   * voltage is treated as an unrecoverable fault since it cannot be cleared by
   * simply removing the hazard. */
  bool unrecoverable =
      payload_contains(payload, len, "fault")        ||
      payload_contains(payload, len, "unrecoverable") ||
      payload_contains(payload, len, "lowvolt")       ||
      payload_contains(payload, len, "undervolt");

  va_signal_safety(ctx, unrecoverable);
}

void va_tick(va_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  /* Highest-priority work, before any audio/cloud/action handling: consume a
   * pending safety preemption.  Doing this unconditionally at the top of every
   * tick guarantees SAFE_STOP entry is never delayed by the current state
   * (需求 2.7, P3). */
  if (ctx->safety_pending)
    {
      /* Clear the latch regardless: even if we are already in SAFE_STOP (no
       * re-entry) the request is satisfied. */
      ctx->safety_pending = false;

      if (ctx->state != VA_SAFE_STOP)
        {
          va_transition(ctx, VA_SAFE_STOP);
        }

      /* Nothing else runs this tick once a safety preemption is handled. */
      return;
    }

  /* Remaining main-loop work (audio/cloud polling, ACTING completion) is
   * added in phase 3.  Task 1.4 only owns the safety-preemption consumer. */
}
