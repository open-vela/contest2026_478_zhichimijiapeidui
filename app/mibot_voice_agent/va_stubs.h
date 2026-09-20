/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - ASR/TTS stub interfaces (SF32 side).
 *
 * These are the Stub_Interface placeholders for speech-to-text (ASR) and
 * text-to-speech (TTS).  The signatures are stable so that swapping the stub
 * body for the real "via ESP32 -> cloud" implementation later does not change
 * caller code structure (需求 9.5).
 *
 * Real landing point (see design.md): generic open-domain ASR/TTS do not run
 * on the MCU.  The real asr_transcribe() waits for the `mibot.asr.v1` text
 * that ESP32 relays from the cloud; the real tts_request() sends `robot.speak`
 * and waits for `AUDIO_DOWN` playback.  The stubs here let LISTENING->THINKING
 * and SPEAKING->IDLE run with no real cloud (需求 9.1, 9.2).
 *
 * Compile-time stub switches (需求 9.5):
 *   MIBOT_VA_ASR_STUB - when defined, asr_transcribe() ignores the PCM and
 *                       returns a fixed transcript (VA_ASR_STUB_TEXT).  When
 *                       NOT defined, asr_transcribe() is a "real
 *                       implementation placeholder" that returns
 *                       VA_STUB_UNIMPLEMENTED; the real body (等 ESP32 回传的
 *                       mibot.asr.v1 文本) lands in task 13.
 *   MIBOT_VA_TTS_STUB - when defined, tts_request() plays a local test tone /
 *                       silence placeholder (on host: records an observable
 *                       request).  When NOT defined, it returns
 *                       VA_STUB_UNIMPLEMENTED; the real body (发 robot.speak
 *                       并等 AUDIO_DOWN) lands in task 13.
 *
 * Either way the function signatures are identical, so the caller (the state
 * machine) is unchanged when the real implementation replaces the stub
 * (需求 9.5).  The host test build defines BOTH macros so the full
 * LISTENING->THINKING->SPEAKING->IDLE turn runs with no cloud (需求 9.1, 9.2).
 */

#ifndef VA_STUBS_H
#define VA_STUBS_H

#include <stddef.h>
#include <stdbool.h>

#include "mibot_voice_agent.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Stub return convention: 0 on success, negative on "not implemented". */
#define VA_STUB_OK           0
#define VA_STUB_UNIMPLEMENTED (-1)

/* Fixed transcript returned by the ASR stub (需求 9.1).  Non-empty so the
 * LISTENING->THINKING flow always has usable text without a real recogniser. */
#define VA_ASR_STUB_TEXT "[asr_stub] 你好"

/* ASR stub: ignores PCM, produces a fixed/stub transcript so the dialog turn
 * can proceed without a real recogniser.  text_out is NUL-terminated on
 * success (truncated to fit text_size).  Returns VA_STUB_OK in stub mode, or
 * VA_STUB_UNIMPLEMENTED for the real-implementation placeholder / bad args. */
int asr_transcribe(const void *pcm, size_t pcm_len,
                   char *text_out, size_t text_size);

/* TTS stub: with no real synthesiser, plays a local test tone / silence
 * placeholder.  On host (no speaker) it records an observable request instead.
 * Returns VA_STUB_OK in stub mode, or VA_STUB_UNIMPLEMENTED for the
 * real-implementation placeholder.  `ctx` may be NULL. */
int tts_request(const char *text, va_context_t *ctx);

/* ------------------------------------------------------------------------- */
/* Stub-mode reporting (需求 9.6).                                            */
/*                                                                            */
/* These let device logs and host tests flag that a stubbed capability is in  */
/* use, so a stub is never mistaken for the real thing.                       */
/* ------------------------------------------------------------------------- */

/* True iff the ASR path is compiled in stub mode (MIBOT_VA_ASR_STUB). */
bool va_asr_stub_active(void);

/* True iff the TTS path is compiled in stub mode (MIBOT_VA_TTS_STUB). */
bool va_tts_stub_active(void);

/* True iff either ASR or TTS is in stub mode. */
bool va_stub_mode_active(void);

/* ------------------------------------------------------------------------- */
/* TTS observability (需求 9.2).                                              */
/*                                                                            */
/* On host there is no speaker, so the TTS stub records that a request was     */
/* made and the last text, which tests can query to prove "TTS was requested" */
/* without touching hardware.                                                  */
/* ------------------------------------------------------------------------- */

/* Number of tts_request() calls served by the stub since the last reset. */
int va_stub_tts_request_count(void);

/* Last text handed to the TTS stub ("" if none).  Never returns NULL. */
const char *va_stub_last_tts_text(void);

/* Reset the TTS observability counters/text (test helper). */
void va_stub_reset_observability(void);

#ifdef __cplusplus
}
#endif

#endif /* VA_STUBS_H */
