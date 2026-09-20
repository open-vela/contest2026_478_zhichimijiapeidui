#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The bridge exposes a native RFC1055 SLIP endpoint for the SF32 Route-A
 * network link.  It is deliberately independent from the existing AA55
 * control/audio UART.
 */
typedef enum mibot_net_bridge_backend {
  MIBOT_NET_BRIDGE_BACKEND_DISABLED = 0,
  MIBOT_NET_BRIDGE_BACKEND_SLIP = 1,
  MIBOT_NET_BRIDGE_BACKEND_PPP_SERVER = 2,
} mibot_net_bridge_backend_t;

typedef struct mibot_net_bridge_status {
  bool configured;
  bool running;
  bool link_up;
  mibot_net_bridge_backend_t backend;
  uint32_t rx_bytes;
  uint32_t tx_bytes;
  uint32_t rx_errors;
  uint32_t tx_errors;
} mibot_net_bridge_status_t;

/**
 * Start the optional native SLIP bridge.
 *
 * With CONFIG_MIBOT_NET_BRIDGE unset this function returns
 * ESP_ERR_NOT_SUPPORTED and does not claim any UART or network resources.
 * When enabled, the caller must have initialized esp-netif and the default
 * event loop (the existing controller does this while bringing up Wi-Fi).
 */
esp_err_t mibot_net_bridge_start(void);

/** Stop and release the optional PPP bridge. */
esp_err_t mibot_net_bridge_stop(void);

/** Return a snapshot of bridge state and bounded I/O counters. */
esp_err_t mibot_net_bridge_get_status(mibot_net_bridge_status_t *status);

/** Return whether the build-time bridge option is enabled. */
bool mibot_net_bridge_is_configured(void);

#ifdef __cplusplus
}
#endif
