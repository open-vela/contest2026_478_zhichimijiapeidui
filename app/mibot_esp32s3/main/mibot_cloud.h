#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG 0x102
#endif
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE 0x103
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cloud_state_t {
  CLOUD_STATE_DISCONNECTED = 0,
  CLOUD_STATE_CONNECTING = 1,
  CLOUD_STATE_CONNECTED = 2,
} cloud_state_t;

typedef enum cloud_frame_kind_t {
  CLOUD_FRAME_TEXT = 1,
  CLOUD_FRAME_BINARY = 2,
} cloud_frame_kind_t;

typedef struct cloud_request_ctx_t {
  char trace_id[64];
  char command_id[64];
  int64_t created_at_ms;
  int64_t expires_at_ms;
} cloud_request_ctx_t;

typedef void (*cloud_on_data_fn)(cloud_frame_kind_t kind, const uint8_t *data,
                                 size_t length,
                                 const cloud_request_ctx_t *ctx, void *user);
typedef void (*cloud_on_state_fn)(cloud_state_t state, void *user);

typedef struct cloud_callbacks_t {
  cloud_on_data_fn on_data;
  cloud_on_state_fn on_state;
  void *user;
} cloud_callbacks_t;

typedef struct cloud_config_t {
  const char *endpoint;
  const char *credential;
  uint32_t buffer_size;
  uint32_t network_timeout_ms;
} cloud_config_t;

typedef struct cloud_adapter_t cloud_adapter_t;
typedef struct cloud_adapter_vtable_t {
  esp_err_t (*connect)(cloud_adapter_t *adapter);
  esp_err_t (*send)(cloud_adapter_t *adapter, cloud_frame_kind_t kind,
                    const uint8_t *data, size_t length,
                    const cloud_request_ctx_t *ctx, uint32_t timeout_ms);
  cloud_state_t (*state)(const cloud_adapter_t *adapter);
  void (*close)(cloud_adapter_t *adapter);
} cloud_adapter_vtable_t;

struct cloud_adapter_t {
  const cloud_adapter_vtable_t *vtable;
  void *impl;
};

// The contract intentionally exposes only generic endpoint/credential fields;
// service-specific names and authentication formats belong in the backend.
cloud_adapter_t *cloud_adapter_ws_create(const cloud_config_t *config,
                                          const cloud_callbacks_t *callbacks);

#ifdef __cplusplus
}
#endif
