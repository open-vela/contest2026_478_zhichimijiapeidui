#pragma once

/*
 * Route-A transport over the existing SF32 AA55 UART. Each 0x70/0x71
 * payload is one complete IPv4 packet. AA55 framing, sequencing and CRC are
 * owned by mibot_controller.cpp; this module only bridges raw L3 packets.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MIBOT_SHARED_NET_RX = 0x70, /* SF32 -> ESP32 */
  MIBOT_SHARED_NET_TX = 0x71, /* ESP32 -> SF32 */
};

typedef esp_err_t (*mibot_shared_net_tx_fn)(uint8_t type, uint8_t flags,
                                             const uint8_t *payload,
                                             uint16_t length, void *context);

typedef enum mibot_shared_net_backend {
  MIBOT_SHARED_NET_BACKEND_DISABLED = 0,
  MIBOT_SHARED_NET_BACKEND_RAW_IPV4 = 1,
} mibot_shared_net_backend_t;

typedef struct mibot_shared_net_status {
  bool configured;
  bool running;
  bool link_up;
  mibot_shared_net_backend_t backend;
  uint32_t rx_bytes;
  uint32_t tx_bytes;
  uint32_t rx_dropped;
  uint32_t tx_dropped;
  uint32_t rx_errors;
  uint32_t tx_errors;
} mibot_shared_net_status_t;

/* Start the static raw-IPv4 interface and its bounded packet workers. */
esp_err_t mibot_shared_net_bridge_start(mibot_shared_net_tx_fn tx,
                                         void *context);

esp_err_t mibot_shared_net_bridge_stop(void);

/* Called by the AA55 parser after version, length and CRC validation. */
esp_err_t mibot_shared_net_bridge_receive(uint8_t type, uint8_t flags,
                                          const uint8_t *payload,
                                          uint16_t length);

esp_err_t mibot_shared_net_bridge_get_status(mibot_shared_net_status_t *status);

bool mibot_shared_net_bridge_is_configured(void);
bool mibot_shared_net_bridge_is_link_up(void);

#ifdef __cplusplus
}
#endif
