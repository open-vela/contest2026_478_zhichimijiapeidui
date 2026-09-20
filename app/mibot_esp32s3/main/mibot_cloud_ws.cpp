#include "mibot_cloud.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM) && __has_include("esp_websocket_client.h")
#define MIBOT_HAVE_WS 1
#else
#define MIBOT_HAVE_WS 0
#endif

#if MIBOT_HAVE_WS
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <stdio.h>
#endif

namespace {
#if MIBOT_HAVE_WS
constexpr char TAG[] = "mibot_cloud";
struct WsImpl {
  esp_websocket_client_handle_t client = nullptr;
  cloud_callbacks_t callbacks{};
  cloud_request_ctx_t active_ctx{};
  SemaphoreHandle_t ctx_mutex = nullptr;
  volatile cloud_state_t state = CLOUD_STATE_DISCONNECTED;
  uint8_t *fragment = nullptr;
  size_t fragment_len = 0;
  size_t fragment_capacity = 0;
  cloud_frame_kind_t fragment_kind = CLOUD_FRAME_BINARY;
  /* Owned copy of the well-formed HTTP header block handed to the client. */
  char *headers = nullptr;
};

void notify_state(WsImpl *impl, cloud_state_t state) {
  impl->state = state;
  if (impl->callbacks.on_state != nullptr) impl->callbacks.on_state(state, impl->callbacks.user);
}

const char *ws_error_type_name(esp_websocket_error_type_t type) {
  switch (type) {
    case WEBSOCKET_ERROR_TYPE_NONE:          return "none";
    case WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT: return "tcp_transport";
    case WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT:  return "pong_timeout";
    case WEBSOCKET_ERROR_TYPE_HANDSHAKE:     return "handshake";
    default:                                 return "?";
  }
}

/* Session bookkeeping.  The gateway kept reporting close 1006 mid-utterance and
 * this handler used to be silent, so there was no way to tell a pong timeout
 * from a socket error from our own reconnect racing an live session.  Counting
 * sessions also exposes the case where two connections overlap. */
uint32_t g_ws_sessions = 0;
int64_t g_ws_connected_us = 0;

void ws_event_handler(void *arg, esp_event_base_t, int32_t event_id, void *event_data) {
  auto *impl = static_cast<WsImpl *>(arg);
  auto *event = static_cast<esp_websocket_event_data_t *>(event_data);
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      g_ws_connected_us = esp_timer_get_time();
      ESP_LOGI(TAG, "ws connected (session #%lu)",
               static_cast<unsigned long>(++g_ws_sessions));
      notify_state(impl, CLOUD_STATE_CONNECTED);
      break;
    case WEBSOCKET_EVENT_CLOSED:
      ESP_LOGW(TAG, "ws closed cleanly after %lld ms (session #%lu)",
               g_ws_connected_us == 0 ? -1
                 : (esp_timer_get_time() - g_ws_connected_us) / 1000,
               static_cast<unsigned long>(g_ws_sessions));
      notify_state(impl, CLOUD_STATE_DISCONNECTED);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG,
               "ws %s after %lld ms: type=%s ws_status=%d sock_errno=%d "
               "tls_err=0x%x tls_stack=0x%x heap=%lu",
               event_id == WEBSOCKET_EVENT_ERROR ? "error" : "disconnected",
               g_ws_connected_us == 0 ? -1
                 : (esp_timer_get_time() - g_ws_connected_us) / 1000,
               event != nullptr ? ws_error_type_name(event->error_handle.error_type) : "?",
               event != nullptr ? event->error_handle.esp_ws_handshake_status_code : -1,
               event != nullptr ? event->error_handle.esp_transport_sock_errno : -1,
               event != nullptr ? (unsigned)event->error_handle.esp_tls_last_esp_err : 0u,
               event != nullptr ? (unsigned)event->error_handle.esp_tls_stack_err : 0u,
               static_cast<unsigned long>(esp_get_free_heap_size()));
      notify_state(impl, CLOUD_STATE_DISCONNECTED);
      break;
    case WEBSOCKET_EVENT_DATA: {
      if (event == nullptr || event->data_ptr == nullptr || event->data_len == 0) break;
      // Control frames are not application data.
      if (event->op_code == 0x08 || event->op_code == 0x09 || event->op_code == 0x0A) break;
      const bool continuation = event->op_code == 0x00;
      if (!continuation) {
        impl->fragment_kind = event->op_code == 0x01 ? CLOUD_FRAME_TEXT : CLOUD_FRAME_BINARY;
        impl->fragment_len = 0;
      }
      const size_t needed = impl->fragment_len + static_cast<size_t>(event->data_len);
      if (needed > impl->fragment_capacity) {
        size_t capacity = impl->fragment_capacity == 0 ? 1024 : impl->fragment_capacity;
        while (capacity < needed) capacity *= 2;
        uint8_t *next = static_cast<uint8_t *>(realloc(impl->fragment, capacity));
        if (next == nullptr) {
          impl->fragment_len = 0;
          break;
        }
        impl->fragment = next;
        impl->fragment_capacity = capacity;
      }
      memcpy(impl->fragment + impl->fragment_len, event->data_ptr, event->data_len);
      impl->fragment_len = needed;
      const bool complete = event->payload_offset + event->data_len >= event->payload_len;
      if (complete && impl->callbacks.on_data != nullptr) {
        cloud_request_ctx_t ctx{};
        if (impl->ctx_mutex != nullptr && xSemaphoreTake(impl->ctx_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          ctx = impl->active_ctx;
          xSemaphoreGive(impl->ctx_mutex);
        }
        impl->callbacks.on_data(impl->fragment_kind, impl->fragment, impl->fragment_len, &ctx,
                                impl->callbacks.user);
        impl->fragment_len = 0;
      }
      break;
    }
    default:
      break;
  }
}

esp_err_t ws_connect(cloud_adapter_t *adapter) {
  auto *impl = static_cast<WsImpl *>(adapter->impl);
  if (impl == nullptr || impl->client == nullptr) return ESP_ERR_INVALID_STATE;
  notify_state(impl, CLOUD_STATE_CONNECTING);
  const esp_err_t result = esp_websocket_client_start(impl->client);
  if (result != ESP_OK) notify_state(impl, CLOUD_STATE_DISCONNECTED);
  return result;
}

esp_err_t ws_send(cloud_adapter_t *adapter, cloud_frame_kind_t kind, const uint8_t *data,
                  size_t length, const cloud_request_ctx_t *ctx, uint32_t timeout_ms) {
  auto *impl = static_cast<WsImpl *>(adapter->impl);
  if (impl == nullptr || impl->client == nullptr || impl->state != CLOUD_STATE_CONNECTED ||
      (length > 0 && data == nullptr)) return ESP_ERR_INVALID_STATE;
  if (ctx != nullptr && impl->ctx_mutex != nullptr && xSemaphoreTake(impl->ctx_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    impl->active_ctx = *ctx;
    xSemaphoreGive(impl->ctx_mutex);
  }
  const int64_t started_us = esp_timer_get_time();
  const int written = kind == CLOUD_FRAME_TEXT
      ? esp_websocket_client_send_text(impl->client, reinterpret_cast<const char *>(data), length,
                                       pdMS_TO_TICKS(timeout_ms))
      : esp_websocket_client_send_bin(impl->client, reinterpret_cast<const char *>(data), length,
                                      pdMS_TO_TICKS(timeout_ms));
  if (written != static_cast<int>(length)) {
    /* A partial or failed send used to be silent here and only bumped a
     * counter two layers up, so a transport that was already dying looked like
     * a clean session right up to the close.  The uplink sends 3200 B every
     * 100 ms against a 200 ms timeout, so this is also where Wi-Fi stalls
     * surface first. */
    ESP_LOGE(TAG, "ws send %s failed: wrote %d/%u in %lld ms (timeout %lu ms)",
             kind == CLOUD_FRAME_TEXT ? "text" : "bin", written,
             static_cast<unsigned>(length),
             (esp_timer_get_time() - started_us) / 1000,
             static_cast<unsigned long>(timeout_ms));
    return ESP_FAIL;
  }
  return ESP_OK;
}

cloud_state_t ws_state(const cloud_adapter_t *adapter) {
  const auto *impl = static_cast<const WsImpl *>(adapter->impl);
  return impl == nullptr ? CLOUD_STATE_DISCONNECTED : impl->state;
}

void ws_close(cloud_adapter_t *adapter) {
  if (adapter == nullptr) return;
  auto *impl = static_cast<WsImpl *>(adapter->impl);
  if (impl != nullptr) {
    if (impl->client != nullptr) {
      esp_websocket_client_stop(impl->client);
      esp_websocket_client_destroy(impl->client);
    }
    if (impl->ctx_mutex != nullptr) vSemaphoreDelete(impl->ctx_mutex);
    free(impl->fragment);
    free(impl->headers);
    delete impl;
  }
  free(adapter);
}

const cloud_adapter_vtable_t kVtable = {ws_connect, ws_send, ws_state, ws_close};
#endif
}  // namespace

extern "C" cloud_adapter_t *cloud_adapter_ws_create(const cloud_config_t *config,
                                                       const cloud_callbacks_t *callbacks) {
#if !MIBOT_HAVE_WS
  (void)config;
  (void)callbacks;
  return nullptr;
#else
  // An empty endpoint means cloud access is intentionally disabled for this
  // build (the local loopback stages do not require a cloud service).  Do not
  // pass it to esp_websocket_client: the transport assumes a host string and
  // would dereference a null host while parsing the URI.
  if (config == nullptr || config->endpoint == nullptr || config->endpoint[0] == '\0') return nullptr;
  auto *adapter = static_cast<cloud_adapter_t *>(calloc(1, sizeof(cloud_adapter_t)));
  auto *impl = new WsImpl{};
  if (adapter == nullptr || impl == nullptr) {
    free(adapter);
    delete impl;
    return nullptr;
  }
  impl->callbacks = callbacks != nullptr ? *callbacks : cloud_callbacks_t{};
  impl->ctx_mutex = xSemaphoreCreateMutex();
  esp_websocket_client_config_t cfg = {};
  cfg.uri = config->endpoint;

  // esp_websocket_client copies `headers` verbatim into the HTTP upgrade
  // request, so it must be well-formed header lines.  The configured
  // credential is just a bearer token, and passing it raw produced a
  // malformed handshake that the server closed (observed as
  // ESP_ERR_ESP_TLS_TCP_CLOSED_FIN with no session on the peer).
  if (config->credential != nullptr && config->credential[0] != '\0') {
    if (strstr(config->credential, ": ") != nullptr) {
      // Already a full header block supplied by the integrator.
      impl->headers = strdup(config->credential);
    } else {
      const char *prefix = "Authorization: Bearer ";
      const size_t size = strlen(prefix) + strlen(config->credential) + 3;
      impl->headers = static_cast<char *>(malloc(size));
      if (impl->headers != nullptr) {
        snprintf(impl->headers, size, "%s%s\r\n", prefix, config->credential);
      }
    }
    cfg.headers = impl->headers;
  }

  // Only attach the certificate bundle for TLS endpoints.  A plain ws:// URI
  // (used by the local mock gateway in tools/mock_cloud_audio.py) must go over
  // the TCP transport.
  const bool secure = strncmp(config->endpoint, "wss://", 6) == 0;
  if (secure) {
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.disable_auto_reconnect = true;

  // Keepalive: the client defaults to a 10 s ping and aborts the session when
  // it does not see a PONG in time, which showed up as the gateway session
  // being torn down (close 1006) roughly every 10 s while idle.  Give the
  // exchange more room and do not drop the session on a missed PONG; the
  // audio task already reconnects with backoff when the transport really dies.
  cfg.keep_alive_enable = true;
  cfg.ping_interval_sec = 15;
  cfg.pingpong_timeout_sec = 60;
  cfg.disable_pingpong_discon = true;
  cfg.buffer_size = config->buffer_size != 0 ? config->buffer_size : 4096;
  /* The client task runs the receive path, which reassembles fragments and
   * hands them to the audio queue; give it headroom over the 6 KiB default. */
  cfg.task_stack = 8192;
  cfg.task_prio = 5;
  /* network_timeout_ms aborts an in-flight network operation.  Keep the
   * library default (10 s) as the floor so an idle session is not torn down. */
  cfg.network_timeout_ms = config->network_timeout_ms > 10000 ? config->network_timeout_ms : 10000;
  impl->client = esp_websocket_client_init(&cfg);
  if (impl->client == nullptr || impl->ctx_mutex == nullptr) {
    if (impl->client != nullptr) esp_websocket_client_destroy(impl->client);
    if (impl->ctx_mutex != nullptr) vSemaphoreDelete(impl->ctx_mutex);
    free(impl->headers);
    delete impl;
    free(adapter);
    return nullptr;
  }
  esp_websocket_register_events(impl->client, WEBSOCKET_EVENT_ANY, ws_event_handler, impl);
  adapter->vtable = &kVtable;
  adapter->impl = impl;
  ESP_LOGI(TAG, "cloud endpoint configured (%s transport, credentials redacted)",
           secure ? "TLS" : "plain TCP");
  return adapter;
#endif
}
