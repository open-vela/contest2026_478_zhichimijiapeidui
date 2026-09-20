#include "mibot_net_bridge.h"

#include "sdkconfig.h"

#ifndef CONFIG_MIBOT_NET_BRIDGE
#define CONFIG_MIBOT_NET_BRIDGE 0
#endif
#ifndef CONFIG_LWIP_PPP_SUPPORT
#define CONFIG_LWIP_PPP_SUPPORT 0
#endif
#ifndef CONFIG_LWIP_PPP_SERVER_SUPPORT
#define CONFIG_LWIP_PPP_SERVER_SUPPORT 0
#endif

#if CONFIG_MIBOT_NET_BRIDGE && CONFIG_LWIP_PPP_SUPPORT && \
    CONFIG_LWIP_PPP_SERVER_SUPPORT
#define MIBOT_NET_BRIDGE_REAL 1
#else
#define MIBOT_NET_BRIDGE_REAL 0
#endif

#if MIBOT_NET_BRIDGE_REAL

#include <atomic>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include "mibot_config.h"

#ifndef CONFIG_MIBOT_NET_BRIDGE_UART_NUM
#define CONFIG_MIBOT_NET_BRIDGE_UART_NUM 1
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO
#define CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO 17
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO
#define CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO 18
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_UART_BAUD
#define CONFIG_MIBOT_NET_BRIDGE_UART_BAUD 1000000
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_UART_RX_BUFFER
#define CONFIG_MIBOT_NET_BRIDGE_UART_RX_BUFFER 8192
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_UART_TX_BUFFER
#define CONFIG_MIBOT_NET_BRIDGE_UART_TX_BUFFER 8192
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_TASK_STACK
#define CONFIG_MIBOT_NET_BRIDGE_TASK_STACK 4096
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_LOCAL_IP
#define CONFIG_MIBOT_NET_BRIDGE_LOCAL_IP "10.42.0.1"
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_PEER_IP
#define CONFIG_MIBOT_NET_BRIDGE_PEER_IP "10.42.0.2"
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_DNS1
#define CONFIG_MIBOT_NET_BRIDGE_DNS1 "1.1.1.1"
#endif
#ifndef CONFIG_MIBOT_NET_BRIDGE_DNS2
#define CONFIG_MIBOT_NET_BRIDGE_DNS2 "8.8.8.8"
#endif

#ifndef CONFIG_MIBOT_NET_BRIDGE_NAPT
#define CONFIG_MIBOT_NET_BRIDGE_NAPT 0
#endif

namespace {

constexpr char TAG[] = "mibot_net_bridge";
constexpr size_t RX_CHUNK_SIZE = 1024;
constexpr uint32_t UART_TX_TIMEOUT_MS = 250;

struct BridgeContext {
  uart_port_t uart = UART_NUM_MAX;
  esp_netif_t *netif = nullptr;
  SemaphoreHandle_t tx_mutex = nullptr;
  TaskHandle_t rx_task = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> link_up{false};
  std::atomic<uint32_t> rx_bytes{0};
  std::atomic<uint32_t> tx_bytes{0};
  std::atomic<uint32_t> rx_errors{0};
  std::atomic<uint32_t> tx_errors{0};
  bool owns_uart = false;
  bool events_registered = false;
  bool netif_started = false;
};

BridgeContext g_ctx;

static_assert(CONFIG_MIBOT_NET_BRIDGE_UART_NUM >= 0 &&
                  CONFIG_MIBOT_NET_BRIDGE_UART_NUM < UART_NUM_MAX,
              "PPP bridge UART number is invalid");
static_assert(CONFIG_MIBOT_NET_BRIDGE_UART_NUM != MIBOT_SF32_UART,
              "PPP bridge must not reuse the AA55/audio UART");
static_assert(CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO != 35 &&
                  CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO != 36 &&
                  CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO != 37,
              "PPP bridge TX GPIO is reserved by octal PSRAM");
static_assert(CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO != 35 &&
                  CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO != 36 &&
                  CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO != 37,
              "PPP bridge RX GPIO is reserved by octal PSRAM");
static_assert(CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO !=
                  CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO,
              "PPP bridge TX and RX GPIOs must be different");
static_assert(CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO != MIBOT_SF32_TX &&
                  CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO != MIBOT_SF32_RX &&
                  CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO != MIBOT_SF32_TX &&
                  CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO != MIBOT_SF32_RX,
              "PPP bridge GPIOs must not reuse the AA55/audio UART pins");

bool parse_ipv4(const char *text, esp_ip4_addr_t *out) {
  if (text == nullptr || out == nullptr) return false;
  ip4_addr_t parsed = {};
  if (ip4addr_aton(text, &parsed) == 0) return false;
  out->addr = parsed.addr;
  return true;
}

esp_err_t ppp_uart_transmit(void *handle, void *buffer, size_t length) {
  auto *ctx = static_cast<BridgeContext *>(handle);
  if (ctx == nullptr || buffer == nullptr || length == 0 ||
      !ctx->running.load(std::memory_order_acquire) || ctx->uart >= UART_NUM_MAX) {
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(ctx->tx_mutex, pdMS_TO_TICKS(UART_TX_TIMEOUT_MS)) !=
      pdTRUE) {
    ctx->tx_errors.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_TIMEOUT;
  }
  const int written = uart_write_bytes(ctx->uart, buffer, length);
  xSemaphoreGive(ctx->tx_mutex);
  if (written != static_cast<int>(length)) {
    ctx->tx_errors.fetch_add(1, std::memory_order_relaxed);
    return ESP_FAIL;
  }
  ctx->tx_bytes.fetch_add(static_cast<uint32_t>(length),
                          std::memory_order_relaxed);
  return ESP_OK;
}

void ppp_event_handler(void *, esp_event_base_t base, int32_t event_id,
                       void *event_data) {
  if (base != IP_EVENT || event_data == nullptr) return;
  const auto *event = static_cast<const ip_event_got_ip_t *>(event_data);
  if (event->esp_netif != g_ctx.netif) return;

  if (event_id == IP_EVENT_PPP_GOT_IP) {
    g_ctx.link_up.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "PPP link up");
#if CONFIG_MIBOT_NET_BRIDGE_NAPT
    const esp_err_t napt_result = esp_netif_napt_enable(g_ctx.netif);
    if (napt_result != ESP_OK) {
      ESP_LOGE(TAG, "NAPT enable failed: %s", esp_err_to_name(napt_result));
    }
#endif
  } else if (event_id == IP_EVENT_PPP_LOST_IP) {
    g_ctx.link_up.store(false, std::memory_order_release);
    ESP_LOGW(TAG, "PPP link down");
  }
}

void ppp_rx_task(void *) {
  uint8_t buffer[RX_CHUNK_SIZE];
  while (g_ctx.running.load(std::memory_order_acquire)) {
    const int count = uart_read_bytes(g_ctx.uart, buffer, sizeof(buffer),
                                      pdMS_TO_TICKS(100));
    if (count <= 0) continue;
    g_ctx.rx_bytes.fetch_add(static_cast<uint32_t>(count),
                             std::memory_order_relaxed);
    // esp_netif_receive() copies/queues the bytes through IDF's PPPoS input
    // path. The UART buffer can therefore be reused on the next iteration.
    const esp_err_t result = esp_netif_receive(g_ctx.netif, buffer,
                                               static_cast<size_t>(count),
                                               nullptr);
    if (result != ESP_OK) {
      g_ctx.rx_errors.fetch_add(1, std::memory_order_relaxed);
      ESP_LOGW(TAG, "PPP RX handoff failed: %s", esp_err_to_name(result));
    }
  }
  vTaskDelete(nullptr);
}

void unregister_events() {
  if (!g_ctx.events_registered) return;
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                               &ppp_event_handler);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                               &ppp_event_handler);
  g_ctx.events_registered = false;
}

void cleanup_after_start_failure() {
  g_ctx.running.store(false, std::memory_order_release);
  if (g_ctx.rx_task != nullptr) {
    vTaskDelete(g_ctx.rx_task);
    g_ctx.rx_task = nullptr;
  }
  if (g_ctx.netif != nullptr && g_ctx.netif_started) {
    // esp_netif_start()/stop() are private in IDF 6.x.  The public action
    // wrappers are the supported application entry points and invoke the
    // same IPC-safe lifecycle operations.
    esp_netif_action_stop(g_ctx.netif, nullptr, 0, nullptr);
    g_ctx.netif_started = false;
  }
  unregister_events();
  if (g_ctx.netif != nullptr) {
    esp_netif_destroy(g_ctx.netif);
    g_ctx.netif = nullptr;
  }
  if (g_ctx.owns_uart) {
    uart_driver_delete(g_ctx.uart);
    g_ctx.owns_uart = false;
  }
  if (g_ctx.tx_mutex != nullptr) {
    vSemaphoreDelete(g_ctx.tx_mutex);
    g_ctx.tx_mutex = nullptr;
  }
  g_ctx.uart = UART_NUM_MAX;
}

}  // namespace

extern "C" esp_err_t mibot_net_bridge_start(void) {
  if (g_ctx.running.load(std::memory_order_acquire)) {
    return ESP_ERR_INVALID_STATE;
  }

  g_ctx.uart = static_cast<uart_port_t>(CONFIG_MIBOT_NET_BRIDGE_UART_NUM);
  if (uart_is_driver_installed(g_ctx.uart)) {
    ESP_LOGE(TAG, "UART%d is already owned; refusing to steal it",
             CONFIG_MIBOT_NET_BRIDGE_UART_NUM);
    g_ctx.uart = UART_NUM_MAX;
    return ESP_ERR_INVALID_STATE;
  }

  uart_config_t uart_config = {};
  uart_config.baud_rate = CONFIG_MIBOT_NET_BRIDGE_UART_BAUD;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_DISABLE;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = 0;
  uart_config.source_clk = UART_SCLK_DEFAULT;
  esp_err_t result = uart_param_config(g_ctx.uart, &uart_config);
  if (result != ESP_OK) {
    cleanup_after_start_failure();
    return result;
  }
  result = uart_set_pin(g_ctx.uart, CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO,
                        CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (result != ESP_OK) {
    cleanup_after_start_failure();
    return result;
  }
  result = uart_driver_install(g_ctx.uart, CONFIG_MIBOT_NET_BRIDGE_UART_RX_BUFFER,
                               CONFIG_MIBOT_NET_BRIDGE_UART_TX_BUFFER, 0,
                               nullptr, 0);
  if (result != ESP_OK) {
    cleanup_after_start_failure();
    return result;
  }
  g_ctx.owns_uart = true;
  g_ctx.rx_bytes.store(0, std::memory_order_relaxed);
  g_ctx.tx_bytes.store(0, std::memory_order_relaxed);
  g_ctx.rx_errors.store(0, std::memory_order_relaxed);
  g_ctx.tx_errors.store(0, std::memory_order_relaxed);
  g_ctx.link_up.store(false, std::memory_order_release);
  g_ctx.tx_mutex = xSemaphoreCreateMutex();
  if (g_ctx.tx_mutex == nullptr) {
    cleanup_after_start_failure();
    return ESP_ERR_NO_MEM;
  }

  esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_PPP();
  g_ctx.netif = esp_netif_new(&netif_config);
  if (g_ctx.netif == nullptr) {
    cleanup_after_start_failure();
    return ESP_ERR_NO_MEM;
  }

  esp_netif_driver_ifconfig_t driver_config = {};
  driver_config.handle = &g_ctx;
  driver_config.transmit = &ppp_uart_transmit;
  result = esp_netif_set_driver_config(g_ctx.netif, &driver_config);
  if (result != ESP_OK) {
    cleanup_after_start_failure();
    return result;
  }

  esp_netif_ppp_config_t ppp_config = {};
  ppp_config.ppp_phase_event_enabled = true;
  ppp_config.ppp_error_event_enabled = true;
  ppp_config.ppp_passive = true;
  if (!parse_ipv4(CONFIG_MIBOT_NET_BRIDGE_LOCAL_IP,
                  &ppp_config.ppp_our_ip4_addr) ||
      !parse_ipv4(CONFIG_MIBOT_NET_BRIDGE_PEER_IP,
                  &ppp_config.ppp_their_ip4_addr) ||
      !parse_ipv4(CONFIG_MIBOT_NET_BRIDGE_DNS1,
                  &ppp_config.ppp_dns1_addr) ||
      !parse_ipv4(CONFIG_MIBOT_NET_BRIDGE_DNS2,
                  &ppp_config.ppp_dns2_addr)) {
    ESP_LOGE(TAG, "Invalid PPP IPv4 configuration");
    cleanup_after_start_failure();
    return ESP_ERR_INVALID_ARG;
  }
  result = esp_netif_ppp_set_params(g_ctx.netif, &ppp_config);
  if (result != ESP_OK) {
    cleanup_after_start_failure();
    return result;
  }

  result = esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                      &ppp_event_handler, nullptr);
  if (result != ESP_OK) {
    cleanup_after_start_failure();
    return result;
  }
  result = esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                      &ppp_event_handler, nullptr);
  if (result != ESP_OK) {
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                 &ppp_event_handler);
    cleanup_after_start_failure();
    return result;
  }
  g_ctx.events_registered = true;

  g_ctx.running.store(true, std::memory_order_release);
  if (xTaskCreate(ppp_rx_task, "mibot_ppp_rx", CONFIG_MIBOT_NET_BRIDGE_TASK_STACK,
                  nullptr, 8, &g_ctx.rx_task) != pdPASS) {
    cleanup_after_start_failure();
    return ESP_ERR_NO_MEM;
  }

  // The direct esp_netif_start()/stop() functions are private in IDF 6.x.
  // Use the public event-action wrappers; PPP start is asynchronous after the
  // passive listener is installed, so the actual link state is reported by
  // IP_EVENT_PPP_GOT_IP/LOST_IP below.
  esp_netif_action_start(g_ctx.netif, nullptr, 0, nullptr);
  g_ctx.netif_started = true;

  ESP_LOGI(TAG,
           "PPP server ready on UART%d GPIO%d/%d @ %d; peer=%s local=%s",
           CONFIG_MIBOT_NET_BRIDGE_UART_NUM,
           CONFIG_MIBOT_NET_BRIDGE_UART_TX_GPIO,
           CONFIG_MIBOT_NET_BRIDGE_UART_RX_GPIO,
           CONFIG_MIBOT_NET_BRIDGE_UART_BAUD, CONFIG_MIBOT_NET_BRIDGE_PEER_IP,
           CONFIG_MIBOT_NET_BRIDGE_LOCAL_IP);
  return ESP_OK;
}

extern "C" esp_err_t mibot_net_bridge_stop(void) {
  if (!g_ctx.running.load(std::memory_order_acquire)) {
    return ESP_ERR_INVALID_STATE;
  }
  g_ctx.running.store(false, std::memory_order_release);
  g_ctx.link_up.store(false, std::memory_order_release);
  // Stop the reader before destroying the netif it hands bytes to.
  if (g_ctx.rx_task != nullptr) {
    vTaskDelete(g_ctx.rx_task);
    g_ctx.rx_task = nullptr;
  }
  if (g_ctx.netif != nullptr) {
#if CONFIG_MIBOT_NET_BRIDGE_NAPT
    esp_netif_napt_disable(g_ctx.netif);
#endif
    // See the start path above: action_stop is the public IDF lifecycle API.
    esp_netif_action_stop(g_ctx.netif, nullptr, 0, nullptr);
    g_ctx.netif_started = false;
    esp_netif_destroy(g_ctx.netif);
    g_ctx.netif = nullptr;
  }
  unregister_events();
  if (g_ctx.owns_uart) {
    uart_driver_delete(g_ctx.uart);
    g_ctx.owns_uart = false;
  }
  if (g_ctx.tx_mutex != nullptr) {
    vSemaphoreDelete(g_ctx.tx_mutex);
    g_ctx.tx_mutex = nullptr;
  }
  g_ctx.uart = UART_NUM_MAX;
  return ESP_OK;
}

extern "C" esp_err_t mibot_net_bridge_get_status(
    mibot_net_bridge_status_t *status) {
  if (status == nullptr) return ESP_ERR_INVALID_ARG;
  status->configured = true;
  status->running = g_ctx.running.load(std::memory_order_acquire);
  status->link_up = g_ctx.link_up.load(std::memory_order_acquire);
  status->backend = MIBOT_NET_BRIDGE_BACKEND_PPP_SERVER;
  status->rx_bytes = g_ctx.rx_bytes.load(std::memory_order_relaxed);
  status->tx_bytes = g_ctx.tx_bytes.load(std::memory_order_relaxed);
  status->rx_errors = g_ctx.rx_errors.load(std::memory_order_relaxed);
  status->tx_errors = g_ctx.tx_errors.load(std::memory_order_relaxed);
  return ESP_OK;
}

extern "C" bool mibot_net_bridge_is_configured(void) { return true; }

#else  // MIBOT_NET_BRIDGE_REAL

extern "C" esp_err_t mibot_net_bridge_start(void) {
  // Keep the default AA55/audio image resource-neutral. In particular, do not
  // install a second UART driver or initialize PPP when the option is off.
  return ESP_ERR_NOT_SUPPORTED;
}

extern "C" esp_err_t mibot_net_bridge_stop(void) {
  return ESP_ERR_NOT_SUPPORTED;
}

extern "C" esp_err_t mibot_net_bridge_get_status(
    mibot_net_bridge_status_t *status) {
  if (status == nullptr) return ESP_ERR_INVALID_ARG;
  *status = {};
  status->configured = false;
  status->backend = CONFIG_MIBOT_NET_BRIDGE
                        ? MIBOT_NET_BRIDGE_BACKEND_SLIP
                        : MIBOT_NET_BRIDGE_BACKEND_DISABLED;
  return ESP_OK;
}

extern "C" bool mibot_net_bridge_is_configured(void) { return false; }

#endif  // MIBOT_NET_BRIDGE_REAL
