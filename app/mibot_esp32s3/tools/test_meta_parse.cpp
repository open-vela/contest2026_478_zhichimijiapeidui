// Host check for audio_meta_parse() against the exact AUDIO_UP metadata the
// SF32 voice agent emits.  Deterministic: no board, no serial timing.
//
// Build & run (WSL):
//   g++ -std=c++17 -DMIBOT_HOST_TEST -I../main test_meta_parse.cpp \
//       ../main/mibot_audio_frame.cpp -o /tmp/test_meta_parse && /tmp/test_meta_parse

#include <cstdio>
#include <cstring>

#include "mibot_audio_frame.h"

namespace {

int check(const char *label, const char *json) {
  AudioMeta meta{};
  const int rc = audio_meta_parse(json, strlen(json), &meta);
  printf("%-28s rc=%2d", label, rc);
  if (rc == 0) {
    printf("  schema=%s stream_id=%s codec=%s rate=%u ch=%u frame_ms=%u bytes=%u eos=%u",
           meta.schema, meta.stream_id, meta.codec,
           (unsigned)meta.sample_rate, (unsigned)meta.channels,
           (unsigned)meta.frame_ms, (unsigned)meta.bytes, (unsigned)meta.eos);
  }
  printf("\n");
  return rc;
}

}  // namespace

int main() {
  int failures = 0;

  // Exactly what mibot_voice_agent_main.c's va_send_audio_meta() produces.
  const char *start_meta =
      "{\"schema\":\"mibot.audio.v1\",\"stream_id\":\"va_1\","
      "\"codec\":\"pcm_s16le\",\"sample_rate\":16000,\"channels\":1,"
      "\"frame_ms\":20,\"bytes\":640,\"eos\":false}";
  const char *eos_meta =
      "{\"schema\":\"mibot.audio.v1\",\"stream_id\":\"va_1\","
      "\"codec\":\"pcm_s16le\",\"sample_rate\":16000,\"channels\":1,"
      "\"frame_ms\":20,\"bytes\":640,\"eos\":true}";

  if (check("sf32 start meta", start_meta) != 0) failures++;
  if (check("sf32 eos meta", eos_meta) != 0) failures++;

  // The ESP32's own serializer, as a reference for what it accepts.
  AudioMeta ref{};
  snprintf(ref.schema, sizeof(ref.schema), "%s", "mibot.audio.v1");
  snprintf(ref.stream_id, sizeof(ref.stream_id), "%s", "va_1");
  snprintf(ref.codec, sizeof(ref.codec), "%s", "pcm_s16le");
  ref.sample_rate = 16000;
  ref.channels = 1;
  ref.frame_ms = 20;
  ref.bytes = 640;
  ref.eos = 0;
  char serialized[256] = {};
  if (audio_meta_serialize(&ref, serialized, sizeof(serialized)) > 0) {
    printf("esp32 serialized         : %s\n", serialized);
    if (check("esp32 round-trip", serialized) != 0) failures++;
    printf("byte-identical to sf32    : %s\n",
           strcmp(serialized, start_meta) == 0 ? "yes" : "NO");
  } else {
    printf("esp32 serialize failed\n");
    failures++;
  }

  printf("%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
