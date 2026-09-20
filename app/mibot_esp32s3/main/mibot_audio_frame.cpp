#include "mibot_audio_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr char kSchema[] = "mibot.audio.v1";
constexpr char kCodec[] = "pcm_s16le";
uint32_t g_stream_counter = 0;

void skip_json_ws(const char *json, size_t length, size_t *offset) {
  while (*offset < length && (json[*offset] == ' ' || json[*offset] == '\n' ||
                              json[*offset] == '\r' || json[*offset] == '\t')) {
    ++*offset;
  }
}

bool find_json_value(const char *json, size_t length, const char *key,
                     size_t *value_start, size_t *value_end, bool *is_string) {
  if (json == nullptr || key == nullptr || value_start == nullptr ||
      value_end == nullptr || is_string == nullptr) return false;
  char needle[48];
  const int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;
  for (size_t i = 0; i + static_cast<size_t>(n) <= length; ++i) {
    if (memcmp(json + i, needle, static_cast<size_t>(n)) != 0) continue;
    size_t pos = i + static_cast<size_t>(n);
    skip_json_ws(json, length, &pos);
    if (pos >= length || json[pos++] != ':') continue;
    skip_json_ws(json, length, &pos);
    if (pos >= length) return false;
    *is_string = json[pos] == '"';
    *value_start = pos;
    if (*is_string) {
      ++pos;
      bool escaped = false;
      while (pos < length) {
        const char c = json[pos++];
        if (!escaped && c == '"') { *value_end = pos - 1; return true; }
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
      }
      return false;
    }
    while (pos < length && json[pos] != ',' && json[pos] != '}') ++pos;
    size_t end = pos;
    while (end > *value_start && (json[end - 1] == ' ' || json[end - 1] == '\n' ||
                                  json[end - 1] == '\r' || json[end - 1] == '\t')) --end;
    *value_end = end;
    return end > *value_start;
  }
  return false;
}

bool copy_json_string(const char *json, size_t length, const char *key,
                      char *out, size_t out_capacity) {
  size_t start = 0, end = 0;
  bool is_string = false;
  if (out == nullptr || out_capacity == 0 ||
      !find_json_value(json, length, key, &start, &end, &is_string) ||
      !is_string || end <= start + 1) return false;
  size_t written = 0;
  for (size_t pos = start + 1; pos < end; ++pos) {
    char value = json[pos];
    if (value == '\\' && pos + 1 < end) {
      const char escaped = json[++pos];
      switch (escaped) {
        case '"': value = '"'; break;
        case '\\': value = '\\'; break;
        case '/': value = '/'; break;
        case 'b': value = '\b'; break;
        case 'f': value = '\f'; break;
        case 'n': value = '\n'; break;
        case 'r': value = '\r'; break;
        case 't': value = '\t'; break;
        default: return false;
      }
    }
    if (written + 1 >= out_capacity) return false;
    out[written++] = value;
  }
  out[written] = '\0';
  return true;
}

bool copy_json_number(const char *json, size_t length, const char *key,
                      uint32_t *out) {
  size_t start = 0, end = 0;
  bool is_string = false;
  if (out == nullptr || !find_json_value(json, length, key, &start, &end, &is_string) ||
      is_string || end <= start) return false;
  char buffer[24] = {};
  const size_t count = end - start;
  if (count >= sizeof(buffer)) return false;
  memcpy(buffer, json + start, count);
  char *parse_end = nullptr;
  const unsigned long value = strtoul(buffer, &parse_end, 10);
  if (parse_end == buffer || *parse_end != '\0' || value > 0xffffffffUL) return false;
  *out = static_cast<uint32_t>(value);
  return true;
}

bool copy_json_bool(const char *json, size_t length, const char *key,
                    uint8_t *out) {
  size_t start = 0, end = 0;
  bool is_string = false;
  if (out == nullptr || !find_json_value(json, length, key, &start, &end, &is_string) ||
      is_string) return false;
  if (end - start == 4 && memcmp(json + start, "true", 4) == 0) { *out = 1; return true; }
  if (end - start == 5 && memcmp(json + start, "false", 5) == 0) { *out = 0; return true; }
  return false;
}
}  // namespace

extern "C" size_t audio_frame_split(const uint8_t *input, size_t input_len,
                                      uint8_t *out, size_t out_capacity) {
  if ((input_len > 0 && input == nullptr) || (input_len > 0 && out == nullptr)) return 0;
  if (input_len > SIZE_MAX - (MIBOT_AUDIO_FRAME_BYTES - 1)) return 0;
  const size_t frames = input_len == 0 ? 0 : (input_len + MIBOT_AUDIO_FRAME_BYTES - 1) / MIBOT_AUDIO_FRAME_BYTES;
  if (frames > 0 && (frames > SIZE_MAX / MIBOT_AUDIO_FRAME_BYTES ||
                     out_capacity < frames * MIBOT_AUDIO_FRAME_BYTES)) return 0;
  for (size_t frame = 0; frame < frames; ++frame) {
    const size_t offset = frame * MIBOT_AUDIO_FRAME_BYTES;
    const size_t remaining = input_len > offset ? input_len - offset : 0;
    const size_t count = remaining > MIBOT_AUDIO_FRAME_BYTES ? MIBOT_AUDIO_FRAME_BYTES : remaining;
    memmove(out + offset, input + offset, count);
    if (count < MIBOT_AUDIO_FRAME_BYTES) memset(out + offset + count, 0, MIBOT_AUDIO_FRAME_BYTES - count);
  }
  return frames;
}

extern "C" int audio_meta_serialize(const AudioMeta *meta, char *out, size_t out_capacity) {
  if (meta == nullptr || out == nullptr || out_capacity == 0) return -1;
  const char *schema = kSchema;
  const char *codec = meta->codec[0] != '\0' ? meta->codec : kCodec;
  const int written = snprintf(out, out_capacity,
      "{\"schema\":\"%s\",\"stream_id\":\"%s\",\"codec\":\"%s\",\"sample_rate\":%u,\"channels\":%u,\"frame_ms\":%u,\"bytes\":%u,\"eos\":%s}",
      schema, meta->stream_id, codec, static_cast<unsigned>(meta->sample_rate),
      static_cast<unsigned>(meta->channels), static_cast<unsigned>(meta->frame_ms),
      static_cast<unsigned>(meta->bytes), meta->eos ? "true" : "false");
  return written >= 0 && static_cast<size_t>(written) < out_capacity ? written : -1;
}

extern "C" int audio_meta_parse(const char *json, size_t length, AudioMeta *out) {
  if (json == nullptr || out == nullptr || length == 0) return -1;
  AudioMeta parsed{};
  if (!copy_json_string(json, length, "schema", parsed.schema, sizeof(parsed.schema)) ||
      strcmp(parsed.schema, kSchema) != 0 ||
      !copy_json_string(json, length, "stream_id", parsed.stream_id, sizeof(parsed.stream_id)) ||
      parsed.stream_id[0] == '\0' ||
      !copy_json_string(json, length, "codec", parsed.codec, sizeof(parsed.codec)) ||
      !copy_json_number(json, length, "sample_rate", &parsed.sample_rate)) return -1;
  uint32_t value = 0;
  if (!copy_json_number(json, length, "channels", &value) || value > 0xffff) return -1;
  parsed.channels = static_cast<uint16_t>(value);
  if (!copy_json_number(json, length, "frame_ms", &value) || value > 0xffff) return -1;
  parsed.frame_ms = static_cast<uint16_t>(value);
  if (!copy_json_number(json, length, "bytes", &value) || value > 0xffff) return -1;
  parsed.bytes = static_cast<uint16_t>(value);
  if (!copy_json_bool(json, length, "eos", &parsed.eos)) return -1;
  *out = parsed;
  return 0;
}

extern "C" int audio_meta_looks_like_json(const uint8_t *data, size_t length) {
  if (data == nullptr || length < 2) return 0;
  size_t start = 0;
  while (start < length && (data[start] == ' ' || data[start] == '\n' || data[start] == '\r' || data[start] == '\t')) ++start;
  if (start >= length || data[start] != '{') return 0;
  for (size_t i = start; i + 8 <= length; ++i) if (memcmp(data + i, "\"schema\"", 8) == 0) return 1;
  return 0;
}

extern "C" int utf8_codepoint_count(const uint8_t *data, size_t length) {
  if (data == nullptr && length != 0) return -1;
  int count = 0;
  for (size_t i = 0; i < length; ++i, ++count) {
    const uint8_t lead = data[i];
    size_t width = 0;
    uint32_t codepoint = 0;
    if (lead <= 0x7f) { width = 1; codepoint = lead; }
    else if (lead >= 0xc2 && lead <= 0xdf) { width = 2; codepoint = lead & 0x1f; }
    else if (lead >= 0xe0 && lead <= 0xef) { width = 3; codepoint = lead & 0x0f; }
    else if (lead >= 0xf0 && lead <= 0xf4) { width = 4; codepoint = lead & 0x07; }
    else return -1;
    if (i + width > length) return -1;
    for (size_t j = 1; j < width; ++j) {
      if ((data[i + j] & 0xc0) != 0x80) return -1;
      codepoint = (codepoint << 6) | (data[i + j] & 0x3f);
    }
    if ((width == 2 && codepoint < 0x80) || (width == 3 && codepoint < 0x800) ||
        (width == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return -1;
    i += width - 1;
  }
  return count;
}

extern "C" int audio_stream_id_generate(char *out, size_t out_capacity) {
  if (out == nullptr || out_capacity < 9) return -1;
  const int written = snprintf(out, out_capacity, "aud_%04u", static_cast<unsigned>(++g_stream_counter % 10000));
  return written >= 0 && static_cast<size_t>(written) < out_capacity ? 0 : -1;
}

extern "C" uint32_t audio_next_backoff_ms(uint32_t current_ms) {
  if (current_ms == 0) return MIBOT_AUDIO_RECONNECT_MIN_MS;
  if (current_ms >= MIBOT_AUDIO_RECONNECT_MAX_MS / 2) return MIBOT_AUDIO_RECONNECT_MAX_MS;
  const uint32_t next = current_ms * 2;
  return next > MIBOT_AUDIO_RECONNECT_MAX_MS ? MIBOT_AUDIO_RECONNECT_MAX_MS : next;
}
