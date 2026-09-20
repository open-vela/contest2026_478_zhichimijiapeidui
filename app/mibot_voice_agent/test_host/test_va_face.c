/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host test for the LCD face geometry and animation schedule (需求 4.1-4.5).
 *
 * Compiles the production va_face.c directly, so the tested logic and the
 * shipped logic are the same translation unit.  What this can prove without a
 * panel: that every state has a distinct, drawable, on-canvas shape; that the
 * blink schedule actually fires and completes; that a blink never makes the
 * face vanish; that SAFE_STOP stays inert; and that the redraw trigger
 * (va_face_equal) is neither always-true nor always-false.
 *
 * What it cannot prove: how any of it looks.  That is what face_preview.py and
 * the panel are for.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "va_face.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                                     \
  do                                                                         \
    {                                                                        \
      g_checks++;                                                            \
      if (!(cond))                                                           \
        {                                                                    \
          printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);           \
          g_failures++;                                                      \
        }                                                                    \
    }                                                                        \
  while (0)

static const va_state_t ALL_STATES[] =
{
  VA_IDLE, VA_LISTENING, VA_THINKING, VA_ACTING, VA_SPEAKING, VA_SAFE_STOP
};

#define STATE_COUNT ((int)(sizeof(ALL_STATES) / sizeof(ALL_STATES[0])))

static const char *state_name(va_state_t s)
{
  switch (s)
    {
      case VA_IDLE:      return "IDLE";
      case VA_LISTENING: return "LISTENING";
      case VA_THINKING:  return "THINKING";
      case VA_ACTING:    return "ACTING";
      case VA_SPEAKING:  return "SPEAKING";
      case VA_SAFE_STOP: return "SAFE_STOP";
      default:           return "?";
    }
}

/* Does an eye stay inside the region the renderer clears?  A shape that leaves
 * it would smear, because only that rectangle is repainted per frame. */
static bool eye_within_region(const va_eye_t *eye, int nominal_cx, int nominal_cy)
{
  int cx = nominal_cx + eye->dx;
  int cy = nominal_cy + eye->dy;
  int x0 = cx - eye->w / 2;
  int x1 = cx + eye->w / 2;
  int y0 = cy - eye->h / 2;
  int y1 = cy + eye->h / 2;

  /* Rows are checked against the eye band, not the full region: the renderer
   * only repaints the taller region when the speaking bar is on screen, so an
   * eye that grew past the eye band would smear in every other state. */
  return x0 >= VA_FACE_REGION_X &&
         x1 <= VA_FACE_REGION_X + VA_FACE_REGION_W &&
         y0 >= VA_FACE_EYE_BAND_Y &&
         y1 <= VA_FACE_EYE_BAND_Y + VA_FACE_EYE_BAND_H;
}

/* The eye band must itself be inside the region, otherwise the SPEAKING frames
 * (which repaint the region) would leave the eye rows stale. */
static void test_eye_band_inside_region(void)
{
  CHECK(VA_FACE_EYE_BAND_Y >= VA_FACE_REGION_Y &&
        VA_FACE_EYE_BAND_Y + VA_FACE_EYE_BAND_H <=
            VA_FACE_REGION_Y + VA_FACE_REGION_H,
        "eye band is contained in the face region");
}

/* Same containment question for the speaking bar. */
static bool bar_within_region(const va_eye_t *bar)
{
  int cx = VA_FACE_CX + bar->dx;
  int cy = VA_FACE_BAR_CY + bar->dy;

  if (bar->h == 0)
    {
      return true;
    }
  return cx - bar->w / 2 >= VA_FACE_REGION_X &&
         cx + bar->w / 2 <= VA_FACE_REGION_X + VA_FACE_REGION_W &&
         cy - bar->h / 2 >= VA_FACE_REGION_Y &&
         cy + bar->h / 2 <= VA_FACE_REGION_Y + VA_FACE_REGION_H;
}

/* The renderer repaints only VA_FACE_REGION_*, so anything drawn outside it
 * would smear across frames.  Walk the full animation of every state, at every
 * audio level, and require containment throughout -- this is the check that
 * caught the THINKING/ACTING dx shift pushing the right eye 3 px past the
 * region edge. */
static void test_animation_stays_in_region(void)
{
  int i;

  for (i = 0; i < STATE_COUNT; i++)
    {
      va_face_anim_t anim;
      uint32_t t;
      char msg[112];
      bool eyes_ok = true;
      bool bar_ok = true;

      va_face_anim_init(&anim, 0);
      for (t = 0; t < 20000; t += 20)
        {
          va_face_t face;
          uint8_t level = (uint8_t)(t % 256);

          va_face_update(&anim, ALL_STATES[i], t, level, &face);
          if (!eye_within_region(&face.left, VA_FACE_CX - VA_FACE_EYE_DX,
                                 VA_FACE_CY) ||
              !eye_within_region(&face.right, VA_FACE_CX + VA_FACE_EYE_DX,
                                 VA_FACE_CY))
            {
              eyes_ok = false;
            }
          if (!bar_within_region(&face.bar))
            {
              bar_ok = false;
            }
        }

      snprintf(msg, sizeof(msg),
               "%s eyes stay inside the clear region while animating",
               state_name(ALL_STATES[i]));
      CHECK(eyes_ok, msg);
      snprintf(msg, sizeof(msg),
               "%s bar stays inside the clear region while animating",
               state_name(ALL_STATES[i]));
      CHECK(bar_ok, msg);
    }
}

static void test_base_geometry(void)
{
  int i;

  for (i = 0; i < STATE_COUNT; i++)
    {
      va_face_t face;
      char msg[96];

      va_face_base(ALL_STATES[i], &face);

      snprintf(msg, sizeof(msg), "%s has a drawable left eye",
               state_name(ALL_STATES[i]));
      CHECK(face.left.w > 0 && face.left.h > 0, msg);

      snprintf(msg, sizeof(msg), "%s eyes are symmetric",
               state_name(ALL_STATES[i]));
      CHECK(memcmp(&face.left, &face.right, sizeof(face.left)) == 0, msg);

      /* Radius must be drawable: the renderer clamps, but a table value that
       * needs clamping means the table disagrees with the intended shape. */
      snprintf(msg, sizeof(msg), "%s radius fits the box",
               state_name(ALL_STATES[i]));
      CHECK(face.left.r <= face.left.w / 2 && face.left.r <= face.left.h / 2,
            msg);

      snprintf(msg, sizeof(msg), "%s left eye inside the clear region",
               state_name(ALL_STATES[i]));
      CHECK(eye_within_region(&face.left, VA_FACE_CX - VA_FACE_EYE_DX,
                              VA_FACE_CY), msg);

      snprintf(msg, sizeof(msg), "%s right eye inside the clear region",
               state_name(ALL_STATES[i]));
      CHECK(eye_within_region(&face.right, VA_FACE_CX + VA_FACE_EYE_DX,
                              VA_FACE_CY), msg);
    }
}

/* Every state must be visually distinguishable, otherwise the display conveys
 * nothing.  Colour alone counts, because that is a deliberate design choice for
 * SAFE_STOP vs a blink. */
static void test_states_distinct(void)
{
  int i;
  int j;

  for (i = 0; i < STATE_COUNT; i++)
    {
      for (j = i + 1; j < STATE_COUNT; j++)
        {
          va_face_t a;
          va_face_t b;
          char msg[96];

          va_face_base(ALL_STATES[i], &a);
          va_face_base(ALL_STATES[j], &b);
          snprintf(msg, sizeof(msg), "%s differs from %s",
                   state_name(ALL_STATES[i]), state_name(ALL_STATES[j]));
          CHECK(!va_face_equal(&a, &b), msg);
        }
    }
}

static void test_colors_distinct(void)
{
  int i;
  int j;

  for (i = 0; i < STATE_COUNT; i++)
    {
      CHECK(va_face_color(ALL_STATES[i]) != 0x0000,
            "state colour is not black");
      for (j = i + 1; j < STATE_COUNT; j++)
        {
          char msg[96];
          snprintf(msg, sizeof(msg), "%s colour differs from %s",
                   state_name(ALL_STATES[i]), state_name(ALL_STATES[j]));
          CHECK(va_face_color(ALL_STATES[i]) != va_face_color(ALL_STATES[j]),
                msg);
        }
    }
}

/* The blink must actually happen, complete, and repeat.  A schedule that never
 * fires would leave a frozen face; one that never finishes would leave the eyes
 * shut. */
static void test_blink_fires_and_completes(void)
{
  va_face_anim_t anim;
  va_face_t face;
  uint32_t t;
  int blinks = 0;
  int min_h = 32767;
  int max_h = 0;
  bool inside_blink = false;

  va_face_anim_init(&anim, 1000);

  /* 30 s at 20 ms, the real main-loop tick. */
  for (t = 1000; t < 31000; t += 20)
    {
      va_face_update(&anim, VA_IDLE, t, 0, &face);
      if (face.left.h < min_h)
        {
          min_h = face.left.h;
        }
      if (face.left.h > max_h)
        {
          max_h = face.left.h;
        }
      if (!inside_blink && face.left.h < 40)
        {
          inside_blink = true;
          blinks++;
        }
      else if (inside_blink && face.left.h > 100)
        {
          inside_blink = false;
        }
    }

  /* Gap is 2.6-6.2 s, so 30 s must contain several blinks but not dozens. */
  CHECK(blinks >= 4, "blink fires repeatedly over 30 s");
  CHECK(blinks <= 12, "blink does not fire absurdly often");
  CHECK(max_h >= 120, "eyes return to the open height");
  CHECK(min_h <= 20, "eyes actually close");
  /* Never fully gone: a vanished face reads as a dead display. */
  CHECK(min_h >= 4, "a closed eye still leaves a visible line");
}

static void test_blink_jitter(void)
{
  va_face_anim_t a;
  va_face_anim_t b;

  /* Different boot times must not blink in lockstep, otherwise two units on a
   * bench look like clones. */
  va_face_anim_init(&a, 1000);
  va_face_anim_init(&b, 7777);
  CHECK(a.next_blink_ms - 1000 != b.next_blink_ms - 7777,
        "blink phase differs with the seed");

  /* And the interval must stay inside the designed window. */
  CHECK(a.next_blink_ms - 1000 >= VA_FACE_BLINK_MIN_GAP_MS,
        "first blink respects the minimum gap");
  CHECK(a.next_blink_ms - 1000 <= VA_FACE_BLINK_MAX_GAP_MS,
        "first blink respects the maximum gap");
}

/* SAFE_STOP must look stopped: no blink, no drift. */
static void test_safe_stop_is_inert(void)
{
  va_face_anim_t anim;
  va_face_t first;
  va_face_t later;
  uint32_t t;

  CHECK(!va_face_state_animates(VA_SAFE_STOP), "SAFE_STOP does not animate");

  va_face_anim_init(&anim, 0);
  va_face_update(&anim, VA_SAFE_STOP, 0, 0, &first);
  for (t = 20; t < 20000; t += 20)
    {
      va_face_update(&anim, VA_SAFE_STOP, t, 0, &later);
      if (!va_face_equal(&first, &later))
        {
          CHECK(false, "SAFE_STOP face never changes");
          return;
        }
    }
  CHECK(true, "SAFE_STOP face never changes");
}

/* The redraw trigger has to be selective: always-equal means the animation is
 * dead, always-different means a redraw every tick. */
static void test_equal_is_selective(void)
{
  va_face_anim_t anim;
  va_face_t previous;
  va_face_t current;
  uint32_t t;
  int changes = 0;
  int frames = 0;

  va_face_anim_init(&anim, 500);
  va_face_update(&anim, VA_IDLE, 500, 0, &previous);
  for (t = 520; t < 10500; t += 20)
    {
      va_face_update(&anim, VA_IDLE, t, 0, &current);
      frames++;
      if (!va_face_equal(&previous, &current))
        {
          changes++;
          previous = current;
        }
    }

  CHECK(changes > 0, "geometry changes at least sometimes");
  CHECK(changes < frames, "geometry does not change every single tick");
  /* Breathing is 2 px over 4.2 s plus a few blinks; a redraw on most ticks
   * would mean the saving is gone. */
  CHECK(changes * 2 < frames, "most ticks need no redraw");
}

/* The SPEAKING bar must react to the audio level and never disappear. */
static void test_speaking_bar(void)
{
  va_face_anim_t anim;
  va_face_t quiet;
  va_face_t loud;
  va_face_t idle;

  va_face_anim_init(&anim, 0);
  va_face_update(&anim, VA_SPEAKING, 100, 0, &quiet);
  va_face_update(&anim, VA_SPEAKING, 100, 255, &loud);
  va_face_update(&anim, VA_IDLE, 100, 255, &idle);

  CHECK(quiet.bar.h > 0, "bar is present during a pause");
  CHECK(loud.bar.h > quiet.bar.h, "bar grows with the audio level");
  CHECK(loud.bar.r <= loud.bar.h / 2, "bar radius stays drawable");
  CHECK(idle.bar.h == 0, "no bar outside SPEAKING");
}

int main(void)
{
  printf("== LCD face geometry tests ==\n");

  test_base_geometry();
  test_eye_band_inside_region();
  test_animation_stays_in_region();
  test_states_distinct();
  test_colors_distinct();
  test_blink_fires_and_completes();
  test_blink_jitter();
  test_safe_stop_is_inert();
  test_equal_is_selective();
  test_speaking_bar();

  if (g_failures != 0)
    {
      printf("%d of %d face checks failed\n", g_failures, g_checks);
      return 1;
    }
  printf("all %d face checks passed\n", g_checks);
  return 0;
}
