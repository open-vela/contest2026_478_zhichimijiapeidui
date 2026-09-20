/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host test for the TOF050C (VL6180X) reading-decode logic and init table.
 *
 * Unlike test_tof_safety.c, this does NOT test a transcription: it compiles the
 * production source esp32s3/mibot_esp32s3/main/mibot_vl6180x.c directly, so the
 * logic under test and the logic that ships are the same translation unit.  That
 * is the whole reason the decode half of the driver was kept free of ESP-IDF
 * includes.
 *
 * What this can and cannot prove, stated plainly because the driver was written
 * without hardware:
 *   CAN  - error-code classification, distance saturation, period encoding,
 *          part-identity check, integrity of the ST init register table, and
 *          that a decoded sample drives the safety gate to the intended verdict.
 *   CANNOT - that the I2C transactions, XSHUT sequencing or address reassignment
 *          in mibot_tof.cpp actually work.  Those need the real modules.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "mibot_vl6180x.h"
#include "tof_safety_model.h"

static int g_failures;

/* Counted so the summary line proves the assertions actually ran.  A silent
 * "passed" is indistinguishable from a test whose loops never executed. */
static int g_checks;

#define CHECK(cond, msg)                                                     \
  do                                                                         \
    {                                                                        \
      g_checks++;                                                            \
      if (!(cond))                                                           \
        {                                                                    \
          printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);           \
          g_failures++;                                                      \
        }                                                                    \
    }                                                                        \
  while (0)

/* Mirrors mibot_config.h. */
#define EDGE_THRESHOLD_MM 50
#define TOF_TIMEOUT_MS 250

/* Build a status register byte from an error code (error is the upper nibble). */
static uint8_t status_of(uint8_t error_code)
{
  return (uint8_t)(error_code << 4);
}

/* ------------------------------------------------------------------------- */
/* Part identity.  TOF200C/TOF400C answer at the same I2C address with a       */
/* different register map, so this check is the only thing standing between a  */
/* wrong module and plausible-looking garbage distances.                       */
/* ------------------------------------------------------------------------- */

static void test_model_id(void)
{
  int i;

  CHECK(mibot_vl6180x_model_id_ok(0xB4), "0xB4 is a VL6180X");

  /* Every other value must be rejected, including the common bus-error reads. */
  for (i = 0; i <= 0xFF; i++)
    {
      if (i == 0xB4)
        {
          continue;
        }
      CHECK(!mibot_vl6180x_model_id_ok((uint8_t)i),
            "non-0xB4 model id must be rejected");
    }
}

/* ------------------------------------------------------------------------- */
/* Inter-measurement period encoding: register counts 10 ms steps biased by 1. */
/* ------------------------------------------------------------------------- */

static void test_period_encoding(void)
{
  /* The value ST's own example uses for 100 ms. */
  CHECK(mibot_vl6180x_encode_period(100) == 9, "100 ms encodes to 9");
  /* The project's configured 30 ms. */
  CHECK(mibot_vl6180x_encode_period(30) == 2, "30 ms encodes to 2");
  CHECK(mibot_vl6180x_encode_period(20) == 1, "20 ms encodes to 1");

  /* Must clamp rather than wrap: a wrapped period would silently make the
   * sensor slower than the safety loop and every poll would read stale. */
  CHECK(mibot_vl6180x_encode_period(0) == 0, "0 clamps to the fastest period");
  CHECK(mibot_vl6180x_encode_period(10) == 0, "10 ms clamps to 0");
  CHECK(mibot_vl6180x_encode_period(100000) == 254, "huge period clamps to 254");
}

static void test_sample_ready(void)
{
  CHECK(mibot_vl6180x_sample_ready(0x04), "range field 4 means sample ready");
  /* Upper bits carry ALS/error status and must be ignored. */
  CHECK(mibot_vl6180x_sample_ready(0xFC | 0x04),
        "upper bits must not affect the range field");
  CHECK(!mibot_vl6180x_sample_ready(0x00), "0 is not ready");
  CHECK(!mibot_vl6180x_sample_ready(0x01), "1 is not ready");
  CHECK(!mibot_vl6180x_sample_ready(0x02), "2 is not ready");
  CHECK(!mibot_vl6180x_sample_ready(0x03), "3 is not ready");
}

/* ------------------------------------------------------------------------- */
/* Decode: a good measurement passes the distance through untouched.          */
/* ------------------------------------------------------------------------- */

static void test_decode_good(void)
{
  vl6180x_sample_t sample;
  int mm;

  for (mm = 0; mm <= 255; mm++)
    {
      sample = mibot_vl6180x_decode(status_of(VL6180X_ERR_NONE), (uint8_t)mm);
      CHECK(sample.valid, "no-error status yields a valid sample");
      CHECK(sample.quality > 0, "valid sample must carry non-zero quality");
      CHECK(sample.mm == (uint16_t)mm, "distance passes through unchanged");
      CHECK(!sample.saturated, "an in-range measurement is not saturated");
    }
}

/* ------------------------------------------------------------------------- */
/* Decode: "no target" is the edge case, not a fault.                         */
/*                                                                            */
/* This is the load-bearing decision in the driver.  Over a table edge the      */
/* sensor cannot converge; if that were reported as a sensor fault, a genuine   */
/* drop-off would be indistinguishable from a broken sensor and the operator    */
/* would see tof_invalid instead of edge_detected.                             */
/* ------------------------------------------------------------------------- */

static void test_decode_no_target(void)
{
  const uint8_t no_target[] = {VL6180X_ERR_NOCONVERGE, VL6180X_ERR_RANGEOFLOW};
  size_t i;

  for (i = 0; i < sizeof(no_target) / sizeof(no_target[0]); i++)
    {
      /* The range byte is meaningless in this state, so feed it garbage and
       * require the decoder to substitute the saturation value regardless. */
      vl6180x_sample_t sample =
          mibot_vl6180x_decode(status_of(no_target[i]), 0x00);

      CHECK(sample.valid, "no-target must stay a usable sample");
      CHECK(sample.quality > 0, "no-target must not look like a sensor fault");
      CHECK(sample.saturated, "no-target must be flagged saturated");
      CHECK(sample.mm == VL6180X_RANGE_MAX_MM,
            "no-target must report the saturated distance");
      CHECK(sample.mm > EDGE_THRESHOLD_MM,
            "saturated distance must exceed the edge threshold");
    }
}

/* ------------------------------------------------------------------------- */
/* Decode: everything ambiguous is unusable.                                  */
/*                                                                            */
/* Requirements traceability: 9.4 - questionable data must never unlock motion.*/
/* ------------------------------------------------------------------------- */

static void test_decode_faults(void)
{
  int error;

  for (error = 0; error <= 15; error++)
    {
      vl6180x_sample_t sample;

      if (error == VL6180X_ERR_NONE || error == VL6180X_ERR_NOCONVERGE ||
          error == VL6180X_ERR_RANGEOFLOW)
        {
          continue;   /* covered by the two tests above */
        }

      /* Feed a plausible short distance: the decoder must still refuse it,
       * otherwise a noisy sensor could report "20 mm, all clear". */
      sample = mibot_vl6180x_decode(status_of((uint8_t)error), 20);
      CHECK(!sample.valid, "ambiguous status must not be valid");
      CHECK(sample.quality == 0, "ambiguous status must have zero quality");
    }
}

/* ------------------------------------------------------------------------- */
/* Init table integrity.                                                      */
/*                                                                            */
/* The table is 38 opaque magic values from ST's application note.  A silent   */
/* edit would produce a sensor that answers but ranges badly, which is exactly  */
/* the failure that is hardest to spot without hardware.                       */
/* ------------------------------------------------------------------------- */

static uint8_t init_value_of(uint16_t reg, bool *found)
{
  size_t i;

  *found = false;
  for (i = 0; i < VL6180X_INIT_SEQUENCE_LEN; i++)
    {
      if (VL6180X_INIT_SEQUENCE[i].reg == reg)
        {
          *found = true;
          return VL6180X_INIT_SEQUENCE[i].value;
        }
    }
  return 0;
}

static void test_init_table(void)
{
  size_t i;
  size_t first_public = (size_t)-1;
  bool found;
  uint8_t value;

  CHECK(VL6180X_INIT_SEQUENCE_LEN == 38, "init table has 38 entries");

  /* No duplicate register writes: a duplicate means an edit landed twice and
   * the later value silently wins. */
  for (i = 0; i < VL6180X_INIT_SEQUENCE_LEN; i++)
    {
      size_t j;
      for (j = i + 1; j < VL6180X_INIT_SEQUENCE_LEN; j++)
        {
          CHECK(VL6180X_INIT_SEQUENCE[i].reg != VL6180X_INIT_SEQUENCE[j].reg,
                "init table must not write the same register twice");
        }
    }

  /* The private block must precede the public block.  0x0207 is the first
   * private write and 0x0011 the first public one. */
  CHECK(VL6180X_INIT_SEQUENCE[0].reg == 0x0207,
        "private block starts at 0x0207");
  for (i = 0; i < VL6180X_INIT_SEQUENCE_LEN; i++)
    {
      if (VL6180X_INIT_SEQUENCE[i].reg == 0x0011)
        {
          first_public = i;
          break;
        }
    }
  CHECK(first_public != (size_t)-1, "public block is present");
  CHECK(first_public >= 30, "all 30 private writes precede the public block");

  /* Spot-check the values this driver actually depends on. */
  value = init_value_of(0x0014, &found);
  CHECK(found, "interrupt config is written");
  CHECK(value == 0x24,
        "0x0014 must be 0x24 so RESULT__INTERRUPT_STATUS_GPIO works as a "
        "data-ready flag for the non-blocking read");

  value = init_value_of(0x0011, &found);
  CHECK(found && value == 0x10, "0x0011 enables new-sample-ready polling");

  value = init_value_of(0x010A, &found);
  CHECK(found && value == 0x30, "0x010A sets the averaging sample period");

  /* A couple of private values, to catch a mangled paste. */
  value = init_value_of(0x0097, &found);
  CHECK(found && value == 0xFD, "private 0x0097 is 0xFD");
  value = init_value_of(0x00DB, &found);
  CHECK(found && value == 0xCE, "private 0x00DB is 0xCE");
}

/* ------------------------------------------------------------------------- */
/* End-to-end at the logic level: decoded sample -> safety gate verdict.       */
/*                                                                            */
/* Joins this driver to the existing P14 gate model so the two halves are       */
/* checked together.  Still no hardware involved.                              */
/* ------------------------------------------------------------------------- */

static tof_gate_input_t gate_with_all(vl6180x_sample_t sample, int motor)
{
  tof_gate_input_t in;
  int i;

  for (i = 0; i < 4; i++)
    {
      in.tof[i].valid = sample.quality > 0;
      in.tof[i].mm = sample.mm;
      in.tof[i].timestamp_ms = 1000;
    }
  in.now_ms = 1000;
  in.timeout_ms = TOF_TIMEOUT_MS;
  in.edge_threshold_mm = EDGE_THRESHOLD_MM;
  in.motion = TOF_MOTION_STANDBY;
  in.left_motor = motor;
  in.right_motor = motor;
  return in;
}

static void test_decode_drives_gate(void)
{
  vl6180x_sample_t good;
  vl6180x_sample_t edge;
  vl6180x_sample_t fault;
  tof_gate_input_t in;

  good = mibot_vl6180x_decode(status_of(VL6180X_ERR_NONE), 30);
  edge = mibot_vl6180x_decode(status_of(VL6180X_ERR_NOCONVERGE), 0);
  fault = mibot_vl6180x_decode(status_of(VL6180X_ERR_SNR), 30);

  /* Robot sitting on the desk: forward motion allowed. */
  in = gate_with_all(good, 40);
  CHECK(tof_gate_decide(&in) == TOF_GATE_ALLOW,
        "a good short reading allows forward motion");

  /* At the edge: denied, and specifically as a safety/edge denial. */
  in = gate_with_all(edge, 40);
  CHECK(tof_gate_decide(&in) == TOF_GATE_DENY_SAFETY,
        "a saturated reading denies forward motion as an edge, not a ToF fault");

  /* Noisy sensor: denied as a ToF fault. */
  in = gate_with_all(fault, 40);
  CHECK(tof_gate_decide(&in) == TOF_GATE_DENY_TOF,
        "an ambiguous reading denies motion as a ToF fault");

  /* Servo-only steps stay allowed in every case: this is what lets greeting /
   * idle / listening run before the ToF hardware exists at all. */
  in = gate_with_all(fault, 0);
  CHECK(tof_gate_decide(&in) == TOF_GATE_ALLOW,
        "a non-moving step is allowed even with unusable ToF data");
  in = gate_with_all(edge, 0);
  CHECK(tof_gate_decide(&in) == TOF_GATE_ALLOW,
        "a non-moving step is allowed at an edge");
}

int main(void)
{
  printf("== VL6180X (TOF050C) decode tests ==\n");

  test_model_id();
  test_period_encoding();
  test_sample_ready();
  test_decode_good();
  test_decode_no_target();
  test_decode_faults();
  test_init_table();
  test_decode_drives_gate();

  if (g_failures != 0)
    {
      printf("%d of %d VL6180X decode check(s) failed\n", g_failures, g_checks);
      return 1;
    }

  printf("all %d VL6180X decode checks passed\n", g_checks);
  return 0;
}
