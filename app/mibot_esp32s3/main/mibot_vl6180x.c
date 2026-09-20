/*
 * VL6180X pure logic: initialisation table and reading decode.
 *
 * No ESP-IDF include on purpose — this file is compiled both into the firmware
 * and into the host test binary, so the tested code and the shipped code are
 * literally the same translation unit rather than two copies that can drift.
 */

#include "mibot_vl6180x.h"

/* ST VL6180X application note, mandatory private registers followed by the
 * recommended public settings.  Do not reorder: the private block must be
 * written before the public block.
 *
 * The three public values that matter for this project are called out:
 *   0x0011 = 0x10  GPIO1 polling for "new sample ready"
 *   0x010A = 0x30  averaging sample period (noise vs. latency compromise)
 *   0x0014 = 0x24  interrupt on new-sample-ready threshold event
 * The interrupt config is what makes RESULT__INTERRUPT_STATUS_GPIO usable as a
 * data-ready flag, which is how the non-blocking read in mibot_tof.cpp avoids
 * spending the safety task's budget polling for convergence.
 */
const vl6180x_reg_write_t VL6180X_INIT_SEQUENCE[] = {
    /* Mandatory private registers. */
    {0x0207, 0x01},
    {0x0208, 0x01},
    {0x0096, 0x00},
    {0x0097, 0xFD},
    {0x00E3, 0x00},
    {0x00E4, 0x04},
    {0x00E5, 0x02},
    {0x00E6, 0x01},
    {0x00E7, 0x03},
    {0x00F5, 0x02},
    {0x00D9, 0x05},
    {0x00DB, 0xCE},
    {0x00DC, 0x03},
    {0x00DD, 0xF8},
    {0x009F, 0x00},
    {0x00A3, 0x3C},
    {0x00B7, 0x00},
    {0x00BB, 0x3C},
    {0x00B2, 0x09},
    {0x00CA, 0x09},
    {0x0198, 0x01},
    {0x01B0, 0x17},
    {0x01AD, 0x00},
    {0x00FF, 0x05},
    {0x0100, 0x05},
    {0x0199, 0x05},
    {0x01A6, 0x1B},
    {0x01AC, 0x3E},
    {0x01A7, 0x1F},
    {0x0030, 0x00},
    /* Recommended public registers. */
    {0x0011, 0x10},
    {0x010A, 0x30},
    {0x003F, 0x46},
    {0x0031, 0xFF},
    {0x0040, 0x63},
    {0x002E, 0x01},
    {0x003E, 0x31},
    {0x0014, 0x24},
};

const size_t VL6180X_INIT_SEQUENCE_LEN =
    sizeof(VL6180X_INIT_SEQUENCE) / sizeof(VL6180X_INIT_SEQUENCE[0]);

bool mibot_vl6180x_model_id_ok(uint8_t model_id) {
  return model_id == VL6180X_MODEL_ID;
}

uint8_t mibot_vl6180x_encode_period(uint32_t period_ms) {
  /* Register value n means (n + 1) * 10 ms. */
  if (period_ms <= 10u) return 0u;
  uint32_t steps = (period_ms / 10u) - 1u;
  if (steps > 254u) steps = 254u;
  return (uint8_t)steps;
}

bool mibot_vl6180x_sample_ready(uint8_t interrupt_status) {
  return (interrupt_status & VL6180X_RANGE_STATUS_MASK) ==
         VL6180X_RANGE_SAMPLE_READY;
}

vl6180x_sample_t mibot_vl6180x_decode(uint8_t status_reg, uint8_t range_val) {
  vl6180x_sample_t sample;
  sample.mm = 0u;
  sample.quality = 0u;
  sample.valid = false;
  sample.saturated = false;

  const uint8_t error = (uint8_t)(status_reg >> 4);

  switch (error) {
    case VL6180X_ERR_NONE:
      /* Trustworthy measurement. */
      sample.mm = range_val;
      sample.quality = 100u;
      sample.valid = true;
      return sample;

    case VL6180X_ERR_NOCONVERGE:
    case VL6180X_ERR_RANGEOFLOW:
      /* No target within reach.  For a downward-facing edge sensor this is the
       * drop-off case, so report a usable saturated distance and let the edge
       * threshold decide.  Reporting a fault here instead would hide a genuine
       * edge behind a generic "sensor broken" state. */
      sample.mm = VL6180X_RANGE_MAX_MM;
      sample.quality = 100u;
      sample.valid = true;
      sample.saturated = true;
      return sample;

    case VL6180X_ERR_SYSERR_1:
    case VL6180X_ERR_SYSERR_5:
    case VL6180X_ERR_ECEFAIL:
    case VL6180X_ERR_RANGEIGNORE:
    case VL6180X_ERR_SNR:
    case VL6180X_ERR_RAWUFLOW:
    case VL6180X_ERR_RAWOFLOW:
    case VL6180X_ERR_RANGEUFLOW:
    default:
      /* Everything else is ambiguous, and an ambiguous distance must never
       * unlock motion.  quality stays 0 so the ToF lock engages. */
      return sample;
  }
}
