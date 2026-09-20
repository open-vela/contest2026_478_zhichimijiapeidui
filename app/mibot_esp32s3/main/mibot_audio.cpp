#include "mibot_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
#include "cJSON.h"
#endif

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#if __has_include("mibot_secrets.h")
#include "mibot_secrets.h"
#else
#define MIBOT_CLOUD_WS_URI ""
#define MIBOT_CLOUD_API_KEY ""
#endif
#endif

/* Total bytes handed to the SF32 UART, owned by mibot_controller.cpp.  Logged
 * at end-of-stream so it can be compared with the SF32's own receive total.
 * Only that translation unit is linked in the firmware image, so the host test
 * build must not reference it. */
#if defined(ESP_PLATFORM)
extern "C" uint32_t mibot_uart_tx_bytes(void);
#endif

namespace {
constexpr char TAG[] = "mibot_audio";

struct DownlinkStream {
  char stream_id[MIBOT_AUDIO_STREAM_ID_MAX + 1] = {};
  char command_id[MIBOT_COMMAND_ID_MAX] = {};
  char trace_id[64] = {};
  char voice[32] = {};
  char abort_reason[32] = {};
  bool active = false;
  bool interruptible = true;
  bool meta_sent = false;
  bool cloud_finished = false;
  bool eos_sent = false;
  bool aborted = false;
  bool prefill_ready = false;
  int64_t request_sent_ms = 0;
  int64_t first_chunk_ms = 0;
  uint8_t partial[MIBOT_AUDIO_FRAME_BYTES] = {};
  size_t partial_len = 0;
  AudioReplyTarget target{0, -1};
};

struct CloudChunk {
  cloud_frame_kind_t kind;
  cloud_request_ctx_t ctx;
  size_t length;
  uint8_t data[1];
};

struct AudioRuntime {
  AudioRing uplink{};
  AudioRing downlink{};
  SemaphoreHandle_t mutex = nullptr;
  AudioDiag diag{};
  audio_direction_t direction = AUDIO_DIRECTION_IDLE;
  audio_mode_t mode = AUDIO_MODE_CLOUD;
  DownlinkStream stream{};
  cloud_adapter_t *cloud = nullptr;
  QueueHandle_t cloud_queue = nullptr;
  audio_send_frame_fn sender = nullptr;
  void *sender_user = nullptr;
  volatile int capture_fd = -1;
  bool initialized = false;
  bool degraded = true;
  bool up_meta_seen = false;
  bool up_blocked = false;
  bool up_flush_requested = false;
  bool have_last_seq = false;
  uint16_t last_seq = 0;
  char up_stream_id[MIBOT_AUDIO_STREAM_ID_MAX + 1] = {};
  AudioReplyTarget up_target{0, -1};
  int64_t last_up_ms = 0;
  uint32_t reconnect_ms = MIBOT_AUDIO_RECONNECT_MIN_MS;
  int64_t next_reconnect_ms = 0;
  uint32_t down_interval_hist[51] = {};
  uint32_t down_interval_samples = 0;
  int64_t down_window_start_ms = 0;
  uint32_t down_window_frames = 0;
  int64_t tx_last_snapshot_ms = -1;
  int64_t tx_last_down_send_ms = 0;
  enum CacheState : uint8_t { CACHE_FREE = 0, CACHE_IN_PROGRESS = 1, CACHE_DONE = 2 };
  struct CommandSlot {
    CacheState state = CACHE_FREE;
    char command_id[MIBOT_COMMAND_ID_MAX] = {};
    int64_t updated_ms = 0;
    bool ok = false;
    char error_code[32] = {};
    char reason[32] = {};
    char stream_id[MIBOT_AUDIO_STREAM_ID_MAX + 1] = {};
  };
  CommandSlot command_cache[MIBOT_AUDIO_CMD_CACHE_SLOTS] = {};
};

AudioRuntime g_audio;
#if defined(ESP_PLATFORM)
TaskHandle_t g_audio_up_task_handle = nullptr;
TaskHandle_t g_audio_tx_task_handle = nullptr;
TaskHandle_t g_audio_dn_task_handle = nullptr;
#endif

void process_cloud_chunk(CloudChunk *chunk);
void audio_tx_step_internal();

#if defined(ESP_PLATFORM)
int64_t clock_ms() { return esp_timer_get_time() / 1000; }
#elif defined(MIBOT_HOST_TEST)
extern "C" int64_t fake_clock_now(void);
int64_t clock_ms() { return fake_clock_now(); }
#else
int64_t clock_ms() { return 0; }
#endif

void lock_audio() {
  if (g_audio.mutex != nullptr) xSemaphoreTake(g_audio.mutex, portMAX_DELAY);
}
void unlock_audio() {
  if (g_audio.mutex != nullptr) xSemaphoreGive(g_audio.mutex);
}

int send_audio_frame(uint8_t type, uint8_t flags, const uint8_t *payload,
                     uint16_t length, const AudioReplyTarget *target) {
  if (g_audio.sender == nullptr) return ESP_FAIL;
  return g_audio.sender(type, flags, payload, length, target, g_audio.sender_user);
}

cloud_state_t current_cloud_state() {
  const cloud_adapter_t *cloud = g_audio.cloud;
  if (cloud == nullptr || cloud->vtable == nullptr || cloud->vtable->state == nullptr) {
    return CLOUD_STATE_DISCONNECTED;
  }
  return cloud->vtable->state(cloud);
}

void add_audio_counters(cJSON *audio, const AudioDiag &d) {
#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
  if (audio == nullptr) return;
#define ADD_COUNTER(name) cJSON_AddNumberToObject(audio, #name, static_cast<double>(d.name))
  ADD_COUNTER(audio_up_frames_received);
  ADD_COUNTER(audio_up_crc_error);
  ADD_COUNTER(audio_up_size_error);
  ADD_COUNTER(audio_up_seq_gap);
  ADD_COUNTER(audio_up_meta_error);
  ADD_COUNTER(uplink_dropped);
  ADD_COUNTER(uplink_send_error);
  ADD_COUNTER(downlink_decode_error);
  ADD_COUNTER(downlink_dropped);
  ADD_COUNTER(downlink_underrun);
  ADD_COUNTER(downlink_aborted);
  ADD_COUNTER(audio_down_frames_sent);
  ADD_COUNTER(audio_down_write_error);
#undef ADD_COUNTER
#else
  (void)audio;
  (void)d;
#endif
}

void add_audio_diag(cJSON *audio, bool detailed, cloud_state_t cloud_state) {
#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
  if (audio == nullptr) return;
  const AudioDiag d = g_audio.diag;
  cJSON_AddStringToObject(audio, "buffer_profile",
                          g_audio.uplink.psram ? "PSRAM_PROFILE" : "SRAM_PROFILE");
  cJSON_AddStringToObject(audio, "direction", audio_direction_name(g_audio.direction));
  cJSON_AddStringToObject(audio, "mode", audio_mode_name(g_audio.mode));
  cJSON_AddStringToObject(audio, "cloud_state", cloud_state == CLOUD_STATE_CONNECTED
                                                       ? "connected"
                                                       : cloud_state == CLOUD_STATE_CONNECTING
                                                           ? "connecting"
                                                           : "disconnected");
  cJSON_AddBoolToObject(audio, "degraded", g_audio.degraded);
  cJSON_AddStringToObject(audio, "stream_id", g_audio.stream.stream_id);
  if (!detailed) return;
  add_audio_counters(audio, d);
  cJSON_AddNumberToObject(audio, "audio_down_bytes_1s", d.audio_down_bytes_1s_snapshot);
  cJSON_AddNumberToObject(audio, "uart_bytes_1s", d.audio_down_bytes_1s_snapshot);
  cJSON_AddNumberToObject(audio, "uplink_latency_ms", d.uplink_latency_ms);
  cJSON_AddNumberToObject(audio, "downlink_latency_ms", d.downlink_latency_ms);
  cJSON_AddNumberToObject(audio, "tx_lock_max_ms", d.tx_lock_max_ms);
  cJSON_AddNumberToObject(audio, "tx_lock_over_budget", d.tx_lock_over_budget);
  cJSON_AddNumberToObject(audio, "audio_down_interval_min_ms", d.audio_down_interval_min_ms);
  cJSON_AddNumberToObject(audio, "audio_down_interval_max_ms", d.audio_down_interval_max_ms);
  cJSON_AddNumberToObject(audio, "audio_down_interval_p99_ms", d.audio_down_interval_p99_ms);
  cJSON_AddNumberToObject(audio, "audio_down_50_frames_ms", d.audio_down_50_frames_ms);
  cJSON_AddNumberToObject(audio, "uart_rx_peak_bytes", d.uart_rx_peak_bytes);
  cJSON_AddNumberToObject(audio, "uart_frame_process_max_ms", d.uart_frame_process_max_ms);
  cJSON_AddNumberToObject(audio, "safety_period_min_ms", d.safety_period_min_ms);
  cJSON_AddNumberToObject(audio, "safety_period_max_ms", d.safety_period_max_ms);
#else
  (void)audio;
  (void)detailed;
  (void)cloud_state;
#endif
}

void emit_event(const char *event, const char *severity, cJSON *data = nullptr) {
#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return;
  cJSON_AddStringToObject(root, "schema", "mibot.event.v1");
  cJSON_AddStringToObject(root, "event", event);
  cJSON_AddStringToObject(root, "severity", severity);
  cJSON_AddStringToObject(root, "source", "esp32");
  cJSON_AddItemToObject(root, "data", data != nullptr ? data : cJSON_CreateObject());
  cJSON_AddBoolToObject(root, "requires_ack", false);
  char *text = cJSON_PrintUnformatted(root);
  if (text != nullptr) {
    const AudioReplyTarget target{0, -1};
    send_audio_frame(MIBOT_MSG_EVENT, 0, reinterpret_cast<const uint8_t *>(text),
                     static_cast<uint16_t>(strlen(text)), &target);
    cJSON_free(text);
  }
  cJSON_Delete(root);
#else
  (void)event; (void)severity; (void)data;
#endif
}

const char *audio_error_message(const char *error_code) {
  if (error_code == nullptr) return "invalid argument";
  struct ErrorText { const char *code; const char *message; };
  static constexpr ErrorText errors[] = {
      {"E_INVALID_ARG", "invalid argument"},
      {"E_EXPIRED", "command expired"},
      {"E_DUPLICATE", "duplicate command"},
      {"E_BUSY", "audio stream is busy"},
      {"E_SAFETY_LOCK", "safety lock is active"},
      {"E_TOF_INVALID", "time-of-flight sensor is invalid"},
      {"E_MOTOR_STALL", "motor stall detected"},
      {"E_SERVO_LIMIT", "servo limit exceeded"},
      {"E_LOW_BATTERY", "battery is low"},
      {"E_UART_TIMEOUT", "UART timed out"},
      {"E_CLOUD_TIMEOUT", "cloud service timed out"},
      {"E_PROTOCOL_CRC", "protocol CRC check failed"},
      {"E_UNSUPPORTED", "audio format is unsupported"},
  };
  for (const auto &entry : errors) {
    if (strcmp(entry.code, error_code) == 0) return entry.message;
  }
  return "audio command failed";
}

void reply_audio_command(const char *command_id, bool ok, const char *error_code,
                         const char *reason, const char *stream_id,
                         const AudioReplyTarget *target) {
#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return;
  cJSON_AddBoolToObject(root, "ok", ok);
  cJSON_AddStringToObject(root, "command_id", command_id != nullptr ? command_id : "");
  cJSON_AddStringToObject(root, "state", ok ? "completed" : "rejected");
  cJSON *result = cJSON_AddObjectToObject(root, "result");
  cJSON_AddStringToObject(result, "stream_id", stream_id != nullptr ? stream_id : "");
  cJSON_AddStringToObject(result, "reason", reason != nullptr ? reason : "");
  if (ok) cJSON_AddNullToObject(root, "error");
  else {
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(error, "code", error_code != nullptr ? error_code : "E_INVALID_ARG");
    cJSON_AddStringToObject(error, "message", audio_error_message(error_code));
  }
  char *text = cJSON_PrintUnformatted(root);
  if (text != nullptr) {
    const uint8_t type = ok ? MIBOT_MSG_ACK : MIBOT_MSG_NACK;
    const uint8_t flags = MIBOT_FLAG_RESPONSE | (ok ? 0 : MIBOT_FLAG_ERROR);
    send_audio_frame(type, flags, reinterpret_cast<const uint8_t *>(text),
                     static_cast<uint16_t>(strlen(text)), target);
    cJSON_free(text);
  }
  cJSON_Delete(root);
#else
  (void)command_id; (void)ok; (void)error_code; (void)reason; (void)stream_id; (void)target;
#endif
}

AudioRuntime::CommandSlot *cache_find(const char *command_id) {
  if (command_id == nullptr || command_id[0] == '\0') return nullptr;
  for (auto &slot : g_audio.command_cache) {
    if (slot.state != AudioRuntime::CACHE_FREE && strcmp(slot.command_id, command_id) == 0) return &slot;
  }
  return nullptr;
}

AudioRuntime::CommandSlot *cache_begin(const char *command_id) {
  if (command_id == nullptr || command_id[0] == '\0') return nullptr;
  if (auto *existing = cache_find(command_id)) return existing;
  AudioRuntime::CommandSlot *selected = nullptr;
  int64_t oldest = INT64_MAX;
  for (auto &slot : g_audio.command_cache) {
    if (slot.state == AudioRuntime::CACHE_FREE) {
      selected = &slot;
      break;
    }
    if (slot.state == AudioRuntime::CACHE_DONE && slot.updated_ms < oldest) {
      selected = &slot;
      oldest = slot.updated_ms;
    }
  }
  if (selected == nullptr) return nullptr;
  *selected = AudioRuntime::CommandSlot{};
  selected->state = AudioRuntime::CACHE_IN_PROGRESS;
  strncpy(selected->command_id, command_id, sizeof(selected->command_id) - 1);
  selected->updated_ms = clock_ms();
  return selected;
}

void cache_done(const char *command_id, bool ok, const char *error_code,
                const char *reason, const char *stream_id) {
  auto *slot = cache_find(command_id);
  if (slot == nullptr) return;
  slot->state = AudioRuntime::CACHE_DONE;
  slot->updated_ms = clock_ms();
  slot->ok = ok;
  strncpy(slot->error_code, error_code != nullptr ? error_code : "", sizeof(slot->error_code) - 1);
  strncpy(slot->reason, reason != nullptr ? reason : "", sizeof(slot->reason) - 1);
  strncpy(slot->stream_id, stream_id != nullptr ? stream_id : "", sizeof(slot->stream_id) - 1);
}

bool cache_replay_or_reject(const char *command_id, const AudioReplyTarget *target) {
  auto *slot = cache_find(command_id);
  if (slot == nullptr) return false;
  if (slot->state == AudioRuntime::CACHE_IN_PROGRESS) {
    reply_audio_command(command_id, false, "E_DUPLICATE", "", "", target);
    return true;
  }
  if (slot->state == AudioRuntime::CACHE_DONE) {
    reply_audio_command(command_id, slot->ok, slot->ok ? nullptr : slot->error_code,
                        slot->reason, slot->stream_id, target);
    return true;
  }
  return false;
}

void direction_enter(audio_direction_t direction) {
  lock_audio();
  if (g_audio.direction != direction) g_audio.direction = direction;
  unlock_audio();
}

void downlink_abort_internal(const char *reason) {
  lock_audio();
  if (g_audio.stream.active) {
    strncpy(g_audio.stream.abort_reason, reason != nullptr ? reason : "abort",
            sizeof(g_audio.stream.abort_reason) - 1);
    g_audio.stream.aborted = true;
    g_audio.stream.cloud_finished = true;
    g_audio.stream.prefill_ready = true;
    g_audio.stream.partial_len = 0;
    ring_reset(&g_audio.downlink);
    g_audio.diag.downlink_aborted++;
  }
  unlock_audio();
}

void cloud_state_changed(cloud_state_t state, void *) {
  const bool was_degraded = g_audio.degraded;
  g_audio.degraded = state != CLOUD_STATE_CONNECTED;
  if (!was_degraded && g_audio.degraded) {
    g_audio.next_reconnect_ms = clock_ms() + g_audio.reconnect_ms;
    emit_event("cloud_disconnected", "warning");
    downlink_abort_internal("cloud_disconnected");
  } else if (was_degraded && !g_audio.degraded) {
    g_audio.reconnect_ms = MIBOT_AUDIO_RECONNECT_MIN_MS;
    g_audio.next_reconnect_ms = 0;
    emit_event("cloud_reconnected", "info");
  }
}

void cloud_data_received(cloud_frame_kind_t kind, const uint8_t *data, size_t length,
                         const cloud_request_ctx_t *ctx, void *) {
#if defined(ESP_PLATFORM)
  if (data == nullptr || length == 0 || g_audio.cloud_queue == nullptr) return;
  const size_t max_chunk = g_audio.uplink.psram ? MIBOT_AUDIO_CLOUD_CHUNK_MAX_PSRAM
                                                : MIBOT_AUDIO_CLOUD_CHUNK_MAX_SRAM;
  if (length > max_chunk) {
    g_audio.diag.downlink_dropped++;
    return;
  }
  const size_t allocation_size = sizeof(CloudChunk) + length - 1;
  CloudChunk *chunk = static_cast<CloudChunk *>(heap_caps_malloc(
      allocation_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (chunk == nullptr) {
    chunk = static_cast<CloudChunk *>(heap_caps_malloc(allocation_size,
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (chunk == nullptr) {
    g_audio.diag.downlink_dropped++;
    return;
  }
  chunk->kind = kind;
  chunk->ctx = ctx != nullptr ? *ctx : cloud_request_ctx_t{};
  chunk->length = length;
  memcpy(chunk->data, data, length);
  if (xQueueSend(g_audio.cloud_queue, &chunk, 0) != pdTRUE) {
    heap_caps_free(chunk);
    g_audio.diag.downlink_dropped++;
  }
#elif defined(MIBOT_HOST_TEST)
  if (data == nullptr || length == 0) return;
  const size_t max_chunk = g_audio.uplink.psram ? MIBOT_AUDIO_CLOUD_CHUNK_MAX_PSRAM
                                                : MIBOT_AUDIO_CLOUD_CHUNK_MAX_SRAM;
  if (length > max_chunk) {
    g_audio.diag.downlink_dropped++;
    return;
  }
  CloudChunk *chunk = static_cast<CloudChunk *>(heap_caps_malloc(sizeof(CloudChunk) + length - 1, 0));
  if (chunk == nullptr) {
    g_audio.diag.downlink_dropped++;
    return;
  }
  chunk->kind = kind;
  chunk->ctx = ctx != nullptr ? *ctx : cloud_request_ctx_t{};
  chunk->length = length;
  memcpy(chunk->data, data, length);
  process_cloud_chunk(chunk);
#else
  (void)kind; (void)data; (void)length; (void)ctx;
#endif
}

void downlink_flush_partial_locked() {
  if (g_audio.stream.partial_len == 0) return;
  memset(g_audio.stream.partial + g_audio.stream.partial_len, 0,
         MIBOT_AUDIO_FRAME_BYTES - g_audio.stream.partial_len);
  if (!ring_push_drop_newest(&g_audio.downlink, g_audio.stream.partial)) {
    g_audio.diag.downlink_dropped++;
  }
  g_audio.stream.partial_len = 0;
}

const char *stream_error_code(const char *reason) {
  if (reason != nullptr && strcmp(reason, "unsupported") == 0) return "E_UNSUPPORTED";
  if (reason != nullptr && (strcmp(reason, "cloud_timeout") == 0 ||
                            strcmp(reason, "cloud_disconnected") == 0)) {
    return "E_CLOUD_TIMEOUT";
  }
  return "E_BUSY";
}

#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
/*
 * Cloud text frames use a deliberately small ASR envelope.  Do not infer a
 * transcript from an arbitrary JSON object containing a "text" field: TTS
 * metadata, command replies and provider diagnostics use that field too.
 *
 * Accepted cloud input:
 *   {"type":"asr_result","text":"...","final":true}
 *
 * A few ASR gateways call the same boolean "is_final"; that alias is
 * accepted only when the explicit asr_result type is present.  The SF32
 * contract consumes only final results, so partial updates are intentionally
 * recognized and dropped here.
 *
 * The function returns true when the object is an explicit ASR envelope,
 * including malformed/partial envelopes.  That lets the caller keep ASR
 * packets out of the TTS metadata path.  It returns false for unrelated
 * cloud text.
 */
bool forward_cloud_asr_result(const cJSON *root) {
  if (root == nullptr || !cJSON_IsObject(root)) return false;

  const auto string_equals = [](const cJSON *item, const char *expected) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr || expected == nullptr) {
      return false;
    }
    const size_t expected_length = strlen(expected);
    return strlen(item->valuestring) == expected_length &&
           strcmp(item->valuestring, expected) == 0;
  };

  const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
  const cJSON *schema_item = cJSON_GetObjectItemCaseSensitive(root, "schema");
  const cJSON *event_item = cJSON_GetObjectItemCaseSensitive(root, "event");
  const bool explicit_cloud_asr = string_equals(type_item, "asr_result");
  const bool canonical_asr = string_equals(schema_item, MIBOT_SCHEMA_ASR_V1) &&
                             string_equals(event_item, MIBOT_ASR_EVENT_TEXT);
  if (!explicit_cloud_asr && !canonical_asr) return false;

  const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
  const char *transcript = cJSON_GetStringValue(text_item);
  const cJSON *final_item = cJSON_GetObjectItemCaseSensitive(root, "final");
  if (final_item == nullptr && explicit_cloud_asr) {
    final_item = cJSON_GetObjectItemCaseSensitive(root, "is_final");
  }
  if (!cJSON_IsBool(final_item) || !cJSON_IsTrue(final_item) ||
      !cJSON_IsString(text_item) || transcript == nullptr ||
      transcript[0] == '\0') {
    return true;
  }

  cJSON *canonical = cJSON_CreateObject();
  if (canonical == nullptr) return true;
  if (cJSON_AddStringToObject(canonical, "schema", MIBOT_SCHEMA_ASR_V1) == nullptr ||
      cJSON_AddStringToObject(canonical, "event", MIBOT_ASR_EVENT_TEXT) == nullptr ||
      cJSON_AddBoolToObject(canonical, "final", true) == nullptr ||
      cJSON_AddStringToObject(canonical, "text", transcript) == nullptr) {
    cJSON_Delete(canonical);
    return true;
  }

  char *serialized = cJSON_PrintUnformatted(canonical);
  if (serialized != nullptr) {
    const size_t length = strlen(serialized);
    if (length <= MIBOT_MAX_FRAME_PAYLOAD) {
      const AudioReplyTarget target{0, -1};
      (void)send_audio_frame(MIBOT_MSG_AI_RESPONSE, MIBOT_FLAG_RESPONSE,
                             reinterpret_cast<const uint8_t *>(serialized),
                             static_cast<uint16_t>(length), &target);
    }
    cJSON_free(serialized);
  }
  cJSON_Delete(canonical);
  return true;
}
#endif

void process_cloud_chunk(CloudChunk *chunk) {
  if (chunk == nullptr) return;
  if (chunk->kind == CLOUD_FRAME_TEXT) {
#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
    cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(chunk->data), chunk->length);
    if (root != nullptr) {
      if (forward_cloud_asr_result(root)) {
        cJSON_Delete(root);
        heap_caps_free(chunk);
        return;
      }
      const cJSON *codec = cJSON_GetObjectItemCaseSensitive(root, "codec");
      const cJSON *sample_rate = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
      const cJSON *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
      const bool unsupported = (cJSON_IsString(codec) && strcmp(codec->valuestring, "pcm_s16le") != 0) ||
                               (cJSON_IsNumber(sample_rate) && sample_rate->valueint != MIBOT_AUDIO_SAMPLE_RATE) ||
                               (cJSON_IsNumber(channels) && channels->valueint != MIBOT_AUDIO_CHANNELS);
      if (unsupported) {
        cJSON *data = cJSON_CreateObject();
        if (cJSON_IsString(codec)) cJSON_AddStringToObject(data, "codec", codec->valuestring);
        if (cJSON_IsNumber(sample_rate)) cJSON_AddNumberToObject(data, "sample_rate", sample_rate->valuedouble);
        if (cJSON_IsNumber(channels)) cJSON_AddNumberToObject(data, "channels", channels->valuedouble);
        emit_event("audio_codec_unsupported", "error", data);
        downlink_abort_internal("unsupported");
      }
      cJSON *eos = cJSON_GetObjectItemCaseSensitive(root, "eos");
      if (!unsupported && cJSON_IsTrue(eos)) {
        lock_audio();
        const bool context_matches = chunk->ctx.command_id[0] == '\0' ||
                                     g_audio.stream.command_id[0] == '\0' ||
                                     strcmp(chunk->ctx.command_id, g_audio.stream.command_id) == 0;
        if (context_matches) {
          downlink_flush_partial_locked();
          g_audio.stream.cloud_finished = true;
          g_audio.stream.prefill_ready = true;
        }
        unlock_audio();
      }
      cJSON_Delete(root);
    }
#endif
    heap_caps_free(chunk);
    return;
  }
  lock_audio();
  const bool context_matches = chunk->ctx.command_id[0] == '\0' ||
                               g_audio.stream.command_id[0] == '\0' ||
                               strcmp(chunk->ctx.command_id, g_audio.stream.command_id) == 0;
  if (!g_audio.stream.active || g_audio.stream.aborted || !context_matches) {
    if (!context_matches) g_audio.diag.downlink_dropped++;
    unlock_audio();
    heap_caps_free(chunk);
    return;
  }
  if (g_audio.stream.first_chunk_ms == 0) g_audio.stream.first_chunk_ms = clock_ms();
  size_t offset = 0;
  while (offset < chunk->length) {
    const size_t copy = (MIBOT_AUDIO_FRAME_BYTES - g_audio.stream.partial_len) < (chunk->length - offset)
                            ? MIBOT_AUDIO_FRAME_BYTES - g_audio.stream.partial_len
                            : chunk->length - offset;
    memcpy(g_audio.stream.partial + g_audio.stream.partial_len, chunk->data + offset, copy);
    g_audio.stream.partial_len += copy;
    offset += copy;
    if (g_audio.stream.partial_len == MIBOT_AUDIO_FRAME_BYTES) {
      if (!ring_push_drop_newest(&g_audio.downlink, g_audio.stream.partial)) g_audio.diag.downlink_dropped++;
      g_audio.stream.partial_len = 0;
    }
  }
  unlock_audio();
  heap_caps_free(chunk);
}

void audio_up_task(void *) {
#if defined(ESP_PLATFORM)
  /* These two buffers are 3840 bytes together.  Keeping them on the task stack
   * overflowed it as soon as the first uplink chunk was assembled and pushed
   * through the websocket send path ("stack overflow in task mibot_audio_up",
   * RTC_SW_CPU_RST), which rebooted the ESP32 on every utterance and looked
   * like a flaky cloud link.  Static storage keeps the task stack for the call
   * chain only. */
  static uint8_t frame[MIBOT_AUDIO_FRAME_BYTES];
  static uint8_t aggregate[MIBOT_AUDIO_UPLINK_CHUNK_BYTES];
  size_t aggregate_frames = 0;
  auto flush_aggregate = [&]() {
    if (aggregate_frames == 0) return;
    const size_t length = aggregate_frames * MIBOT_AUDIO_FRAME_BYTES;
    audio_mode_t mode;
    int capture_fd;
    lock_audio();
    mode = g_audio.mode;
    capture_fd = g_audio.capture_fd;
    unlock_audio();
    if (mode == AUDIO_MODE_LOOPBACK) {
      for (size_t offset = 0; offset < length; offset += MIBOT_AUDIO_FRAME_BYTES) {
        if (!ring_push_drop_newest(&g_audio.downlink, aggregate + offset)) g_audio.diag.downlink_dropped++;
      }
      lock_audio();
      if (!g_audio.stream.active || g_audio.stream.aborted || g_audio.stream.eos_sent) {
        g_audio.stream = DownlinkStream{};
        g_audio.stream.active = true;
        g_audio.stream.target = g_audio.up_target;
        audio_stream_id_generate(g_audio.stream.stream_id, sizeof(g_audio.stream.stream_id));
      }
      g_audio.stream.active = true;
      g_audio.stream.cloud_finished = g_audio.up_flush_requested;
      g_audio.stream.target = g_audio.up_target;
      if (g_audio.stream.stream_id[0] == '\0') {
        audio_stream_id_generate(g_audio.stream.stream_id, sizeof(g_audio.stream.stream_id));
      }
      unlock_audio();
      direction_enter(AUDIO_DIRECTION_DOWNLINK);
    } else if (mode == AUDIO_MODE_CAPTURE) {
      if (capture_fd < 0) {
        g_audio.diag.uplink_dropped += static_cast<uint32_t>(aggregate_frames);
      } else {
        size_t sent = 0;
        while (sent < length) {
          const int written = send(capture_fd, aggregate + sent, length - sent, 0);
          if (written <= 0) { g_audio.diag.uplink_dropped += static_cast<uint32_t>(aggregate_frames); break; }
          sent += static_cast<size_t>(written);
        }
      }
    } else if (mode == AUDIO_MODE_CLOUD && g_audio.cloud != nullptr && g_audio.cloud->vtable != nullptr) {
      const cloud_request_ctx_t ctx{};
      if (g_audio.cloud->vtable->send(g_audio.cloud, CLOUD_FRAME_BINARY, aggregate, length,
                                      &ctx, MIBOT_AUDIO_CLOUD_SEND_TIMEOUT_MS) != ESP_OK) {
        g_audio.diag.uplink_send_error++;
      }
    } else if (mode == AUDIO_MODE_CLOUD) {
      g_audio.diag.uplink_send_error++;
    }
    aggregate_frames = 0;
  };
  while (true) {
    if (!ring_pop(&g_audio.uplink, frame, 20)) {
      lock_audio();
      const bool flush_requested = g_audio.up_flush_requested;
      const bool silent = g_audio.up_meta_seen && g_audio.last_up_ms != 0 &&
                          clock_ms() - g_audio.last_up_ms >= MIBOT_AUDIO_UP_SILENCE_MS;
      unlock_audio();
      if (flush_requested || silent) {
        flush_aggregate();
        lock_audio();
        const bool send_end = g_audio.up_flush_requested || silent;
        const audio_mode_t mode = g_audio.mode;
        g_audio.up_flush_requested = false;
        unlock_audio();
        if (send_end) {
          if (mode == AUDIO_MODE_LOOPBACK) {
            lock_audio();
            g_audio.stream.cloud_finished = true;
            g_audio.stream.prefill_ready = true;
            unlock_audio();
          }
          if (mode == AUDIO_MODE_CLOUD && g_audio.cloud != nullptr && g_audio.cloud->vtable != nullptr) {
            const cloud_request_ctx_t ctx{};
            /* Send exactly the JSON text: the previous length included the
             * NUL terminator, so peers received `{"eos":true}\0` and failed to
             * parse the uplink end-of-stream marker. */
            static constexpr char kUplinkEos[] = "{\"eos\":true}";
            g_audio.cloud->vtable->send(g_audio.cloud, CLOUD_FRAME_TEXT,
                reinterpret_cast<const uint8_t *>(kUplinkEos),
                sizeof(kUplinkEos) - 1, &ctx,
                MIBOT_AUDIO_CLOUD_SEND_TIMEOUT_MS);
          }
          direction_enter(AUDIO_DIRECTION_IDLE);
          lock_audio();
          g_audio.up_meta_seen = false;
          g_audio.have_last_seq = false;
          unlock_audio();
        }
      }
      continue;
    }
    memcpy(aggregate + aggregate_frames * MIBOT_AUDIO_FRAME_BYTES, frame, MIBOT_AUDIO_FRAME_BYTES);
    ++aggregate_frames;
    audio_mode_t mode;
    lock_audio();
    mode = g_audio.mode;
    unlock_audio();
    if (mode != AUDIO_MODE_CLOUD || aggregate_frames == MIBOT_AUDIO_UPLINK_CHUNK_FRAMES) flush_aggregate();
    g_audio.last_up_ms = clock_ms();
    // Capture can receive a burst of ready frames. Yield briefly so the
    // lower-priority UART/reporting and Wi-Fi tasks keep their heartbeats.
    vTaskDelay(pdMS_TO_TICKS(1));
  }
#else
  vTaskDelete(nullptr);
#endif
}

void audio_tx_step_internal() {
  uint8_t frame[MIBOT_AUDIO_FRAME_BYTES];
  char meta_text[256];
  AudioReplyTarget target{0, -1};
  switch (tx_plan_next()) {
    case AUDIO_TX_IDLE:
      break;
    case AUDIO_TX_SEND_META: {
      lock_audio();
      if (!g_audio.stream.active || g_audio.stream.meta_sent) {
        unlock_audio();
        break;
      }
      target = g_audio.stream.target;
      AudioMeta meta{};
      strncpy(meta.schema, "mibot.audio.v1", sizeof(meta.schema) - 1);
      strncpy(meta.stream_id, g_audio.stream.stream_id, sizeof(meta.stream_id) - 1);
      strncpy(meta.codec, "pcm_s16le", sizeof(meta.codec) - 1);
      meta.sample_rate = MIBOT_AUDIO_SAMPLE_RATE;
      meta.channels = MIBOT_AUDIO_CHANNELS;
      meta.frame_ms = MIBOT_AUDIO_FRAME_MS;
      meta.bytes = MIBOT_AUDIO_FRAME_BYTES;
      meta.eos = 0;
      if (audio_meta_serialize(&meta, meta_text, sizeof(meta_text)) >= 0) {
        g_audio.stream.meta_sent = true;
        unlock_audio();
        send_audio_frame(MIBOT_MSG_AUDIO_DOWN, 0, reinterpret_cast<const uint8_t *>(meta_text),
                         static_cast<uint16_t>(strlen(meta_text)), &target);
      } else {
        unlock_audio();
      }
      break;
    }
    case AUDIO_TX_PREFILL_WAIT: {
      lock_audio();
      if (!g_audio.stream.active) {
        unlock_audio();
        break;
      }
      if (!g_audio.stream.prefill_ready) {
        const uint16_t prefill_depth =
            g_audio.mode == AUDIO_MODE_LOOPBACK ? MIBOT_AUDIO_LOOPBACK_PREFILL_DEPTH
                                                 : MIBOT_AUDIO_PREFILL_DEPTH;
        if (ring_count(&g_audio.downlink) >= prefill_depth || g_audio.stream.cloud_finished) {
          g_audio.stream.prefill_ready = true;
        }
        unlock_audio();
        break;
      }
      target = g_audio.stream.target;
      memset(frame, 0, sizeof(frame));
      unlock_audio();
      if (send_audio_frame(MIBOT_MSG_AUDIO_DOWN, 0, frame, MIBOT_AUDIO_FRAME_BYTES, &target) == ESP_OK) {
        g_audio.diag.audio_down_frames_sent++;
        g_audio.diag.downlink_underrun++;
        g_audio.diag.audio_down_bytes_1s += MIBOT_AUDIO_FRAME_BYTES;
      } else {
        g_audio.diag.audio_down_write_error++;
      }
      const int64_t sent_at = clock_ms();
      if (g_audio.tx_last_down_send_ms != 0) {
        audio_link_record_downlink_interval(static_cast<uint32_t>(sent_at - g_audio.tx_last_down_send_ms));
      }
      g_audio.tx_last_down_send_ms = sent_at;
      break;
    }
    case AUDIO_TX_SEND_PCM: {
      lock_audio();
      if (!g_audio.stream.active || !g_audio.stream.prefill_ready ||
          !ring_pop_nowait(&g_audio.downlink, frame)) {
        unlock_audio();
        break;
      }
      target = g_audio.stream.target;
      unlock_audio();
      if (send_audio_frame(MIBOT_MSG_AUDIO_DOWN, 0, frame, MIBOT_AUDIO_FRAME_BYTES, &target) == ESP_OK) {
        g_audio.diag.audio_down_frames_sent++;
        g_audio.diag.audio_down_bytes_1s += MIBOT_AUDIO_FRAME_BYTES;
      } else {
        g_audio.diag.audio_down_write_error++;
      }
      const int64_t sent_at = clock_ms();
      if (g_audio.tx_last_down_send_ms != 0) {
        audio_link_record_downlink_interval(static_cast<uint32_t>(sent_at - g_audio.tx_last_down_send_ms));
      }
      g_audio.tx_last_down_send_ms = sent_at;
      break;
    }
    case AUDIO_TX_SEND_EOS: {
      lock_audio();
      if (!g_audio.stream.active || !g_audio.stream.prefill_ready || !g_audio.stream.cloud_finished) {
        unlock_audio();
        break;
      }
      downlink_flush_partial_locked();
      if (ring_count(&g_audio.downlink) > 0) {
        unlock_audio();
        break;
      }
      target = g_audio.stream.target;
      g_audio.stream.eos_sent = true;
      g_audio.stream.active = false;
      const bool aborted = g_audio.stream.aborted;
      char command_id[MIBOT_COMMAND_ID_MAX] = {};
      char stream_id[MIBOT_AUDIO_STREAM_ID_MAX + 1] = {};
      char reason[32] = {};
      strncpy(command_id, g_audio.stream.command_id, sizeof(command_id) - 1);
      strncpy(stream_id, g_audio.stream.stream_id, sizeof(stream_id) - 1);
      strncpy(reason, g_audio.stream.abort_reason, sizeof(reason) - 1);
      unlock_audio();
      AudioMeta meta{};
      strncpy(meta.schema, "mibot.audio.v1", sizeof(meta.schema) - 1);
      strncpy(meta.stream_id, stream_id, sizeof(meta.stream_id) - 1);
      strncpy(meta.codec, "pcm_s16le", sizeof(meta.codec) - 1);
      meta.sample_rate = MIBOT_AUDIO_SAMPLE_RATE;
      meta.channels = MIBOT_AUDIO_CHANNELS;
      meta.frame_ms = MIBOT_AUDIO_FRAME_MS;
      meta.bytes = MIBOT_AUDIO_FRAME_BYTES;
      meta.eos = 1;
      if (audio_meta_serialize(&meta, meta_text, sizeof(meta_text)) >= 0)
        send_audio_frame(MIBOT_MSG_AUDIO_DOWN, 0, reinterpret_cast<const uint8_t *>(meta_text),
                         static_cast<uint16_t>(strlen(meta_text)), &target);
      direction_enter(AUDIO_DIRECTION_IDLE);
#if defined(ESP_PLATFORM)
      ESP_LOGI(TAG, "AUDIO_DOWN eos frames_sent=%lu write_error=%lu "
                    "underrun=%lu uart_tx_bytes=%lu",
               static_cast<unsigned long>(g_audio.diag.audio_down_frames_sent),
               static_cast<unsigned long>(g_audio.diag.audio_down_write_error),
               static_cast<unsigned long>(g_audio.diag.downlink_underrun),
               static_cast<unsigned long>(mibot_uart_tx_bytes()));
#endif
      if (command_id[0] != '\0') {
        const char *error = aborted ? stream_error_code(reason) : nullptr;
        cache_done(command_id, !aborted, error, aborted ? reason : "", stream_id);
        reply_audio_command(command_id, !aborted, error, aborted ? reason : "", stream_id, &target);
      }
      break;
    }
  }
  const int64_t now = clock_ms();
  if (g_audio.tx_last_snapshot_ms < 0) {
    g_audio.tx_last_snapshot_ms = now;
  } else if (now - g_audio.tx_last_snapshot_ms >= 1000) {
    g_audio.diag.audio_down_bytes_1s_snapshot = g_audio.diag.audio_down_bytes_1s;
    g_audio.diag.audio_down_bytes_1s = 0;
    g_audio.tx_last_snapshot_ms = now;
  }
}

void audio_tx_task(void *) {
#if defined(ESP_PLATFORM)
  g_audio.tx_last_snapshot_ms = clock_ms();
  g_audio.tx_last_down_send_ms = 0;

  /* One frame per frame period, on a fixed phase.
   *
   * A wall-clock "catch up on frames we owe" variant was tried and made things
   * strictly worse (SF32 underrun 52 -> 226).  Two reasons, both inherent to
   * audio_tx_step_internal() being a state-machine step rather than a pure
   * "send one frame":
   *   - When the downlink ring is momentarily empty the step sends nothing, but
   *     the scheduler still consumed that slot, so the frame was never owed to
   *     anyone and silently vanished.
   *   - Calling the step off-schedule to keep the meta/eos phases responsive
   *     also dispatches PCM, so it drained the ring at the poll rate instead of
   *     the frame rate.
   * The real cause of the original underruns was upstream: the gateway pushed
   * TTS at ~6x real time and the 25-frame downlink ring dropped the surplus.
   * With the gateway paced to real time this fixed phase is the right schedule,
   * and it is the one that has actually been exercised. */
  TickType_t next_wakeup = xTaskGetTickCount();
  while (true) {
    audio_tx_step_internal();
    xTaskDelayUntil(&next_wakeup, pdMS_TO_TICKS(MIBOT_AUDIO_FRAME_MS));
  }
#else
  vTaskDelete(nullptr);
#endif
}

void audio_dn_task(void *) {
#if defined(ESP_PLATFORM)
  while (true) {
    CloudChunk *chunk = nullptr;
    if (xQueueReceive(g_audio.cloud_queue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) process_cloud_chunk(chunk);
    if (g_audio.stream.active && g_audio.stream.request_sent_ms != 0 && g_audio.stream.first_chunk_ms == 0 &&
        clock_ms() - g_audio.stream.request_sent_ms > MIBOT_AUDIO_TTS_FIRST_CHUNK_TIMEOUT_MS) {
      downlink_abort_internal("cloud_timeout");
    }
    if (g_audio.cloud != nullptr && g_audio.cloud->vtable != nullptr) {
      const cloud_state_t state = g_audio.cloud->vtable->state(g_audio.cloud);
      if (state != CLOUD_STATE_CONNECTED && state != CLOUD_STATE_CONNECTING) {
        g_audio.degraded = true;
        const int64_t now = clock_ms();
        if (g_audio.next_reconnect_ms == 0) g_audio.next_reconnect_ms = now + g_audio.reconnect_ms;
        if (now >= g_audio.next_reconnect_ms) {
          const esp_err_t result = g_audio.cloud->vtable->connect(g_audio.cloud);
          if (result != ESP_OK) g_audio.reconnect_ms = audio_next_backoff_ms(g_audio.reconnect_ms);
          g_audio.next_reconnect_ms = now + g_audio.reconnect_ms;
        }
      }
    }
  }
#else
  vTaskDelete(nullptr);
#endif
}

void replace_active_stream() {
  DownlinkStream old{};
  lock_audio();
  if (!g_audio.stream.active) {
    unlock_audio();
    return;
  }
  old = g_audio.stream;
  g_audio.stream.active = false;
  g_audio.stream.aborted = true;
  ring_reset(&g_audio.downlink);
  g_audio.diag.downlink_aborted++;
  unlock_audio();

  AudioMeta meta{};
  strncpy(meta.schema, "mibot.audio.v1", sizeof(meta.schema) - 1);
  strncpy(meta.stream_id, old.stream_id, sizeof(meta.stream_id) - 1);
  strncpy(meta.codec, "pcm_s16le", sizeof(meta.codec) - 1);
  meta.sample_rate = MIBOT_AUDIO_SAMPLE_RATE;
  meta.channels = MIBOT_AUDIO_CHANNELS;
  meta.frame_ms = MIBOT_AUDIO_FRAME_MS;
  meta.bytes = MIBOT_AUDIO_FRAME_BYTES;
  meta.eos = 1;
  char text[256] = {};
  if (audio_meta_serialize(&meta, text, sizeof(text)) >= 0) {
    send_audio_frame(MIBOT_MSG_AUDIO_DOWN, 0, reinterpret_cast<const uint8_t *>(text),
                     static_cast<uint16_t>(strlen(text)), &old.target);
  }
  direction_enter(AUDIO_DIRECTION_IDLE);
  if (old.command_id[0] != '\0') {
    cache_done(old.command_id, false, "E_BUSY", "mode_switch", old.stream_id);
    reply_audio_command(old.command_id, false, "E_BUSY", "mode_switch", old.stream_id, &old.target);
  }
}

void start_stream(const char *command_id, const char *trace_id, bool interruptible,
                  const char *voice, const AudioReplyTarget *target) {
  replace_active_stream();
  lock_audio();
  ring_reset(&g_audio.downlink);
  g_audio.stream = DownlinkStream{};
  g_audio.stream.active = true;
  g_audio.stream.interruptible = interruptible;
  g_audio.stream.target = target != nullptr ? *target : AudioReplyTarget{0, -1};
  strncpy(g_audio.stream.command_id, command_id != nullptr ? command_id : "", sizeof(g_audio.stream.command_id) - 1);
  strncpy(g_audio.stream.trace_id, trace_id != nullptr ? trace_id : "", sizeof(g_audio.stream.trace_id) - 1);
  strncpy(g_audio.stream.voice, voice != nullptr ? voice : "default", sizeof(g_audio.stream.voice) - 1);
  audio_stream_id_generate(g_audio.stream.stream_id, sizeof(g_audio.stream.stream_id));
  g_audio.stream.request_sent_ms = clock_ms();
  unlock_audio();
  direction_enter(AUDIO_DIRECTION_DOWNLINK);
}

void generate_test_tone_internal(uint32_t duration_ms, uint32_t freq_hz,
                                 const char *command_id, const AudioReplyTarget *target) {
  start_stream(command_id, "", true, "test", target);
  const size_t frames = (duration_ms + MIBOT_AUDIO_FRAME_MS - 1) / MIBOT_AUDIO_FRAME_MS;
  const double amplitude = 32767.0 * 0.5011872336;
  for (size_t frame = 0; frame < frames; ++frame) {
    int16_t pcm[MIBOT_AUDIO_FRAME_SAMPLES];
    for (size_t sample = 0; sample < MIBOT_AUDIO_FRAME_SAMPLES; ++sample) {
      const double t = static_cast<double>(frame * MIBOT_AUDIO_FRAME_SAMPLES + sample) /
                       MIBOT_AUDIO_SAMPLE_RATE;
      pcm[sample] = static_cast<int16_t>(sin(2.0 * M_PI * freq_hz * t) * amplitude);
    }
    while (!ring_push_drop_newest(&g_audio.downlink, reinterpret_cast<const uint8_t *>(pcm))) {
      vTaskDelay(pdMS_TO_TICKS(MIBOT_AUDIO_FRAME_MS));
    }
  }
  lock_audio();
  g_audio.stream.cloud_finished = true;
  unlock_audio();
}
}  // namespace

extern "C" int audio_link_init(void) {
  if (g_audio.initialized) return ESP_OK;
  g_audio.mutex = xSemaphoreCreateMutex();
  if (g_audio.mutex == nullptr) return ESP_ERR_NO_MEM;
#if defined(ESP_PLATFORM) && defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
  multi_heap_info_t internal_before{}, psram_before{};
  heap_caps_get_info(&internal_before, MALLOC_CAP_INTERNAL);
  heap_caps_get_info(&psram_before, MALLOC_CAP_SPIRAM);
#endif
  bool psram = ring_init(&g_audio.uplink, MIBOT_AUDIO_UP_DEPTH_PSRAM, 1) &&
               ring_init(&g_audio.downlink, MIBOT_AUDIO_DN_DEPTH_PSRAM, 1);
  if (!psram) {
    ring_free(&g_audio.uplink);
    ring_free(&g_audio.downlink);
    if (!ring_init(&g_audio.uplink, MIBOT_AUDIO_UP_DEPTH_SRAM, 0) ||
        !ring_init(&g_audio.downlink, MIBOT_AUDIO_DN_DEPTH_SRAM, 0)) {
      ring_free(&g_audio.uplink);
      ring_free(&g_audio.downlink);
      vSemaphoreDelete(g_audio.mutex);
      g_audio.mutex = nullptr;
      emit_event("audio_no_mem", "error");
      return ESP_ERR_NO_MEM;
    }
    emit_event("audio_buffer_degraded", "warning");
  }
#if defined(ESP_PLATFORM)
  g_audio.cloud_queue = xQueueCreate(MIBOT_AUDIO_CLOUD_CHUNK_QUEUE_DEPTH, sizeof(CloudChunk *));
  if (g_audio.cloud_queue == nullptr) {
    ring_free(&g_audio.uplink);
    ring_free(&g_audio.downlink);
    vSemaphoreDelete(g_audio.mutex);
    g_audio.mutex = nullptr;
    emit_event("audio_no_mem", "error");
    return ESP_ERR_NO_MEM;
  }
  const cloud_callbacks_t callbacks{cloud_data_received, cloud_state_changed, nullptr};
  const cloud_config_t config{MIBOT_CLOUD_WS_URI, MIBOT_CLOUD_API_KEY, 4096, 5000};
  g_audio.cloud = cloud_adapter_ws_create(&config, &callbacks);
  if (g_audio.cloud != nullptr) g_audio.cloud->vtable->connect(g_audio.cloud);
  else ESP_LOGI(TAG, "cloud endpoint not configured; running without cloud adapter");
  /* 8 KB: the websocket/TLS send path below this task needs several KB of
   * headroom even with the audio buffers moved to static storage. */
  xTaskCreatePinnedToCore(audio_up_task, "mibot_audio_up", 8192, nullptr, 11, &g_audio_up_task_handle, 0);
  /* 6 KB: audio_tx_step_internal() holds a 640-byte frame plus a 256-byte meta
   * buffer and then calls into the AA55/UART send path. */
  xTaskCreatePinnedToCore(audio_tx_task, "mibot_audio_tx", 6144, nullptr, 11, &g_audio_tx_task_handle, 1);
  xTaskCreatePinnedToCore(audio_dn_task, "mibot_audio_dn", 6144, nullptr, 11, &g_audio_dn_task_handle, 0);
#if defined(ESP_PLATFORM) && defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
  multi_heap_info_t internal_after{}, psram_after{};
  heap_caps_get_info(&internal_after, MALLOC_CAP_INTERNAL);
  heap_caps_get_info(&psram_after, MALLOC_CAP_SPIRAM);
  ESP_LOGI(TAG, "audio buffers: internal free %u -> %u, PSRAM free %u -> %u, heap free=%u",
           static_cast<unsigned>(internal_before.total_free_bytes), static_cast<unsigned>(internal_after.total_free_bytes),
           static_cast<unsigned>(psram_before.total_free_bytes), static_cast<unsigned>(psram_after.total_free_bytes),
           static_cast<unsigned>(esp_get_free_heap_size()));
#endif
#endif
  g_audio.initialized = true;
  return ESP_OK;
}

extern "C" void audio_link_set_frame_sender(audio_send_frame_fn sender, void *user) {
  g_audio.sender = sender;
  g_audio.sender_user = user;
}

extern "C" int audio_link_handle_command(const uint8_t *payload, uint16_t length,
                                           const AudioReplyTarget *target) {
#if !defined(ESP_PLATFORM) && !defined(MIBOT_HOST_TEST)
  (void)payload; (void)length; (void)target;
  return 0;
#else
  cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(payload), length);
  if (root == nullptr || !cJSON_IsObject(root)) { cJSON_Delete(root); return 0; }
  const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "name"));
  const char *command_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "command_id"));
  const char *trace_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "trace_id"));
  cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
  if (name == nullptr || !cJSON_IsObject(args)) { cJSON_Delete(root); return 0; }
  const bool cached_command = strcmp(name, "robot.speak") == 0 ||
                              strcmp(name, "robot.stop_audio") == 0 ||
                              strcmp(name, "robot.play_test_tone") == 0;
  if (cached_command && command_id != nullptr && command_id[0] != '\0') {
    if (cache_replay_or_reject(command_id, target)) { cJSON_Delete(root); return 1; }
    if (cache_begin(command_id) == nullptr) {
      reply_audio_command(command_id, false, "E_BUSY", "", "", target);
      cJSON_Delete(root);
      return 1;
    }
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    if (cJSON_IsNumber(expires) && expires->valuedouble < static_cast<double>(clock_ms())) {
      cache_done(command_id, false, "E_EXPIRED", "", "");
      reply_audio_command(command_id, false, "E_EXPIRED", "", "", target);
      cJSON_Delete(root);
      return 1;
    }
  }
  if (strcmp(name, "robot.get_audio_stats") == 0) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON_AddStringToObject(reply, "command_id", command_id != nullptr ? command_id : "");
    cJSON_AddStringToObject(reply, "state", "completed");
    cJSON *result = cJSON_AddObjectToObject(reply, "result");
    cJSON *audio = cJSON_AddObjectToObject(result, "audio");
    const cloud_state_t cloud_state = current_cloud_state();
    lock_audio();
    add_audio_diag(audio, true, cloud_state);
    unlock_audio();
    cJSON_AddNullToObject(reply, "error");
    char *text = cJSON_PrintUnformatted(reply);
    if (text != nullptr) { send_audio_frame(MIBOT_MSG_ACK, MIBOT_FLAG_RESPONSE, reinterpret_cast<const uint8_t *>(text), static_cast<uint16_t>(strlen(text)), target); cJSON_free(text); }
    cJSON_Delete(reply); cJSON_Delete(root); return 1;
  }
  if (strcmp(name, "robot.reset_audio_stats") == 0) {
    audio_diag_reset();
    reply_audio_command(command_id, true, nullptr, "", "", target);
    cJSON_Delete(root); return 1;
  }
  if (strcmp(name, "robot.set_audio_mode") == 0) {
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "mode"));
    audio_mode_t next = AUDIO_MODE_CLOUD;
    if (mode == nullptr) {
      reply_audio_command(command_id, false, "E_INVALID_ARG", "", "", target);
      cJSON_Delete(root);
      return 1;
    }
    else if (strcmp(mode, "cloud") == 0) next = AUDIO_MODE_CLOUD;
    else if (strcmp(mode, "loopback") == 0) next = AUDIO_MODE_LOOPBACK;
    else if (strcmp(mode, "capture") == 0) next = AUDIO_MODE_CAPTURE;
    else { reply_audio_command(command_id, false, "E_INVALID_ARG", "", "", target); cJSON_Delete(root); return 1; }
    // Complete any prior stream before changing routing mode.  Merely
    // clearing the rings leaves stale stream flags (aborted/cloud_finished)
    // that can make the next loopback stream emit premature silence/EOS.
    replace_active_stream();
    lock_audio();
    g_audio.mode = next;
    ring_reset(&g_audio.uplink);
    ring_reset(&g_audio.downlink);
    g_audio.stream = DownlinkStream{};
    g_audio.up_meta_seen = false;
    g_audio.up_blocked = false;
    g_audio.up_flush_requested = false;
    unlock_audio();
    reply_audio_command(command_id, true, nullptr, "", "", target); cJSON_Delete(root); return 1;
  }
  if (strcmp(name, "robot.stop_audio") == 0) {
    char stopped_stream_id[MIBOT_AUDIO_STREAM_ID_MAX + 1] = {};
    lock_audio();
    if (g_audio.stream.active) {
      strncpy(stopped_stream_id, g_audio.stream.stream_id, sizeof(stopped_stream_id) - 1);
    }
    unlock_audio();
    downlink_abort_internal("stop_audio");
    cache_done(command_id, true, nullptr, "stop_audio", stopped_stream_id);
    reply_audio_command(command_id, true, nullptr, "stop_audio", stopped_stream_id, target);
    cJSON_Delete(root); return 1;
  }
  if (strcmp(name, "robot.play_test_tone") == 0) {
    const int duration = cJSON_GetObjectItemCaseSensitive(args, "duration_ms") != nullptr ?
        cJSON_GetObjectItemCaseSensitive(args, "duration_ms")->valueint : 1000;
    const int freq = cJSON_GetObjectItemCaseSensitive(args, "freq_hz") != nullptr ?
        cJSON_GetObjectItemCaseSensitive(args, "freq_hz")->valueint : 440;
    if (duration < 20 || duration > 10000 || freq < 20 || freq > 8000) {
      cache_done(command_id, false, "E_INVALID_ARG", "", "");
      reply_audio_command(command_id, false, "E_INVALID_ARG", "", "", target);
    } else {
      generate_test_tone_internal(static_cast<uint32_t>(duration), static_cast<uint32_t>(freq), command_id, target);
    }
    cJSON_Delete(root); return 1;
  }
  if (strcmp(name, "robot.speak") == 0) {
    const char *text_value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "text"));
    const int text_len = text_value != nullptr ? utf8_codepoint_count(reinterpret_cast<const uint8_t *>(text_value), strlen(text_value)) : -1;
    if (text_len < 1 || text_len > MIBOT_AUDIO_SPEAK_TEXT_MAX_CP) {
      cache_done(command_id, false, "E_INVALID_ARG", "", "");
      reply_audio_command(command_id, false, "E_INVALID_ARG", "", "", target);
    }
    else if (g_audio.degraded || g_audio.cloud == nullptr || g_audio.cloud->vtable->state(g_audio.cloud) != CLOUD_STATE_CONNECTED) {
      cache_done(command_id, false, "E_CLOUD_TIMEOUT", "", "");
      reply_audio_command(command_id, false, "E_CLOUD_TIMEOUT", "", "", target);
    }
    else {
      const char *voice = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "voice"));
      const bool interruptible = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "interruptible"));
      start_stream(command_id, trace_id, interruptible, voice != nullptr ? voice : "default", target);
      cJSON *request_root = cJSON_CreateObject();
      if (request_root != nullptr) {
        cJSON_AddStringToObject(request_root, "type", "tts_request");
        cJSON_AddStringToObject(request_root, "text", text_value);
        cJSON_AddStringToObject(request_root, "voice", voice != nullptr ? voice : "default");
        cJSON_AddStringToObject(request_root, "stream_id", g_audio.stream.stream_id);
      }
      char *request_text = request_root != nullptr ? cJSON_PrintUnformatted(request_root) : nullptr;
      cloud_request_ctx_t ctx{};
      strncpy(ctx.trace_id, trace_id != nullptr ? trace_id : "", sizeof(ctx.trace_id) - 1);
      strncpy(ctx.command_id, command_id != nullptr ? command_id : "", sizeof(ctx.command_id) - 1);
      ctx.created_at_ms = clock_ms(); ctx.expires_at_ms = ctx.created_at_ms + MIBOT_AUDIO_TTS_FIRST_CHUNK_TIMEOUT_MS;
      const esp_err_t result = request_text != nullptr && strlen(request_text) <= MIBOT_MAX_FRAME_PAYLOAD
          ? g_audio.cloud->vtable->send(g_audio.cloud, CLOUD_FRAME_TEXT,
                                        reinterpret_cast<const uint8_t *>(request_text), strlen(request_text),
                                        &ctx, MIBOT_AUDIO_CLOUD_SEND_TIMEOUT_MS)
          : ESP_ERR_INVALID_ARG;
      if (request_text != nullptr) cJSON_free(request_text);
      if (request_root != nullptr) cJSON_Delete(request_root);
      if (result != ESP_OK) { downlink_abort_internal("cloud_timeout"); }
      else { /* The success reply is emitted by audio_tx_task after EOS. */ }
    }
    cJSON_Delete(root); return 1;
  }
  cJSON_Delete(root);
  return 0;
#endif
}

extern "C" void audio_link_fill_telemetry(cJSON *root) {
#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
  if (root == nullptr) return;
  const cloud_state_t cloud_state = current_cloud_state();
  lock_audio();
  cJSON *audio = cJSON_AddObjectToObject(root, "audio");
  const bool detailed =
#if defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
      true;
#else
      false;
#endif
  add_audio_diag(audio, detailed, cloud_state);
  unlock_audio();
#else
  (void)root;
#endif
}

extern "C" void audio_link_on_audio_up(uint8_t, const uint8_t *payload,
                                         uint16_t length, uint16_t seq,
                                         const AudioReplyTarget *target) {
  if (payload == nullptr) return;
  g_audio.diag.audio_up_frames_received++;
  g_audio.last_up_ms = clock_ms();
  lock_audio();
  const bool previous_stream_ended = g_audio.up_meta_seen && g_audio.up_flush_requested;
  unlock_audio();
  const bool looks_like_meta = audio_meta_looks_like_json(payload, length) != 0;
  if (previous_stream_ended && !looks_like_meta) {
    // An EOS metadata frame closes the current uplink stream. Do not let
    // late PCM from that stream leak into the next one.
    g_audio.diag.uplink_dropped++;
    return;
  }
  // Metadata may arrive both at stream start and as the EOS marker for an
  // active stream. Parse it before the PCM length check so EOS is not counted
  // as a malformed audio frame.
  if (looks_like_meta) {
    // Once a malformed metadata frame blocks a stream, binary PCM is silently
    // discarded until a new metadata candidate arrives. Do not turn every
    // subsequent PCM frame into another metadata error.
    AudioMeta meta{};
    if (audio_meta_parse(reinterpret_cast<const char *>(payload), length, &meta) == 0) {
      lock_audio();
      const bool downlink_active = g_audio.direction == AUDIO_DIRECTION_DOWNLINK &&
                                    g_audio.stream.active;
      strncpy(g_audio.up_stream_id, meta.stream_id, sizeof(g_audio.up_stream_id) - 1);
      g_audio.up_target = target != nullptr ? *target : AudioReplyTarget{0, -1};
      g_audio.up_meta_seen = true;
      g_audio.up_blocked = false;
      g_audio.up_flush_requested = meta.eos != 0;
      g_audio.have_last_seq = false;
      unlock_audio();
      // Keep an active downlink visible until the first PCM frame so the
      // interruptible/Barge-In gate can make its decision.
      if (!meta.eos && !downlink_active) direction_enter(AUDIO_DIRECTION_UPLINK);
    } else {
      /* A malformed metadata candidate used to set up_blocked, which silently
       * discarded every remaining PCM frame of the utterance: one corrupted
       * frame turned the whole turn into "0 bytes of audio" at the cloud.
       * Fail open instead - count it and keep streaming.  Only a stream that
       * never had valid metadata stays blocked, because without it the frame
       * layout and stream id are unknown. */
      g_audio.diag.audio_up_meta_error++;
      if (!g_audio.up_meta_seen) {
        g_audio.up_blocked = true;
      }
#if defined(ESP_PLATFORM)
      char preview[192];
      const size_t copy = length < sizeof(preview) - 1 ? length : sizeof(preview) - 1;
      memcpy(preview, payload, copy);
      preview[copy] = '\0';
      ESP_LOGW(TAG, "AUDIO_UP meta rejected (len=%u, stream %s): %s",
               static_cast<unsigned>(length),
               g_audio.up_meta_seen ? "continues" : "blocked", preview);
#endif
    }
    return;
  }
  if (!g_audio.up_meta_seen || g_audio.up_blocked) {
    // Once a malformed metadata frame blocks a stream, binary PCM is silently
    // discarded until a new metadata candidate arrives. Do not turn every
    // subsequent PCM frame into another metadata error.
    if (g_audio.up_blocked) return;
    g_audio.diag.audio_up_meta_error++;
    g_audio.up_blocked = true;
    return;
  }
  if (length != MIBOT_AUDIO_FRAME_BYTES) { g_audio.diag.audio_up_size_error++; return; }
  if (g_audio.have_last_seq) {
    const uint16_t delta = static_cast<uint16_t>(seq - g_audio.last_seq);
    if (delta == 0 || delta > MIBOT_AUDIO_SEQ_GAP_MAX) g_audio.diag.audio_up_seq_gap++;
  }
  g_audio.last_seq = seq;
  g_audio.have_last_seq = true;
  lock_audio();
  const bool downlink_active = g_audio.direction == AUDIO_DIRECTION_DOWNLINK && g_audio.stream.active;
  const bool loopback = g_audio.mode == AUDIO_MODE_LOOPBACK;
  const bool barge = downlink_active && !loopback && g_audio.stream.interruptible;
  const bool ignore_barge = downlink_active && !loopback && !g_audio.stream.interruptible;
  const bool degraded = g_audio.degraded && g_audio.mode == AUDIO_MODE_CLOUD;
  unlock_audio();
  if (barge) { downlink_abort_internal("barge_in"); return; }
  if (ignore_barge) { g_audio.diag.uplink_dropped++; return; }
  if (degraded || !ring_push_drop_oldest(&g_audio.uplink, payload)) g_audio.diag.uplink_dropped++;
  if (!loopback) direction_enter(AUDIO_DIRECTION_UPLINK);
}

extern "C" void audio_link_on_frame_error(uint8_t type) {
  if (type == MIBOT_MSG_AUDIO_UP) g_audio.diag.audio_up_crc_error++;
  else if (type == MIBOT_MSG_AUDIO_DOWN) g_audio.diag.downlink_dropped++;
}

extern "C" void audio_link_on_estop(void) { downlink_abort_internal("estop"); }
extern "C" void audio_link_set_capture_fd(int fd) { g_audio.capture_fd = fd; }
extern "C" void audio_link_on_tcp_disconnected(int fd) {
  lock_audio();
  const bool owns_stream = g_audio.stream.active && g_audio.stream.target.transport == 1 &&
                           g_audio.stream.target.socket_fd == fd;
  unlock_audio();
  if (owns_stream) downlink_abort_internal("transport_closed");
}
extern "C" void audio_link_on_wifi_disconnected(void) { cloud_state_changed(CLOUD_STATE_DISCONNECTED, nullptr); }
extern "C" void audio_link_record_tx_lock_duration(uint32_t duration_ms) {
#if defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
  if (duration_ms > g_audio.diag.tx_lock_max_ms) g_audio.diag.tx_lock_max_ms = duration_ms;
  if (duration_ms > MIBOT_AUDIO_TX_LOCK_BUDGET_MS) ++g_audio.diag.tx_lock_over_budget;
#else
  (void)duration_ms;
#endif
}
extern "C" void audio_link_record_downlink_interval(uint32_t interval_ms) {
#if defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
  if (g_audio.diag.audio_down_interval_min_ms == 0 || interval_ms < g_audio.diag.audio_down_interval_min_ms)
    g_audio.diag.audio_down_interval_min_ms = interval_ms;
  if (interval_ms > g_audio.diag.audio_down_interval_max_ms) g_audio.diag.audio_down_interval_max_ms = interval_ms;
  const uint32_t bucket = interval_ms > 50 ? 50 : interval_ms;
  ++g_audio.down_interval_hist[bucket];
  ++g_audio.down_interval_samples;
  const uint32_t rank = (g_audio.down_interval_samples * 99U + 99U) / 100U;
  uint32_t cumulative = 0;
  for (uint32_t index = 0; index <= 50; ++index) {
    cumulative += g_audio.down_interval_hist[index];
    if (cumulative >= rank) { g_audio.diag.audio_down_interval_p99_ms = index; break; }
  }
  const int64_t now = clock_ms();
  if (g_audio.down_window_frames == 0) g_audio.down_window_start_ms = now;
  ++g_audio.down_window_frames;
  if (g_audio.down_window_frames == 50) {
    g_audio.diag.audio_down_50_frames_ms = static_cast<uint32_t>(now - g_audio.down_window_start_ms);
    g_audio.down_window_frames = 0;
  }
#else
  (void)interval_ms;
#endif
}
extern "C" void audio_link_record_uart_metrics(uint32_t buffered_bytes, uint32_t process_ms) {
#if defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
  if (buffered_bytes > g_audio.diag.uart_rx_peak_bytes) g_audio.diag.uart_rx_peak_bytes = buffered_bytes;
  if (process_ms > g_audio.diag.uart_frame_process_max_ms) g_audio.diag.uart_frame_process_max_ms = process_ms;
#else
  (void)buffered_bytes; (void)process_ms;
#endif
}
extern "C" void audio_link_record_safety_period(uint32_t period_ms) {
#if defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
  if (g_audio.diag.safety_period_min_ms == 0 || period_ms < g_audio.diag.safety_period_min_ms)
    g_audio.diag.safety_period_min_ms = period_ms;
  if (period_ms > g_audio.diag.safety_period_max_ms) g_audio.diag.safety_period_max_ms = period_ms;
#else
  (void)period_ms;
#endif
}
extern "C" void audio_link_log_task_contract(void) {
#if defined(ESP_PLATFORM)
  const TaskHandle_t handles[] = {g_audio_up_task_handle, g_audio_tx_task_handle, g_audio_dn_task_handle};
  const char *names[] = {"mibot_audio_up", "mibot_audio_tx", "mibot_audio_dn"};
  for (size_t index = 0; index < 3; ++index) {
    if (handles[index] == nullptr) {
      ESP_LOGE(TAG, "%s task was not created", names[index]);
      continue;
    }
    const UBaseType_t priority = uxTaskPriorityGet(handles[index]);
    const BaseType_t core = xTaskGetCoreID(handles[index]);
    ESP_LOGI(TAG, "%s priority=%u core=%d", names[index], static_cast<unsigned>(priority), static_cast<int>(core));
    const BaseType_t expected_core = index == 1 ? 1 : 0;
    if (priority != 11 || core != expected_core) ESP_LOGE(TAG, "%s violates audio task contract", names[index]);
  }
#endif
}
extern "C" void audio_link_on_cloud_data(cloud_frame_kind_t kind, const uint8_t *data,
                                           size_t length, const cloud_request_ctx_t *ctx) {
  cloud_data_received(kind, data, length, ctx, nullptr);
}

extern "C" audio_direction_t audio_direction(void) { return g_audio.direction; }
extern "C" const char *audio_direction_name(audio_direction_t direction) {
  switch (direction) { case AUDIO_DIRECTION_UPLINK: return "UPLINK"; case AUDIO_DIRECTION_DOWNLINK: return "DOWNLINK"; default: return "IDLE"; }
}
extern "C" audio_mode_t audio_mode(void) { return g_audio.mode; }
extern "C" const char *audio_mode_name(audio_mode_t mode) {
  switch (mode) { case AUDIO_MODE_LOOPBACK: return "LOOPBACK"; case AUDIO_MODE_CAPTURE: return "CAPTURE"; default: return "CLOUD"; }
}
extern "C" void audio_diag_reset(void) { memset(&g_audio.diag, 0, sizeof(g_audio.diag)); }
extern "C" const AudioDiag *audio_diag_snapshot(void) { return &g_audio.diag; }
extern "C" uint16_t audio_uplink_count(void) { return ring_count(&g_audio.uplink); }
extern "C" uint16_t audio_downlink_count(void) { return ring_count(&g_audio.downlink); }
extern "C" void audio_diag_write_counters(cJSON *audio) {
#if defined(ESP_PLATFORM) || defined(MIBOT_HOST_TEST)
  lock_audio();
  add_audio_counters(audio, g_audio.diag);
  unlock_audio();
#else
  (void)audio;
#endif
}
extern "C" void audio_send_event(const char *event, const char *severity, cJSON *data) {
  emit_event(event, severity, data);
}
extern "C" void audio_reply_command(const char *command_id, int ok, const char *error_code,
                                      const char *reason, const char *stream_id,
                                      const AudioReplyTarget *target) {
  reply_audio_command(command_id, ok != 0, error_code, reason, stream_id, target);
}
extern "C" void direction_enter_uplink(void) { direction_enter(AUDIO_DIRECTION_UPLINK); }
extern "C" void direction_enter_idle(void) { direction_enter(AUDIO_DIRECTION_IDLE); }
extern "C" void direction_enter_downlink(void) { direction_enter(AUDIO_DIRECTION_DOWNLINK); }
extern "C" void downlink_abort(const char *reason) { downlink_abort_internal(reason); }
extern "C" void downlink_stream_finish(void) {
  lock_audio();
  if (g_audio.stream.active) g_audio.stream.cloud_finished = true;
  unlock_audio();
}
extern "C" audio_tx_plan_t tx_plan_next(void) {
  lock_audio();
  audio_tx_plan_t plan = AUDIO_TX_IDLE;
  if (g_audio.stream.active) {
    if (!g_audio.stream.meta_sent) plan = AUDIO_TX_SEND_META;
    else if (!g_audio.stream.prefill_ready) plan = AUDIO_TX_PREFILL_WAIT;
    else if (ring_count(&g_audio.downlink) > 0) plan = AUDIO_TX_SEND_PCM;
    else if (g_audio.stream.cloud_finished) plan = AUDIO_TX_SEND_EOS;
    else plan = AUDIO_TX_PREFILL_WAIT;
  }
  unlock_audio();
  return plan;
}
#if defined(MIBOT_HOST_TEST)
extern "C" void audio_link_host_tx_step(void) { audio_tx_step_internal(); }
extern "C" void audio_link_host_attach_cloud(cloud_adapter_t *adapter) {
  g_audio.cloud = adapter;
  g_audio.degraded = adapter == nullptr || adapter->vtable == nullptr ||
                     adapter->vtable->state(adapter) != CLOUD_STATE_CONNECTED;
}
#endif
extern "C" void generate_test_tone(uint32_t duration_ms, uint32_t freq_hz,
                                    const char *command_id, const AudioReplyTarget *target) {
  generate_test_tone_internal(duration_ms, freq_hz, command_id, target);
}
