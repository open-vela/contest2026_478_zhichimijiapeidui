/*
 * Four-channel VL6180X (TOF050C) driver.  See mibot_tof.h for the contract and
 * mibot_vl6180x.h for the register map and decode rules.
 *
 * Bring-up shape, and why it is this shape:
 *
 * Every module answers at VL6180X_DEFAULT_ADDR out of reset, so they cannot all
 * be on the bus at once.  Each has an XSHUT line held low to keep it in reset;
 * bring-up raises exactly one, reassigns it to a unique address, configures it,
 * and only then moves on.  The assignment lives in a volatile register, so this
 * has to be redone on every boot — and, because a brownout silently sends a
 * module back to the default address, mibot_tof_maintain() can redo it for a
 * single channel at runtime.
 *
 * A channel that never appears is left marked unusable.  That is the whole point
 * of the conservative default: no reading means the gate keeps the motors
 * locked, rather than boot failing or a fake distance unlocking motion.
 */

#include "mibot_tof.h"

#include "mibot_config.h"
#include "mibot_vl6180x.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "mibot_tof";

constexpr gpio_num_t XSHUT_PINS[MIBOT_TOF_COUNT] = {
    MIBOT_TOF_XSHUT_0, MIBOT_TOF_XSHUT_1, MIBOT_TOF_XSHUT_2, MIBOT_TOF_XSHUT_3};

constexpr uint8_t TARGET_ADDR[MIBOT_TOF_COUNT] = {
    MIBOT_TOF_ADDR_0, MIBOT_TOF_ADDR_1, MIBOT_TOF_ADDR_2, MIBOT_TOF_ADDR_3};

constexpr const char *CHANNEL_NAMES[MIBOT_TOF_COUNT] = {
    "front_left", "front_right", "rear_left", "rear_right"};

// The four XSHUT lines must be distinct, or bring-up would leave two sensors
// sharing the factory address and silently read whichever answered first.
static_assert(MIBOT_TOF_XSHUT_0 != MIBOT_TOF_XSHUT_1 &&
              MIBOT_TOF_XSHUT_0 != MIBOT_TOF_XSHUT_2 &&
              MIBOT_TOF_XSHUT_0 != MIBOT_TOF_XSHUT_3 &&
              MIBOT_TOF_XSHUT_1 != MIBOT_TOF_XSHUT_2 &&
              MIBOT_TOF_XSHUT_1 != MIBOT_TOF_XSHUT_3 &&
              MIBOT_TOF_XSHUT_2 != MIBOT_TOF_XSHUT_3);

// Likewise the runtime addresses, and none of them may be the factory default:
// that address is reserved so recovery can tell a reverted sensor apart.
static_assert(MIBOT_TOF_ADDR_0 != MIBOT_TOF_ADDR_1 &&
              MIBOT_TOF_ADDR_0 != MIBOT_TOF_ADDR_2 &&
              MIBOT_TOF_ADDR_0 != MIBOT_TOF_ADDR_3 &&
              MIBOT_TOF_ADDR_1 != MIBOT_TOF_ADDR_2 &&
              MIBOT_TOF_ADDR_1 != MIBOT_TOF_ADDR_3 &&
              MIBOT_TOF_ADDR_2 != MIBOT_TOF_ADDR_3);
static_assert(MIBOT_TOF_ADDR_0 != VL6180X_DEFAULT_ADDR &&
              MIBOT_TOF_ADDR_1 != VL6180X_DEFAULT_ADDR &&
              MIBOT_TOF_ADDR_2 != VL6180X_DEFAULT_ADDR &&
              MIBOT_TOF_ADDR_3 != VL6180X_DEFAULT_ADDR);

// The ranging period has to stay under the safety loop's 50 ms, otherwise a poll
// can find no fresh sample and the gate would flap between valid and stale.
static_assert(MIBOT_TOF_RANGE_PERIOD_MS < 50);
// Staleness budget must not exceed the gate's own freshness window, or this
// module would hand out readings the gate already considers expired.
static_assert(MIBOT_TOF_SAMPLE_MAX_AGE_MS <= MIBOT_TOF_TIMEOUT_MS);

// The VL6180X needs ~1.4 ms after XSHUT release before it answers; round up.
constexpr uint32_t BOOT_DELAY_MS = 3;

struct Channel {
  uint8_t address = 0;   // 0 == not brought up
  bool ready = false;
  uint16_t mm = 0;
  uint8_t quality = 0;
  int64_t sample_ms = 0;      // when the last fresh sample was decoded
  int64_t last_recovery_ms = 0;
};

Channel g_channels[MIBOT_TOF_COUNT];
bool g_gpio_ready = false;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

TickType_t i2c_timeout() { return pdMS_TO_TICKS(MIBOT_TOF_I2C_TIMEOUT_MS); }

esp_err_t write_reg8(uint8_t address, uint16_t reg, uint8_t value) {
  // 16-bit register index, big endian, then the data byte.
  const uint8_t payload[3] = {static_cast<uint8_t>(reg >> 8),
                              static_cast<uint8_t>(reg & 0xFF), value};
  return i2c_master_write_to_device(MIBOT_I2C_PORT, address, payload,
                                    sizeof(payload), i2c_timeout());
}

esp_err_t read_reg8(uint8_t address, uint16_t reg, uint8_t *value) {
  const uint8_t index[2] = {static_cast<uint8_t>(reg >> 8),
                            static_cast<uint8_t>(reg & 0xFF)};
  return i2c_master_write_read_device(MIBOT_I2C_PORT, address, index,
                                      sizeof(index), value, 1, i2c_timeout());
}

// Apply the ST init blob and start continuous ranging. The caller has already
// moved the sensor to `address`.
esp_err_t configure_sensor(uint8_t address) {
  uint8_t fresh = 0;
  esp_err_t result = read_reg8(address, VL6180X_REG_SYSTEM_FRESH_OUT_OF_RESET,
                               &fresh);
  if (result != ESP_OK) return result;

  // Only load the blob on a genuinely fresh part. Re-applying it to a running
  // sensor is not harmful but wastes ~40 transactions on every recovery pass.
  if (fresh == 0x01) {
    for (size_t index = 0; index < VL6180X_INIT_SEQUENCE_LEN; ++index) {
      result = write_reg8(address, VL6180X_INIT_SEQUENCE[index].reg,
                          VL6180X_INIT_SEQUENCE[index].value);
      if (result != ESP_OK) return result;
    }
    result = write_reg8(address, VL6180X_REG_SYSTEM_FRESH_OUT_OF_RESET, 0x00);
    if (result != ESP_OK) return result;
  }

  result = write_reg8(address, VL6180X_REG_SYSRANGE_INTERMEASUREMENT_PERIOD,
                      mibot_vl6180x_encode_period(MIBOT_TOF_RANGE_PERIOD_MS));
  if (result != ESP_OK) return result;
  result = write_reg8(address, VL6180X_REG_SYSTEM_INTERRUPT_CLEAR,
                      VL6180X_INTERRUPT_CLEAR_ALL);
  if (result != ESP_OK) return result;
  // Continuous mode: the sensor keeps measuring on its own so the safety loop
  // never has to wait for convergence.
  return write_reg8(address, VL6180X_REG_SYSRANGE_START,
                    VL6180X_MODE_CONTINUOUS | VL6180X_START_STOP);
}

// Raise one XSHUT, confirm the part, move it off the default address and start
// it ranging. Leaves g_channels[index] updated either way.
esp_err_t bring_up_channel(uint8_t index) {
  Channel &channel = g_channels[index];
  channel.ready = false;
  channel.address = 0;
  channel.quality = 0;

  gpio_set_level(XSHUT_PINS[index], 1);
  vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));

  uint8_t model_id = 0;
  esp_err_t result = read_reg8(VL6180X_DEFAULT_ADDR,
                               VL6180X_REG_IDENTIFICATION_MODEL_ID, &model_id);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "%s: no response at default address 0x%02X (%s)",
             CHANNEL_NAMES[index], VL6180X_DEFAULT_ADDR,
             esp_err_to_name(result));
    gpio_set_level(XSHUT_PINS[index], 0);
    return result;
  }
  if (!mibot_vl6180x_model_id_ok(model_id)) {
    // Worth shouting about: TOF200C/TOF400C answer at the same address with a
    // different register map, so a wrong module here would otherwise produce
    // plausible-looking garbage distances.
    ESP_LOGE(TAG,
             "%s: MODEL_ID 0x%02X is not a VL6180X (expected 0x%02X). Wrong "
             "module? TOF050C=VL6180X, TOF200C=VL53L0X, TOF400C=VL53L1X",
             CHANNEL_NAMES[index], model_id, VL6180X_MODEL_ID);
    gpio_set_level(XSHUT_PINS[index], 0);
    return ESP_ERR_NOT_SUPPORTED;
  }

  // Move off the shared default address before the next channel wakes up.
  result = write_reg8(VL6180X_DEFAULT_ADDR,
                      VL6180X_REG_I2C_SLAVE_DEVICE_ADDRESS, TARGET_ADDR[index]);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "%s: address reassignment failed (%s)", CHANNEL_NAMES[index],
             esp_err_to_name(result));
    gpio_set_level(XSHUT_PINS[index], 0);
    return result;
  }

  result = configure_sensor(TARGET_ADDR[index]);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "%s: configuration failed at 0x%02X (%s)",
             CHANNEL_NAMES[index], TARGET_ADDR[index], esp_err_to_name(result));
    gpio_set_level(XSHUT_PINS[index], 0);
    return result;
  }

  channel.address = TARGET_ADDR[index];
  channel.ready = true;
  channel.sample_ms = 0;
  ESP_LOGI(TAG, "%s: VL6180X ready at 0x%02X, continuous %d ms",
           CHANNEL_NAMES[index], TARGET_ADDR[index],
           MIBOT_TOF_RANGE_PERIOD_MS);
  return ESP_OK;
}

}  // namespace

esp_err_t mibot_tof_init(void) {
  gpio_config_t xshut_config = {};
  for (uint8_t index = 0; index < MIBOT_TOF_COUNT; ++index) {
    xshut_config.pin_bit_mask |= 1ULL << XSHUT_PINS[index];
  }
  xshut_config.mode = GPIO_MODE_OUTPUT;
  xshut_config.pull_up_en = GPIO_PULLUP_DISABLE;
  xshut_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  xshut_config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t result = gpio_config(&xshut_config);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "XSHUT GPIO config failed: %s", esp_err_to_name(result));
    return result;
  }

  // Hold every sensor in reset so exactly one at a time owns the default
  // address during bring-up.
  for (uint8_t index = 0; index < MIBOT_TOF_COUNT; ++index) {
    gpio_set_level(XSHUT_PINS[index], 0);
  }
  vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));
  g_gpio_ready = true;

  for (uint8_t index = 0; index < MIBOT_TOF_COUNT; ++index) {
    (void)bring_up_channel(index);
  }

  const uint8_t ready = mibot_tof_ready_count();
  if (ready == MIBOT_TOF_COUNT) {
    ESP_LOGI(TAG, "all %d ToF channels ready", MIBOT_TOF_COUNT);
  } else {
    // Not an error return: partial ToF must keep motion locked, not stop boot.
    ESP_LOGW(TAG,
             "only %u/%d ToF channels ready; motion stays locked until every "
             "channel reports",
             static_cast<unsigned>(ready), MIBOT_TOF_COUNT);
  }
  return ESP_OK;
}

esp_err_t mibot_tof_read(uint8_t index, uint16_t *mm, uint8_t *quality) {
  if (index >= MIBOT_TOF_COUNT || mm == nullptr || quality == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  Channel &channel = g_channels[index];
  *mm = 0;
  *quality = 0;
  if (!channel.ready) return ESP_ERR_INVALID_STATE;

  uint8_t interrupt_status = 0;
  esp_err_t result = read_reg8(channel.address,
                               VL6180X_REG_RESULT_INTERRUPT_STATUS_GPIO,
                               &interrupt_status);
  if (result != ESP_OK) {
    // Stopped answering: most likely a brownout took it back to the default
    // address. Drop it so maintain() can re-adopt it.
    channel.ready = false;
    channel.quality = 0;
    ESP_LOGW(TAG, "%s: read failed at 0x%02X (%s), dropping channel",
             CHANNEL_NAMES[index], channel.address, esp_err_to_name(result));
    return ESP_ERR_INVALID_STATE;
  }

  if (mibot_vl6180x_sample_ready(interrupt_status)) {
    uint8_t status_reg = 0;
    uint8_t range_val = 0;
    result = read_reg8(channel.address, VL6180X_REG_RESULT_RANGE_STATUS,
                       &status_reg);
    if (result == ESP_OK) {
      result = read_reg8(channel.address, VL6180X_REG_RESULT_RANGE_VAL,
                         &range_val);
    }
    // Always clear, otherwise the data-ready flag latches and every later poll
    // decodes the same stale sample.
    (void)write_reg8(channel.address, VL6180X_REG_SYSTEM_INTERRUPT_CLEAR,
                     VL6180X_INTERRUPT_CLEAR_ALL);
    if (result != ESP_OK) {
      channel.quality = 0;
      return ESP_FAIL;
    }

    const vl6180x_sample_t sample =
        mibot_vl6180x_decode(status_reg, range_val);
    channel.mm = sample.mm;
    channel.quality = sample.quality;
    if (sample.valid) {
      channel.sample_ms = now_ms();
    }
  }

  if (channel.quality == 0) return ESP_FAIL;
  if (channel.sample_ms == 0 ||
      now_ms() - channel.sample_ms > MIBOT_TOF_SAMPLE_MAX_AGE_MS) {
    // Refuse to pass off an old distance as current: the gate's freshness check
    // stamps whatever we return with "now", so staleness has to be caught here.
    return ESP_ERR_TIMEOUT;
  }

  *mm = channel.mm;
  *quality = channel.quality;
  return ESP_OK;
}

void mibot_tof_maintain(void) {
  if (!g_gpio_ready) return;

  const int64_t now = now_ms();
  for (uint8_t index = 0; index < MIBOT_TOF_COUNT; ++index) {
    Channel &channel = g_channels[index];
    if (channel.ready) continue;
    if (channel.last_recovery_ms != 0 &&
        now - channel.last_recovery_ms < MIBOT_TOF_RECOVERY_INTERVAL_MS) {
      continue;
    }
    channel.last_recovery_ms = now;
    // Full reset of this channel only: pulling XSHUT low guarantees it is back
    // at the default address, so re-adoption does not depend on knowing what
    // state it failed in.
    gpio_set_level(XSHUT_PINS[index], 0);
    vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));
    if (bring_up_channel(index) == ESP_OK) {
      ESP_LOGI(TAG, "%s: recovered", CHANNEL_NAMES[index]);
    }
    // One channel per call keeps the caller's period predictable.
    return;
  }
}

bool mibot_tof_channel_ready(uint8_t index) {
  if (index >= MIBOT_TOF_COUNT) return false;
  return g_channels[index].ready;
}

uint8_t mibot_tof_ready_count(void) {
  uint8_t count = 0;
  for (uint8_t index = 0; index < MIBOT_TOF_COUNT; ++index) {
    if (g_channels[index].ready) ++count;
  }
  return count;
}
