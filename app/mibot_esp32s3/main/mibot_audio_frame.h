#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "mibot_config.h"
#else
#define MIBOT_MAX_FRAME_PAYLOAD 4096
#define MIBOT_AUDIO_SAMPLE_RATE 16000
#define MIBOT_AUDIO_CHANNELS 1
#define MIBOT_AUDIO_FRAME_MS 20
#define MIBOT_AUDIO_FRAME_SAMPLES 320
#define MIBOT_AUDIO_FRAME_BYTES 640
#define MIBOT_AUDIO_STREAM_ID_MAX 31
#define MIBOT_AUDIO_RECONNECT_MIN_MS 1000
#define MIBOT_AUDIO_RECONNECT_MAX_MS 30000
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioMeta {
  char schema[20];
  char stream_id[MIBOT_AUDIO_STREAM_ID_MAX + 1];
  char codec[20];
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t frame_ms;
  uint16_t bytes;
  uint8_t eos;
} AudioMeta;

size_t audio_frame_split(const uint8_t *input, size_t input_len, uint8_t *out,
                         size_t out_capacity);
int audio_meta_serialize(const AudioMeta *meta, char *out, size_t out_capacity);
int audio_meta_parse(const char *json, size_t length, AudioMeta *out);
int audio_meta_looks_like_json(const uint8_t *data, size_t length);
int utf8_codepoint_count(const uint8_t *data, size_t length);
int audio_stream_id_generate(char *out, size_t out_capacity);
uint32_t audio_next_backoff_ms(uint32_t current_ms);

#ifdef __cplusplus
}
#endif
