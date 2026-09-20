/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - LCD face geometry (需求 4.1-4.5).
 *
 * The face is two rounded rectangles and nothing else: no mouth, no pupils, no
 * decoration.  Every state is the same shape deformed, described by four
 * numbers per eye, which is what makes a state change a plain interpolation and
 * an intensity of 0..1 meaningful.
 *
 * This header is deliberately free of NuttX, framebuffer and board headers so
 * the geometry and the animation schedule can be exercised on a host.  The
 * pixel pushing lives in mibot_voice_agent_main.c.
 *
 * Panel: CO5300 AMOLED, 390x450, RGB565.  Black is genuinely unlit on AMOLED,
 * so the face is drawn on black rather than on a filled colour field: it costs
 * less power and avoids the burn-in a full-screen colour would accelerate.
 */

#ifndef __EXAMPLES_MIBOT_AGENT_VA_FACE_H
#define __EXAMPLES_MIBOT_AGENT_VA_FACE_H

#include <stdbool.h>
#include <stdint.h>

#include "mibot_voice_agent.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ------------------------------------------------------------------------- */
/* Canvas constants, in panel pixels.                                        */
/* ------------------------------------------------------------------------- */

#define VA_FACE_CANVAS_W   390
#define VA_FACE_CANVAS_H   450

/* Nominal eye centres.  Slightly above the vertical centre (225) because a
 * dead-centre pair reads as a bit lifeless. */
#define VA_FACE_CX         195
#define VA_FACE_CY         214
#define VA_FACE_EYE_DX      78

/* The speaking bar sits below the eyes; it is the only non-eye element. */
#define VA_FACE_BAR_CY     330

/* Bounding box of every pixel the face can ever touch.
 *
 * A redraw clears and flushes only the *rows* in this box (Y..Y+H), which is
 * what makes blink animation affordable when the framebuffer lives in PSRAM.
 * It deliberately does NOT narrow the flush horizontally: the panel driver only
 * uses its chunked DMA path when the flushed row width equals the framebuffer
 * stride, so a narrower rectangle is far slower than a full-width one.  X and W
 * therefore exist as a design guard, asserted by test_va_face.c, rather than as
 * the flush extent.
 *
 * The box must cover the widest/tallest state *including* the per-state dx/dy
 * shifts and the breathing drift, and the speaking bar.  The worst cases today:
 *   x: LISTENING w=106 at dx=0   -> 64 .. 326
 *      THINKING  w=100 at dx=+14 -> 81 .. 337   <- rightmost
 *   y: LISTENING h=146           -> 139 .. 289
 *      speaking bar at 330, h=26 -> 315 .. 345  <- lowest
 * test_va_face.c asserts this containment over the whole animation, so a future
 * table edit that outgrows the region fails the build rather than leaving a
 * smear on the panel. */
#define VA_FACE_REGION_X    52
#define VA_FACE_REGION_Y   130
#define VA_FACE_REGION_W   292
#define VA_FACE_REGION_H   228

/* Rows the eyes alone can touch.  Frame cost is very nearly linear in the
 * number of flushed rows (the panel DMA dominates, not the pixel writes), and
 * the speaking bar is the only thing that needs the lower two thirds of the
 * region.  Every state except SPEAKING therefore flushes this shorter band,
 * which is what buys enough frames to render a blink's opening ramp instead of
 * snapping back open.  Eye extremes today: LISTENING h=146 -> 139..289 with the
 * breathing drift; THINKING dy=-22 -> 163..221; SPEAKING dy=-10 -> 145..263. */
#define VA_FACE_EYE_BAND_Y 136
#define VA_FACE_EYE_BAND_H 158

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int16_t w;     /* width  */
  int16_t h;     /* height; 0 means "not drawn"                      */
  int16_t r;     /* corner radius, clamped to min(w, h) / 2 on draw  */
  int16_t dx;    /* offset from the nominal eye centre               */
  int16_t dy;
} va_eye_t;

typedef struct
{
  va_eye_t left;
  va_eye_t right;
  va_eye_t bar;        /* speaking cue; h == 0 when absent */
  uint16_t color;      /* RGB565 */
} va_face_t;

/* Animation state.  Owned by the caller so the composition function stays
 * free of globals and can be host-tested deterministically. */
typedef struct
{
  uint32_t rng;             /* blink interval jitter               */
  uint32_t next_blink_ms;
  uint32_t blink_start_ms;  /* 0 when not blinking                 */
} va_face_anim_t;

/* Blink shape: close, hold shut, open.  160 ms total is close to a human
 * blink; much faster looks like a glitch and much slower looks sleepy. */
#define VA_FACE_BLINK_CLOSE_MS   60
#define VA_FACE_BLINK_HOLD_MS    40
#define VA_FACE_BLINK_OPEN_MS    60
#define VA_FACE_BLINK_TOTAL_MS   (VA_FACE_BLINK_CLOSE_MS + \
                                  VA_FACE_BLINK_HOLD_MS + \
                                  VA_FACE_BLINK_OPEN_MS)

/* Gap between blinks, jittered in this window so it never looks metronomic. */
#define VA_FACE_BLINK_MIN_GAP_MS 2600
#define VA_FACE_BLINK_MAX_GAP_MS 6200

/* Slow vertical drift.  Two jobs: it keeps an idle face from looking frozen,
 * and it moves the lit pixels around, which is the cheapest burn-in mitigation
 * available on an AMOLED. */
#define VA_FACE_BREATH_PERIOD_MS 4200
#define VA_FACE_BREATH_AMP_PX    2

/* Resting colour per Main_State, RGB565.
 *
 * Muted tones rather than the saturated RGB565 corners: full-saturation
 * primaries look harsh on this panel.  Hue per state is unchanged, so the
 * one-colour-per-state contract still holds. */
#define VA_FACE_COLOR_IDLE      0x6F1F   /* mint    */
#define VA_FACE_COLOR_LISTENING 0x5EBF   /* sky     */
#define VA_FACE_COLOR_THINKING  0xFEB1   /* amber   */
#define VA_FACE_COLOR_ACTING    0xFD6E   /* orange  */
#define VA_FACE_COLOR_SPEAKING  0xBD7F   /* violet  */
#define VA_FACE_COLOR_SAFE_STOP 0xFB6F   /* red     */

/* Resting geometry for a state, with no animation applied. */
void va_face_base(va_state_t state, va_face_t *out);

/* Colour for a state. */
uint16_t va_face_color(va_state_t state);

void va_face_anim_init(va_face_anim_t *anim, uint32_t now_ms);

/* Compose the face to draw now: base geometry for `state`, plus blink and
 * breathing, plus the speaking bar scaled by `speak_level` (0..255, only
 * consulted in VA_SPEAKING).  Advances the blink schedule, so call it once per
 * render tick.
 */
void va_face_update(va_face_anim_t *anim, va_state_t state, uint32_t now_ms,
                    uint8_t speak_level, va_face_t *out);

/* True when two compositions are pixel-identical, so the caller can skip a
 * redraw.  This is what keeps the animation from costing a redraw every tick. */
bool va_face_equal(const va_face_t *a, const va_face_t *b);

/* Whether a state animates at all.  SAFE_STOP is deliberately static: a
 * blinking or breathing face would undercut "stopped". */
bool va_face_state_animates(va_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* __EXAMPLES_MIBOT_AGENT_VA_FACE_H */
