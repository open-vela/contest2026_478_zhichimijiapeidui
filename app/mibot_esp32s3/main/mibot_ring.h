#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include "stubs/esp_stubs.h"
#endif

#ifndef MIBOT_RING_FRAME_BYTES
#define MIBOT_RING_FRAME_BYTES 640
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioRing {
  uint8_t *items;
  uint16_t capacity;
  uint16_t head;
  uint16_t count;
  uint32_t pushed_ok;
  uint32_t popped;
  uint32_t dropped;
  SemaphoreHandle_t mutex;
  SemaphoreHandle_t available;
  uint8_t psram;
} AudioRing;

int ring_init(AudioRing *ring, uint16_t capacity, int prefer_psram);
int ring_push_drop_oldest(AudioRing *ring, const uint8_t *frame);
int ring_push_drop_newest(AudioRing *ring, const uint8_t *frame);
int ring_pop(AudioRing *ring, uint8_t *frame, uint32_t timeout_ms);
int ring_pop_nowait(AudioRing *ring, uint8_t *frame);
void ring_reset(AudioRing *ring);
uint16_t ring_count(const AudioRing *ring);
void ring_free(AudioRing *ring);

#ifdef __cplusplus
}
#endif
