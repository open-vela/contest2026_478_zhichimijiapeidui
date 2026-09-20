#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The callback runs on the gateway worker task, never on the UART parser. */
typedef void (*mibot_deepseek_response_fn)(bool ok, const char *payload,
                                            uint16_t length, void *context);

/* Queue one bounded request for the DeepSeek gateway worker. */
esp_err_t mibot_deepseek_gateway_submit(
    const uint8_t *request, uint16_t length,
    mibot_deepseek_response_fn callback, void *context);

#ifdef __cplusplus
}
#endif
