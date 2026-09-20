/* Hardware-free tests for the padded/dense frame conversion helpers used by
 * the SF32 audio path.
 *
 * AUDPRC mono mode gives every 16-bit sample its own 32-bit FIFO word, so
 * capture has to drop the odd int16 slots and playback has to zero them.
 * Both loops live in DMA interrupt context, which makes them awkward to
 * exercise on target.  sf_compact_frame()/sf_expand_frame() isolate them as
 * pure functions so this command can check them with no codec involved.
 *
 * Validates: Requirements 5
 */
#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PBT_MAX_SAMPLES  320
#define PBT_ITERATIONS   200
#define PBT_CANARY       ((int16_t)0x5a5a)

extern void sf_compact_frame(int16_t *dst, const int16_t *padded, int samples);
extern void sf_expand_frame(int16_t *padded, const int16_t *src, int samples);

/* Deterministic generator.  A fixed seed keeps a reported counter-example
 * reproducible across runs, which matters when the only channel back from
 * the board is the NSH console.
 */
static uint32_t g_seed;

static uint32_t pbt_rand(void)
{
  g_seed = g_seed * 1103515245u + 12345u;
  return g_seed >> 8;
}

static int16_t pbt_sample(void)
{
  return (int16_t)(uint16_t)(pbt_rand() & 0xffffu);
}

static int pbt_samples(void)
{
  return 1 + (int)(pbt_rand() % PBT_MAX_SAMPLES);
}

/* P2: compaction keeps the even slots in order and loses no sample.  The odd
 * slots are filled with garbage rather than zero so that a helper reading the
 * wrong slot cannot pass by accident.
 */
static int prop_compact_order(void)
{
  static int16_t padded[PBT_MAX_SAMPLES * 2];
  static int16_t dense[PBT_MAX_SAMPLES];
  int iter;

  for (iter = 0; iter < PBT_ITERATIONS; iter++)
    {
      int samples = pbt_samples();
      int i;

      for (i = 0; i < samples; i++)
        {
          padded[i * 2]     = pbt_sample();
          padded[i * 2 + 1] = pbt_sample();
        }

      for (i = 0; i < PBT_MAX_SAMPLES; i++)
        {
          dense[i] = PBT_CANARY;
        }

      sf_compact_frame(dense, padded, samples);

      for (i = 0; i < samples; i++)
        {
          if (dense[i] != padded[i * 2])
            {
              printf("audio_pbt: FAIL P2 seed=%lu samples=%d i=%d "
                     "got=%d want=%d\n", (unsigned long)g_seed, samples, i,
                     dense[i], padded[i * 2]);
              return -1;
            }
        }

      for (i = samples; i < PBT_MAX_SAMPLES; i++)
        {
          if (dense[i] != PBT_CANARY)
            {
              printf("audio_pbt: FAIL P2 overrun seed=%lu samples=%d i=%d\n",
                     (unsigned long)g_seed, samples, i);
              return -1;
            }
        }
    }

  printf("audio_pbt: PASS P2 compact keeps order, %d cases\n", PBT_ITERATIONS);
  return 0;
}

/* P3: after expansion every odd int16 slot is zero.  A non-zero high half
 * word reaches the DAC as noise, so this is the one that must never regress.
 */
static int prop_expand_odd_zero(void)
{
  static int16_t padded[PBT_MAX_SAMPLES * 2];
  static int16_t dense[PBT_MAX_SAMPLES];
  int iter;

  for (iter = 0; iter < PBT_ITERATIONS; iter++)
    {
      int samples = pbt_samples();
      int i;

      for (i = 0; i < samples; i++)
        {
          dense[i] = pbt_sample();
        }

      /* Pre-dirty the whole buffer: expansion has to clear the odd slots
       * itself and must not rely on the caller zeroing them. */
      for (i = 0; i < PBT_MAX_SAMPLES * 2; i++)
        {
          padded[i] = PBT_CANARY;
        }

      sf_expand_frame(padded, dense, samples);

      for (i = 0; i < samples; i++)
        {
          if (padded[i * 2 + 1] != 0)
            {
              printf("audio_pbt: FAIL P3 seed=%lu samples=%d slot=%d "
                     "got=%d\n", (unsigned long)g_seed, samples, i * 2 + 1,
                     padded[i * 2 + 1]);
              return -1;
            }

          if (padded[i * 2] != dense[i])
            {
              printf("audio_pbt: FAIL P3 payload seed=%lu samples=%d i=%d "
                     "got=%d want=%d\n", (unsigned long)g_seed, samples, i,
                     padded[i * 2], dense[i]);
              return -1;
            }
        }

      for (i = samples * 2; i < PBT_MAX_SAMPLES * 2; i++)
        {
          if (padded[i] != PBT_CANARY)
            {
              printf("audio_pbt: FAIL P3 overrun seed=%lu samples=%d i=%d\n",
                     (unsigned long)g_seed, samples, i);
              return -1;
            }
        }
    }

  printf("audio_pbt: PASS P3 expand zeroes odd slots, %d cases\n",
         PBT_ITERATIONS);
  return 0;
}

/* Round trip: expand then compact returns the original dense frame.  This is
 * the driver-level half of the loopback check in requirement 5.3.
 */
static int prop_roundtrip(void)
{
  static int16_t padded[PBT_MAX_SAMPLES * 2];
  static int16_t dense[PBT_MAX_SAMPLES];
  static int16_t back[PBT_MAX_SAMPLES];
  int iter;

  for (iter = 0; iter < PBT_ITERATIONS; iter++)
    {
      int samples = pbt_samples();
      int i;

      for (i = 0; i < samples; i++)
        {
          dense[i] = pbt_sample();
        }

      sf_expand_frame(padded, dense, samples);
      sf_compact_frame(back, padded, samples);

      for (i = 0; i < samples; i++)
        {
          if (back[i] != dense[i])
            {
              printf("audio_pbt: FAIL roundtrip seed=%lu samples=%d i=%d "
                     "got=%d want=%d\n", (unsigned long)g_seed, samples, i,
                     back[i], dense[i]);
              return -1;
            }
        }
    }

  printf("audio_pbt: PASS roundtrip expand->compact, %d cases\n",
         PBT_ITERATIONS);
  return 0;
}

/* Fixed example taken from a real capture dump: "record head -14 0 -23 0
 * -23 0 -21 0".  Keeps the observed hardware layout pinned to the code.
 */
static int unit_capture_layout(void)
{
  const int16_t padded[8] =
  {
    -14, 0, -23, 0, -23, 0, -21, 0
  };

  const int16_t want[4] =
  {
    -14, -23, -23, -21
  };

  int16_t dense[4];
  int16_t again[8];
  int i;

  sf_compact_frame(dense, padded, 4);
  for (i = 0; i < 4; i++)
    {
      if (dense[i] != want[i])
        {
          printf("audio_pbt: FAIL capture-layout i=%d got=%d want=%d\n",
                 i, dense[i], want[i]);
          return -1;
        }
    }

  sf_expand_frame(again, dense, 4);
  if (memcmp(again, padded, sizeof(padded)) != 0)
    {
      printf("audio_pbt: FAIL capture-layout expand mismatch\n");
      return -1;
    }

  printf("audio_pbt: PASS capture-layout fixed example\n");
  return 0;
}

/* A zero-length frame must touch nothing.  The stream code never asks for
 * one, but the guard keeps the loop bound honest.
 */
static int unit_zero_length(void)
{
  int16_t padded[2] = { PBT_CANARY, PBT_CANARY };
  int16_t dense[2]  = { PBT_CANARY, PBT_CANARY };

  sf_compact_frame(dense, padded, 0);
  sf_expand_frame(padded, dense, 0);

  if (padded[0] != PBT_CANARY || padded[1] != PBT_CANARY ||
      dense[0] != PBT_CANARY || dense[1] != PBT_CANARY)
    {
      printf("audio_pbt: FAIL zero-length wrote outside the frame\n");
      return -1;
    }

  printf("audio_pbt: PASS zero-length is a no-op\n");
  return 0;
}

int main(int argc, char **argv)
{
  unsigned long seed = 1;
  int failed = 0;

  if (argc > 1)
    {
      if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0)
        {
          printf("usage: audio_pbt [seed]\n");
          return 0;
        }

      seed = strtoul(argv[1], NULL, 0);
    }

  g_seed = (uint32_t)seed;
  printf("audio_pbt: seed=%lu samples<=%d iterations=%d\n",
         seed, PBT_MAX_SAMPLES, PBT_ITERATIONS);

  failed += unit_capture_layout() != 0;
  failed += unit_zero_length() != 0;
  failed += prop_compact_order() != 0;
  failed += prop_expand_odd_zero() != 0;
  failed += prop_roundtrip() != 0;

  if (failed == 0)
    {
      printf("audio_pbt: ALL PASS (5/5)\n");
      return 0;
    }

  printf("audio_pbt: FAILED %d of 5\n", failed);
  return 1;
}