/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - ASR/TTS stub implementations (SF32 side).
 *
 * Task 3.1 scope: the stub bodies for asr_transcribe() and tts_request(),
 * their compile-time switches (MIBOT_VA_ASR_STUB / MIBOT_VA_TTS_STUB), the
 * stub-mode reporting helpers (需求 9.6) and host-observable TTS state.
 *
 * Architecture (see design.md "桩接口的真实落点"): generic open-domain ASR/TTS
 * do not run on the MCU.  When the stub macros are NOT defined these functions
 * are "real implementation placeholders" that return VA_STUB_UNIMPLEMENTED; the
 * real bodies land in task 13:
 *   - asr_transcribe(): wait for the `mibot.asr.v1` text ESP32 relays from the
 *     cloud ASR (via AUDIO_UP -> ESP32 -> WSS -> cloud -> AI_RESPONSE).
 *   - tts_request(): send `robot.speak` and wait for `AUDIO_DOWN` playback.
 * The function signatures never change, so the state-machine caller is
 * unchanged when the real code replaces the stub (需求 9.5).
 *
 * Kept hardware-agnostic so it compiles on host as well; any board_audio_* or
 * NuttX usage is guarded with `#if defined(__NuttX__)`.
 */

#include "va_stubs.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Stub-mode logging (需求 9.6).                                              */
/*                                                                            */
/* Every stub-mode use is logged so a stubbed capability is never mistaken    */
/* for a real one.  syslog() on NuttX, printf() on host.                      */
/* ------------------------------------------------------------------------- */

#if defined(__NuttX__)
#  include <syslog.h>
#  define VA_STUB_LOG(...) syslog(LOG_INFO, __VA_ARGS__)
#else
#  define VA_STUB_LOG(...) printf(__VA_ARGS__)
#endif

/* ------------------------------------------------------------------------- */
/* TTS observability (需求 9.2).                                              */
/* ------------------------------------------------------------------------- */

static int  g_tts_request_count;
static char g_tts_last_text[VA_PENDING_ANSWER_SIZE];

/* ------------------------------------------------------------------------- */
/* ASR stub (需求 9.1, 9.5, 9.6).                                             */
/* ------------------------------------------------------------------------- */

int asr_transcribe(const void *pcm, size_t pcm_len,
                   char *text_out, size_t text_size)
{
  if (text_out == NULL || text_size == 0)
    {
      return VA_STUB_UNIMPLEMENTED;
    }

#if defined(MIBOT_VA_ASR_STUB)
  /* Stub mode: ignore the PCM and return a fixed transcript so the
   * LISTENING -> THINKING flow runs with no real recogniser (需求 9.1). */
  (void)pcm;

  VA_STUB_LOG("[va_stub] ASR stub mode active, ignoring %zu PCM bytes, "
              "returning fixed transcript\n", pcm_len);

  /* Copy the fixed transcript, always NUL-terminated even if truncated. */
  strncpy(text_out, VA_ASR_STUB_TEXT, text_size - 1);
  text_out[text_size - 1] = '\0';

  return VA_STUB_OK;
#else
  /* Real-implementation placeholder (需求 9.5): the real ASR text arrives from
   * the cloud via ESP32 as `mibot.asr.v1`; wiring that in is task 13.  Signature
   * stays stable so the caller is unchanged when the real body lands. */
  (void)pcm;
  (void)pcm_len;
  text_out[0] = '\0';
  return VA_STUB_UNIMPLEMENTED;
#endif
}

/* ------------------------------------------------------------------------- */
/* TTS stub (需求 9.2, 9.5, 9.6).                                             */
/* ------------------------------------------------------------------------- */

int tts_request(const char *text, va_context_t *ctx)
{
  (void)ctx;

#if defined(MIBOT_VA_TTS_STUB)
  /* Stub mode: no real synthesiser, so play a local test tone / silence
   * placeholder.  On host there is no speaker, so we only record an observable
   * request (需求 9.2). */
  const char *safe_text = (text != NULL) ? text : "";

  VA_STUB_LOG("[va_stub] TTS stub mode active, request for \"%s\" -> "
              "local test tone / silence placeholder\n", safe_text);

  /* Record for host observability: prove "TTS was requested" without hardware. */
  g_tts_request_count++;
  strncpy(g_tts_last_text, safe_text, sizeof(g_tts_last_text) - 1);
  g_tts_last_text[sizeof(g_tts_last_text) - 1] = '\0';

#if defined(__NuttX__)
  /* Device-only: emit a short local test tone through the speaker so the
   * SPEAKING state has something audible.  The concrete board_audio_* tone
   * generation is wired with the audio integration in task 9.3; keeping the
   * hook here documents where the real device tone belongs. */
  /* TODO(task 9.3): board_audio_play_test_tone(); */
#endif

  return VA_STUB_OK;
#else
  /* Real-implementation placeholder (需求 9.5): the real TTS is driven by
   * sending `robot.speak` to ESP32 and awaiting `AUDIO_DOWN` playback; wiring
   * that in is task 13.  Signature stays stable. */
  (void)text;
  return VA_STUB_UNIMPLEMENTED;
#endif
}

/* ------------------------------------------------------------------------- */
/* Stub-mode reporting (需求 9.6).                                            */
/* ------------------------------------------------------------------------- */

bool va_asr_stub_active(void)
{
#if defined(MIBOT_VA_ASR_STUB)
  return true;
#else
  return false;
#endif
}

bool va_tts_stub_active(void)
{
#if defined(MIBOT_VA_TTS_STUB)
  return true;
#else
  return false;
#endif
}

bool va_stub_mode_active(void)
{
  return va_asr_stub_active() || va_tts_stub_active();
}

/* ------------------------------------------------------------------------- */
/* TTS observability accessors (需求 9.2).                                    */
/* ------------------------------------------------------------------------- */

int va_stub_tts_request_count(void)
{
  return g_tts_request_count;
}

const char *va_stub_last_tts_text(void)
{
  return g_tts_last_text;
}

void va_stub_reset_observability(void)
{
  g_tts_request_count = 0;
  g_tts_last_text[0] = '\0';
}
