#include "mibot_shared_net_bridge.h"

#include "sdkconfig.h"

#ifndef CONFIG_MIBOT_SHARED_NET_BRIDGE
#define CONFIG_MIBOT_SHARED_NET_BRIDGE 0
#endif
#ifndef CONFIG_LWIP_IPV4
#define CONFIG_LWIP_IPV4 0
#endif

#if CONFIG_MIBOT_SHARED_NET_BRIDGE && CONFIG_LWIP_IPV4
#define MIBOT_SHARED_NET_BRIDGE_REAL 1
#else
#define MIBOT_SHARED_NET_BRIDGE_REAL 0
#endif

#if MIBOT_SHARED_NET_BRIDGE_REAL

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/esp_netif_net_stack.h"

#ifndef CONFIG_MIBOT_SHARED_NET_MAX_PACKET
#define CONFIG_MIBOT_SHARED_NET_MAX_PACKET 2048
#endif
#ifndef CONFIG_MIBOT_SHARED_NET_RX_QUEUE_DEPTH
#define CONFIG_MIBOT_SHARED_NET_RX_QUEUE_DEPTH 8
#endif
#ifndef CONFIG_MIBOT_SHARED_NET_TX_QUEUE_DEPTH
#define CONFIG_MIBOT_SHARED_NET_TX_QUEUE_DEPTH 8
#endif
#ifndef CONFIG_MIBOT_SHARED_NET_TASK_STACK
#define CONFIG_MIBOT_SHARED_NET_TASK_STACK 3072
#endif
#ifndef CONFIG_MIBOT_SHARED_NET_LOCAL_IP
#define CONFIG_MIBOT_SHARED_NET_LOCAL_IP "10.42.0.1"
#endif
#ifndef CONFIG_MIBOT_SHARED_NET_PEER_IP
#define CONFIG_MIBOT_SHARED_NET_PEER_IP "10.42.0.2"
#endif
#ifndef CONFIG_MIBOT_SHARED_NET_NETMASK
#define CONFIG_MIBOT_SHARED_NET_NETMASK "255.255.255.252"
#endif
#ifndef CONFIG_MIBOT_SHARED_NET_MTU
#define CONFIG_MIBOT_SHARED_NET_MTU 1200
#endif
#ifndef CONFIG_MIBOT_SHARED_NET_NAPT
#define CONFIG_MIBOT_SHARED_NET_NAPT 0
#endif
#ifndef CONFIG_LWIP_IP_FORWARD
#define CONFIG_LWIP_IP_FORWARD 0
#endif
#ifndef CONFIG_LWIP_IPV4_NAPT
#define CONFIG_LWIP_IPV4_NAPT 0
#endif

#if CONFIG_MIBOT_SHARED_NET_NAPT && CONFIG_LWIP_IP_FORWARD && \
    CONFIG_LWIP_IPV4_NAPT
#define MIBOT_SHARED_NET_NAPT_REAL 1
#else
#define MIBOT_SHARED_NET_NAPT_REAL 0
#endif

namespace {

constexpr char TAG[] = "mibot_shared_net";
constexpr TickType_t QUEUE_WAIT = pdMS_TO_TICKS(100);
constexpr size_t IPV4_MIN_HEADER = 20;

/* Queue ownership is explicit: the producer allocates, and the worker frees. */
struct Packet {
  uint16_t length;
  uint8_t data[1];
};

struct BridgeContext {
  esp_netif_t *netif = nullptr;
  QueueHandle_t rx_queue = nullptr;
  QueueHandle_t tx_queue = nullptr;
  TaskHandle_t rx_task = nullptr;
  TaskHandle_t tx_task = nullptr;
  SemaphoreHandle_t lifecycle_lock = nullptr;
  mibot_shared_net_tx_fn tx = nullptr;
  void *tx_context = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> link_up{false};
  std::atomic<uint32_t> rx_bytes{0};
  std::atomic<uint32_t> tx_bytes{0};
  std::atomic<uint32_t> rx_dropped{0};
  std::atomic<uint32_t> tx_dropped{0};
  std::atomic<uint32_t> rx_errors{0};
  std::atomic<uint32_t> tx_errors{0};
  bool netif_started = false;
  bool napt_enabled = false;
};

BridgeContext g_ctx;

static_assert(CONFIG_MIBOT_SHARED_NET_MAX_PACKET > IPV4_MIN_HEADER &&
                  CONFIG_MIBOT_SHARED_NET_MAX_PACKET <= UINT16_MAX,
              "shared network packet size must fit an IPv4 packet and AA55 length");
static_assert(CONFIG_MIBOT_SHARED_NET_MTU >= IPV4_MIN_HEADER &&
                  CONFIG_MIBOT_SHARED_NET_MTU <=
                      CONFIG_MIBOT_SHARED_NET_MAX_PACKET,
              "shared network MTU must fit the packet limit");
static_assert(CONFIG_MIBOT_SHARED_NET_RX_QUEUE_DEPTH > 0,
              "shared network RX queue must not be empty");
static_assert(CONFIG_MIBOT_SHARED_NET_TX_QUEUE_DEPTH > 0,
              "shared network TX queue must not be empty");

bool valid_ipv4_packet(const uint8_t *data, size_t length) {
  if (data == nullptr || length < IPV4_MIN_HEADER ||
      length > CONFIG_MIBOT_SHARED_NET_MAX_PACKET) {
    return false;
  }

  const uint8_t version = static_cast<uint8_t>(data[0] >> 4);
  const size_t header_length = static_cast<size_t>(data[0] & 0x0f) * 4;
  if (version != 4 || header_length < IPV4_MIN_HEADER ||
      header_length > length) {
    return false;
  }

  const size_t total_length = (static_cast<size_t>(data[2]) << 8) | data[3];
  return total_length == length && total_length >= header_length;
}

Packet *packet_alloc(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0 ||
      length > CONFIG_MIBOT_SHARED_NET_MAX_PACKET || length > UINT16_MAX) {
    return nullptr;
  }
  const size_t bytes = offsetof(Packet, data) + length;
  auto *packet = static_cast<Packet *>(
      heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (packet == nullptr) return nullptr;
  packet->length = static_cast<uint16_t>(length);
  std::memcpy(packet->data, data, length);
  return packet;
}

Packet *packet_alloc_pbuf(struct pbuf *p) {
  if (p == nullptr || p->tot_len == 0 ||
      p->tot_len > CONFIG_MIBOT_SHARED_NET_MAX_PACKET) {
    return nullptr;
  }
  const size_t length = p->tot_len;
  const size_t bytes = offsetof(Packet, data) + length;
  auto *packet = static_cast<Packet *>(
      heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (packet == nullptr) return nullptr;
  packet->length = static_cast<uint16_t>(length);
  if (pbuf_copy_partial(p, packet->data, static_cast<u16_t>(length), 0) !=
          length ||
      !valid_ipv4_packet(packet->data, length)) {
    heap_caps_free(packet);
    return nullptr;
  }
  return packet;
}

void packet_free(Packet *packet) {
  if (packet != nullptr) heap_caps_free(packet);
}

void drain_queue(QueueHandle_t queue) {
  if (queue == nullptr) return;
  Packet *packet = nullptr;
  while (xQueueReceive(queue, &packet, 0) == pdTRUE) {
    packet_free(packet);
    packet = nullptr;
  }
}

esp_err_t enqueue_tx(Packet *packet) {
  if (packet == nullptr || g_ctx.tx_queue == nullptr ||
      !g_ctx.running.load(std::memory_order_acquire)) {
    g_ctx.tx_errors.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_INVALID_STATE;
  }
  if (xQueueSend(g_ctx.tx_queue, &packet, 0) != pdTRUE) {
    g_ctx.tx_dropped.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

/* Called by lwIP's TCP/IP thread. It never blocks on the AA55 UART. */
err_t raw_ip_output(struct netif *, struct pbuf *p, const ip4_addr_t *) {
  Packet *packet = packet_alloc_pbuf(p);
  if (packet == nullptr) {
    g_ctx.tx_errors.fetch_add(1, std::memory_order_relaxed);
    return ERR_MEM;
  }
  const esp_err_t result = enqueue_tx(packet);
  if (result != ESP_OK) {
    packet_free(packet);
    return result == ESP_ERR_TIMEOUT ? ERR_TIMEOUT : ERR_IF;
  }
  return ERR_OK;
}

/* A raw L3 netif has no Ethernet header, ARP, or PPP control bytes. */
err_t raw_l3_init(struct netif *netif) {
  if (netif == nullptr) return ERR_ARG;
  netif->name[0] = 's';
  netif->name[1] = 'f';
  netif->mtu = CONFIG_MIBOT_SHARED_NET_MTU;
  netif->flags = NETIF_FLAG_LINK_UP;
  netif->output = &raw_ip_output;
  netif->linkoutput = nullptr;
  netif->hwaddr_len = 0;
  return ERR_OK;
}

/* esp_netif_receive() gives us one complete IPv4 packet at a time. */
esp_err_t raw_l3_input(void *handle, void *buffer, size_t length, void *) {
  auto *netif = static_cast<struct netif *>(handle);
  if (netif == nullptr || buffer == nullptr ||
      !valid_ipv4_packet(static_cast<const uint8_t *>(buffer), length) ||
      netif->input == nullptr) {
    g_ctx.rx_errors.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_INVALID_ARG;
  }

  auto *p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(length), PBUF_POOL);
  if (p == nullptr) {
    g_ctx.rx_dropped.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_NO_MEM;
  }
  if (pbuf_take(p, buffer, length) != ERR_OK) {
    pbuf_free(p);
    g_ctx.rx_errors.fetch_add(1, std::memory_order_relaxed);
    return ESP_FAIL;
  }

  const err_t result = netif->input(p, netif);
  if (result != ERR_OK) {
    pbuf_free(p);
    g_ctx.rx_errors.fetch_add(1, std::memory_order_relaxed);
    return ESP_FAIL;
  }
  return ESP_OK;
}

/* This callback is reached through esp_netif_transmit() if a future stack
 * path uses that API directly. The normal raw_ip_output path queues itself so
 * that it can flatten chained pbufs without assuming contiguous storage. */
esp_err_t raw_driver_transmit(void *, void *buffer, size_t length) {
  if (buffer == nullptr || !valid_ipv4_packet(
                              static_cast<const uint8_t *>(buffer), length)) {
    g_ctx.tx_errors.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_INVALID_ARG;
  }
  Packet *packet = packet_alloc(static_cast<const uint8_t *>(buffer), length);
  if (packet == nullptr) {
    g_ctx.tx_dropped.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_NO_MEM;
  }
  const esp_err_t result = enqueue_tx(packet);
  if (result != ESP_OK) packet_free(packet);
  return result;
}

void shared_rx_task(void *) {
  while (g_ctx.running.load(std::memory_order_acquire)) {
    Packet *packet = nullptr;
    if (xQueueReceive(g_ctx.rx_queue, &packet, QUEUE_WAIT) != pdTRUE ||
        packet == nullptr) {
      continue;
    }
    const esp_err_t result = esp_netif_receive(
        g_ctx.netif, packet->data, packet->length, nullptr);
    if (result != ESP_OK) {
      g_ctx.rx_errors.fetch_add(1, std::memory_order_relaxed);
      ESP_LOGW(TAG, "IPv4 RX handoff failed: %s", esp_err_to_name(result));
    } else {
      g_ctx.rx_bytes.fetch_add(packet->length, std::memory_order_relaxed);
    }
    packet_free(packet);
  }
  g_ctx.rx_task = nullptr;
  vTaskDelete(nullptr);
}

void shared_tx_task(void *) {
  while (g_ctx.running.load(std::memory_order_acquire)) {
    Packet *packet = nullptr;
    if (xQueueReceive(g_ctx.tx_queue, &packet, QUEUE_WAIT) != pdTRUE ||
        packet == nullptr) {
      continue;
    }
    const auto tx = g_ctx.tx;
    const esp_err_t result =
        tx != nullptr
            ? tx(MIBOT_SHARED_NET_TX, 0, packet->data, packet->length,
                 g_ctx.tx_context)
            : ESP_ERR_INVALID_STATE;
    if (result != ESP_OK) {
      g_ctx.tx_errors.fetch_add(1, std::memory_order_relaxed);
      ESP_LOGW(TAG, "IPv4 TX AA55 handoff failed: %s", esp_err_to_name(result));
    } else {
      g_ctx.tx_bytes.fetch_add(packet->length, std::memory_order_relaxed);
    }
    packet_free(packet);
  }
  g_ctx.tx_task = nullptr;
  vTaskDelete(nullptr);
}

void cleanup_netif() {
  g_ctx.running.store(false, std::memory_order_release);
  g_ctx.link_up.store(false, std::memory_order_release);

  if (g_ctx.rx_task != nullptr) {
    vTaskDelete(g_ctx.rx_task);
    g_ctx.rx_task = nullptr;
  }
  if (g_ctx.tx_task != nullptr) {
    vTaskDelete(g_ctx.tx_task);
    g_ctx.tx_task = nullptr;
  }
  drain_queue(g_ctx.rx_queue);
  drain_queue(g_ctx.tx_queue);
  if (g_ctx.rx_queue != nullptr) {
    vQueueDelete(g_ctx.rx_queue);
    g_ctx.rx_queue = nullptr;
  }
  if (g_ctx.tx_queue != nullptr) {
    vQueueDelete(g_ctx.tx_queue);
    g_ctx.tx_queue = nullptr;
  }

  if (g_ctx.netif != nullptr) {
#if MIBOT_SHARED_NET_NAPT_REAL
    if (g_ctx.napt_enabled) {
      esp_netif_napt_disable(g_ctx.netif);
      g_ctx.napt_enabled = false;
    }
#endif
    if (g_ctx.netif_started) {
      esp_netif_action_stop(g_ctx.netif, nullptr, 0, nullptr);
      g_ctx.netif_started = false;
    }
    esp_netif_destroy(g_ctx.netif);
    g_ctx.netif = nullptr;
  }
  g_ctx.tx = nullptr;
  g_ctx.tx_context = nullptr;
}

bool parse_ipv4(const char *text, esp_ip4_addr_t *address) {
  return text != nullptr && address != nullptr &&
         esp_netif_str_to_ip4(text, address) == ESP_OK;
}

}  // namespace

extern "C" esp_err_t mibot_shared_net_bridge_start(
    mibot_shared_net_tx_fn tx, void *context) {
  if (tx == nullptr) return ESP_ERR_INVALID_ARG;
  if (g_ctx.lifecycle_lock == nullptr) {
    g_ctx.lifecycle_lock = xSemaphoreCreateMutex();
    if (g_ctx.lifecycle_lock == nullptr) return ESP_ERR_NO_MEM;
  }
  if (xSemaphoreTake(g_ctx.lifecycle_lock, portMAX_DELAY) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  if (g_ctx.running.load(std::memory_order_acquire)) {
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return ESP_ERR_INVALID_STATE;
  }

  esp_netif_ip_info_t ip_info = {};
  esp_ip4_addr_t peer_ip = {};
  if (!parse_ipv4(CONFIG_MIBOT_SHARED_NET_LOCAL_IP, &ip_info.ip) ||
      !parse_ipv4(CONFIG_MIBOT_SHARED_NET_NETMASK, &ip_info.netmask) ||
      !parse_ipv4(CONFIG_MIBOT_SHARED_NET_PEER_IP, &peer_ip)) {
    ESP_LOGE(TAG, "invalid raw IPv4 configuration");
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return ESP_ERR_INVALID_ARG;
  }
  ip_info.gw.addr = 0;

  esp_netif_inherent_config_t inherent = {};
  inherent.flags = ESP_NETIF_FLAG_AUTOUP;
  inherent.ip_info = &ip_info;
  inherent.get_ip_event = IP_EVENT_CUSTOM_GOT_IP;
  inherent.lost_ip_event = IP_EVENT_CUSTOM_LOST_IP;
  inherent.if_key = "MIBOT_SF32";
  inherent.if_desc = "sf32 raw IPv4";
  inherent.route_prio = 1;
  inherent.mtu = CONFIG_MIBOT_SHARED_NET_MTU;

  const esp_netif_netstack_config_t stack = {
      .lwip = {
          .init_fn = &raw_l3_init,
          .input_fn = &raw_l3_input,
      },
  };
  const esp_netif_config_t netif_config = {
      .base = &inherent,
      .driver = nullptr,
      .stack = &stack,
  };

  g_ctx.tx = tx;
  g_ctx.tx_context = context;
  g_ctx.rx_bytes.store(0, std::memory_order_relaxed);
  g_ctx.tx_bytes.store(0, std::memory_order_relaxed);
  g_ctx.rx_dropped.store(0, std::memory_order_relaxed);
  g_ctx.tx_dropped.store(0, std::memory_order_relaxed);
  g_ctx.rx_errors.store(0, std::memory_order_relaxed);
  g_ctx.tx_errors.store(0, std::memory_order_relaxed);
  g_ctx.link_up.store(false, std::memory_order_release);
  g_ctx.napt_enabled = false;

  g_ctx.netif = esp_netif_new(&netif_config);
  if (g_ctx.netif == nullptr) {
    cleanup_netif();
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return ESP_ERR_NO_MEM;
  }

  esp_netif_driver_ifconfig_t driver_config = {};
  driver_config.handle = &g_ctx;
  driver_config.transmit = &raw_driver_transmit;
  esp_err_t result = esp_netif_set_driver_config(g_ctx.netif, &driver_config);
  if (result != ESP_OK) {
    cleanup_netif();
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return result;
  }
  result = esp_netif_set_ip_info(g_ctx.netif, &ip_info);
  if (result != ESP_OK) {
    cleanup_netif();
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return result;
  }

  g_ctx.rx_queue = xQueueCreate(CONFIG_MIBOT_SHARED_NET_RX_QUEUE_DEPTH,
                                sizeof(Packet *));
  g_ctx.tx_queue = xQueueCreate(CONFIG_MIBOT_SHARED_NET_TX_QUEUE_DEPTH,
                                sizeof(Packet *));
  if (g_ctx.rx_queue == nullptr || g_ctx.tx_queue == nullptr) {
    cleanup_netif();
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return ESP_ERR_NO_MEM;
  }

  g_ctx.running.store(true, std::memory_order_release);
  if (xTaskCreate(shared_rx_task, "mibot_net_rx", CONFIG_MIBOT_SHARED_NET_TASK_STACK,
                  nullptr, 8, &g_ctx.rx_task) != pdPASS ||
      xTaskCreate(shared_tx_task, "mibot_net_tx", CONFIG_MIBOT_SHARED_NET_TASK_STACK,
                  nullptr, 8, &g_ctx.tx_task) != pdPASS) {
    cleanup_netif();
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return ESP_ERR_NO_MEM;
  }

  /* The interface is a static, always-present L3 peer; no PPP negotiation or
   * link event is needed. NAPT is attached to this ingress side so lwIP can
   * translate SF32-originated traffic before forwarding to Wi-Fi. */
  esp_netif_action_start(g_ctx.netif, nullptr, 0, nullptr);
  g_ctx.netif_started = true;
#if MIBOT_SHARED_NET_NAPT_REAL
  result = esp_netif_napt_enable(g_ctx.netif);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "NAPT enable failed: %s", esp_err_to_name(result));
    cleanup_netif();
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return result;
  }
  g_ctx.napt_enabled = true;
#endif
  g_ctx.link_up.store(true, std::memory_order_release);
  xSemaphoreGive(g_ctx.lifecycle_lock);

  ESP_LOGI(TAG, "raw IPv4 bridge ready: local=%s peer=%s netmask=%s mtu=%d",
           CONFIG_MIBOT_SHARED_NET_LOCAL_IP, CONFIG_MIBOT_SHARED_NET_PEER_IP,
           CONFIG_MIBOT_SHARED_NET_NETMASK, CONFIG_MIBOT_SHARED_NET_MTU);
  return ESP_OK;
}

extern "C" esp_err_t mibot_shared_net_bridge_stop(void) {
  if (g_ctx.lifecycle_lock == nullptr) return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(g_ctx.lifecycle_lock, portMAX_DELAY) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  if (!g_ctx.running.load(std::memory_order_acquire)) {
    xSemaphoreGive(g_ctx.lifecycle_lock);
    return ESP_ERR_INVALID_STATE;
  }
  cleanup_netif();
  xSemaphoreGive(g_ctx.lifecycle_lock);
  return ESP_OK;
}

extern "C" esp_err_t mibot_shared_net_bridge_receive(
    uint8_t type, uint8_t, const uint8_t *payload, uint16_t length) {
  if (type != MIBOT_SHARED_NET_RX) return ESP_ERR_NOT_SUPPORTED;
  if (payload == nullptr || !valid_ipv4_packet(payload, length)) {
    g_ctx.rx_errors.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_INVALID_ARG;
  }
  Packet *packet = packet_alloc(payload, length);
  if (packet == nullptr) {
    g_ctx.rx_dropped.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_NO_MEM;
  }
  if (g_ctx.lifecycle_lock == nullptr ||
      xSemaphoreTake(g_ctx.lifecycle_lock, 0) != pdTRUE) {
    packet_free(packet);
    g_ctx.rx_dropped.fetch_add(1, std::memory_order_relaxed);
    return ESP_ERR_INVALID_STATE;
  }
  const bool running = g_ctx.running.load(std::memory_order_acquire);
  const bool queued = running && g_ctx.rx_queue != nullptr &&
                      xQueueSend(g_ctx.rx_queue, &packet, 0) == pdTRUE;
  xSemaphoreGive(g_ctx.lifecycle_lock);
  if (!queued) {
    packet_free(packet);
    g_ctx.rx_dropped.fetch_add(1, std::memory_order_relaxed);
    return running ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

extern "C" esp_err_t mibot_shared_net_bridge_get_status(
    mibot_shared_net_status_t *status) {
  if (status == nullptr) return ESP_ERR_INVALID_ARG;
  *status = {};
  status->configured = true;
  status->running = g_ctx.running.load(std::memory_order_acquire);
  status->link_up = g_ctx.link_up.load(std::memory_order_acquire);
  status->backend = MIBOT_SHARED_NET_BACKEND_RAW_IPV4;
  status->rx_bytes = g_ctx.rx_bytes.load(std::memory_order_relaxed);
  status->tx_bytes = g_ctx.tx_bytes.load(std::memory_order_relaxed);
  status->rx_dropped = g_ctx.rx_dropped.load(std::memory_order_relaxed);
  status->tx_dropped = g_ctx.tx_dropped.load(std::memory_order_relaxed);
  status->rx_errors = g_ctx.rx_errors.load(std::memory_order_relaxed);
  status->tx_errors = g_ctx.tx_errors.load(std::memory_order_relaxed);
  return ESP_OK;
}

extern "C" bool mibot_shared_net_bridge_is_configured(void) { return true; }

extern "C" bool mibot_shared_net_bridge_is_link_up(void) {
  return g_ctx.link_up.load(std::memory_order_acquire);
}

#else  // MIBOT_SHARED_NET_BRIDGE_REAL

extern "C" esp_err_t mibot_shared_net_bridge_start(
    mibot_shared_net_tx_fn, void *) {
  return ESP_ERR_NOT_SUPPORTED;
}

extern "C" esp_err_t mibot_shared_net_bridge_stop(void) {
  return ESP_ERR_NOT_SUPPORTED;
}

extern "C" esp_err_t mibot_shared_net_bridge_receive(
    uint8_t, uint8_t, const uint8_t *, uint16_t) {
  return ESP_ERR_NOT_SUPPORTED;
}

extern "C" esp_err_t mibot_shared_net_bridge_get_status(
    mibot_shared_net_status_t *status) {
  if (status == nullptr) return ESP_ERR_INVALID_ARG;
  *status = {};
  status->configured = false;
  status->backend = MIBOT_SHARED_NET_BACKEND_DISABLED;
  return ESP_OK;
}

extern "C" bool mibot_shared_net_bridge_is_configured(void) { return false; }
extern "C" bool mibot_shared_net_bridge_is_link_up(void) { return false; }

#endif  // MIBOT_SHARED_NET_BRIDGE_REAL
