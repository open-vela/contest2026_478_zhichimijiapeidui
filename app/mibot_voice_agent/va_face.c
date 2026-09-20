/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - LCD face geometry and animation schedule.
 *
 * Pure logic: no framebuffer, no NuttX, no clock of its own (time arrives as a
 * parameter).  That keeps the whole emotional vocabulary testable on a host and
 * leaves mibot_voice_agent_main.c responsible only for turning geometry into
 * pixels.
 */

#include "va_face.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Resting geometry.                                                          */
/*                                                                            */
/* IDLE is the reference shape; every other state is a deformation of it, which */
/* is why interpolating between any two of these produces a usable transition. */
/* ------------------------------------------------------------------------- */

void va_face_base(va_state_t state, va_face_t *out)
{
  if (out == NULL)
    {
      return;
    }

  memset(out, 0, sizeof(*out));
  out->color = va_face_color(state);

  switch (state)
    {
      case VA_IDLE:
        /* Resting shape. */
        out->left = (va_eye_t){100, 124, 46, 0, 0};
        break;

      case VA_LISTENING:
        /* Wide awake: taller and a hair wider. */
        out->left = (va_eye_t){106, 146, 50, 0, 0};
        break;

      case VA_THINKING:
        /* Squint plus a glance up and to one side. */
        out->left = (va_eye_t){100, 58, 28, 14, -22};
        break;

      case VA_ACTING:
        /* Busy: squashed and shifted, as if leaning into the motion. */
        out->left = (va_eye_t){104, 104, 44, 10, -6};
        break;

      case VA_SPEAKING:
        /* Same eyes as IDLE, lifted slightly; the cue is the bar below. */
        out->left = (va_eye_t){100, 118, 44, 0, -10};
        break;

      case VA_SAFE_STOP:
        /* Shut: thin flat bars.  Colour is what separates this from a blink. */
        out->left = (va_eye_t){100, 22, 11, 0, 0};
        break;

      default:
        out->left = (va_eye_t){100, 124, 46, 0, 0};
        break;
    }

  /* Both eyes are identical in every state.  Asymmetry reads as "confused"
   * rather than as any of these six states, so it is not used here. */
  out->right = out->left;

  if (state == VA_SPEAKING)
    {
      /* Nominal bar; va_face_update() scales the height by the audio level. */
      out->bar = (va_eye_t){64, 26, 13, 0, 0};
    }
}

uint16_t va_face_color(va_state_t state)
{
  switch (state)
    {
      case VA_IDLE:      return VA_FACE_COLOR_IDLE;
      case VA_LISTENING: return VA_FACE_COLOR_LISTENING;
      case VA_THINKING:  return VA_FACE_COLOR_THINKING;
      case VA_ACTING:    return VA_FACE_COLOR_ACTING;
      case VA_SPEAKING:  return VA_FACE_COLOR_SPEAKING;
      case VA_SAFE_STOP: return VA_FACE_COLOR_SAFE_STOP;
      default:           return VA_FACE_COLOR_IDLE;
    }
}

bool va_face_state_animates(va_state_t state)
{
  /* SAFE_STOP must look inert.  THINKING keeps the breathing drift but does not
   * blink: blinking an already-squinted eye just flickers. */
  return state != VA_SAFE_STOP;
}

static bool va_face_state_blinks(va_state_t state)
{
  return state == VA_IDLE || state == VA_LISTENING ||
         state == VA_ACTING || state == VA_SPEAKING;
}

/* ------------------------------------------------------------------------- */
/* Blink schedule.                                                            */
/* ------------------------------------------------------------------------- */

static uint32_t va_face_rand(va_face_anim_t *anim)
{
  /* Small LCG.  Deterministic from the seed so a host test can assert the
   * schedule instead of merely observing that something happened. */
  anim->rng = anim->rng * 1664525u + 1013904223u;
  return anim->rng >> 16;
}

static void va_face_schedule_blink(va_face_anim_t *anim, uint32_t now_ms)
{
  const uint32_t span = VA_FACE_BLINK_MAX_GAP_MS - VA_FACE_BLINK_MIN_GAP_MS;

  anim->next_blink_ms = now_ms + VA_FACE_BLINK_MIN_GAP_MS +
                        (va_face_rand(anim) % (span + 1u));
  anim->blink_start_ms = 0;
}

void va_face_anim_init(va_face_anim_t *anim, uint32_t now_ms)
{
  if (anim == NULL)
    {
      return;
    }
  /* Seed off the boot time so two units on a bench do not blink in lockstep. */
  anim->rng = now_ms ^ 0x9e3779b9u;
  va_face_schedule_blink(anim, now_ms);
}

/* Eye height during a blink, as a permille of the open height.  Returns 1000
 * when not blinking. */
static uint32_t va_face_blink_scale(const va_face_anim_t *anim,
                                    uint32_t now_ms)
{
  uint32_t elapsed;

  if (anim->blink_start_ms == 0)
    {
      return 1000u;
    }

  elapsed = now_ms - anim->blink_start_ms;
  if (elapsed < VA_FACE_BLINK_CLOSE_MS)
    {
      /* Closing. */
      return 1000u - (elapsed * 1000u) / VA_FACE_BLINK_CLOSE_MS;
    }
  if (elapsed < VA_FACE_BLINK_CLOSE_MS + VA_FACE_BLINK_HOLD_MS)
    {
      return 0u;
    }
  if (elapsed < VA_FACE_BLINK_TOTAL_MS)
    {
      /* Opening. */
      const uint32_t into_open =
          elapsed - VA_FACE_BLINK_CLOSE_MS - VA_FACE_BLINK_HOLD_MS;
      return (into_open * 1000u) / VA_FACE_BLINK_OPEN_MS;
    }
  return 1000u;
}

/* Triangle wave in [-VA_FACE_BREATH_AMP_PX, +VA_FACE_BREATH_AMP_PX].  A
 * triangle rather than a sine keeps this integer-only; at 2 px amplitude the
 * difference is invisible. */
static int16_t va_face_breath_offset(uint32_t now_ms)
{
  const uint32_t half = VA_FACE_BREATH_PERIOD_MS / 2u;
  const uint32_t phase = now_ms % VA_FACE_BREATH_PERIOD_MS;
  const int32_t amp = VA_FACE_BREATH_AMP_PX;
  int32_t value;

  if (phase < half)
    {
      value = -amp + (int32_t)((phase * (uint32_t)(2 * amp)) / half);
    }
  else
    {
      value = amp - (int32_t)(((phase - half) * (uint32_t)(2 * amp)) / half);
    }
  return (int16_t)value;
}

void va_face_update(va_face_anim_t *anim, va_state_t state, uint32_t now_ms,
                    uint8_t speak_level, va_face_t *out)
{
  uint32_t scale = 1000u;
  int16_t breath = 0;

  if (out == NULL)
    {
      return;
    }

  va_face_base(state, out);

  if (anim == NULL || !va_face_state_animates(state))
    {
      return;
    }

  /* Advance the blink schedule.  Done here rather than in a separate step so a
   * caller cannot render a frame from a stale schedule. */
  if (va_face_state_blinks(state))
    {
      if (anim->blink_start_ms != 0 &&
          now_ms - anim->blink_start_ms >= VA_FACE_BLINK_TOTAL_MS)
        {
          va_face_schedule_blink(anim, now_ms);
        }
      else if (anim->blink_start_ms == 0 &&
               (int32_t)(now_ms - anim->next_blink_ms) >= 0)
        {
          anim->blink_start_ms = now_ms;
        }
      scale = va_face_blink_scale(anim, now_ms);
    }
  else
    {
      /* Keep the schedule moving so leaving this state does not immediately
       * blink because the deadline expired while squinting. */
      if ((int32_t)(now_ms - anim->next_blink_ms) >= 0)
        {
          va_face_schedule_blink(anim, now_ms);
        }
    }

  breath = va_face_breath_offset(now_ms);

  out->left.h  = (int16_t)((out->left.h  * (int32_t)scale) / 1000);
  out->right.h = (int16_t)((out->right.h * (int32_t)scale) / 1000);

  /* A fully closed eye still needs one visible line, otherwise the face
   * vanishes mid-blink and reads as a display glitch. */
  if (out->left.h < 4)
    {
      out->left.h = 4;
      out->left.r = 2;
    }
  if (out->right.h < 4)
    {
      out->right.h = 4;
      out->right.r = 2;
    }
  if (out->left.r > out->left.h / 2)
    {
      out->left.r = out->left.h / 2;
    }
  if (out->right.r > out->right.h / 2)
    {
      out->right.r = out->right.h / 2;
    }

  out->left.dy  = (int16_t)(out->left.dy + breath);
  out->right.dy = (int16_t)(out->right.dy + breath);

  if (state == VA_SPEAKING && out->bar.h > 0)
    {
      /* Bar height tracks the downlink audio level.  A floor keeps the bar
       * present during a pause so the cue does not flicker away between
       * syllables. */
      const int32_t full = out->bar.h;
      int32_t h = (full * 40 + (full * 60 * speak_level) / 255) / 100;

      if (h < 6)
        {
          h = 6;
        }
      out->bar.h = (int16_t)h;
      if (out->bar.r > out->bar.h / 2)
        {
          out->bar.r = (int16_t)(out->bar.h / 2);
        }
      out->bar.dy = breath;
    }
}

bool va_face_equal(const va_face_t *a, const va_face_t *b)
{
  if (a == NULL || b == NULL)
    {
      return false;
    }
  return memcmp(a, b, sizeof(*a)) == 0;
}
