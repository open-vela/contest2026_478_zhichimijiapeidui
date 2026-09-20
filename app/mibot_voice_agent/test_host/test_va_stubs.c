/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side tests for the Mibot Voice Agent ASR/TTS stub interfaces (task 3.1).
 *
 * The host build defines MIBOT_VA_ASR_STUB and MIBOT_VA_TTS_STUB (see
 * test_host/CMakeLists.txt), so both stubs take their stub branches.  These
 * tests check:
 *   - asr_transcribe() returns a non-empty NUL-terminated transcript and
 *     VA_STUB_OK in stub mode, and rejects bad args (需求 9.1, 9.5).
 *   - tts_request() returns VA_STUB_OK and records an observable request
 *     (需求 9.2).
 *   - the stub-mode reporting helpers flag stub mode (需求 9.6).
 *   - the stubbed ASR text drives a full LISTENING->THINKING->SPEAKING->IDLE
 *     turn without being blocked (P13 basic connectivity, 需求 9.1, 9.2).
 *
 * The full P13 property test is optional task 3.2.  This uses the same tiny
 * dependency-free assertion harness as test_va_state_machine.c.
 */

#include <stdio.h>
#include <string.h>

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
      else                                                                   \
        {                                                                    \
          printf("ok: %s\n", (msg));                                         \
        }                                                                    \
    }                                                                        \
  while (0)

/* ------------------------------------------------------------------------- */
/* Stub-mode reporting (需求 9.6).                                            */
/* ------------------------------------------------------------------------- */

static void test_stub_mode_flags(void)
{
  /* The host build compiles both stubs in. */
  CHECK(va_asr_stub_active() == true, "ASR stub mode active on host build");
  CHECK(va_tts_stub_active() == true, "TTS stub mode active on host build");
  CHECK(va_stub_mode_active() == true, "stub mode active when either is stubbed");
}

/* ------------------------------------------------------------------------- */
/* ASR stub (需求 9.1, 9.5).                                                  */
/* ------------------------------------------------------------------------- */

static void test_asr_returns_fixed_transcript(void)
{
  char text[64];
  int rc;

  memset(text, 0xAA, sizeof(text));   /* poison to prove NUL-termination */
  rc = asr_transcribe(NULL, 0, text, sizeof(text));

  CHECK(rc == VA_STUB_OK, "asr_transcribe returns VA_STUB_OK in stub mode");
  CHECK(text[0] != '\0', "asr_transcribe returns non-empty transcript");
  CHECK(strlen(text) < sizeof(text), "asr transcript is NUL-terminated in buffer");
  CHECK(strcmp(text, VA_ASR_STUB_TEXT) == 0,
        "asr_transcribe returns the fixed stub transcript");
}

/* PCM content is ignored by the stub: any buffer yields the same transcript. */
static void test_asr_ignores_pcm(void)
{
  char text[64];
  unsigned char pcm[8] = {1, 2, 3, 4, 5, 6, 7, 8};

  CHECK(asr_transcribe(pcm, sizeof(pcm), text, sizeof(text)) == VA_STUB_OK,
        "asr_transcribe accepts a PCM buffer in stub mode");
  CHECK(strcmp(text, VA_ASR_STUB_TEXT) == 0,
        "asr_transcribe ignores PCM and returns the fixed transcript");
}

/* Bad arguments are rejected. */
static void test_asr_rejects_bad_args(void)
{
  char text[8];

  CHECK(asr_transcribe(NULL, 0, NULL, sizeof(text)) == VA_STUB_UNIMPLEMENTED,
        "asr_transcribe rejects NULL text_out");
  CHECK(asr_transcribe(NULL, 0, text, 0) == VA_STUB_UNIMPLEMENTED,
        "asr_transcribe rejects zero text_size");
}

/* A tiny buffer still yields a NUL-terminated (truncated) string, no overflow. */
static void test_asr_truncates_into_small_buffer(void)
{
  char text[4];

  memset(text, 0xAA, sizeof(text));
  CHECK(asr_transcribe(NULL, 0, text, sizeof(text)) == VA_STUB_OK,
        "asr_transcribe succeeds with a small buffer");
  CHECK(text[sizeof(text) - 1] == '\0',
        "asr transcript is NUL-terminated even when truncated");
}

/* ------------------------------------------------------------------------- */
/* TTS stub (需求 9.2).                                                       */
/* ------------------------------------------------------------------------- */

static void test_tts_records_observable_request(void)
{
  int rc;

  va_stub_reset_observability();
  CHECK(va_stub_tts_request_count() == 0, "TTS request count starts at zero");

  rc = tts_request("hello there", NULL);
  CHECK(rc == VA_STUB_OK, "tts_request returns VA_STUB_OK in stub mode");
  CHECK(va_stub_tts_request_count() == 1, "tts_request records one request");
  CHECK(strcmp(va_stub_last_tts_text(), "hello there") == 0,
        "tts_request records the requested text");

  rc = tts_request("second", NULL);
  CHECK(rc == VA_STUB_OK, "second tts_request returns VA_STUB_OK");
  CHECK(va_stub_tts_request_count() == 2, "tts_request count increments");
  CHECK(strcmp(va_stub_last_tts_text(), "second") == 0,
        "tts_request records the latest text");
}

/* A NULL text must not crash and is recorded as empty. */
static void test_tts_handles_null_text(void)
{
  va_stub_reset_observability();
  CHECK(tts_request(NULL, NULL) == VA_STUB_OK,
        "tts_request accepts NULL text in stub mode");
  CHECK(va_stub_tts_request_count() == 1, "NULL-text request is still recorded");
  CHECK(strcmp(va_stub_last_tts_text(), "") == 0,
        "NULL text recorded as empty string");
}

/* ------------------------------------------------------------------------- */
/* P13 basic connectivity: stubbed ASR/TTS do not block the dialog turn.      */
/* Validates: Requirements 9.1, 9.2                                           */
/* ------------------------------------------------------------------------- */

static void test_stub_pipeline_completes_dialog_turn(void)
{
  va_context_t ctx;
  char transcript[64];

  va_stub_reset_observability();

  va_init(&ctx, NULL);
  ctx.cloud_available = true;

  /* IDLE -> LISTENING */
  CHECK(va_handle_event(&ctx, VA_EVENT_WAKE) && ctx.state == VA_LISTENING,
        "P13: WAKE moves IDLE -> LISTENING");

  /* The stubbed ASR yields a non-empty transcript so the turn can proceed. */
  CHECK(asr_transcribe(NULL, 0, transcript, sizeof(transcript)) == VA_STUB_OK
        && transcript[0] != '\0',
        "P13: stub ASR yields non-empty transcript to drive THINKING");

  /* LISTENING -> THINKING (ASR done) */
  CHECK(va_handle_event(&ctx, VA_EVENT_ASR_DONE) && ctx.state == VA_THINKING,
        "P13: ASR_DONE moves LISTENING -> THINKING");

  /* THINKING -> SPEAKING (text-only reply) */
  strncpy(ctx.pending_answer, "stub answer", sizeof(ctx.pending_answer) - 1);
  CHECK(va_handle_event(&ctx, VA_EVENT_TEXT_ONLY) && ctx.state == VA_SPEAKING,
        "P13: TEXT_ONLY moves THINKING -> SPEAKING");

  /* The stubbed TTS accepts the request without blocking. */
  CHECK(tts_request(ctx.pending_answer, &ctx) == VA_STUB_OK,
        "P13: stub TTS accepts the SPEAKING request");
  CHECK(va_stub_tts_request_count() == 1, "P13: TTS was requested once");

  /* SPEAKING -> IDLE (playback eos) */
  CHECK(va_handle_event(&ctx, VA_EVENT_SPEAK_DONE) && ctx.state == VA_IDLE,
        "P13: SPEAK_DONE moves SPEAKING -> IDLE");
}

int main(void)
{
  test_stub_mode_flags();

  test_asr_returns_fixed_transcript();
  test_asr_ignores_pcm();
  test_asr_rejects_bad_args();
  test_asr_truncates_into_small_buffer();

  test_tts_records_observable_request();
  test_tts_handles_null_text();

  test_stub_pipeline_completes_dialog_turn();

  if (g_failures == 0)
    {
      printf("\nAll tests passed.\n");
      return 0;
    }

  printf("\n%d test(s) failed.\n", g_failures);
  return 1;
}
