/*
 * VL6180X — host-portable register map and reading-decode logic.
 *
 * The TOF050C module (50 cm short-range variant of the TOF050C/200C/400C
 * family) carries an ST VL6180X.  This is worth stating explicitly because the
 * three family members are three different chips with incompatible register
 * maps:
 *
 *   TOF050C -> VL6180X   (this file)   16-bit register index, distance in mm
 *   TOF200C -> VL53L0X                 8-bit register index
 *   TOF400C -> VL53L1X                 16-bit register index, different map
 *
 * Both use 7-bit I2C address 0x29 out of reset, so a successful bus probe does
 * NOT tell you which chip is on the board.  Call mibot_vl6180x_model_id_ok()
 * on register 0x000 to actually confirm the part before trusting any reading.
 *
 * This file deliberately has no ESP-IDF dependency: it holds the parts that are
 * pure data and pure decision so they can be compiled and tested on a host.
 * The I2C transactions live in mibot_tof.cpp.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Register map (16-bit index, big endian on the wire) ----------------- */

#define VL6180X_REG_IDENTIFICATION_MODEL_ID 0x0000
#define VL6180X_REG_SYSTEM_MODE_GPIO1 0x0011
#define VL6180X_REG_SYSTEM_INTERRUPT_CONFIG 0x0014
#define VL6180X_REG_SYSTEM_INTERRUPT_CLEAR 0x0015
#define VL6180X_REG_SYSTEM_FRESH_OUT_OF_RESET 0x0016
#define VL6180X_REG_SYSRANGE_START 0x0018
#define VL6180X_REG_SYSRANGE_INTERMEASUREMENT_PERIOD 0x001B
#define VL6180X_REG_RESULT_RANGE_STATUS 0x004D
#define VL6180X_REG_RESULT_INTERRUPT_STATUS_GPIO 0x004F
#define VL6180X_REG_RESULT_RANGE_VAL 0x0062
#define VL6180X_REG_I2C_SLAVE_DEVICE_ADDRESS 0x0212

/* Factory default 7-bit address, shared by every module on the bus until the
 * host reassigns it.  Reassignment is volatile: a sensor that browns out comes
 * back at this address, which is why mibot_tof.cpp keeps it reserved. */
#define VL6180X_DEFAULT_ADDR 0x29

/* IDENTIFICATION__MODEL_ID reads back this constant on a genuine VL6180X. */
#define VL6180X_MODEL_ID 0xB4

/* SYSRANGE__START bits. */
#define VL6180X_START_STOP 0x01
#define VL6180X_MODE_CONTINUOUS 0x02

/* SYSTEM__INTERRUPT_CLEAR: clear range, ALS and error interrupts. */
#define VL6180X_INTERRUPT_CLEAR_ALL 0x07

/* RESULT__INTERRUPT_STATUS_GPIO range field: 4 == new sample ready. */
#define VL6180X_RANGE_STATUS_MASK 0x07
#define VL6180X_RANGE_SAMPLE_READY 0x04

/* RESULT__RANGE_STATUS error codes, taken from the upper nibble. */
enum {
  VL6180X_ERR_NONE = 0,
  VL6180X_ERR_SYSERR_1 = 1,
  VL6180X_ERR_SYSERR_5 = 5,
  VL6180X_ERR_ECEFAIL = 6,
  VL6180X_ERR_NOCONVERGE = 7,
  VL6180X_ERR_RANGEIGNORE = 8,
  VL6180X_ERR_SNR = 11,
  VL6180X_ERR_RAWUFLOW = 12,
  VL6180X_ERR_RAWOFLOW = 13,
  VL6180X_ERR_RANGEUFLOW = 14,
  VL6180X_ERR_RANGEOFLOW = 15,
};

/* RESULT__RANGE_VAL is a single byte, so a reading saturates here.  The part is
 * specified to roughly 200 mm; the module is marketed as 50 cm. */
#define VL6180X_RANGE_MAX_MM 255

/* --- Initialisation table ------------------------------------------------ */

typedef struct {
  uint16_t reg;
  uint8_t value;
} vl6180x_reg_write_t;

/* The mandatory private-register load plus the recommended public settings from
 * ST's VL6180X application note (the "page 24" sequence).  Applied once after
 * SYSTEM__FRESH_OUT_OF_RESET reads 1.  Kept as data rather than a code block so
 * a host test can assert the table did not get mangled. */
extern const vl6180x_reg_write_t VL6180X_INIT_SEQUENCE[];
extern const size_t VL6180X_INIT_SEQUENCE_LEN;

/* Returns true when a MODEL_ID read matches a genuine VL6180X. */
bool mibot_vl6180x_model_id_ok(uint8_t model_id);

/* Encode a millisecond ranging period into SYSRANGE__INTERMEASUREMENT_PERIOD.
 * The register counts 10 ms steps biased by one, so 100 ms -> 9.  Values are
 * clamped into the representable range instead of wrapping. */
uint8_t mibot_vl6180x_encode_period(uint32_t period_ms);

/* --- Reading decode ------------------------------------------------------ */

/* What one sample means for the motion-safety gate.
 *
 * The gate consumes (mm, quality) and treats quality == 0 as "sensor not
 * usable", which locks motion.  front_edge_locked() separately treats a large
 * distance on a downward-facing front sensor as a drop-off.  Those two paths
 * need different failures routed to them:
 *
 *   - "no target in range" is the *expected* reading at a table edge.  It must
 *     surface as a usable sample with a saturated distance so the edge logic
 *     fires and telemetry carries an honest number, rather than as a sensor
 *     fault.
 *   - a real fault (bus error, system error, noise, out-of-spec raw value) must
 *     surface as unusable so the conservative ToF lock engages.
 *
 * Both outcomes stop the motors; the distinction is which event the operator
 * sees (edge_detected vs tof_invalid) and whether the number can be trusted. */
typedef struct {
  uint16_t mm;
  uint8_t quality;  /* 0 == unusable sample; the gate keys off this */
  bool valid;
  bool saturated; /* true when mm was substituted for an out-of-range target */
} vl6180x_sample_t;

/* Decode RESULT__RANGE_STATUS + RESULT__RANGE_VAL into a sample.
 *
 * `status_reg` is the raw register byte (the error code is its upper nibble).
 * Pure function: no I2C, no clock, no globals. */
vl6180x_sample_t mibot_vl6180x_decode(uint8_t status_reg, uint8_t range_val);

/* True when RESULT__INTERRUPT_STATUS_GPIO reports a fresh range sample. */
bool mibot_vl6180x_sample_ready(uint8_t interrupt_status);

#ifdef __cplusplus
}
#endif
