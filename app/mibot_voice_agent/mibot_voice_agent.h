/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - dialog main state machine core (SF32 side).
 *
 * The state machine core in this header is deliberately hardware-agnostic: it
 * depends on nothing from NuttX or the SF32 board, so it can be compiled and
 * unit/property tested on a host with plain gcc/clang.  All hardware effects
 * (LCD expression, motion/servo action commands, audio) are expressed as
 * injectable callbacks (function pointers) so the host build can substitute
 * recording stubs instead of touching board_audio_* or NuttX APIs.
 *
 * Requirements traceability: 1.1, 1.2 (state set and single-valued state).
 */

#ifndef MIBOT_VOICE_AGENT_H
#define MIBOT_VOICE_AGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ------------------------------------------------------------------------- */
/* Main_State (需求 1.1)                                                      */
/* ------------------------------------------------------------------------- */

typedef enum
{
  VA_IDLE = 0,
  VA_LISTENING,
  VA_THINKING,
  VA_ACTING,
  VA_SPEAKING,
  VA_SAFE_STOP,
} va_state_t;

/* Number of legal Main_State values (needed for host model checks). */
#define VA_STATE_COUNT 6

/* ------------------------------------------------------------------------- */
/* Execution mode (需求 6.1-6.6).                                             */
/*                                                                            */
/* Sequential_Mode (default) runs the dialog turn one step at a time: the     */
/* ACTING bounded action fully completes before SPEAKING playback begins, so  */
/* the two never overlap in time (P7).  Concurrent_Mode (compile-time         */
/* MIBOT_VA_CONCURRENT) lets SPEAKING playback overlap ACTING once the final  */
/* text is known.  Crucially, BOTH modes share the exact same Main_State set  */
/* and the same legal transition edges (va_transition_allowed is             */
/* mode-independent); only the *timing* of when playback starts differs       */
/* (P10).  The mode is therefore not a state and never changes a transition   */
/* edge - it only gates whether audio-down playback may start during ACTING.  */
/* ------------------------------------------------------------------------- */

typedef enum
{
  VA_MODE_SEQUENTIAL = 0,   /* ACTING then SPEAKING, no time overlap (P7)      */
  VA_MODE_CONCURRENT,       /* SPEAKING may overlap ACTING                     */
} va_exec_mode_t;

/* ------------------------------------------------------------------------- */
/* UART frame types (subset needed by the safety-preemption path, task 1.4). */
/*                                                                            */
/* Only the EVENT type is consumed here: ESP32 reports physical Safety_Event  */
/* (急停/边缘/堵转过流/低压) inside EVENT frames.  The full frame decoding for  */
/* ACK/NACK/AUDIO_DOWN/AI_RESPONSE lands with the real parser in task 9.1.    */
/* ------------------------------------------------------------------------- */

#define VA_FRAME_EVENT 0x20u

/* ------------------------------------------------------------------------- */
/* Events that drive the state machine (需求 1.3-1.10, 3.4).                  */
/*                                                                            */
/* These are the abstract stimuli the dialog main loop feeds into            */
/* va_handle_event().  They decouple the transition logic from the concrete  */
/* source (UART frame, NSH command, wake word, cloud result), so the host    */
/* tests can drive arbitrary event sequences and task 1.3/1.4 can reuse them. */
/* ------------------------------------------------------------------------- */

typedef enum
{
  VA_EVENT_WAKE = 0,      /* 触发对话事件 (IDLE -> LISTENING, cloud-gated)     */
  VA_EVENT_ASR_DONE,      /* 录音结束/静音 (LISTENING -> THINKING)            */
  VA_EVENT_TOOL_CALLS,    /* DeepSeek 返回 tool_calls (THINKING -> ACTING)     */
  VA_EVENT_TEXT_ONLY,     /* DeepSeek 只返回文本 (THINKING -> SPEAKING)        */
  VA_EVENT_ACTION_DONE,   /* 动作完成 (ACTING -> SPEAKING/IDLE)                */
  VA_EVENT_SPEAK_DONE,    /* 播放完成 eos (SPEAKING -> IDLE)                   */
  VA_EVENT_CLOUD_FAIL,    /* 云端不可用/出错 (LISTENING/THINKING -> IDLE)      */
  VA_EVENT_SAFETY,        /* 安全事件 (置 safety_pending, va_tick 转 SAFE_STOP) */
  VA_EVENT_SAFE_CLEAR,    /* 危险解除 (SAFE_STOP -> IDLE)                      */
} va_event_t;

/* Bounds for the two staged payload buffers carried in the context. */
#define VA_PENDING_ACTION_SIZE 16
#define VA_PENDING_ANSWER_SIZE 512

/* ------------------------------------------------------------------------- */
/* Dialog-driven action whitelist (需求 8.5, 8.6, 5.2, 5.3).                  */
/*                                                                            */
/* The ACTING state executes a dialog-driven Bounded_Action selected by a     */
/* Tool_Call.  Its value is restricted to these six emotion actions.  The     */
/* three state-bound actions (idle/listening/thinking) are NOT here: they are */
/* auto-played on entering IDLE/LISTENING/THINKING and must never be used as  */
/* dialog-driven actions (P11, P12).                                          */
/* ------------------------------------------------------------------------- */

#define VA_ACTING_ACTION_COUNT 6

/* ------------------------------------------------------------------------- */
/* Injectable hardware-effect callbacks.                                     */
/*                                                                            */
/* The pure-logic layer never calls board_audio_* / LCD / UART directly.     */
/* Instead va_transition() and va_tick() invoke these hooks.  On device the   */
/* hooks are wired to the real drivers; on host they are recording stubs.     */
/* All hooks may be NULL (no-op), which keeps the core usable in isolation.   */
/* ------------------------------------------------------------------------- */

struct va_context_s;   /* forward declaration */

typedef struct
{
  /* Render the LCD expression for the given state. */
  void (*lcd_show)(struct va_context_s *ctx, va_state_t state, void *user);

  /* Send a "robot.perform_action <name>" style motion/servo command. */
  void (*action_start)(struct va_context_s *ctx, const char *action, void *user);

  /* Stop the current bounded action (end old state hardware effect). */
  void (*action_stop)(struct va_context_s *ctx, void *user);

  /* Start/stop microphone AUDIO_UP capture (LISTENING). */
  void (*audio_up_start)(struct va_context_s *ctx, void *user);
  void (*audio_up_stop)(struct va_context_s *ctx, void *user);

  /* Start/stop speaker AUDIO_DOWN playback (SPEAKING). */
  void (*audio_down_start)(struct va_context_s *ctx, void *user);
  void (*audio_down_stop)(struct va_context_s *ctx, void *user);

  /* Opaque user pointer handed back to every hook. */
  void *user;
} va_hooks_t;

/* ------------------------------------------------------------------------- */
/* va_context_t                                                               */
/*                                                                            */
/* Main_State plus the two orthogonal transition-condition dimensions         */
/* (cloud_available, fault) and the parser-set safety_pending flag.  The      */
/* condition dimensions are NOT independent Main_State values (需求 2.8, 3.2).*/
/* ------------------------------------------------------------------------- */

typedef struct va_context_s
{
  va_state_t state;             /* current Main_State (single-valued)        */
  bool cloud_available;         /* transition condition, not a state         */
  bool fault;                   /* recoverable vs. unrecoverable in SAFE_STOP */
  bool safety_pending;          /* set by parser, consumed by va_tick        */

  char pending_action[VA_PENDING_ACTION_SIZE];  /* ACTING dialog-driven action */
  char pending_answer[VA_PENDING_ANSWER_SIZE];  /* SPEAKING final text         */

  va_hooks_t hooks;             /* injectable hardware effects               */
} va_context_t;

/* ------------------------------------------------------------------------- */
/* Public API (pure logic).                                                   */
/* ------------------------------------------------------------------------- */

/* Initialise a context to a well-defined IDLE resting state.
 * hooks may be NULL, in which case all hardware effects are no-ops. */
void va_init(va_context_t *ctx, const va_hooks_t *hooks);

/* The single write point for Main_State (需求 1.11, 1.12).
 *
 * va_transition() is the ONLY place that mutates ctx->state.  It:
 *   1. Rejects illegal transitions (returns false, state unchanged) so that
 *      state only ever moves along the design's legal transition edges.
 *   2. On a legal transition, runs "stop old-state hardware effects -> change
 *      state -> start new-state hardware effects" in that order (需求 1.12).
 *
 * A no-op self-transition (next == current) is treated as illegal and
 * rejected so hardware effects are not re-fired.  Returns true iff the
 * transition was accepted and performed. */
bool va_transition(va_context_t *ctx, va_state_t next);

/* Returns true iff moving from `from` to `to` is a legal transition edge as
 * defined by the design state diagram.  Pure predicate, no side effects. */
bool va_transition_allowed(va_state_t from, va_state_t to);

/* Feed an abstract event into the state machine (需求 1.3-1.10, 3.3, 3.4).
 *
 * This is the event-driven transition entry point used by the dialog main
 * loop and by the host tests to drive arbitrary event sequences.  It maps the
 * (current state, event, condition flags) triple to at most one va_transition
 * call.  VA_EVENT_SAFETY only latches ctx->safety_pending; the actual move to
 * SAFE_STOP is performed by va_tick (task 1.4).  Returns true iff a state
 * transition occurred. */
bool va_handle_event(va_context_t *ctx, va_event_t event);

/* Latch a pending safety preemption (需求 2.1, 2.7, 2.8).
 *
 * Called from any context that detects a physical Safety_Event (the UART
 * parser via va_on_frame, or a direct VA_EVENT_SAFETY through va_handle_event).
 * It does NOT transition here: it only sets ctx->safety_pending so the actual
 * move to SAFE_STOP happens at the top of the next va_tick, unblocked by
 * audio/cloud/action work.  `unrecoverable` sets ctx->fault to distinguish an
 * unrecoverable fault from a recoverable stop without adding a new state
 * (需求 2.8). */
void va_signal_safety(va_context_t *ctx, bool unrecoverable);

/* Feed a decoded UART frame into the state machine (需求 2.1, 2.7, task 1.4).
 *
 * For EVENT frames that carry an ESP32-reported Safety_Event (急停/边缘/堵转/
 * 低压), this latches ctx->safety_pending (and ctx->fault for unrecoverable
 * classes) rather than transitioning in the parser context.  The unconditional
 * move to SAFE_STOP is performed by va_tick.  Other frame types are decoded in
 * task 9.1 and are ignored here.  `payload`/`len` describe the frame body and
 * may be NULL/0. */
void va_on_frame(va_context_t *ctx, uint8_t type,
                 const uint8_t *payload, uint16_t len);

/* Main-loop step with unconditional SAFE_STOP preemption (需求 2.7, task 1.4).
 *
 * The very first thing va_tick does, before any audio/cloud/action handling,
 * is consume ctx->safety_pending: if set and not already in SAFE_STOP, it
 * transitions to SAFE_STOP and clears the flag.  This guarantees the SAFE_STOP
 * entry is never delayed by the current audio/cloud/action state.  Other
 * main-loop work (audio/cloud polling) is added in phase 3. */
void va_tick(va_context_t *ctx);

/* Human-readable name for a state; returns "IDLE" for VA_IDLE etc.
 * Never returns NULL. */
const char *va_state_name(va_state_t s);

/* The execution mode compiled into this build (需求 6.1, 6.3).  Returns
 * VA_MODE_CONCURRENT iff MIBOT_VA_CONCURRENT is defined, else
 * VA_MODE_SEQUENTIAL (the first-version default).  Pure, no side effects. */
va_exec_mode_t va_exec_mode(void);

/* True iff AUDIO_DOWN playback (SPEAKING output) is allowed to start while the
 * dialog is still in ACTING (需求 6.4).  This is the ONLY behavioural
 * difference between the two modes: Sequential returns false (playback waits
 * for ACTING to finish, so ACTING and SPEAKING never overlap, P7); Concurrent
 * returns true.  The Main_State set and transition edges are identical in both
 * modes (P10); this predicate never affects which transitions are legal. */
bool va_playback_may_overlap_acting(void);

/* Returns true iff `action` is a legal dialog-driven ACTING action, i.e. one
 * of the six emotion actions happy/sad/confused/surprised/cute/greeting
 * (需求 8.5).  The state-bound actions idle/listening/thinking are rejected:
 * they are auto-played on state entry, never dialog-driven (P11, P12).  A NULL
 * or empty action is rejected.  Pure predicate, no side effects. */
bool va_action_allowed(const char *action);

/* Safe entry into ACTING with whitelist validation (需求 8.6, 5.3, P12).
 *
 * If `action` is not on the dialog-driven whitelist (see va_action_allowed),
 * this returns false, does NOT change ctx->state and does NOT touch
 * ctx->pending_action, so a rejected Tool_Call produces no motion command.
 * On a legal action, it copies the action into ctx->pending_action and drives
 * the THINKING -> ACTING transition, returning its result (which also fails if
 * the current state is not THINKING).  Returns true iff ACTING was entered. */
bool va_request_acting(va_context_t *ctx, const char *action);

#ifdef __cplusplus
}
#endif

#endif /* MIBOT_VOICE_AGENT_H */
