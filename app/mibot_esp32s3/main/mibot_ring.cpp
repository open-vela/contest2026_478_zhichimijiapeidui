#include "mibot_ring.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#else
#include <stdlib.h>
#endif

namespace {
void lock(AudioRing *ring) { xSemaphoreTake(ring->mutex, portMAX_DELAY); }
void unlock(AudioRing *ring) { xSemaphoreGive(ring->mutex); }
uint8_t *slot(AudioRing *ring, uint16_t index) {
  return ring->items + static_cast<size_t>(index) * MIBOT_RING_FRAME_BYTES;
}
}

extern "C" int ring_init(AudioRing *ring, uint16_t capacity, int prefer_psram) {
  if (ring == nullptr || capacity == 0) return 0;
  *ring = AudioRing{};
  ring->capacity = capacity;
  ring->mutex = xSemaphoreCreateMutex();
  ring->available = xSemaphoreCreateCounting(capacity, 0);
  if (ring->mutex == nullptr || ring->available == nullptr) {
    ring_free(ring);
    return 0;
  }
  const size_t bytes = static_cast<size_t>(capacity) * MIBOT_RING_FRAME_BYTES;
#if defined(ESP_PLATFORM)
  if (prefer_psram) {
    ring->items = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    ring->psram = ring->items != nullptr;
  } else {
    ring->items = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL));
    ring->psram = 0;
  }
#else
  ring->psram = prefer_psram ? 1 : 0;
  ring->items = static_cast<uint8_t *>(heap_caps_malloc(bytes, 0));
#endif
  if (ring->items == nullptr) {
    ring_free(ring);
    return 0;
  }
  memset(ring->items, 0, bytes);
  return 1;
}

extern "C" int ring_push_drop_oldest(AudioRing *ring, const uint8_t *frame) {
  if (ring == nullptr || frame == nullptr || ring->items == nullptr) return 0;
  lock(ring);
  if (ring->count == ring->capacity) {
    ring->head = static_cast<uint16_t>((ring->head + 1) % ring->capacity);
    ring->dropped++;
    const uint16_t index = static_cast<uint16_t>((ring->head + ring->count - 1) % ring->capacity);
    memcpy(slot(ring, index), frame, MIBOT_RING_FRAME_BYTES);
    ring->pushed_ok++;
    unlock(ring);
    return 1;
  }
  const uint16_t index = static_cast<uint16_t>((ring->head + ring->count) % ring->capacity);
  memcpy(slot(ring, index), frame, MIBOT_RING_FRAME_BYTES);
  ring->count++;
  ring->pushed_ok++;
  xSemaphoreGive(ring->available);
  unlock(ring);
  return 1;
}

extern "C" int ring_push_drop_newest(AudioRing *ring, const uint8_t *frame) {
  if (ring == nullptr || frame == nullptr || ring->items == nullptr) return 0;
  lock(ring);
  if (ring->count == ring->capacity) {
    ring->dropped++;
    unlock(ring);
    return 0;
  }
  const uint16_t index = static_cast<uint16_t>((ring->head + ring->count) % ring->capacity);
  memcpy(slot(ring, index), frame, MIBOT_RING_FRAME_BYTES);
  ring->count++;
  ring->pushed_ok++;
  xSemaphoreGive(ring->available);
  unlock(ring);
  return 1;
}

extern "C" int ring_pop(AudioRing *ring, uint8_t *frame, uint32_t timeout_ms) {
  if (ring == nullptr || frame == nullptr || ring->items == nullptr) return 0;
  if (xSemaphoreTake(ring->available, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return 0;
  lock(ring);
  if (ring->count == 0) {
    unlock(ring);
    return 0;
  }
  memcpy(frame, slot(ring, ring->head), MIBOT_RING_FRAME_BYTES);
  ring->head = static_cast<uint16_t>((ring->head + 1) % ring->capacity);
  ring->count--;
  ring->popped++;
  unlock(ring);
  return 1;
}

extern "C" int ring_pop_nowait(AudioRing *ring, uint8_t *frame) {
  if (ring == nullptr || frame == nullptr || ring->items == nullptr || ring->mutex == nullptr) return 0;
  lock(ring);
  if (ring->count == 0) {
    unlock(ring);
    return 0;
  }
  memcpy(frame, slot(ring, ring->head), MIBOT_RING_FRAME_BYTES);
  ring->head = static_cast<uint16_t>((ring->head + 1) % ring->capacity);
  ring->count--;
  ring->popped++;
  unlock(ring);
  return 1;
}

extern "C" void ring_reset(AudioRing *ring) {
  if (ring == nullptr || ring->mutex == nullptr) return;
  lock(ring);
  ring->head = 0;
  ring->count = 0;
  while (xSemaphoreTake(ring->available, 0) == pdTRUE) {
  }
  unlock(ring);
}

extern "C" uint16_t ring_count(const AudioRing *ring) {
  if (ring == nullptr || ring->mutex == nullptr) return 0;
  AudioRing *mutable_ring = const_cast<AudioRing *>(ring);
  lock(mutable_ring);
  const uint16_t result = mutable_ring->count;
  unlock(mutable_ring);
  return result;
}

extern "C" void ring_free(AudioRing *ring) {
  if (ring == nullptr) return;
  if (ring->items != nullptr) {
#if defined(ESP_PLATFORM)
    heap_caps_free(ring->items);
#else
    heap_caps_free(ring->items);
#endif
  }
  if (ring->available != nullptr) vSemaphoreDelete(ring->available);
  if (ring->mutex != nullptr) vSemaphoreDelete(ring->mutex);
  *ring = AudioRing{};
}
