#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mibot_audio_frame.h"
#include "mibot_cloud.h"
#include "mibot_protocol.h"
#include "mibot_ring.h"

#if !defined(ESP_PLATFORM)
#define MIBOT_COMMAND_ID_MAX 64
#define MIBOT_AUDIO_UP_DEPTH_PSRAM 25
#define MIBOT_AUDIO_UP_DEPTH_SRAM 10
#define MIBOT_AUDIO_DN_DEPTH_PSRAM 25
#define MIBOT_AUDIO_DN_DEPTH_SRAM 15
#define MIBOT_AUDIO_CLOUD_CHUNK_QUEUE_DEPTH 4
#define MIBOT_AUDIO_PREFILL_DEPTH 3
#define MIBOT_AUDIO_LOOPBACK_PREFILL_DEPTH 15
#define MIBOT_AUDIO_UPLINK_CHUNK_FRAMES 5
#define MIBOT_AUDIO_UPLINK_CHUNK_BYTES 3200
#define MIBOT_AUDIO_UP_SILENCE_MS 500
/* Duplicate of mibot_config.h's definition -- keep the two in step.  A value
 * that is too small does not merely fail one send, it kills the WebSocket
 * session; see the long note in mibot_config.h. */
#define MIBOT_AUDIO_CLOUD_SEND_TIMEOUT_MS 4000
/* Duplicate of mibot_config.h's definition -- keep the two in step.  5 s was
 * shorter than edge-tts needs for a long reply (measured 6 s), which aborted the
 * stream just before the audio arrived. */
#define MIBOT_AUDIO_TTS_FIRST_CHUNK_TIMEOUT_MS 20000
#define MIBOT_AUDIO_TX_LOCK_BUDGET_MS 8
#define MIBOT_AUDIO_SEQ_GAP_MAX 64
#define MIBOT_AUDIO_CMD_CACHE_SLOTS 8
#define MIBOT_AUDIO_SPEAK_TEXT_MAX_CP 500
#define MIBOT_AUDIO_CLOUD_CHUNK_MAX_PSRAM 4096
#define MIBOT_AUDIO_CLOUD_CHUNK_MAX_SRAM 2048
#define ESP_ERR_NO_MEM 0x101
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

typedef enum audio_direction_t {
  AUDIO_DIRECTION_IDLE = 0,
  AUDIO_DIRECTION_UPLINK = 1,
  AUDIO_DIRECTION_DOWNLINK = 2,
} audio_direction_t;

typedef enum audio_mode_t {
  AUDIO_MODE_CLOUD = 0,
  AUDIO_MODE_LOOPBACK = 1,
  AUDIO_MODE_CAPTURE = 2,
} audio_mode_t;

typedef enum audio_tx_plan_t {
  AUDIO_TX_IDLE = 0,
  AUDIO_TX_SEND_META = 1,
  AUDIO_TX_PREFILL_WAIT = 2,
  AUDIO_TX_SEND_PCM = 3,
  AUDIO_TX_SEND_EOS = 4,
} audio_tx_plan_t;

typedef struct AudioDiag {
  uint32_t audio_up_frames_received;
  uint32_t audio_up_crc_error;
  uint32_t audio_up_size_error;
  uint32_t audio_up_seq_gap;
  uint32_t audio_up_meta_error;
  uint32_t uplink_dropped;
  uint32_t uplink_send_error;
  uint32_t downlink_decode_error;
  uint32_t downlink_dropped;
  uint32_t downlink_underrun;
  uint32_t downlink_aborted;
  uint32_t audio_down_frames_sent;
  uint32_t audio_down_write_error;
  uint32_t audio_down_bytes_1s;
  uint32_t audio_down_bytes_1s_snapshot;
  int32_t uplink_latency_ms;
  int32_t downlink_latency_ms;
  uint32_t tx_lock_max_ms;
  uint32_t tx_lock_over_budget;
  uint32_t audio_down_interval_min_ms;
  uint32_t audio_down_interval_max_ms;
  uint32_t audio_down_interval_p99_ms;
  uint32_t audio_down_50_frames_ms;
  uint32_t uart_rx_peak_bytes;
  uint32_t uart_frame_process_max_ms;
  uint32_t safety_period_min_ms;
  uint32_t safety_period_max_ms;
} AudioDiag;

typedef struct AudioReplyTarget {
  int transport; /* 0 = UART, 1 = Wi-Fi TCP */
  int socket_fd;
} AudioReplyTarget;

typedef int (*audio_send_frame_fn)(uint8_t type, uint8_t flags,
                                   const uint8_t *payload, uint16_t length,
                                   const AudioReplyTarget *target, void *user);

int audio_link_init(void);
void audio_link_set_frame_sender(audio_send_frame_fn sender, void *user);
int audio_link_handle_command(const uint8_t *payload, uint16_t length,
                              const AudioReplyTarget *target);
void audio_link_fill_telemetry(cJSON *root);
void audio_link_on_audio_up(uint8_t flags, const uint8_t *payload,
                            uint16_t length, uint16_t seq,
                            const AudioReplyTarget *target);
void audio_link_on_frame_error(uint8_t type);
void audio_link_on_estop(void);
void audio_link_set_capture_fd(int fd);
void audio_link_on_tcp_disconnected(int fd);
void audio_link_on_wifi_disconnected(void);
void audio_link_record_tx_lock_duration(uint32_t duration_ms);
void audio_link_record_downlink_interval(uint32_t interval_ms);
void audio_link_record_uart_metrics(uint32_t buffered_bytes, uint32_t process_ms);
void audio_link_record_safety_period(uint32_t period_ms);
void audio_link_log_task_contract(void);
void audio_link_on_cloud_data(cloud_frame_kind_t kind, const uint8_t *data,
                              size_t length, const cloud_request_ctx_t *ctx);

audio_direction_t audio_direction(void);
const char *audio_direction_name(audio_direction_t direction);
audio_mode_t audio_mode(void);
const char *audio_mode_name(audio_mode_t mode);
void audio_diag_reset(void);
const AudioDiag *audio_diag_snapshot(void);
void audio_diag_write_counters(cJSON *audio);
void audio_send_event(const char *event, const char *severity, cJSON *data);
void audio_reply_command(const char *command_id, int ok, const char *error_code,
                         const char *reason, const char *stream_id,
                         const AudioReplyTarget *target);
void direction_enter_uplink(void);
void direction_enter_idle(void);
void direction_enter_downlink(void);
void downlink_abort(const char *reason);
void downlink_stream_finish(void);
audio_tx_plan_t tx_plan_next(void);
#if defined(MIBOT_HOST_TEST)
void audio_link_host_tx_step(void);
void audio_link_host_attach_cloud(cloud_adapter_t *adapter);
#endif
void generate_test_tone(uint32_t duration_ms, uint32_t freq_hz,
                        const char *command_id, const AudioReplyTarget *target);
uint16_t audio_uplink_count(void);
uint16_t audio_downlink_count(void);

#ifdef __cplusplus
}
#endif
