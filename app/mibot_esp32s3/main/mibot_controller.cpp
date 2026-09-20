#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "mibot_config.h"
#include "mibot_audio.h"
#include "mibot_tof.h"
#include "mibot_net_bridge.h"
#include "mibot_shared_net_bridge.h"
#if defined(CONFIG_MIBOT_DEEPSEEK_GATEWAY) && CONFIG_MIBOT_DEEPSEEK_GATEWAY
#include "deepseek_gateway.h"
#endif

namespace {

constexpr char TAG[] = "mibot";
#define MIBOT_RETURN_ON_ERROR(expr, message)      \
  do {                                            \
    const esp_err_t _result = (expr);             \
    if (_result != ESP_OK) {                      \
      ESP_LOGE(TAG, "%s: %s", (message),        \
               esp_err_to_name(_result));        \
      return _result;                              \
    }                                              \
  } while (0)
constexpr uint8_t SOF0 = 0xAA;
constexpr uint8_t SOF1 = 0x55;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t FLAG_ACK_REQUEST = MIBOT_FLAG_ACK_REQUEST;
constexpr uint8_t FLAG_RESPONSE = MIBOT_FLAG_RESPONSE;
constexpr uint8_t FLAG_ERROR = MIBOT_FLAG_ERROR;
constexpr size_t FRAME_HEADER_SIZE = 7;

enum class TransportKind : uint8_t { Uart, WifiTcp };

struct ReplyTarget {
  TransportKind kind;
  int socket_fd;
};

constexpr ReplyTarget UART_TARGET{TransportKind::Uart, -1};

enum MessageType : uint8_t {
  MSG_HELLO = MIBOT_MSG_HELLO,
  MSG_HELLO_ACK = MIBOT_MSG_HELLO_ACK,
  MSG_COMMAND = MIBOT_MSG_COMMAND,
  MSG_ACK = MIBOT_MSG_ACK,
  MSG_NACK = MIBOT_MSG_NACK,
  MSG_EVENT = MIBOT_MSG_EVENT,
  MSG_TELEMETRY = MIBOT_MSG_TELEMETRY,
  MSG_AUDIO_UP = MIBOT_MSG_AUDIO_UP,
  MSG_AUDIO_DOWN = MIBOT_MSG_AUDIO_DOWN,
  MSG_AI_REQUEST = MIBOT_MSG_AI_REQUEST,
  MSG_AI_RESPONSE = MIBOT_MSG_AI_RESPONSE,
  MSG_SHARED_NET_RX = MIBOT_SHARED_NET_RX,
  MSG_SHARED_NET_TX = MIBOT_SHARED_NET_TX,
  MSG_PING = MIBOT_MSG_PING,
  MSG_PONG = MIBOT_MSG_PONG,
};

enum class MotionState : uint8_t {
  Init,
  Standby,
  Running,
  Braking,
  Hold,
  Fault,
};

struct TofReading {
  uint16_t mm = 0;
  uint8_t quality = 0;
  bool valid = false;
  int64_t timestamp_ms = 0;
};

struct RobotState {
  MotionState motion = MotionState::Init;
  int16_t left_target = 0;
  int16_t right_target = 0;
  float left_servo_deg = 90.0f;
  float right_servo_deg = 90.0f;
  int64_t motion_deadline_ms = 0;
  int64_t last_sf32_rx_ms = 0;
  bool status_led_on = false;
  bool action_active = false;
  bool motor_test_active = false;
  int64_t motor_test_deadline_ms = 0;
  char motor_test_command_id[64] = {};
  ReplyTarget motor_test_target = UART_TARGET;
  char action_name[24] = {};
  TofReading tof[4];
};

// An action is a bounded, device-owned sequence. Cloud/SF32 sends only the
// action name and intensity; the ESP32 owns the actual motor and servo values.
struct ActionStep {
  uint16_t duration_ms;
  int16_t left_motor;
  int16_t right_motor;
  float left_servo_deg;
  float right_servo_deg;
  const char *expression;
  float expression_intensity;
};

struct ActionPlan {
  const char *name;
  const ActionStep *steps;
  size_t step_count;
};

constexpr size_t ACTION_COMMAND_ID_MAX = 64;

struct ActionRuntime {
  bool active = false;
  const ActionPlan *plan = nullptr;
  size_t step_index = 0;
  int64_t step_deadline_ms = 0;
  float intensity = 1.0f;
  char command_id[ACTION_COMMAND_ID_MAX] = {};
  ReplyTarget target = UART_TARGET;
};

RobotState g_state;
ActionRuntime g_action;
SemaphoreHandle_t g_state_mutex = nullptr;
SemaphoreHandle_t g_tx_mutex = nullptr;
SemaphoreHandle_t g_status_led_mutex = nullptr;
QueueHandle_t g_tcp_tx_queue = nullptr;
volatile int g_tcp_active_fd = -1;
volatile uint32_t g_tcp_generation = 0;
uint16_t g_tx_sequence = 1;
TaskHandle_t g_uart_task_handle = nullptr;
TaskHandle_t g_wifi_tcp_task_handle = nullptr;
TaskHandle_t g_safety_task_handle = nullptr;
TaskHandle_t g_action_task_handle = nullptr;
TaskHandle_t g_reporting_task_handle = nullptr;
TaskHandle_t g_tcp_tx_task_handle = nullptr;

constexpr size_t TCP_TX_QUEUE_DEPTH = 128;

struct TcpTxPacket {
  int socket_fd;
  uint32_t generation;
  size_t length;
  uint8_t data[1];
};

rmt_channel_handle_t g_status_led_channel = nullptr;
rmt_encoder_handle_t g_status_led_encoder = nullptr;
esp_netif_t *g_wifi_sta_netif = nullptr;
EventGroupHandle_t g_wifi_events = nullptr;
constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

esp_err_t set_status_led_color(uint8_t red, uint8_t green, uint8_t blue);

constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t MOTOR_TIMER = LEDC_TIMER_0;
constexpr ledc_timer_t SERVO_TIMER = LEDC_TIMER_1;
constexpr ledc_channel_t MOTOR_A_CHANNEL = LEDC_CHANNEL_0;
constexpr ledc_channel_t MOTOR_B_CHANNEL = LEDC_CHANNEL_1;
constexpr ledc_channel_t SERVO_LEFT_CHANNEL = LEDC_CHANNEL_2;
constexpr ledc_channel_t SERVO_RIGHT_CHANNEL = LEDC_CHANNEL_3;
constexpr uint32_t MOTOR_MAX_DUTY = (1U << 10) - 1;
constexpr uint32_t SERVO_MAX_DUTY = (1U << 14) - 1;

static_assert(MIBOT_I2C_SDA != GPIO_NUM_35 && MIBOT_I2C_SDA != GPIO_NUM_36 && MIBOT_I2C_SDA != GPIO_NUM_37);
static_assert(MIBOT_I2C_SCL != GPIO_NUM_35 && MIBOT_I2C_SCL != GPIO_NUM_36 && MIBOT_I2C_SCL != GPIO_NUM_37);
static_assert(MIBOT_SF32_RX != GPIO_NUM_35 && MIBOT_SF32_RX != GPIO_NUM_36 && MIBOT_SF32_RX != GPIO_NUM_37);
static_assert(MIBOT_SF32_TX != GPIO_NUM_35 && MIBOT_SF32_TX != GPIO_NUM_36 && MIBOT_SF32_TX != GPIO_NUM_37);
static_assert(MIBOT_STATUS_LED_GPIO != GPIO_NUM_35 && MIBOT_STATUS_LED_GPIO != GPIO_NUM_36 && MIBOT_STATUS_LED_GPIO != GPIO_NUM_37);
static_assert(MIBOT_MOTOR_AIN1 != GPIO_NUM_35 && MIBOT_MOTOR_AIN1 != GPIO_NUM_36 && MIBOT_MOTOR_AIN1 != GPIO_NUM_37);
static_assert(MIBOT_MOTOR_AIN2 != GPIO_NUM_35 && MIBOT_MOTOR_AIN2 != GPIO_NUM_36 && MIBOT_MOTOR_AIN2 != GPIO_NUM_37);
static_assert(MIBOT_MOTOR_PWMA != GPIO_NUM_35 && MIBOT_MOTOR_PWMA != GPIO_NUM_36 && MIBOT_MOTOR_PWMA != GPIO_NUM_37);
static_assert(MIBOT_MOTOR_BIN1 != GPIO_NUM_35 && MIBOT_MOTOR_BIN1 != GPIO_NUM_36 && MIBOT_MOTOR_BIN1 != GPIO_NUM_37);
static_assert(MIBOT_MOTOR_BIN2 != GPIO_NUM_35 && MIBOT_MOTOR_BIN2 != GPIO_NUM_36 && MIBOT_MOTOR_BIN2 != GPIO_NUM_37);
static_assert(MIBOT_MOTOR_PWMB != GPIO_NUM_35 && MIBOT_MOTOR_PWMB != GPIO_NUM_36 && MIBOT_MOTOR_PWMB != GPIO_NUM_37);
static_assert(MIBOT_MOTOR_STBY != GPIO_NUM_35 && MIBOT_MOTOR_STBY != GPIO_NUM_36 && MIBOT_MOTOR_STBY != GPIO_NUM_37);
static_assert(MIBOT_SERVO_LEFT != GPIO_NUM_35 && MIBOT_SERVO_LEFT != GPIO_NUM_36 && MIBOT_SERVO_LEFT != GPIO_NUM_37);
static_assert(MIBOT_SERVO_RIGHT != GPIO_NUM_35 && MIBOT_SERVO_RIGHT != GPIO_NUM_36 && MIBOT_SERVO_RIGHT != GPIO_NUM_37);
static_assert(MIBOT_TOF_XSHUT_0 != GPIO_NUM_35 && MIBOT_TOF_XSHUT_0 != GPIO_NUM_36 && MIBOT_TOF_XSHUT_0 != GPIO_NUM_37);
static_assert(MIBOT_TOF_XSHUT_1 != GPIO_NUM_35 && MIBOT_TOF_XSHUT_1 != GPIO_NUM_36 && MIBOT_TOF_XSHUT_1 != GPIO_NUM_37);
static_assert(MIBOT_TOF_XSHUT_2 != GPIO_NUM_35 && MIBOT_TOF_XSHUT_2 != GPIO_NUM_36 && MIBOT_TOF_XSHUT_2 != GPIO_NUM_37);
static_assert(MIBOT_TOF_XSHUT_3 != GPIO_NUM_35 && MIBOT_TOF_XSHUT_3 != GPIO_NUM_36 && MIBOT_TOF_XSHUT_3 != GPIO_NUM_37);

// The remaining ToF wiring invariants (distinct XSHUT lines, distinct runtime
// addresses, ranging period vs. safety period) are asserted in mibot_tof.cpp,
// next to the code that depends on them.

const char *const TOF_NAMES[4] = {
    "front_left", "front_right", "rear_left", "rear_right"};

int64_t now_ms() { return esp_timer_get_time() / 1000; }

// Servo angles are logical angles around the neutral pose (90 degrees). If
// the two arms are mechanically mirrored, invert the right channel in
// set_servo() or adjust these table values during first calibration.
constexpr ActionStep HAPPY_STEPS[] = {
    {220, 0, 0, 112.0f, 68.0f, "happy", 0.85f},
    {260, 28, -28, 128.0f, 52.0f, "happy", 1.00f},
    {260, -28, 28, 105.0f, 75.0f, "happy", 1.00f},
    {220, 0, 0, 90.0f, 90.0f, "happy", 0.75f},
};

constexpr ActionStep SAD_STEPS[] = {
    {350, 0, 0, 62.0f, 118.0f, "sad", 0.90f},
    {420, -18, -18, 58.0f, 122.0f, "sad", 0.80f},
    {350, 0, 0, 70.0f, 110.0f, "sad", 0.85f},
};

constexpr ActionStep CONFUSED_STEPS[] = {
    {260, 0, 0, 74.0f, 108.0f, "confused", 0.90f},
    {320, 22, -22, 86.0f, 126.0f, "confused", 1.00f},
    {320, -22, 22, 108.0f, 74.0f, "confused", 1.00f},
    {220, 0, 0, 90.0f, 90.0f, "confused", 0.80f},
};

constexpr ActionStep GREETING_STEPS[] = {
    {250, 0, 0, 120.0f, 70.0f, "happy", 0.90f},
    {260, 0, 0, 145.0f, 70.0f, "happy", 1.00f},
    {260, 0, 0, 115.0f, 70.0f, "happy", 1.00f},
    {260, 0, 0, 145.0f, 70.0f, "happy", 1.00f},
    {220, 0, 0, 90.0f, 90.0f, "happy", 0.80f},
};

constexpr ActionStep THINKING_STEPS[] = {
    {300, 0, 0, 98.0f, 82.0f, "thinking", 0.85f},
    {380, 18, -18, 100.0f, 80.0f, "thinking", 0.90f},
    {380, -18, 18, 100.0f, 80.0f, "thinking", 0.90f},
    {250, 0, 0, 90.0f, 90.0f, "thinking", 0.80f},
};

constexpr ActionStep WARNING_STEPS[] = {
    {180, 0, 0, 72.0f, 108.0f, "warning", 1.00f},
    {320, -30, -30, 62.0f, 118.0f, "warning", 1.00f},
    {320, 0, 0, 72.0f, 108.0f, "warning", 1.00f},
};

// The four tables below complete the Bounded_Action set. surprised/cute may
// briefly drive the motors to exercise the PWM path; idle/listening are
// auto-played on state entry, so they keep both motors at 0 and only move the
// servos, letting them pass apply_action_step_locked() as non-moving steps
// that are never blocked by the ToF/edge safety locks.
constexpr ActionStep SURPRISED_STEPS[] = {
    {150, 20, 20, 128.0f, 128.0f, "surprised", 1.00f},
    {200, -20, -20, 118.0f, 118.0f, "surprised", 0.95f},
    {180, 0, 0, 100.0f, 100.0f, "surprised", 0.80f},
    {200, 0, 0, 90.0f, 90.0f, "surprised", 0.70f},
};

constexpr ActionStep CUTE_STEPS[] = {
    {220, 15, -15, 100.0f, 80.0f, "cute", 0.85f},
    {220, -15, 15, 80.0f, 100.0f, "cute", 0.90f},
    {220, 15, -15, 98.0f, 82.0f, "cute", 0.90f},
    {240, 0, 0, 90.0f, 90.0f, "cute", 0.75f},
};

constexpr ActionStep IDLE_STEPS[] = {
    {400, 0, 0, 90.0f, 90.0f, "idle", 0.60f},
    {400, 0, 0, 90.0f, 90.0f, "idle", 0.55f},
};

constexpr ActionStep LISTENING_STEPS[] = {
    {300, 0, 0, 96.0f, 84.0f, "listening", 0.70f},
    {300, 0, 0, 84.0f, 96.0f, "listening", 0.70f},
    {280, 0, 0, 90.0f, 90.0f, "listening", 0.65f},
};

constexpr ActionPlan ACTION_PLANS[] = {
    {"happy", HAPPY_STEPS, sizeof(HAPPY_STEPS) / sizeof(HAPPY_STEPS[0])},
    {"sad", SAD_STEPS, sizeof(SAD_STEPS) / sizeof(SAD_STEPS[0])},
    {"confused", CONFUSED_STEPS,
     sizeof(CONFUSED_STEPS) / sizeof(CONFUSED_STEPS[0])},
    {"greeting", GREETING_STEPS,
     sizeof(GREETING_STEPS) / sizeof(GREETING_STEPS[0])},
    {"thinking", THINKING_STEPS,
     sizeof(THINKING_STEPS) / sizeof(THINKING_STEPS[0])},
    {"warning", WARNING_STEPS,
     sizeof(WARNING_STEPS) / sizeof(WARNING_STEPS[0])},
    {"surprised", SURPRISED_STEPS,
     sizeof(SURPRISED_STEPS) / sizeof(SURPRISED_STEPS[0])},
    {"cute", CUTE_STEPS, sizeof(CUTE_STEPS) / sizeof(CUTE_STEPS[0])},
    {"idle", IDLE_STEPS, sizeof(IDLE_STEPS) / sizeof(IDLE_STEPS[0])},
    {"listening", LISTENING_STEPS,
     sizeof(LISTENING_STEPS) / sizeof(LISTENING_STEPS[0])},
};

const ActionPlan *find_action_plan(const char *name) {
  if (name == nullptr) return nullptr;
  for (const auto &plan : ACTION_PLANS) {
    if (strcmp(name, plan.name) == 0) return &plan;
  }
  return nullptr;
}

class LockGuard {
 public:
  explicit LockGuard(SemaphoreHandle_t mutex) : mutex_(mutex) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
#if defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
    started_us_ = esp_timer_get_time();
#endif
  }
  ~LockGuard() {
#if defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
    if (mutex_ == g_tx_mutex) {
      const uint32_t duration_ms = static_cast<uint32_t>((esp_timer_get_time() - started_us_ + 999) / 1000);
      audio_link_record_tx_lock_duration(duration_ms);
    }
#endif
    xSemaphoreGive(mutex_);
  }
  LockGuard(const LockGuard &) = delete;
  LockGuard &operator=(const LockGuard &) = delete;

 private:
  SemaphoreHandle_t mutex_;
#if defined(MIBOT_AUDIO_DIAG_TRACE) && MIBOT_AUDIO_DIAG_TRACE
  int64_t started_us_ = 0;
#endif
};

uint16_t crc16_append(uint16_t crc, const uint8_t *data, size_t length) {
  while (length--) {
    crc ^= static_cast<uint16_t>(*data++) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) != 0
                ? static_cast<uint16_t>((crc << 1) ^ 0x1021U)
                : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

esp_err_t socket_write_all(int socket_fd, uint32_t generation,
                           const uint8_t *data, size_t length) {
  while (length > 0) {
    if (socket_fd != g_tcp_active_fd || generation != g_tcp_generation) {
      return ESP_ERR_INVALID_STATE;
    }
    // A blocking send() can hold lwIP's socket lock while wifi_tcp_task is
    // waiting to recv() the next uplink frame. Use non-blocking writes and
    // yield between retries so ingress and egress continue independently.
    const int written = send(socket_fd, data, length, MSG_DONTWAIT);
    if (written < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      return ESP_FAIL;
    }
    if (written == 0) return ESP_FAIL;
    data += written;
    length -= static_cast<size_t>(written);
  }
  return ESP_OK;
}

void tcp_tx_task(void *) {
  while (true) {
    TcpTxPacket *packet = nullptr;
    if (xQueueReceive(g_tcp_tx_queue, &packet, pdMS_TO_TICKS(100)) != pdTRUE ||
        packet == nullptr) {
      continue;
    }
    if (packet->socket_fd == g_tcp_active_fd &&
        packet->generation == g_tcp_generation) {
      socket_write_all(packet->socket_fd, packet->generation, packet->data,
                       packet->length);
    }
    heap_caps_free(packet);
  }
}

/* Bytes accepted by uart_write_bytes() for the SF32 link, counted so the
 * SF32's own "va: rx ... bytes=" total can be compared against it.  The two
 * agreeing means the link delivers every byte and any lost frame was discarded
 * by the receiver; the SF32 total falling short means bytes never arrive. */
uint32_t g_uart_tx_bytes = 0;

esp_err_t uart_write_all(const uint8_t *data, size_t length) {
  if (length == 0) return ESP_OK;
  if (data == nullptr) return ESP_ERR_INVALID_ARG;

  /* uart_write_bytes() normally accepts the complete buffer, but it is
   * allowed to return a short write when the TX ring is under pressure.
   * A partial AA55 frame cannot be retried as a new frame: the peer would
   * parse the following control packet in the middle of its payload.  Keep
   * the frame contiguous under g_tx_mutex and finish the short write with a
   * bounded wait. */
  size_t offset = 0;
  const int64_t deadline_us = esp_timer_get_time() + 500000;
  while (offset < length) {
    const int written = uart_write_bytes(
        MIBOT_SF32_UART,
        reinterpret_cast<const char *>(data + offset),
        length - offset);
    if (written > 0) {
      offset += static_cast<size_t>(written);
      g_uart_tx_bytes += static_cast<uint32_t>(written);
      continue;
    }
    if (written < 0 || esp_timer_get_time() >= deadline_us) {
      return written < 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return ESP_OK;
}

esp_err_t send_frame(uint8_t type, uint8_t flags, const uint8_t *payload,
                     uint16_t length, ReplyTarget target = UART_TARGET) {
  if (length > MIBOT_MAX_FRAME_PAYLOAD || (length > 0 && payload == nullptr)) {
    return ESP_ERR_INVALID_ARG;
  }

  uint16_t sequence = 0;
  {
    LockGuard sequence_lock(g_tx_mutex);
    sequence = g_tx_sequence++;
  }
  const uint8_t prefix[2] = {SOF0, SOF1};
  const uint8_t header[FRAME_HEADER_SIZE] = {
      PROTOCOL_VERSION, type, flags,
      static_cast<uint8_t>(sequence & 0xFF),
      static_cast<uint8_t>(sequence >> 8),
      static_cast<uint8_t>(length & 0xFF),
      static_cast<uint8_t>(length >> 8),
  };

  uint16_t crc = crc16_append(0xFFFF, header, sizeof(header));
  crc = crc16_append(crc, payload, length);
  const uint8_t crc_bytes[2] = {
      static_cast<uint8_t>(crc & 0xFF), static_cast<uint8_t>(crc >> 8)};

  if (target.kind == TransportKind::WifiTcp) {
    // Coalesce one TCP protocol frame before writing.  Four independent
    // send() calls (SOF/header/payload/CRC) can fill lwIP's small TX window
    // during a long 50 fps stream and hold g_tx_mutex for seconds.
    const size_t packet_size = sizeof(prefix) + sizeof(header) + length + sizeof(crc_bytes);
    auto *packet = static_cast<uint8_t *>(malloc(packet_size));
    if (packet == nullptr) return ESP_ERR_NO_MEM;
    size_t offset = 0;
    memcpy(packet + offset, prefix, sizeof(prefix));
    offset += sizeof(prefix);
    memcpy(packet + offset, header, sizeof(header));
    offset += sizeof(header);
    if (length > 0) {
      memcpy(packet + offset, payload, length);
      offset += length;
    }
    memcpy(packet + offset, crc_bytes, sizeof(crc_bytes));
    if (g_tcp_tx_queue == nullptr || target.socket_fd != g_tcp_active_fd) {
      free(packet);
      return ESP_ERR_INVALID_STATE;
    }
    const size_t allocation_size = sizeof(TcpTxPacket) + packet_size - 1;
    auto *queued = static_cast<TcpTxPacket *>(heap_caps_malloc(
        allocation_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (queued == nullptr) {
      free(packet);
      return ESP_ERR_NO_MEM;
    }
    queued->socket_fd = target.socket_fd;
    queued->generation = g_tcp_generation;
    queued->length = packet_size;
    memcpy(queued->data, packet, packet_size);
    free(packet);
    if (xQueueSend(g_tcp_tx_queue, &queued, 0) != pdTRUE) {
      heap_caps_free(queued);
      return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
  }

  // UART writes must remain serialized with one another, but Wi-Fi writes do
  // not hold this global lock while waiting for network space.
  LockGuard uart_lock(g_tx_mutex);
  const auto write = [target](const uint8_t *data, size_t size) {
    return target.kind == TransportKind::Uart
               ? uart_write_all(data, size)
               : ESP_ERR_INVALID_STATE;
  };
  MIBOT_RETURN_ON_ERROR(write(prefix, sizeof(prefix)),
                        "Transport prefix write failed");
  MIBOT_RETURN_ON_ERROR(write(header, sizeof(header)),
                        "Transport header write failed");
  if (length > 0) {
    MIBOT_RETURN_ON_ERROR(write(payload, length),
                          "Transport payload write failed");
  }
  const esp_err_t result = write(crc_bytes, sizeof(crc_bytes));
  return result;
}

/* Shared Route-A bridge callback.  The bridge never writes the UART itself;
 * this keeps AA55 sequence allocation, CRC and the TX mutex in one place. */
esp_err_t shared_net_send_frame(uint8_t type, uint8_t flags,
                                const uint8_t *payload, uint16_t length,
                                void *) {
  return send_frame(type, flags, payload, length, UART_TARGET);
}

int audio_send_frame_bridge(uint8_t type, uint8_t flags, const uint8_t *payload,
                            uint16_t length, const AudioReplyTarget *target,
                            void *) {
  const ReplyTarget actual = target != nullptr && target->transport != 0
                                 ? ReplyTarget{TransportKind::WifiTcp, target->socket_fd}
                                 : UART_TARGET;
  return send_frame(type, flags, payload, length, actual);
}

esp_err_t send_json(uint8_t type, uint8_t flags, cJSON *root,
                    ReplyTarget target = UART_TARGET) {
  char *text = cJSON_PrintUnformatted(root);
  if (text == nullptr) return ESP_ERR_NO_MEM;
  const size_t length = strlen(text);
  const esp_err_t result =
      length <= MIBOT_MAX_FRAME_PAYLOAD
          ? send_frame(type, flags, reinterpret_cast<const uint8_t *>(text),
                       static_cast<uint16_t>(length), target)
          : ESP_ERR_INVALID_SIZE;
  cJSON_free(text);
  return result;
}

void send_ai_error_response(const char *code, const char *message,
                            ReplyTarget target) {
  cJSON *root = cJSON_CreateObject();
  cJSON *error = root != nullptr
                     ? cJSON_AddObjectToObject(root, "error")
                     : nullptr;
  if (root == nullptr || error == nullptr ||
      cJSON_AddStringToObject(root, "schema", "mibot.ai.response.v1") == nullptr ||
      cJSON_AddBoolToObject(root, "ok", false) == nullptr ||
      cJSON_AddStringToObject(error, "code", code != nullptr ? code : "E_GATEWAY") == nullptr ||
      cJSON_AddStringToObject(error, "message", message != nullptr ? message : "") == nullptr) {
    cJSON_Delete(root);
    return;
  }
  const esp_err_t result = send_json(MSG_AI_RESPONSE, FLAG_RESPONSE | FLAG_ERROR,
                                     root, target);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "AI error response dropped: %s", esp_err_to_name(result));
  }
  cJSON_Delete(root);
}

#if defined(CONFIG_MIBOT_DEEPSEEK_GATEWAY) && CONFIG_MIBOT_DEEPSEEK_GATEWAY
void deepseek_response_callback(bool ok, const char *payload, uint16_t length,
                                void *) {
  if (payload == nullptr || length == 0 || length > MIBOT_MAX_FRAME_PAYLOAD) {
    send_ai_error_response("E_RESPONSE_TOO_LARGE",
                           "gateway response does not fit one AA55 frame",
                           UART_TARGET);
    return;
  }
  const uint8_t flags = static_cast<uint8_t>(FLAG_RESPONSE |
                                             (ok ? 0 : FLAG_ERROR));
  const esp_err_t result = send_frame(
      MSG_AI_RESPONSE, flags, reinterpret_cast<const uint8_t *>(payload),
      length, UART_TARGET);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "AI response dropped: %s", esp_err_to_name(result));
  } else {
    ESP_LOGI(TAG, "AI_RESPONSE -> SF32 payload=%u uart_tx_bytes=%lu",
             static_cast<unsigned>(length),
             static_cast<unsigned long>(g_uart_tx_bytes));
  }
}
#endif

/* Publish the ESP32 uplink state to the SF32 over the business UART.  The
 * event is deliberately independent from the Wi-Fi TCP service: the SF32
 * needs this bit before it is allowed to start its own HTTPS clients. */
void send_network_ready_event(bool ready,
                              const esp_netif_ip_info_t *ip_info = nullptr) {
  if (g_tx_mutex == nullptr) return;

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return;
  cJSON_AddStringToObject(root, "schema", "mibot.net.v1");
  cJSON_AddBoolToObject(root, "ready", ready);

  if (ready && ip_info != nullptr && ip_info->ip.addr != 0) {
    char ip[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &ip_info->ip.addr, ip, sizeof(ip)) != nullptr) {
      cJSON_AddStringToObject(root, "ip", ip);
    }
  }

  const esp_err_t result = send_json(MSG_EVENT, 0, root, UART_TARGET);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "network readiness event dropped: %s",
             esp_err_to_name(result));
  }
  cJSON_Delete(root);
}

void send_current_network_ready_event() {
  bool ready = false;
  esp_netif_ip_info_t ip_info = {};
  const esp_netif_ip_info_t *ip_info_ptr = nullptr;

  if (g_wifi_events != nullptr) {
    ready = (xEventGroupGetBits(g_wifi_events) & WIFI_CONNECTED_BIT) != 0;
  }
  if (ready && g_wifi_sta_netif != nullptr &&
      esp_netif_get_ip_info(g_wifi_sta_netif, &ip_info) == ESP_OK) {
    ip_info_ptr = &ip_info;
  }
  send_network_ready_event(ready, ip_info_ptr);
}

void send_event(const char *event, const char *severity,
                const char *source = "esp32.safety",
                ReplyTarget target = UART_TARGET) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return;
  cJSON_AddStringToObject(root, "schema", "mibot.event.v1");
  cJSON_AddStringToObject(root, "event", event);
  cJSON_AddStringToObject(root, "severity", severity);
  cJSON_AddStringToObject(root, "source", source);
  cJSON_AddBoolToObject(root, "requires_ack", false);
  send_json(MSG_EVENT, 0, root, target);
  cJSON_Delete(root);
}

void send_action_event(const char *phase, const char *action,
                       const ActionStep *step, size_t step_index,
                       size_t step_count, float intensity,
                       ReplyTarget command_target) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return;
  cJSON_AddStringToObject(root, "schema", "mibot.event.v1");
  cJSON_AddStringToObject(root, "event", "action_update");
  cJSON_AddStringToObject(root, "severity", "info");
  cJSON_AddStringToObject(root, "source", "esp32.action");
  cJSON_AddBoolToObject(root, "requires_ack", false);
  cJSON *data = cJSON_AddObjectToObject(root, "data");
  cJSON_AddStringToObject(data, "phase", phase);
  cJSON_AddStringToObject(data, "action", action);
  cJSON_AddNumberToObject(data, "step", static_cast<double>(step_index + 1));
  cJSON_AddNumberToObject(data, "step_count", static_cast<double>(step_count));
  cJSON_AddStringToObject(data, "expression", step->expression);
  cJSON_AddNumberToObject(data, "expression_intensity",
                          step->expression_intensity * intensity);
  cJSON_AddNumberToObject(
      data, "left_servo_deg", 90.0 + (step->left_servo_deg - 90.0) * intensity);
  cJSON_AddNumberToObject(
      data, "right_servo_deg", 90.0 + (step->right_servo_deg - 90.0) * intensity);
  cJSON_AddNumberToObject(data, "left_motor", step->left_motor * intensity);
  cJSON_AddNumberToObject(data, "right_motor", step->right_motor * intensity);

  // A Wi-Fi action still needs to update the SF32 LCD. Send its progress to
  // the command origin and mirror it to the UART link for the local Agent.
  send_json(MSG_EVENT, 0, root, command_target);
  if (command_target.kind != TransportKind::Uart) {
    send_json(MSG_EVENT, 0, root, UART_TARGET);
  }
  cJSON_Delete(root);
}

void send_expression_update(const char *expression, float intensity,
                            uint16_t duration_ms, ReplyTarget command_target) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return;
  cJSON_AddStringToObject(root, "schema", "mibot.event.v1");
  cJSON_AddStringToObject(root, "event", "expression_update");
  cJSON_AddStringToObject(root, "severity", "info");
  cJSON_AddStringToObject(root, "source", "esp32.expression");
  cJSON_AddBoolToObject(root, "requires_ack", false);
  cJSON *data = cJSON_AddObjectToObject(root, "data");
  cJSON_AddStringToObject(data, "name", expression);
  cJSON_AddNumberToObject(data, "intensity", intensity);
  cJSON_AddNumberToObject(data, "duration_ms", duration_ms);
  send_json(MSG_EVENT, 0, root, command_target);
  if (command_target.kind != TransportKind::Uart) {
    send_json(MSG_EVENT, 0, root, UART_TARGET);
  }
  cJSON_Delete(root);
}

const char *motion_state_name(MotionState state) {
  switch (state) {
    case MotionState::Init: return "MOTION_INIT";
    case MotionState::Standby: return "STANDBY";
    case MotionState::Running: return "RUNNING";
    case MotionState::Braking: return "BRAKING";
    case MotionState::Hold: return "HOLD";
    case MotionState::Fault: return "FAULT";
  }
  return "FAULT";
}

// WS2812B timing at a 10 MHz RMT resolution (0.1 us per tick).
constexpr uint32_t STATUS_LED_RMT_RESOLUTION_HZ = 10000000;
constexpr rmt_symbol_word_t WS2812_ZERO = {
    .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0};
constexpr rmt_symbol_word_t WS2812_ONE = {
    .duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0};
constexpr rmt_symbol_word_t WS2812_RESET = {
    .duration0 = 250, .level0 = 0, .duration1 = 250, .level1 = 0};

size_t ws2812_encode(const void *data, size_t data_size,
                     size_t symbols_written, size_t symbols_free,
                     rmt_symbol_word_t *symbols, bool *done, void *) {
  if (symbols_free < 8) return 0;
  const auto *bytes = static_cast<const uint8_t *>(data);
  const size_t byte_index = symbols_written / 8;
  if (byte_index >= data_size) {
    symbols[0] = WS2812_RESET;
    *done = true;
    return 1;
  }

  const uint8_t value = bytes[byte_index];
  for (uint8_t bit = 0; bit < 8; ++bit) {
    symbols[bit] = (value & (0x80U >> bit)) != 0 ? WS2812_ONE : WS2812_ZERO;
  }
  return 8;
}

esp_err_t init_status_led() {
  rmt_tx_channel_config_t channel_config = {};
  channel_config.gpio_num = MIBOT_STATUS_LED_GPIO;
  channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
  channel_config.resolution_hz = STATUS_LED_RMT_RESOLUTION_HZ;
  channel_config.mem_block_symbols = 64;
  channel_config.trans_queue_depth = 2;
  MIBOT_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config,
                                            &g_status_led_channel),
                        "Status LED RMT init failed");

  rmt_simple_encoder_config_t encoder_config = {};
  encoder_config.callback = ws2812_encode;
  encoder_config.arg = nullptr;
  encoder_config.min_chunk_size = 8;
  MIBOT_RETURN_ON_ERROR(rmt_new_simple_encoder(&encoder_config,
                                               &g_status_led_encoder),
                        "Status LED encoder init failed");
  MIBOT_RETURN_ON_ERROR(rmt_enable(g_status_led_channel),
                        "Status LED RMT enable failed");
  return ESP_OK;
}

esp_err_t set_status_led_color(uint8_t red, uint8_t green, uint8_t blue) {
  if (g_status_led_channel == nullptr || g_status_led_encoder == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t grb[3] = {green, red, blue};
  rmt_transmit_config_t tx_config = {};
  tx_config.loop_count = 0;
  LockGuard lock(g_status_led_mutex);
  esp_err_t result = rmt_transmit(g_status_led_channel, g_status_led_encoder,
                                  grb, sizeof(grb), &tx_config);
  if (result == ESP_OK) {
    result = rmt_tx_wait_all_done(g_status_led_channel, 100);
  }
  if (result == ESP_OK) {
    LockGuard state_lock(g_state_mutex);
    g_state.status_led_on = red != 0 || green != 0 || blue != 0;
  }
  return result;
}

esp_err_t set_status_led(bool on) {
  return set_status_led_color(0, on ? 32 : 0, 0);
}

int16_t clamp_motor(int value) {
  return static_cast<int16_t>(std::max(-100, std::min(100, value)));
}

void set_motor(gpio_num_t in1, gpio_num_t in2, ledc_channel_t channel,
               int16_t speed) {
  speed = clamp_motor(speed);
  if (speed == 0) {
    gpio_set_level(in1, 0);
    gpio_set_level(in2, 0);
    ledc_set_duty(LEDC_MODE, channel, 0);
    ledc_update_duty(LEDC_MODE, channel);
    return;
  }
  const bool forward = speed > 0;
  gpio_set_level(in1, forward);
  gpio_set_level(in2, !forward);
  const uint32_t duty =
      static_cast<uint32_t>(std::abs(speed)) * MOTOR_MAX_DUTY / 100;
  ledc_set_duty(LEDC_MODE, channel, duty);
  ledc_update_duty(LEDC_MODE, channel);
}

void stop_motors(bool brake) {
  // TB6612 standby guarantees disabled outputs. Direction levels document the
  // requested stop mode but the board ultimately enters standby for safety.
  gpio_set_level(MIBOT_MOTOR_AIN1, brake);
  gpio_set_level(MIBOT_MOTOR_AIN2, brake);
  gpio_set_level(MIBOT_MOTOR_BIN1, brake);
  gpio_set_level(MIBOT_MOTOR_BIN2, brake);
  ledc_set_duty(LEDC_MODE, MOTOR_A_CHANNEL, 0);
  ledc_update_duty(LEDC_MODE, MOTOR_A_CHANNEL);
  ledc_set_duty(LEDC_MODE, MOTOR_B_CHANNEL, 0);
  ledc_update_duty(LEDC_MODE, MOTOR_B_CHANNEL);
  gpio_set_level(MIBOT_MOTOR_STBY, 0);
}

void set_servo(ledc_channel_t channel, float degrees) {
  degrees = std::max(0.0f, std::min(180.0f, degrees));
  const uint32_t pulse_us = 500 + static_cast<uint32_t>(2000 * degrees / 180);
  const uint32_t duty = pulse_us * SERVO_MAX_DUTY / 20000;
  ledc_set_duty(LEDC_MODE, channel, duty);
  ledc_update_duty(LEDC_MODE, channel);
}

esp_err_t init_pwm_and_gpio() {
  gpio_config_t output_config = {};
  output_config.pin_bit_mask = (1ULL << MIBOT_MOTOR_AIN1) |
                               (1ULL << MIBOT_MOTOR_AIN2) |
                               (1ULL << MIBOT_MOTOR_BIN1) |
                               (1ULL << MIBOT_MOTOR_BIN2) |
                               (1ULL << MIBOT_MOTOR_STBY);
  // INPUT_OUTPUT rather than OUTPUT so gpio_get_level() reads back the level
  // actually on the pin.  With plain GPIO_MODE_OUTPUT the input path is not
  // enabled and gpio_get_level() returns 0 unconditionally, which made the
  // "motor A test: ... STBY=%d AIN1=%d AIN2=%d" diagnostic print zeros even
  // when the outputs were driven correctly -- reading it as "STBY is low, the
  // driver is in standby" is a trap.  Enabling the input path costs nothing and
  // does not affect the output driver.
  output_config.mode = GPIO_MODE_INPUT_OUTPUT;
  output_config.pull_up_en = GPIO_PULLUP_DISABLE;
  output_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  output_config.intr_type = GPIO_INTR_DISABLE;
  MIBOT_RETURN_ON_ERROR(gpio_config(&output_config), "GPIO init failed");

  ledc_timer_config_t motor_timer = {};
  motor_timer.speed_mode = LEDC_MODE;
  motor_timer.duty_resolution = LEDC_TIMER_10_BIT;
  motor_timer.timer_num = MOTOR_TIMER;
  motor_timer.freq_hz = 20000;
  motor_timer.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config_t servo_timer = {};
  servo_timer.speed_mode = LEDC_MODE;
  servo_timer.duty_resolution = LEDC_TIMER_14_BIT;
  servo_timer.timer_num = SERVO_TIMER;
  servo_timer.freq_hz = 50;
  servo_timer.clk_cfg = LEDC_AUTO_CLK;
  MIBOT_RETURN_ON_ERROR(ledc_timer_config(&motor_timer),
                        "Motor timer init failed");
  MIBOT_RETURN_ON_ERROR(ledc_timer_config(&servo_timer),
                        "Servo timer init failed");

  ledc_channel_config_t channels[4] = {};
  channels[0].gpio_num = MIBOT_MOTOR_PWMA;
  channels[0].speed_mode = LEDC_MODE;
  channels[0].channel = MOTOR_A_CHANNEL;
  channels[0].timer_sel = MOTOR_TIMER;
  channels[1].gpio_num = MIBOT_MOTOR_PWMB;
  channels[1].speed_mode = LEDC_MODE;
  channels[1].channel = MOTOR_B_CHANNEL;
  channels[1].timer_sel = MOTOR_TIMER;
  channels[2].gpio_num = MIBOT_SERVO_LEFT;
  channels[2].speed_mode = LEDC_MODE;
  channels[2].channel = SERVO_LEFT_CHANNEL;
  channels[2].timer_sel = SERVO_TIMER;
  channels[3].gpio_num = MIBOT_SERVO_RIGHT;
  channels[3].speed_mode = LEDC_MODE;
  channels[3].channel = SERVO_RIGHT_CHANNEL;
  channels[3].timer_sel = SERVO_TIMER;
  for (const auto &channel : channels) {
    MIBOT_RETURN_ON_ERROR(ledc_channel_config(&channel),
                          "LEDC channel init failed");
  }
  stop_motors(false);
  set_servo(SERVO_LEFT_CHANNEL, 90.0f);
  set_servo(SERVO_RIGHT_CHANNEL, 90.0f);
  return ESP_OK;
}

esp_err_t init_uart() {
  uart_config_t config = {};
  config.baud_rate = MIBOT_SF32_BAUD;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.rx_flow_ctrl_thresh = 0;
  config.source_clk = UART_SCLK_DEFAULT;
  MIBOT_RETURN_ON_ERROR(uart_param_config(MIBOT_SF32_UART, &config),
                        "UART config failed");
  MIBOT_RETURN_ON_ERROR(
      uart_set_pin(MIBOT_SF32_UART, MIBOT_SF32_TX, MIBOT_SF32_RX,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
      "UART pin config failed");
  const esp_err_t result =
      /* Uplink PCM is a sustained ~32 kB/s stream on top of control traffic.  With
   * an 8 KB RX ring the measured peak occupancy reached 7177 bytes, so a brief
   * stall in this task overflowed the ring and silently lost AUDIO_UP frames
   * (observed as ~1/3 of the utterance missing and truncated fragments being
   * misread as metadata).  32 KB buys ~1 s of margin. */
  uart_driver_install(MIBOT_SF32_UART, 32768, 16384, 0, nullptr, 0);
  if (result == ESP_OK) {
    ESP_LOGI(TAG, "SF32 UART2: GPIO%d(TX)/GPIO%d(RX) @ %d 8N1, flow_control=off",
             MIBOT_SF32_TX, MIBOT_SF32_RX, MIBOT_SF32_BAUD);
  }
  return result;
}

esp_err_t init_i2c() {
  i2c_config_t config = {};
  config.mode = I2C_MODE_MASTER;
  config.sda_io_num = MIBOT_I2C_SDA;
  config.scl_io_num = MIBOT_I2C_SCL;
  config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  config.master.clk_speed = 400000;
  MIBOT_RETURN_ON_ERROR(i2c_param_config(MIBOT_I2C_PORT, &config),
                        "I2C config failed");
  return i2c_driver_install(MIBOT_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

// Distance for one channel. The TOF050C carries a VL6180X; the transactions and
// the four-sensor address dance live in mibot_tof.cpp so this stays a thin seam
// that a different sensor family can be dropped into.
//
// Any non-OK return leaves the reading invalid, which is what keeps motion
// locked. That is deliberate: an unimplemented, absent or confused sensor must
// never look like clear space.
esp_err_t tof_read_mm(uint8_t index, uint16_t *mm, uint8_t *quality) {
#if MIBOT_SIMULATE_TOF
  (void)index;
  *mm = 30;
  *quality = 90;
  return ESP_OK;
#elif !MIBOT_TOF_PRESENT
  // Hardware not fitted: report unusable rather than poll a bus with nothing on
  // it.  Same outcome as four dead sensors, so motion stays locked.
  (void)index;
  (void)mm;
  (void)quality;
  return ESP_ERR_NOT_SUPPORTED;
#else
  return mibot_tof_read(index, mm, quality);
#endif
}

/* Single place the ToF/edge safety gates consult, so MIBOT_TOF_SAFETY_ENFORCE
 * cannot be honoured in one gate and forgotten in another.  When enforcement is
 * off these report "everything is fine" rather than being commented out, which
 * keeps the call sites (and their error codes) intact for when it goes back on. */
bool tof_safety_enforced(void) { return MIBOT_TOF_SAFETY_ENFORCE != 0; }

bool tof_all_valid_locked(int64_t now) {
  for (const auto &reading : g_state.tof) {
    if (!reading.valid || now - reading.timestamp_ms > MIBOT_TOF_TIMEOUT_MS) {
      return false;
    }
  }
  return true;
}

bool front_edge_locked() {
  return (g_state.tof[0].valid &&
          g_state.tof[0].mm > MIBOT_EDGE_THRESHOLD_MM) ||
         (g_state.tof[1].valid &&
          g_state.tof[1].mm > MIBOT_EDGE_THRESHOLD_MM);
}

void reply_command(const char *command_id, bool ok, const char *error_code = nullptr,
                   const char *state = nullptr,
                   ReplyTarget target = UART_TARGET) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return;
  cJSON_AddStringToObject(root, "schema", "mibot.uart.v1");
  cJSON_AddBoolToObject(root, "ok", ok);
  if (command_id != nullptr) {
    cJSON_AddStringToObject(root, "command_id", command_id);
  }
  cJSON_AddStringToObject(root, "state",
                         state != nullptr ? state : (ok ? "accepted" : "rejected"));
  if (!ok) {
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(error, "code",
                           error_code != nullptr ? error_code : "E_INVALID_ARG");
  }
  send_json(ok ? MSG_ACK : MSG_NACK,
            FLAG_RESPONSE | (ok ? 0 : FLAG_ERROR), root, target);
  cJSON_Delete(root);
}

const char *json_string(cJSON *object, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsString(item) ? item->valuestring : nullptr;
}

double json_number(cJSON *object, const char *name, double default_value) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsNumber(item) ? item->valuedouble : default_value;
}

/*
 * Convert the legacy direction/speed shape used by early Skills to the
 * canonical differential-drive fields.  Keep this compatibility at the
 * controller boundary as well as in the SF32 Tool so direct Wi-Fi clients
 * cannot accidentally turn a valid legacy request into a zero-speed command.
 */
bool parse_move_args(cJSON *args, int *linear, int *angular) {
  if (args == nullptr || linear == nullptr || angular == nullptr) return false;

  cJSON *linear_item = cJSON_GetObjectItemCaseSensitive(args, "linear_mm_s");
  cJSON *angular_item = cJSON_GetObjectItemCaseSensitive(args, "angular_deg_s");
  if (linear_item != nullptr || angular_item != nullptr) {
    if (!cJSON_IsNumber(linear_item) || !cJSON_IsNumber(angular_item) ||
        !std::isfinite(linear_item->valuedouble) ||
        !std::isfinite(angular_item->valuedouble)) {
      return false;
    }
    *linear = static_cast<int>(linear_item->valuedouble);
    *angular = static_cast<int>(angular_item->valuedouble);
    return true;
  }

  cJSON *direction_item = cJSON_GetObjectItemCaseSensitive(args, "direction");
  cJSON *speed_item = cJSON_GetObjectItemCaseSensitive(args, "speed");
  if (!cJSON_IsString(direction_item) || !cJSON_IsNumber(speed_item) ||
      !std::isfinite(speed_item->valuedouble) ||
      speed_item->valuedouble < -100.0 || speed_item->valuedouble > 100.0) {
    return false;
  }

  double speed = speed_item->valuedouble;
  if (speed < 0.0) speed = -speed;
  *linear = 0;
  *angular = 0;
  if (strcmp(direction_item->valuestring, "forward") == 0) {
    *linear = static_cast<int>(speed);
  } else if (strcmp(direction_item->valuestring, "backward") == 0) {
    *linear = -static_cast<int>(speed);
  } else if (strcmp(direction_item->valuestring, "left") == 0 ||
             strcmp(direction_item->valuestring, "turn_left") == 0) {
    *angular = static_cast<int>(speed);
  } else if (strcmp(direction_item->valuestring, "right") == 0 ||
             strcmp(direction_item->valuestring, "turn_right") == 0) {
    *angular = -static_cast<int>(speed);
  } else {
    return false;
  }
  return true;
}

bool json_bool(cJSON *object, const char *name, bool default_value) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsBool(item) ? cJSON_IsTrue(item) : default_value;
}

bool action_expression_name_valid(const char *name) {
  static constexpr const char *NAMES[] = {
      "idle", "listening", "thinking", "happy", "sad", "confused",
      "surprised", "cute", "speaking", "warning", "error"};
  if (name == nullptr) return false;
  for (const char *item : NAMES) {
    if (strcmp(name, item) == 0) return true;
  }
  return false;
}

bool apply_action_step_locked(const ActionStep &step, float intensity,
                             int64_t now, const char **error) {
  const int16_t left_motor = static_cast<int16_t>(
      std::lround(static_cast<float>(step.left_motor) * intensity));
  const int16_t right_motor = static_cast<int16_t>(
      std::lround(static_cast<float>(step.right_motor) * intensity));
  const bool moving = left_motor != 0 || right_motor != 0;

  if (g_state.motion == MotionState::Fault ||
      g_state.motion == MotionState::Braking) {
    *error = "E_SAFETY_LOCK";
    return false;
  }
  if (tof_safety_enforced()) {
    if (moving && !tof_all_valid_locked(now)) {
      *error = "E_TOF_INVALID";
      return false;
    }
    if (moving && (left_motor > 0 || right_motor > 0) && front_edge_locked()) {
      *error = "E_SAFETY_LOCK";
      return false;
    }
  }

  const float left_servo =
      90.0f + (step.left_servo_deg - 90.0f) * intensity;
  const float right_servo =
      90.0f + (step.right_servo_deg - 90.0f) * intensity;
  set_servo(SERVO_LEFT_CHANNEL, left_servo);
  set_servo(SERVO_RIGHT_CHANNEL, right_servo);
  g_state.left_servo_deg = left_servo;
  g_state.right_servo_deg = right_servo;

  if (moving) {
    gpio_set_level(MIBOT_MOTOR_STBY, 1);
    set_motor(MIBOT_MOTOR_AIN1, MIBOT_MOTOR_AIN2, MOTOR_A_CHANNEL,
              left_motor);
    set_motor(MIBOT_MOTOR_BIN1, MIBOT_MOTOR_BIN2, MOTOR_B_CHANNEL,
              right_motor);
    g_state.left_target = left_motor;
    g_state.right_target = right_motor;
    g_state.motion = MotionState::Running;
  } else {
    stop_motors(false);
    g_state.left_target = 0;
    g_state.right_target = 0;
    g_state.motion = MotionState::Standby;
  }
  g_state.motion_deadline_ms = now + step.duration_ms;
  return true;
}

bool start_action(const ActionPlan *plan, float intensity,
                  const char *command_id, ReplyTarget target,
                  const char **error) {
  if (plan == nullptr) {
    *error = "E_UNSUPPORTED";
    return false;
  }
  if (intensity < 0.0f || intensity > 1.0f) {
    *error = "E_INVALID_ARG";
    return false;
  }

  const int64_t now = now_ms();
  {
    LockGuard lock(g_state_mutex);
    // A deliberate motor diagnostic must never be interrupted by an action.
    if (g_state.motor_test_active) {
      *error = "E_BUSY";
      return false;
    }
    // Safety locks are absolute.
    if (g_state.motion == MotionState::Fault ||
        g_state.motion == MotionState::Braking) {
      *error = "E_SAFETY_LOCK";
      return false;
    }
    // Preempt any in-progress bounded action rather than returning E_BUSY.
    // Every bounded action is device-owned and short, and the new action's
    // first step is still validated by apply_action_step_locked() below, so
    // cancelling the current one and starting the new one keeps every safety
    // guarantee while letting the dialog switch expressions/actions promptly
    // (e.g. idle -> listening -> thinking on state entry).  The preempted
    // action's requester is not sent a completion event; callers that care
    // use a timeout fallback.
    if (g_action.active || g_state.motion == MotionState::Running) {
      stop_motors(false);
      g_action = ActionRuntime{};
      g_state.action_name[0] = '\0';
      g_state.action_active = false;
      g_state.left_target = 0;
      g_state.right_target = 0;
      g_state.motion = MotionState::Standby;
    }
    g_action.active = true;
    g_action.plan = plan;
    g_action.step_index = 0;
    g_action.intensity = intensity;
    g_action.target = target;
    std::strncpy(g_action.command_id, command_id != nullptr ? command_id : "",
                 sizeof(g_action.command_id) - 1);
    g_action.command_id[sizeof(g_action.command_id) - 1] = '\0';
    std::strncpy(g_state.action_name, plan->name,
                 sizeof(g_state.action_name) - 1);
    g_state.action_name[sizeof(g_state.action_name) - 1] = '\0';
    g_state.action_active = true;
    if (!apply_action_step_locked(plan->steps[0], intensity, now, error)) {
      g_action = ActionRuntime{};
      g_state.action_name[0] = '\0';
      g_state.action_active = false;
      return false;
    }
    g_action.step_deadline_ms = g_state.motion_deadline_ms;
  }
  return true;
}

void action_task(void *) {
  while (true) {
    const int64_t now = now_ms();
    const ActionPlan *plan = nullptr;
    ActionStep step{};
    size_t step_index = 0;
    float intensity = 1.0f;
    ReplyTarget target = UART_TARGET;
    char command_id[ACTION_COMMAND_ID_MAX] = {};
    bool emit_step = false;
    bool emit_completed = false;
    bool emit_failed = false;
    const char *failure = nullptr;

    {
      LockGuard lock(g_state_mutex);
      if (g_action.active) {
        if (g_state.motion == MotionState::Braking ||
            g_state.motion == MotionState::Fault) {
          plan = g_action.plan;
          step_index = g_action.step_index;
          intensity = g_action.intensity;
          target = g_action.target;
          std::strncpy(command_id, g_action.command_id, sizeof(command_id) - 1);
          if (plan != nullptr && step_index < plan->step_count) {
            step = plan->steps[step_index];
          }
          g_action = ActionRuntime{};
          g_state.action_name[0] = '\0';
          g_state.action_active = false;
          emit_failed = true;
          failure = "E_SAFETY_LOCK";
        } else if (now >= g_action.step_deadline_ms) {
          plan = g_action.plan;
          step_index = g_action.step_index + 1;
          intensity = g_action.intensity;
          target = g_action.target;
          std::strncpy(command_id, g_action.command_id, sizeof(command_id) - 1);
          if (step_index >= plan->step_count) {
            stop_motors(false);
            g_state.left_target = 0;
            g_state.right_target = 0;
            g_state.motion = MotionState::Standby;
            g_state.action_name[0] = '\0';
            g_state.action_active = false;
            g_action = ActionRuntime{};
            step = plan->steps[plan->step_count - 1];
            emit_completed = true;
          } else if (apply_action_step_locked(plan->steps[step_index], intensity,
                                              now, &failure)) {
            g_action.step_index = step_index;
            g_action.step_deadline_ms = g_state.motion_deadline_ms;
            step = plan->steps[step_index];
            emit_step = true;
          } else {
            step = plan->steps[step_index];
            g_action = ActionRuntime{};
            g_state.action_name[0] = '\0';
            g_state.action_active = false;
            emit_failed = true;
          }
        }
      }
    }

    if (emit_step) {
      send_action_event("step", plan->name, &step, step_index,
                        plan->step_count, intensity, target);
    } else if (emit_completed) {
      send_action_event("completed", plan->name, &step, plan->step_count - 1,
                        plan->step_count, intensity, target);
      reply_command(command_id, true, nullptr, "completed", target);
    } else if (emit_failed) {
      if (plan != nullptr && plan->step_count > 0) {
        send_action_event("action_aborted", plan->name, &step, step_index,
                          plan->step_count, intensity, target);
      }
      send_event("action_aborted", "critical", "esp32.action", target);
      reply_command(command_id, false,
                    failure != nullptr ? failure : "E_SAFETY_LOCK", "rejected",
                    target);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void handle_command(const uint8_t *payload, uint16_t length,
                    ReplyTarget target = UART_TARGET) {
  cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(payload), length);
  if (root == nullptr || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    reply_command(nullptr, false, "E_INVALID_ARG", nullptr, target);
    return;
  }

  const char *name = json_string(root, "name");
  const char *command_id = json_string(root, "command_id");
  cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
  if (name == nullptr || !cJSON_IsObject(args)) {
    reply_command(command_id, false, "E_INVALID_ARG", nullptr, target);
    cJSON_Delete(root);
    return;
  }

  const AudioReplyTarget audio_target{
      target.kind == TransportKind::Uart ? 0 : 1, target.socket_fd};
  if (audio_link_handle_command(payload, length, &audio_target)) {
    cJSON_Delete(root);
    return;
  }

  if (strcmp(name, "robot.stop") == 0) {
    const bool emergency = json_bool(args, "emergency", false);
    {
      LockGuard lock(g_state_mutex);
      stop_motors(emergency);
      g_state.left_target = 0;
      g_state.right_target = 0;
      g_state.motion = emergency ? MotionState::Braking : MotionState::Standby;
      g_action = ActionRuntime{};
      g_state.motor_test_active = false;
      g_state.motor_test_deadline_ms = 0;
      g_state.motor_test_command_id[0] = '\0';
      g_state.action_name[0] = '\0';
      g_state.action_active = false;
    }
    audio_link_on_estop();
    reply_command(command_id, true, nullptr, nullptr, target);
  } else if (strcmp(name, "robot.test_motor_a") == 0) {
    const cJSON *simulate = cJSON_GetObjectItemCaseSensitive(args, "simulate_tof");
    const int speed = static_cast<int>(json_number(args, "speed", 30));
    const int duration = static_cast<int>(json_number(args, "duration_ms", 500));
    /* Which half-bridge to drive.  The command is still named test_motor_a for
     * compatibility, but a bench probe needs to reach motor B too: the original
     * form drove A and forced B to 0, so B had no test path at all.  Absent or
     * "a" keeps the historical behaviour. */
    const char *channel = json_string(args, "channel");
    if (channel == nullptr) channel = "a";
    const bool drive_a = strcmp(channel, "a") == 0 || strcmp(channel, "both") == 0;
    const bool drive_b = strcmp(channel, "b") == 0 || strcmp(channel, "both") == 0;
    const char *error = nullptr;
    if (!cJSON_IsTrue(simulate)) error = "E_SIMULATE_TOF_REQUIRED";
    else if (!drive_a && !drive_b) error = "E_INVALID_ARG";
    else if (speed < -50 || speed > 50 || speed == 0) error = "E_INVALID_ARG";
    /* Up to 30 s so the outputs can be held steady long enough to probe them
     * with a multimeter.  This is a deliberate diagnostic, not a motion
     * command: it still stops itself at the deadline, and the safety task skips
     * its lost-link brake only while motor_test_active is set, so a hung link
     * during the test is still bounded by that same deadline. */
    else if (duration < 50 || duration > 30000) error = "E_TEST_LIMIT";
    {
      LockGuard lock(g_state_mutex);
      if (error == nullptr && (g_action.active || g_state.motion == MotionState::Braking)) {
        error = "E_BUSY";
      }
      if (error == nullptr) {
        gpio_set_level(MIBOT_MOTOR_STBY, 1);
        set_motor(MIBOT_MOTOR_AIN1, MIBOT_MOTOR_AIN2, MOTOR_A_CHANNEL,
                  static_cast<int16_t>(drive_a ? speed : 0));
        set_motor(MIBOT_MOTOR_BIN1, MIBOT_MOTOR_BIN2, MOTOR_B_CHANNEL,
                  static_cast<int16_t>(drive_b ? speed : 0));
        g_state.left_target = drive_a ? speed : 0;
        g_state.right_target = drive_b ? speed : 0;
        g_state.motion_deadline_ms = now_ms() + duration;
        g_state.motor_test_deadline_ms = g_state.motion_deadline_ms;
        g_state.motor_test_active = true;
        std::strncpy(g_state.motor_test_command_id,
                     command_id != nullptr ? command_id : "",
                     sizeof(g_state.motor_test_command_id) - 1);
        g_state.motor_test_target = target;
        g_state.motion = MotionState::Running;
        // duty is read back from LEDC rather than recomputed, so the line
        // reports what the peripheral is actually driving.  The previous version
        // printed the expected value, which cannot distinguish "PWM is running
        // at 50%" from "the duty was never applied".
        ESP_LOGI(TAG, "motor test ch=%s: speed=%d duration_ms=%d STBY=%d "
                      "AIN1=%d AIN2=%d BIN1=%d BIN2=%d "
                      "PWMA duty=%lu/%lu PWMB duty=%lu",
                 channel,
                 speed, duration, gpio_get_level(MIBOT_MOTOR_STBY),
                 gpio_get_level(MIBOT_MOTOR_AIN1), gpio_get_level(MIBOT_MOTOR_AIN2),
                 gpio_get_level(MIBOT_MOTOR_BIN1), gpio_get_level(MIBOT_MOTOR_BIN2),
                 static_cast<unsigned long>(ledc_get_duty(LEDC_MODE, MOTOR_A_CHANNEL)),
                 static_cast<unsigned long>(MOTOR_MAX_DUTY),
                 static_cast<unsigned long>(ledc_get_duty(LEDC_MODE, MOTOR_B_CHANNEL)));
      }
    }
    reply_command(command_id, error == nullptr, error,
                  error == nullptr ? "accepted" : "rejected", target);
  } else if (strcmp(name, "robot.move") == 0) {
    int linear = 0;
    int angular = 0;
    const int duration = static_cast<int>(json_number(args, "duration_ms", 500));
    if (!parse_move_args(args, &linear, &angular) ||
        linear < -120 || linear > 120 || angular < -90 || angular > 90 ||
        duration < 50 || duration > 5000) {
      reply_command(command_id, false, "E_INVALID_ARG", nullptr, target);
      cJSON_Delete(root);
      return;
    }

    const bool forward_or_turning = linear > 0 || (linear == 0 && angular != 0);
    const int64_t now = now_ms();
    const char *error = nullptr;
    {
      LockGuard lock(g_state_mutex);
      if (g_action.active) {
        error = "E_BUSY";
      } else if (g_state.motion == MotionState::Fault ||
          g_state.motion == MotionState::Braking) {
        error = "E_SAFETY_LOCK";
      } else if (tof_safety_enforced() && !tof_all_valid_locked(now)) {
        error = "E_TOF_INVALID";
      } else if (tof_safety_enforced() && forward_or_turning &&
                 front_edge_locked()) {
        error = "E_SAFETY_LOCK";
      } else {
        g_state.left_target = clamp_motor(linear + angular);
        g_state.right_target = clamp_motor(linear - angular);
        gpio_set_level(MIBOT_MOTOR_STBY, 1);
        set_motor(MIBOT_MOTOR_AIN1, MIBOT_MOTOR_AIN2,
                  MOTOR_A_CHANNEL, g_state.left_target);
        set_motor(MIBOT_MOTOR_BIN1, MIBOT_MOTOR_BIN2,
                  MOTOR_B_CHANNEL, g_state.right_target);
        g_state.motion_deadline_ms = now + duration;
        g_state.motion = MotionState::Running;
      }
    }
    reply_command(command_id, error == nullptr, error, nullptr, target);
  } else if (strcmp(name, "robot.perform_action") == 0) {
    const char *action_name = json_string(args, "action");
    const float intensity = static_cast<float>(
        json_number(args, "intensity", 1.0));
    const ActionPlan *plan = find_action_plan(action_name);
    const char *error = nullptr;
    const bool started = start_action(plan, intensity, command_id, target, &error);
    reply_command(command_id, started, started ? nullptr : error,
                  started ? "accepted" : "rejected", target);
    if (started) {
      send_action_event("started", plan->name, &plan->steps[0], 0,
                        plan->step_count, intensity, target);
    }
  } else if (strcmp(name, "robot.set_expression") == 0) {
    const char *expression = json_string(args, "name");
    const float intensity = static_cast<float>(
        json_number(args, "intensity", 1.0));
    const int duration = static_cast<int>(
        json_number(args, "duration_ms", 1500));
    if (!action_expression_name_valid(expression) || intensity < 0.0f ||
        intensity > 1.0f || duration < 100 || duration > 10000) {
      reply_command(command_id, false, "E_INVALID_ARG", "rejected", target);
    } else {
      send_expression_update(expression, intensity,
                             static_cast<uint16_t>(duration), target);
      reply_command(command_id, true, nullptr, "completed", target);
    }
  } else if (strcmp(name, "robot.set_arm_pose") == 0) {
    float left;
    float right;
    bool action_active = false;
    {
      LockGuard lock(g_state_mutex);
      action_active = g_action.active;
      left = static_cast<float>(json_number(args, "left_deg",
                                            g_state.left_servo_deg));
      right = static_cast<float>(json_number(args, "right_deg",
                                             g_state.right_servo_deg));
    }
    if (action_active) {
      reply_command(command_id, false, "E_BUSY", "rejected", target);
    } else if (left < 0 || left > 180 || right < 0 || right > 180) {
      reply_command(command_id, false, "E_SERVO_LIMIT", nullptr, target);
    } else {
      set_servo(SERVO_LEFT_CHANNEL, left);
      set_servo(SERVO_RIGHT_CHANNEL, right);
      {
        LockGuard lock(g_state_mutex);
        g_state.left_servo_deg = left;
        g_state.right_servo_deg = right;
      }
      reply_command(command_id, true, nullptr, nullptr, target);
    }
  } else if (strcmp(name, "robot.get_status") == 0 ||
             strcmp(name, "robot.read_floor_sensors") == 0) {
    reply_command(command_id, true, nullptr, "completed", target);
  } else if (strcmp(name, "robot.set_led") == 0) {
    cJSON *on_item = cJSON_GetObjectItemCaseSensitive(args, "on");
    if (!cJSON_IsBool(on_item)) {
      reply_command(command_id, false, "E_INVALID_ARG", nullptr, target);
    } else {
      const esp_err_t result = set_status_led(cJSON_IsTrue(on_item));
      reply_command(command_id, result == ESP_OK,
                    result == ESP_OK ? nullptr : "E_LED_IO", nullptr, target);
    }
  } else if (strcmp(name, "robot.set_text") == 0 ||
             strcmp(name, "display.show_text") == 0) {
    /* LCD is owned by SF32. Forward the original JSON over the business UART;
     * the TCP caller gets an immediate transport acceptance. */
    char *forwarded = cJSON_PrintUnformatted(root);
    const size_t forwarded_len = forwarded != nullptr ? strlen(forwarded) : 0;
    const esp_err_t result = forwarded != nullptr &&
        forwarded_len <= MIBOT_MAX_FRAME_PAYLOAD
        ? send_frame(MSG_COMMAND, FLAG_ACK_REQUEST,
                     reinterpret_cast<const uint8_t *>(forwarded),
                     static_cast<uint16_t>(forwarded_len), UART_TARGET)
        : ESP_ERR_NO_MEM;
    if (forwarded != nullptr) cJSON_free(forwarded);
    reply_command(command_id, result == ESP_OK,
                  result == ESP_OK ? nullptr : "E_UART_WRITE", "accepted", target);
  } else {
    reply_command(command_id, false, "E_UNSUPPORTED", nullptr, target);
  }
  cJSON_Delete(root);
}

void send_hello_ack(ReplyTarget target = UART_TARGET) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "schema", "mibot.uart.v1");
  cJSON_AddStringToObject(root, "device", "esp32s3");
  cJSON_AddStringToObject(root, "firmware", "mibot-esp32-idf-0.1.0");
  cJSON_AddNumberToObject(root, "protocol_version", PROTOCOL_VERSION);
  cJSON_AddNumberToObject(root, "max_frame", MIBOT_MAX_FRAME_PAYLOAD);
  bool status_led_on = false;
  {
    LockGuard lock(g_state_mutex);
    status_led_on = g_state.status_led_on;
  }
  cJSON_AddBoolToObject(root, "status_led_on", status_led_on);
  cJSON *capabilities = cJSON_AddArrayToObject(root, "capabilities");
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("tof"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("motor"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("motor_test_a"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("servo"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("perform_action"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("expression_update"));
  cJSON *actions = cJSON_AddArrayToObject(root, "actions");
  for (const auto &plan : ACTION_PLANS) {
    cJSON_AddItemToArray(actions, cJSON_CreateString(plan.name));
  }
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("wifi_tcp_test"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("status_led"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("audio_uplink"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("audio_downlink"));
  cJSON_AddItemToArray(capabilities, cJSON_CreateString("audio_loopback"));
   cJSON_AddItemToArray(capabilities, cJSON_CreateString("audio_capture"));
   cJSON_AddItemToArray(capabilities, cJSON_CreateString("play_test_tone"));
#if defined(CONFIG_MIBOT_DEEPSEEK_GATEWAY) && CONFIG_MIBOT_DEEPSEEK_GATEWAY
   cJSON_AddItemToArray(capabilities, cJSON_CreateString("deepseek_gateway"));
#endif
   send_json(MSG_HELLO_ACK, FLAG_RESPONSE, root, target);
  cJSON_Delete(root);
}

void handle_frame(uint8_t type, uint8_t flags, const uint8_t *payload,
                  uint16_t length, uint16_t seq,
                  ReplyTarget target = UART_TARGET) {
  (void)flags;
  if (target.kind == TransportKind::Uart) {
    LockGuard lock(g_state_mutex);
    g_state.last_sf32_rx_ms = now_ms();
  }
  if (type == MSG_SHARED_NET_RX) {
    const esp_err_t result = mibot_shared_net_bridge_receive(
        type, flags, payload, length);
    if (result != ESP_OK && result != ESP_ERR_NOT_SUPPORTED) {
      ESP_LOGW(TAG, "shared network RX rejected: %s", esp_err_to_name(result));
    }
    return;
  }
  switch (type) {
    case MSG_AUDIO_UP:
      {
        const AudioReplyTarget audio_target{
            target.kind == TransportKind::Uart ? 0 : 1, target.socket_fd};
        audio_link_on_audio_up(flags, payload, length, seq, &audio_target);
      }
      break;
    case MSG_AUDIO_DOWN:
      audio_link_on_frame_error(MSG_AUDIO_DOWN);
      break;
    case MSG_COMMAND: handle_command(payload, length, target); break;
    case MSG_AI_REQUEST:
      if (target.kind != TransportKind::Uart) {
        send_ai_error_response("E_UNSUPPORTED",
                               "AI_REQUEST is only accepted from SF32 UART",
                               target);
        break;
      }
#if defined(CONFIG_MIBOT_DEEPSEEK_GATEWAY) && CONFIG_MIBOT_DEEPSEEK_GATEWAY
      {
        const esp_err_t result = mibot_deepseek_gateway_submit(
            payload, length, deepseek_response_callback, nullptr);
        ESP_LOGI(TAG, "AI_REQUEST ingress bytes=%u submit=%s",
                 static_cast<unsigned>(length), esp_err_to_name(result));
        if (result != ESP_OK) {
          send_ai_error_response(
              result == ESP_ERR_INVALID_STATE ? "E_GATEWAY_BUSY" : "E_GATEWAY_SUBMIT",
              result == ESP_ERR_INVALID_STATE ? "another DeepSeek request is in flight"
                                              : esp_err_to_name(result),
              target);
        }
      }
#else
      send_ai_error_response("E_GATEWAY_DISABLED",
                             "DeepSeek gateway is disabled in this build",
                             target);
#endif
      break;
    case MSG_PING:
      send_frame(MSG_PONG, FLAG_RESPONSE, payload, length, target);
      break;
    case MSG_HELLO:
      send_hello_ack(target);
      /* GOT_IP may have happened before the SF32 opened its UART.  Replay
       * the current state after every HELLO so that readiness is reliable
       * across boot ordering and UART reconnects. */
      send_current_network_ready_event();
      break;
    default: break;
  }
}

enum class ParserState { Sof0, Sof1, Header, Payload, Crc0, Crc1 };

struct FrameParser {
  ParserState state = ParserState::Sof0;
  uint8_t header[FRAME_HEADER_SIZE] = {};
  uint8_t payload[MIBOT_MAX_FRAME_PAYLOAD] = {};
  uint8_t crc_bytes[2] = {};
  size_t header_index = 0;
  uint16_t payload_index = 0;
  uint16_t payload_length = 0;
};

void feed_frame_parser(FrameParser *parser, const uint8_t *data, size_t length,
                       ReplyTarget target) {
  for (size_t index = 0; index < length; ++index) {
    const uint8_t byte = data[index];
    switch (parser->state) {
      case ParserState::Sof0:
        if (byte == SOF0) parser->state = ParserState::Sof1;
        break;
      case ParserState::Sof1:
        if (byte == SOF1) {
          parser->header_index = 0;
          parser->state = ParserState::Header;
        } else {
          parser->state = byte == SOF0 ? ParserState::Sof1 : ParserState::Sof0;
        }
        break;
      case ParserState::Header:
        parser->header[parser->header_index++] = byte;
        if (parser->header_index == sizeof(parser->header)) {
          parser->payload_length = static_cast<uint16_t>(parser->header[5]) |
                                   (static_cast<uint16_t>(parser->header[6]) << 8);
          if (parser->header[0] != PROTOCOL_VERSION ||
              parser->payload_length > MIBOT_MAX_FRAME_PAYLOAD) {
            if (parser->header[1] == MSG_AUDIO_UP || parser->header[1] == MSG_AUDIO_DOWN) {
              audio_link_on_frame_error(parser->header[1]);
            } else {
              reply_command(nullptr, false, "E_PROTOCOL_CRC", "rejected", target);
            }
            parser->state = ParserState::Sof0;
          } else {
            parser->payload_index = 0;
            parser->state = parser->payload_length > 0 ? ParserState::Payload
                                                        : ParserState::Crc0;
          }
        }
        break;
      case ParserState::Payload:
        parser->payload[parser->payload_index++] = byte;
        if (parser->payload_index == parser->payload_length) {
          parser->state = ParserState::Crc0;
        }
        break;
      case ParserState::Crc0:
        parser->crc_bytes[0] = byte;
        parser->state = ParserState::Crc1;
        break;
      case ParserState::Crc1: {
        parser->crc_bytes[1] = byte;
        uint16_t crc = crc16_append(0xFFFF, parser->header,
                                    sizeof(parser->header));
        crc = crc16_append(crc, parser->payload, parser->payload_length);
        const uint16_t received =
            static_cast<uint16_t>(parser->crc_bytes[0]) |
            (static_cast<uint16_t>(parser->crc_bytes[1]) << 8);
        if (crc == received) {
            const uint16_t sequence = static_cast<uint16_t>(parser->header[3]) |
                                      (static_cast<uint16_t>(parser->header[4]) << 8);
            handle_frame(parser->header[1], parser->header[2], parser->payload,
                       parser->payload_length, sequence, target);
        } else {
          if (parser->header[1] == MSG_AUDIO_UP || parser->header[1] == MSG_AUDIO_DOWN) {
            audio_link_on_frame_error(parser->header[1]);
          } else {
            reply_command(nullptr, false, "E_PROTOCOL_CRC", "rejected", target);
          }
        }
        parser->state = ParserState::Sof0;
        break;
      }
    }
  }
}

void uart_task(void *) {
  /* FrameParser embeds a 4 KiB payload buffer and the read buffer is another
   * KiB.  Keeping both on the task stack leaves too little for handle_frame(),
   * which parses JSON and can call into the LLM gateway; the resulting
   * overflow corrupted FreeRTOS structures and surfaced as a LoadProhibited
   * panic in unrelated tasks.  This task is a singleton, so static is safe. */
  static FrameParser parser;
  /* Larger reads keep up with the audio uplink burst rate. */
  static uint8_t input[1024];
  while (true) {
    const int count = uart_read_bytes(MIBOT_SF32_UART, input, sizeof(input),
                                      pdMS_TO_TICKS(20));
    if (count > 0) {
      const int64_t started_us = esp_timer_get_time();
      feed_frame_parser(&parser, input, static_cast<size_t>(count), UART_TARGET);
      size_t buffered = 0;
      uart_get_buffered_data_len(MIBOT_SF32_UART, &buffered);
      audio_link_record_uart_metrics(static_cast<uint32_t>(buffered),
                                     static_cast<uint32_t>((esp_timer_get_time() - started_us + 999) / 1000));
    }
  }
}

void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id,
                       void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
    audio_link_on_wifi_disconnected();
    send_network_ready_event(false);
    ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const auto *event = static_cast<const ip_event_got_ip_t *>(event_data);
    xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
    send_network_ready_event(true, &event->ip_info);
    ESP_LOGI(TAG, "Wi-Fi connected: IP=" IPSTR " TCP=%d",
             IP2STR(&event->ip_info.ip), MIBOT_WIFI_TCP_PORT);
  }
}

esp_err_t init_wifi_sta() {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    MIBOT_RETURN_ON_ERROR(nvs_flash_erase(), "NVS erase failed");
    result = nvs_flash_init();
  }
  MIBOT_RETURN_ON_ERROR(result, "NVS init failed");
  MIBOT_RETURN_ON_ERROR(esp_netif_init(), "Network interface init failed");
  MIBOT_RETURN_ON_ERROR(esp_event_loop_create_default(),
                        "Default event loop init failed");

  g_wifi_events = xEventGroupCreate();
  if (g_wifi_events == nullptr) return ESP_ERR_NO_MEM;
  g_wifi_sta_netif = esp_netif_create_default_wifi_sta();
  if (g_wifi_sta_netif == nullptr) return ESP_ERR_NO_MEM;

  MIBOT_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    &wifi_event_handler, nullptr),
                        "Wi-Fi event handler registration failed");
  MIBOT_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    &wifi_event_handler, nullptr),
                        "IP event handler registration failed");

  wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
  MIBOT_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), "Wi-Fi init failed");
  MIBOT_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),
                        "Wi-Fi STA mode failed");

  wifi_config_t sta_config = {};
  std::strncpy(reinterpret_cast<char *>(sta_config.sta.ssid), MIBOT_WIFI_STA_SSID,
               sizeof(sta_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char *>(sta_config.sta.password),
               MIBOT_WIFI_STA_PASSWORD, sizeof(sta_config.sta.password) - 1);
  /* Some development routers advertise WPA (authmode 2) rather than WPA2.
   * Accept both so the link can come up; the configured password still
   * protects the association. Production deployments should prefer WPA2/WPA3
   * on the access point. */
  sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
  MIBOT_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config),
                        "Wi-Fi STA config failed");
  MIBOT_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE),
                        "Wi-Fi power-save config failed");
  MIBOT_RETURN_ON_ERROR(esp_wifi_start(), "Wi-Fi start failed");
  ESP_LOGI(TAG, "Wi-Fi STA starting: SSID=%s TCP=%d",
           MIBOT_WIFI_STA_SSID, MIBOT_WIFI_TCP_PORT);
  return ESP_OK;
}

void wifi_tcp_task(void *) {
  const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listen_fd < 0) {
    ESP_LOGE(TAG, "TCP socket create failed: errno=%d", errno);
    vTaskDelete(nullptr);
    return;
  }

  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in server_address = {};
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(MIBOT_WIFI_TCP_PORT);
  if (bind(listen_fd, reinterpret_cast<sockaddr *>(&server_address),
           sizeof(server_address)) < 0 || listen(listen_fd, 1) < 0) {
    ESP_LOGE(TAG, "TCP listen failed: errno=%d", errno);
    close(listen_fd);
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    sockaddr_in client_address = {};
    socklen_t client_length = sizeof(client_address);
    const int client_fd = accept(
        listen_fd, reinterpret_cast<sockaddr *>(&client_address), &client_length);
    if (client_fd < 0) {
      ESP_LOGW(TAG, "TCP accept failed: errno=%d", errno);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    ESP_LOGI(TAG, "PC connected from %s", inet_ntoa(client_address.sin_addr));
    // Audio frames are emitted on a strict 20 ms cadence. Disable Nagle on
    // the test/control socket so the four protocol writes for one frame are
    // not delayed and coalesced with the next frame.
    int no_delay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    // The default lwIP windows are only a few kilobytes. A brief Wi-Fi
    // scheduling pause would otherwise make send() block while the audio TX
    // task is holding the global transport mutex.
    int send_buffer = 64 * 1024;
    int receive_buffer = 32 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    g_tcp_generation = g_tcp_generation + 1;
    g_tcp_active_fd = client_fd;
    audio_link_set_capture_fd(client_fd);
    /* Same reasoning as uart_task: a 4 KiB FrameParser plus the read buffer is
     * too much stack for a task that also parses JSON.  Only one TCP client is
     * served at a time, so a function-local static is safe here. */
    static FrameParser parser;
    static uint8_t input[512];
    parser = FrameParser{};
    while (true) {
      const int count = recv(client_fd, input, sizeof(input), 0);
      if (count <= 0) break;
      feed_frame_parser(&parser, input, static_cast<size_t>(count),
                        {TransportKind::WifiTcp, client_fd});
    }
    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);
    g_tcp_active_fd = -1;
    g_tcp_generation = g_tcp_generation + 1;
    audio_link_set_capture_fd(-1);
    audio_link_on_tcp_disconnected(client_fd);
    ESP_LOGI(TAG, "PC TCP connection closed");
  }
}

void safety_task(void *) {
  int64_t previous_period_ms = 0;
  TickType_t next_wakeup = xTaskGetTickCount();
  bool pending_event = false;
  const char *event_name = nullptr;
  while (true) {
    const int64_t now = now_ms();
    if (previous_period_ms != 0) {
      audio_link_record_safety_period(static_cast<uint32_t>(now - previous_period_ms));
    }
    previous_period_ms = now;
    TofReading readings[4];
    for (uint8_t index = 0; index < 4; ++index) {
      uint16_t mm = 0;
      uint8_t quality = 0;
      const esp_err_t result = tof_read_mm(index, &mm, &quality);
      readings[index] = {mm, quality, result == ESP_OK && quality > 0, now};
    }

    pending_event = false;
    event_name = nullptr;
    bool motor_test_completed = false;
    char motor_test_command_id[64] = {};
    ReplyTarget motor_test_target = UART_TARGET;
    {
      LockGuard lock(g_state_mutex);
      memcpy(g_state.tof, readings, sizeof(readings));
      if (g_state.motion == MotionState::Running) {
        if (tof_safety_enforced() && !g_state.motor_test_active &&
            !tof_all_valid_locked(now)) {
          stop_motors(true);
          g_state.left_target = 0;
          g_state.right_target = 0;
          g_state.motion = MotionState::Braking;
          pending_event = true;
          event_name = "tof_invalid";
        } else if (tof_safety_enforced() && !g_state.motor_test_active &&
                   (g_state.left_target > 0 || g_state.right_target > 0) &&
                   front_edge_locked()) {
          stop_motors(true);
          g_state.left_target = 0;
          g_state.right_target = 0;
          g_state.motion = MotionState::Braking;
          pending_event = true;
          event_name = "edge_detected";
        } else if (now >= g_state.motion_deadline_ms) {
          const bool was_motor_test = g_state.motor_test_active;
          stop_motors(false);
          g_state.left_target = 0;
          g_state.right_target = 0;
          g_state.motor_test_active = false;
          g_state.motor_test_deadline_ms = 0;
          g_state.motion = MotionState::Standby;
          if (was_motor_test) {
            std::strncpy(motor_test_command_id, g_state.motor_test_command_id,
                         sizeof(motor_test_command_id) - 1);
            motor_test_target = g_state.motor_test_target;
            g_state.motor_test_command_id[0] = '\0';
            motor_test_completed = true;
            ESP_LOGI(TAG, "motor test complete: outputs disabled");
          }
        } else if (!g_state.motor_test_active &&
                   now - g_state.last_sf32_rx_ms > 1500) {
          stop_motors(true);
          g_state.left_target = 0;
          g_state.right_target = 0;
          g_state.motion = MotionState::Braking;
          pending_event = true;
          event_name = "uart_timeout";
        }
      }
      if (g_state.motion == MotionState::Braking &&
          tof_all_valid_locked(now) && !front_edge_locked()) {
        g_state.motion = MotionState::Hold;
      }
    }
    if (pending_event) send_event(event_name, "critical");
    if (motor_test_completed) {
      reply_command(motor_test_command_id, true, nullptr, "completed",
                    motor_test_target);
    }
#if !MIBOT_SIMULATE_TOF && MIBOT_TOF_PRESENT
    // Re-adopt any channel that dropped off the bus. No-op while all four are
    // healthy; when one is down it costs a few ms of re-init, which the absolute
    // deadline below absorbs. Called outside the state lock because it sleeps.
    mibot_tof_maintain();
#endif
    // Keep the safety sampler on a fixed 50 ms phase. A relative delay would
    // add the sensor read/decision time to every period and accumulate drift.
    xTaskDelayUntil(&next_wakeup, pdMS_TO_TICKS(50));
  }
}

void send_telemetry() {
  RobotState snapshot;
  {
    LockGuard lock(g_state_mutex);
    snapshot = g_state;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "schema", "mibot.telemetry.v1");
  cJSON_AddNumberToObject(root, "ts_ms", static_cast<double>(now_ms()));
  cJSON_AddStringToObject(root, "motion_state",
                         motion_state_name(snapshot.motion));
  cJSON_AddNumberToObject(root, "battery_mv", 0);
  cJSON *tof = cJSON_AddObjectToObject(root, "tof");
  for (size_t index = 0; index < 4; ++index) {
    cJSON *item = cJSON_AddObjectToObject(tof, TOF_NAMES[index]);
    cJSON_AddNumberToObject(item, "mm", snapshot.tof[index].mm);
    cJSON_AddBoolToObject(item, "valid", snapshot.tof[index].valid);
    cJSON_AddNumberToObject(item, "quality", snapshot.tof[index].quality);
  }
  cJSON *motor = cJSON_AddObjectToObject(root, "motor");
  cJSON_AddNumberToObject(motor, "left", snapshot.left_target);
  cJSON_AddNumberToObject(motor, "right", snapshot.right_target);
  cJSON *servo = cJSON_AddObjectToObject(root, "servo");
  cJSON_AddNumberToObject(servo, "left_deg", snapshot.left_servo_deg);
  cJSON_AddNumberToObject(servo, "right_deg", snapshot.right_servo_deg);
  cJSON_AddBoolToObject(root, "action_active", snapshot.action_active);
  cJSON_AddStringToObject(root, "action", snapshot.action_name);
  cJSON_AddBoolToObject(root, "status_led_on", snapshot.status_led_on);
  audio_link_fill_telemetry(root);
  send_json(MSG_TELEMETRY, 0, root);
  cJSON_Delete(root);
}

void reporting_task(void *) {
  int64_t last_ping = 0;
  int64_t last_telemetry = 0;
  while (true) {
    const int64_t now = now_ms();
    if (now - last_ping >= 500) {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "ts_ms", static_cast<double>(now));
      send_json(MSG_PING, FLAG_ACK_REQUEST, root);
      cJSON_Delete(root);
      last_ping = now;
    }
    MotionState motion;
    {
      LockGuard lock(g_state_mutex);
      motion = g_state.motion;
    }
    const int64_t period = motion == MotionState::Running ? 100 : 1000;
    if (now - last_telemetry >= period) {
      send_telemetry();
      last_telemetry = now;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void log_task_contract() {
  const TaskHandle_t handles[] = {g_safety_task_handle, g_action_task_handle,
                                  g_uart_task_handle, g_wifi_tcp_task_handle,
                                  g_reporting_task_handle};
  const char *names[] = {"mibot_safety", "mibot_action", "mibot_uart",
                         "mibot_wifi_tcp", "mibot_report"};
  const UBaseType_t expected[] = {12, 10, 9, 8, 7};
  const BaseType_t expected_core[] = {1, 1, 1, tskNO_AFFINITY, 0};
  for (size_t index = 0; index < sizeof(handles) / sizeof(handles[0]); ++index) {
    if (handles[index] == nullptr) {
      ESP_LOGE(TAG, "%s task was not created", names[index]);
      continue;
    }
    const UBaseType_t priority = uxTaskPriorityGet(handles[index]);
    const BaseType_t core = xTaskGetCoreID(handles[index]);
    ESP_LOGI(TAG, "%s priority=%u core=%d", names[index],
             static_cast<unsigned>(priority), static_cast<int>(core));
    if (priority != expected[index] || core != expected_core[index])
      ESP_LOGE(TAG, "%s violates task contract", names[index]);
  }
  audio_link_log_task_contract();
}

}  // namespace

/* g_uart_tx_bytes lives in the anonymous namespace above, so it has internal
 * linkage and mibot_audio.cpp cannot name it.  Expose it through a C-linkage
 * accessor instead of moving the counter out of this translation unit. */
extern "C" uint32_t mibot_uart_tx_bytes(void) { return g_uart_tx_bytes; }

extern "C" void app_main(void) {
  g_state_mutex = xSemaphoreCreateMutex();
  g_tx_mutex = xSemaphoreCreateMutex();
  g_status_led_mutex = xSemaphoreCreateMutex();
  g_tcp_tx_queue = xQueueCreate(TCP_TX_QUEUE_DEPTH, sizeof(TcpTxPacket *));
  if (g_state_mutex == nullptr || g_tx_mutex == nullptr ||
      g_status_led_mutex == nullptr || g_tcp_tx_queue == nullptr) {
    ESP_LOGE(TAG, "Failed to create mutexes");
    return;
  }

  ESP_ERROR_CHECK(init_uart());
  ESP_ERROR_CHECK(init_i2c());
#if !MIBOT_SIMULATE_TOF && MIBOT_TOF_PRESENT
  // Must follow init_i2c(). Never fatal: a channel that does not come up stays
  // unusable and keeps motion locked, which is preferable to refusing to boot.
  ESP_ERROR_CHECK(mibot_tof_init());
#endif
  ESP_ERROR_CHECK(init_pwm_and_gpio());
  ESP_ERROR_CHECK(init_status_led());
  ESP_ERROR_CHECK(set_status_led(false));
  ESP_ERROR_CHECK(init_wifi_sta());
#if defined(CONFIG_MIBOT_NET_BRIDGE) && CONFIG_MIBOT_NET_BRIDGE
  ESP_ERROR_CHECK(mibot_net_bridge_start());
#endif
#if defined(CONFIG_MIBOT_SHARED_NET_BRIDGE) && CONFIG_MIBOT_SHARED_NET_BRIDGE
  ESP_ERROR_CHECK(mibot_shared_net_bridge_start(shared_net_send_frame, nullptr));
#endif
  audio_link_set_frame_sender(audio_send_frame_bridge, nullptr);
  const esp_err_t audio_result = audio_link_init();
  if (audio_result != ESP_OK) {
    ESP_LOGE(TAG, "Audio_Link disabled: %s", esp_err_to_name(audio_result));
  }
  {
    LockGuard lock(g_state_mutex);
    g_state.motion = MotionState::Standby;
    g_state.last_sf32_rx_ms = now_ms();
  }

  xTaskCreatePinnedToCore(uart_task, "mibot_uart", 8192, nullptr, 9, &g_uart_task_handle, 1);
  xTaskCreatePinnedToCore(tcp_tx_task, "mibot_tcp_tx", 4096, nullptr, 8,
                          &g_tcp_tx_task_handle, 0);
  xTaskCreatePinnedToCore(wifi_tcp_task, "mibot_wifi_tcp", 8192, nullptr, 8, &g_wifi_tcp_task_handle, tskNO_AFFINITY);
  xTaskCreatePinnedToCore(safety_task, "mibot_safety", 4096, nullptr, 12, &g_safety_task_handle, 1);
  xTaskCreatePinnedToCore(action_task, "mibot_action", 6144, nullptr, 10, &g_action_task_handle, 1);
  xTaskCreatePinnedToCore(reporting_task, "mibot_report", 6144, nullptr, 7, &g_reporting_task_handle, 0);
  log_task_contract();
  send_hello_ack();
  send_current_network_ready_event();
#if MIBOT_SIMULATE_TOF
  // Flag the stubbed ToF path so a simulated sensor is never mistaken for a
  // real one (需求 9.6).  Note this mode does NOT keep motion locked: it hands
  // the safety layer a valid-looking 30 mm on every channel, so the gate opens
  // on fabricated data.  That is why it is a bench-only switch.
  ESP_LOGW(TAG, "ToF SIMULATE mode active (MIBOT_SIMULATE_TOF): fabricated "
                "30 mm readings UNLOCK motion, do not use on a real chassis");
#elif !MIBOT_TOF_PRESENT
  // Hardware absent by configuration.  No bring-up, no 2 s recovery retries,
  // no console flood -- and every reading invalid, so motion stays locked and
  // motion commands keep answering E_TOF_INVALID (需求 9.4).
  ESP_LOGW(TAG, "ToF not fitted (MIBOT_TOF_PRESENT=0): not polling the bus; "
                "readings invalid, forward/turn motion stays locked");
#else
  // Real driver: TOF050C / VL6180X, four channels in continuous ranging.
  // A channel that did not come up leaves its readings invalid, which keeps
  // motion locked (需求 9.4) — so the count below is the thing to check when a
  // motion command is rejected with E_TOF_INVALID.
  ESP_LOGI(TAG, "ToF real mode: VL6180X x%d, %u/%d channels ready",
           MIBOT_TOF_COUNT,
           static_cast<unsigned>(mibot_tof_ready_count()), MIBOT_TOF_COUNT);
#endif
#if !MIBOT_TOF_SAFETY_ENFORCE
  ESP_LOGW(TAG, "*** ToF SAFETY DISABLED (MIBOT_TOF_SAFETY_ENFORCE=0) *** "
                "motion runs with no obstacle and no drop-off detection; "
                "do not run on a surface the robot can fall off");
#endif
  ESP_LOGI(TAG, "Mibot ESP-IDF controller started");
}
