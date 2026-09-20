/* SF32LB52 AUDCODEC/AUDPRC I2S-style lower half.
 *
 * The codec is the analog endpoint. PCM data must enter through AUDPRC
 * TX0/RX0; direct AUDCODEC DMA is only the raw/APB test path and caused the
 * old short-pop symptom without sustained samples reaching the speaker.
 */
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/semaphore.h>
#include <nuttx/irq.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/audio_i2s.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include "bf0_hal.h"
#include "bf0_hal_audprc.h"
#include "dma_config.h"

/* 3200 PCM frames = 200 ms at 16 kHz.  AUDPRC mono DMA stores each
 * 16-bit frame in a 32-bit word: PCM in the even int16 slot and zero in the
 * odd slot.  The direct-tone buffer must use this same padded layout as the
 * working streaming path; supplying dense PCM here made the DMA consume an
 * invalid alternating-word layout and caused a regular audible interruption.
 * 2 kHz has a period of exactly 8 frames, so 3200 frames also wrap on a zero
 * crossing. */
#define SF32_AUDIO_WORDS       1600
#define SF32_TONE_FRAMES       (SF32_AUDIO_WORDS * 2)
#define SF32_TONE_DMA_SAMPLES  (SF32_TONE_FRAMES * 2)
#define SF32_SAMPLE_RATE       16000U

static AUDCODEC_HandleTypeDef g_codec;
static AUDPRC_HandleTypeDef g_prc;
static DMA_HandleTypeDef g_prc_tx_dma;
static DMA_HandleTypeDef g_prc_rx_dma;
static struct ap_buffer_s *g_tx_apb;
static struct ap_buffer_s *g_rx_apb;
static i2s_callback_t g_tx_cb;
static i2s_callback_t g_rx_cb;
static void *g_tx_arg;
static void *g_rx_arg;
static bool g_ready;
static bool g_dma_irq_ready;
static volatile bool g_rx_done;

/* Streaming capture.  20 ms at 16 kHz is 320 samples; AUDPRC pads each
 * sample into a 32-bit word, so one frame occupies 1280 bytes and the
 * circular DMA buffer holds two frames.  The half/full callbacks each
 * compact one frame into dense PCM and push it into a fixed-depth queue.
 * Frame pacing must come from these callbacks: the system tick measured on
 * this board is 20 ms, so usleep() cannot pace a 20 ms stream. */
#define SF32_STREAM_FRAME_SAMPLES  320
#define SF32_STREAM_FRAME_BYTES    (SF32_STREAM_FRAME_SAMPLES * 2)
#define SF32_STREAM_PAD_WORDS      (SF32_STREAM_FRAME_SAMPLES * 2)
#define SF32_STREAM_QUEUE_DEPTH    8

/* Capture gain, in dB, for the AUDCODEC ADC channel.
 *
 * The vendor HAL (HAL_AUDCODEC_Config_ADCPath_Volume in bf0_hal_audcodec_m.c)
 * encodes a dB value as:
 *     rough_vol = (dB + 60) / 6;          -- 6 dB per step
 *     fine_vol  = ((dB + 60) % 6) << 1;   -- 0.5 dB per step
 * with a valid range of -60..+30 dB.  That function is unusable on this part
 * (it writes registers that only exist on 56x/58x, see the note in sf_setup),
 * so the encoding is reproduced here and written directly.
 *
 * Measured on the DevKit at 0 dB (rough 10 / fine 0) with the cloud gateway
 * capturing what actually left the device: normal speech reached rms ~171 and
 * frame peaks 200..660, about -45 dBFS.  Cloud ASR expects roughly -30..-20
 * dBFS, so the stream was ~20 dB too quiet.
 *
 * +20 dB puts normal speech near rms ~1700 / peak ~6600 (20% of full scale)
 * and still leaves about 10 dB of headroom before a raised voice clips.  Do not
 * push this to +30: the remaining headroom would be spent by loud speech. */
#define SF32_ADC_GAIN_DB           20
#define SF32_ADC_ROUGH_VOL         ((SF32_ADC_GAIN_DB + 60) / 6)
#define SF32_ADC_FINE_VOL          (((SF32_ADC_GAIN_DB + 60) % 6) << 1)

/* Frames discarded at the start of every capture.
 *
 * The ADC path is enabled per capture (see board_audio_stream_start), so each
 * stream begins with the analog bias still settling and the channel high-pass
 * filter converging from zero.  Measured: first frame peak 9434 against 400..660
 * for the rest of the utterance -- a DC step, not signal.  At 0 dB that was
 * merely ugly; with +20 dB of gain it would saturate, and it also makes any
 * energy-based VAD or AGC mis-trigger on the first 20 ms.
 *
 * 3 frames = 60 ms, which is below the 3 s capture window and far below human
 * reaction time after a wake trigger, so nothing intelligible is lost. */
#define SF32_STREAM_WARMUP_FRAMES  3

/* Playback queue is deeper than capture: Wi-Fi jitter on the ESP32 side is
 * larger than local capture jitter.  12 frames buffer 240 ms. */
/* 24 frames = 480 ms.  Deeper than the 12 it was, because the ESP32 downlink
 * now pays back frames it owes in small bursts (see audio_tx_task): with only
 * 240 ms the catch-up would overflow the queue and board_audio_play_write()
 * would drop frames, which loses exactly the audio the catch-up recovered. */
/* 40 frames = 800 ms, 25.6 KB of static SRAM.
 *
 * Sized for tolerance rather than for the nominal rate: the ESP32 was measured
 * sending this stream at a clean 19.6 ms/frame with zero drops, yet the SF32
 * still underran, so the two ends are not staying in step for a reason that is
 * not yet isolated.  A large cushion plus a large prefill is what absorbs that
 * regardless of which side is off, at the cost of latency before the reply
 * starts. */
#define SF32_PLAY_QUEUE_DEPTH      40
/* Frames to bank before the DAC is allowed to start.
 *
 * Without this the transfer began on an empty queue, so the DAC was draining
 * from the very first sample while frames were still only arriving at the rate
 * it consumed them.  The queue then sits at 0-1 frames forever and every hiccup
 * anywhere upstream (Wi-Fi jitter, an ESP32 task delay, the SF32 main loop busy
 * in an LCD redraw) is an immediate underrun -- audible as the chopped speech
 * this exists to fix.  6 frames = 120 ms of slack, half the queue, at the cost
 * of that much more latency before the reply starts.  24 frames = 480 ms. */
#define SF32_PLAY_PREFILL_FRAMES   24

/* Bench playback level controls.  Both act only on the sample values written
 * into the DMA buffer, after the frame has been dequeued -- the DAC still runs
 * at its own rate, the queue still drains one frame per fill, and every
 * playback counter (fills, underrun, peak queue depth, per-fill period) is
 * unaffected.  So silencing the output does not change what the dropout
 * instrumentation measures.
 *
 * SF32_PLAY_MUTE      1 = write silence, keeping all timing and queue
 *                     behaviour identical.  Takes precedence over the shift.
 * SF32_PLAY_ATTEN_SHIFT
 *                     bits of arithmetic right shift when not muted: 1 = -6 dB,
 *                     2 = -12 dB, 3 = -18 dB, 0 = pass through.
 *
 * These exist so playback can be exercised without touching the codec gain
 * fields (DAC_CH0_CFG ROUGH_VOL, AUDPRC vol_l/vol_r), whose exact semantics are
 * not pinned down here and where an earlier guess clipped the output.  Both are
 * debugging aids: set MUTE to 0 and the shift to 0 for a release image. */
#define SF32_PLAY_MUTE             0
#define SF32_PLAY_ATTEN_SHIFT      3

static int16_t g_stream_dma[SF32_STREAM_PAD_WORDS * 2]
 __attribute__((aligned(32)));
static int16_t g_stream_queue[SF32_STREAM_QUEUE_DEPTH]
                             [SF32_STREAM_FRAME_SAMPLES];
static volatile uint32_t g_stream_head;
static volatile uint32_t g_stream_tail;
static volatile uint32_t g_stream_dropped;
static volatile uint32_t g_stream_rx_half_count;
static volatile uint32_t g_stream_rx_full_count;
static volatile uint32_t g_stream_error_count;
static volatile bool g_stream_active;
/* Countdown of frames still to be discarded as ADC warm-up, plus a cumulative
 * count so a run can confirm the warm-up actually happened. */
static volatile uint32_t g_stream_warmup;
static volatile uint32_t g_stream_warmup_dropped;

/* Downlink playback.  TX DMA runs continuously over a two-frame circular
 * buffer; the half/full callbacks expand one queued frame into each half.
 * Stopping the DMA on underrun would click, so a missing frame is filled
 * with silence instead. */
static int16_t g_play_dma[SF32_STREAM_PAD_WORDS * 2]
 __attribute__((aligned(32)));
static int16_t g_play_queue[SF32_PLAY_QUEUE_DEPTH]
                           [SF32_STREAM_FRAME_SAMPLES];
static volatile uint32_t g_play_head;
static volatile uint32_t g_play_tail;
static volatile uint32_t g_play_dropped;
static volatile uint32_t g_play_underrun;
/* Queue is accepting frames but the DAC has not been started yet (prefill). */
static volatile bool g_play_armed;
/* Playback instrumentation.  elapsed/fills is the DAC's real cadence: each fill
 * is one DMA half-buffer, which by construction holds 20 ms of audio.  A value
 * near 20 means the geometry is right and any silence came from the stream
 * running out; clearly below 20 means the transfer is consuming faster than
 * 16 kHz and the fault is in the AUDPRC/DAC clock setup. */
static volatile uint32_t g_play_fills;
static volatile uint32_t g_play_peak_queued;
static uint32_t g_play_started_ms;
static volatile bool g_play_active;
static void sf_play_fill(int16_t *padded);
static uint32_t sf_now_ms(void);

/* Posted from the DMA interrupt once per finished frame so the consumer can
 * block instead of polling.  The measured system tick is 20 ms, exactly one
 * frame period, so a usleep-based poll loop cannot keep pace: the earlier
 * `stream 3` run showed 148 empty reads against 150 frames.  The producer
 * never waits and the consumer never posts, so priority inheritance must be
 * disabled (see nxsem_set_protocol note in nuttx/semaphore.h).
 */
static sem_t g_stream_sem;
static bool g_stream_sem_ready;
static void sf_stream_push(const int16_t *padded);

/* Padded/dense conversion.  AUDPRC mono mode gives every 16-bit sample its
 * own 32-bit FIFO word, so the odd int16 slot of each pair is always zero.
 * These two helpers are the only place that layout is encoded.  They are
 * pure: no globals, no printf, no blocking call, because sf_compact_frame()
 * runs in DMA interrupt context.  examples/mibot_agent/audio_pbt.c tests them
 * without any hardware.
 */
void sf_compact_frame(int16_t *dst, const int16_t *padded, int samples);
void sf_expand_frame(int16_t *padded, const int16_t *src, int samples);

static volatile uint32_t g_tx_half_count;
/* Direct-tone DMA diagnostics are counted only in the IRQ handlers and
 * printed after the transfer ends.  This verifies circular DMA continuity
 * without any UART output while the speaker is active. */
static volatile uint32_t g_tone_tx_half_count;
static volatile uint32_t g_tone_tx_full_count;
/* FIFO health captured while the direct tone plays, printed after it ends. */
static uint32_t g_tone_irq_seen;
static uint8_t g_tone_stb_min;
static uint8_t g_tone_stb_max;
static uint8_t g_tone_apb_min;
static uint8_t g_tone_apb_max;
static bool g_audio_registered;
static volatile bool g_tx_hold;
static bool g_diag_printed;
static AUDCODE_DAC_CLK_CONFIG_TYPE g_dac_cfg;
static AUDCODE_ADC_CLK_CONFIG_TYPE g_adc_cfg;
static AUDPRC_DACCfgTypeDef g_prc_dac_cfg;
static AUDPRC_ADCCfgTypeDef g_prc_adc_cfg;
static struct i2s_dev_s g_i2s;

/* Keep the diagnostic source in internal SRAM. The SF32 DMAC cannot fetch
 * from every heap/PSRAM mapping used by the generic audio buffer pool. */
/* Direct tone is padded exactly like g_play_dma: every logical PCM sample
 * occupies a 32-bit AUDPRC word (int16 sample + zero int16 padding). */
static int16_t g_tone_samples[SF32_TONE_DMA_SAMPLES]
  __attribute__((aligned(32)));

extern int board_audio_analog_init(void);
extern int board_audio_pa_start(void);
extern int board_audio_pa_stop(void);

static int sf_rxch(struct i2s_dev_s *d, uint8_t n)
{
  (void)d;
  return n == 1 ? 0 : -EINVAL;
}

static uint32_t sf_rate(struct i2s_dev_s *d, uint32_t r)
{
  (void)d;
  return r == SF32_SAMPLE_RATE ? r : 0;
}

static uint32_t sf_width(struct i2s_dev_s *d, int b)
{
  (void)d;
  return b == 16 ? b : 0;
}

static void sf_dump_registers(void)
{
  if (g_diag_printed)
    {
      return;
    }

  g_diag_printed = true;
  printf("mibot: AUDPRC CFG=0x%08lx TX0=0x%08lx RX0=0x%08lx "
         "DAC0=0x%08lx DAC1=0x%08lx CODEC_CFG=0x%08lx "
         "CODEC_DAC=0x%08lx CODEC_DAC_CH0=0x%08lx\n",
         (unsigned long)g_prc.Instance->CFG,
         (unsigned long)g_prc.Instance->TX_CH0_CFG,
         (unsigned long)g_prc.Instance->RX_CH0_CFG,
         (unsigned long)g_prc.Instance->DAC_PATH_CFG0,
         (unsigned long)g_prc.Instance->DAC_PATH_CFG1,
         (unsigned long)g_codec.Instance->CFG,
         (unsigned long)g_codec.Instance->DAC_CFG,
         (unsigned long)g_codec.Instance->DAC_CH0_CFG);
}

/* The SF32 HAL raises DMA completion through its own vector wrappers.  In
 * NuttX the channel interrupts must be attached explicitly, otherwise the
 * transfer-complete path never runs and irq_unexpected_isr() fires on the
 * first burst.  Mirror the pattern used by sifli_spi.c / sifli_uart.c. */
static int sf_dma_tx_isr(int irq, void *context, void *arg)
{
  (void)irq;
  (void)context;
  (void)arg;

  HAL_DMA_IRQHandler(&g_prc_tx_dma);
  return OK;
}

static int sf_dma_rx_isr(int irq, void *context, void *arg)
{
  (void)irq;
  (void)context;
  (void)arg;
  HAL_DMA_IRQHandler(&g_prc_rx_dma);
  return OK;
}

static void sf_dma_irq_attach(void)
{
  if (g_dma_irq_ready)
    {
      return;
    }

  /* Cortex-M vector numbers are offset by 16 in the NuttX IRQ space. */
  irq_attach(AUDPRC_TX0_DMA_IRQ + 16, sf_dma_tx_isr, NULL);
  up_enable_irq(AUDPRC_TX0_DMA_IRQ + 16);
  irq_attach(AUDPRC_RX0_DMA_IRQ + 16, sf_dma_rx_isr, NULL);
  up_enable_irq(AUDPRC_RX0_DMA_IRQ + 16);
  g_dma_irq_ready = true;
  printf("mibot: audio DMA IRQ attached tx=%d rx=%d\n",
         AUDPRC_TX0_DMA_IRQ + 16, AUDPRC_RX0_DMA_IRQ + 16);
}
static int sf_setup(void)
{
  AUDPRC_ChnlCfgTypeDef tx_cfg;
  AUDPRC_ChnlCfgTypeDef rx_cfg;

  if (g_ready)
    {
      return 0;
    }

  memset(&g_codec, 0, sizeof(g_codec));
  memset(&g_prc, 0, sizeof(g_prc));
  memset(&g_prc_tx_dma, 0, sizeof(g_prc_tx_dma));
  memset(&g_prc_rx_dma, 0, sizeof(g_prc_rx_dma));
  memset(&g_dac_cfg, 0, sizeof(g_dac_cfg));
  memset(&g_adc_cfg, 0, sizeof(g_adc_cfg));
  memset(&g_prc_dac_cfg, 0, sizeof(g_prc_dac_cfg));
  memset(&g_prc_adc_cfg, 0, sizeof(g_prc_adc_cfg));
  memset(&tx_cfg, 0, sizeof(tx_cfg));
  memset(&rx_cfg, 0, sizeof(rx_cfg));

  HAL_RCC_EnableModule(RCC_MOD_AUDPRC);

  /* Normal mode connects AUDPRC to AUDCODEC. Opmode 1 selects the raw APB
   * endpoint and must not be used for the board speaker/microphone path. */
  g_codec.Instance = hwp_audcodec;
  g_codec.Init.en_dly_sel = 0;
  g_codec.Init.dac_cfg.opmode = 0;
  g_codec.Init.adc_cfg.opmode = 0;

  /* DAC clock values are the vendor production table row for SF32LB52X,
   * XTAL, 16 kHz (codec_dac_clk_config_xtal in drv_audcodec_m.c):
   *   {16000, 0, clk_div 1, osr_sel 4, 0x14D, 0, 5, 4, 2, 20, 20, 0}
   * The previous clk_div=10 / osr_sel=2 pair was an ADC-style value: it made
   * the DAC consume the AUDPRC stream at a rate that did not match the
   * AUDPRC 16 kHz strobe (STB divider 3000), so the codec FIFO drifted and
   * the PCM tone dropped out at a fixed cadence.  The codec's internal 1 kHz
   * generator was unaffected, which is why only the DMA path was choppy. */
  g_dac_cfg.samplerate = SF32_SAMPLE_RATE;
  g_dac_cfg.clk_src_sel = 0;
  g_dac_cfg.clk_div = 1;
  g_dac_cfg.osr_sel = 4;
  g_dac_cfg.sinc_gain = 0x14d;
  g_dac_cfg.sel_clk_dac_source = 0;
  g_dac_cfg.diva_clk_dac = 5;
  g_dac_cfg.diva_clk_chop_dac = 4;
  g_dac_cfg.divb_clk_chop_dac = 2;
  g_dac_cfg.diva_clk_chop_bg = 20;
  g_dac_cfg.diva_clk_chop_refgen = 20;
  g_dac_cfg.sel_clk_dac = 0;

  g_adc_cfg.samplerate = SF32_SAMPLE_RATE;
  g_adc_cfg.clk_src_sel = 0;
  g_adc_cfg.clk_div = 10;
  g_adc_cfg.osr_sel = 1;
  g_adc_cfg.sel_clk_adc_source = 0;
  g_adc_cfg.sel_clk_adc = 0;
  g_adc_cfg.diva_clk_adc = 5;
  g_adc_cfg.fsp = 2;
  g_codec.Init.dac_cfg.dac_clk = &g_dac_cfg;
  g_codec.Init.adc_cfg.adc_clk = &g_adc_cfg;

  printf("mibot: audio setup HAL init begin\n");
  if (HAL_AUDCODEC_Init(&g_codec) != HAL_OK)
    {
      return -EIO;
    }

  if (board_audio_analog_init() < 0)
    {
      return -EIO;
    }

  /* HAL_AUDCODEC_Config_TChanel(), _Config_DACPath() and
   * _Config_DACPath_Volume() are unusable on SF32LB52X.  Every one of them
   * dereferences hacodec->Instance_hp, but the HP/LP register blocks only
   * exist on SF32LB56X/58X: this part folds the DAC controls into the single
   * AUDCODEC_TypeDef and defines no AUDCODEC_HP_BASE.  HAL_AUDCODEC_Init()
   * never assigns Instance_hp either, so those writes land on a NULL
   * pointer and are silently dropped, leaving DAC_CH0_CFG at its reset
   * value with ENABLE=0 and SINC_GAIN=0.  Program the 52x registers here.
   */
  g_codec.Instance->DAC_CFG =
      (g_dac_cfg.osr_sel     << AUDCODEC_DAC_CFG_OSR_SEL_Pos)     |
      (0                     << AUDCODEC_DAC_CFG_OP_MODE_Pos)     |
      (0                     << AUDCODEC_DAC_CFG_PATH_RESET_Pos)  |
      (g_dac_cfg.clk_src_sel << AUDCODEC_DAC_CFG_CLK_SRC_SEL_Pos) |
      (g_dac_cfg.clk_div     << AUDCODEC_DAC_CFG_CLK_DIV_Pos);

  /* Field values follow HAL_AUDCODEC_Config_TChanel() in the vendor 52x HAL
   * (bf0_hal_audcodec_m.c): ROUGH_VOL 6, DATA_FORMAT 1, DEM_MODE 2.  The only
   * deliberate difference is DOUT_MUTE: we start muted and unmute after the
   * amplifier has settled so the NS4150B power-up click is inaudible. */
  g_codec.Instance->DAC_CH0_CFG =
      (1                     << AUDCODEC_DAC_CH0_CFG_ENABLE_Pos)      |
      (1                     << AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Pos)   |
      (2                     << AUDCODEC_DAC_CH0_CFG_DEM_MODE_Pos)    |
      (0                     << AUDCODEC_DAC_CH0_CFG_DMA_EN_Pos)      |
      (6                     << AUDCODEC_DAC_CH0_CFG_ROUGH_VOL_Pos)   |
      (0                     << AUDCODEC_DAC_CH0_CFG_FINE_VOL_Pos)    |
      (1                     << AUDCODEC_DAC_CH0_CFG_DATA_FORMAT_Pos) |
      (g_dac_cfg.sinc_gain   << AUDCODEC_DAC_CH0_CFG_SINC_GAIN_Pos)   |
      (0                     << AUDCODEC_DAC_CH0_CFG_DITHER_EN_Pos)   |
      (0                     << AUDCODEC_DAC_CH0_CFG_CLK_ANA_POL_Pos);

  /* DAC_CH0_CFG_EXT is deliberately left at its reset value.  Programming the
   * vendor ramp/zero-adjust bits here made the output gain slew audibly from
   * quiet to loud over the whole tone: that ramp exists for dynamic volume
   * changes, which this driver never performs (it mutes/unmutes instead). */

  /* Same story for the capture side: HAL_AUDCODEC_Config_RChanel() writes
   * Instance_lp, which does not exist on this part.  Values mirror the
   * 52x-correct implementation in bf0_hal_audcodec_m.c. */
  g_codec.Instance->ADC_CFG =
      (g_adc_cfg.osr_sel     << AUDCODEC_ADC_CFG_OSR_SEL_Pos)     |
      (0                     << AUDCODEC_ADC_CFG_OP_MODE_Pos)     |
      (0                     << AUDCODEC_ADC_CFG_PATH_RESET_Pos)  |
      (g_adc_cfg.clk_src_sel << AUDCODEC_ADC_CFG_CLK_SRC_SEL_Pos) |
      (g_adc_cfg.clk_div     << AUDCODEC_ADC_CFG_CLK_DIV_Pos);

  g_codec.Instance->ADC_CH0_CFG =
      (1    << AUDCODEC_ADC_CH0_CFG_ENABLE_Pos)      |
      (0    << AUDCODEC_ADC_CH0_CFG_HPF_BYPASS_Pos)  |
      (0x7  << AUDCODEC_ADC_CH0_CFG_HPF_COEF_Pos)    |
      (0    << AUDCODEC_ADC_CH0_CFG_STB_INV_Pos)     |
      (0    << AUDCODEC_ADC_CH0_CFG_DMA_EN_Pos)      |
      (SF32_ADC_ROUGH_VOL << AUDCODEC_ADC_CH0_CFG_ROUGH_VOL_Pos) |
      (SF32_ADC_FINE_VOL  << AUDCODEC_ADC_CH0_CFG_FINE_VOL_Pos)  |
      (1    << AUDCODEC_ADC_CH0_CFG_DATA_FORMAT_Pos);

  printf("mibot: audio setup codec channels done DAC_CFG=0x%08lx "
         "DAC_CH0=0x%08lx ADC_CFG=0x%08lx ADC_CH0=0x%08lx\n",
         (unsigned long)g_codec.Instance->DAC_CFG,
         (unsigned long)g_codec.Instance->DAC_CH0_CFG,
         (unsigned long)g_codec.Instance->ADC_CFG,
         (unsigned long)g_codec.Instance->ADC_CH0_CFG);

  /* SiFli's 16 kHz XTAL table uses a strobe divider of 3000. */
  g_prc.Instance = hwp_audprc;
  g_prc.Init.clk_div = 1;
  g_prc.Init.adc_div = 3000;
  g_prc.Init.dac_div = 3000;
  g_prc.Init.clk_sel = 0;

  /* Route TX0 once to both output sides; selector 5 means mute. */
  g_prc_dac_cfg.dst_sel = AUDPRC_TX_TO_CODEC;
  g_prc_dac_cfg.mixlsrc0 = 0;
  g_prc_dac_cfg.mixlsrc1 = 5;
  g_prc_dac_cfg.mixrsrc0 = 1;
  g_prc_dac_cfg.mixrsrc1 = 5;
  g_prc_dac_cfg.muxlsrc0 = 0;
  g_prc_dac_cfg.muxlsrc1 = 5;
  g_prc_dac_cfg.muxrsrc0 = 1;
  g_prc_dac_cfg.muxrsrc1 = 5;
  /* 0 dB.  bf0_adc_dac_path_cfg_init() in the vendor driver uses 0 here; the
   * previous +13 on top of a near-full-scale tone clipped the DAC path. */
  g_prc_dac_cfg.vol_l = 0;
  g_prc_dac_cfg.vol_r = 0;
  g_prc_dac_cfg.src_ch_en = 0;

  g_prc_adc_cfg.src_sel = AUDPRC_RX_FROM_CODEC;
  g_prc_adc_cfg.vol_l = 0;
  g_prc_adc_cfg.vol_r = 0;
  g_prc_adc_cfg.src_ch_en = 0;
  g_prc.Init.dac_cfg = g_prc_dac_cfg;
  g_prc.Init.adc_cfg = g_prc_adc_cfg;

  g_prc.hdma[HAL_AUDPRC_TX_CH0] = &g_prc_tx_dma;
  g_prc.hdma[HAL_AUDPRC_RX_CH0] = &g_prc_rx_dma;
#ifdef AUDPRC_TX0_DMA_INSTANCE
  g_prc_tx_dma.Instance = AUDPRC_TX0_DMA_INSTANCE;
#else
  g_prc_tx_dma.Instance = DMA1_Channel1;
#endif
#ifdef AUDPRC_TX0_DMA_REQUEST
  g_prc_tx_dma.Init.Request = AUDPRC_TX0_DMA_REQUEST;
#else
  g_prc_tx_dma.Init.Request = DMA_REQUEST_51;
#endif
#ifdef AUDPRC_RX0_DMA_INSTANCE
  g_prc_rx_dma.Instance = AUDPRC_RX0_DMA_INSTANCE;
#else
  g_prc_rx_dma.Instance = DMA1_Channel4;
#endif
#ifdef AUDPRC_RX0_DMA_REQUEST
  g_prc_rx_dma.Init.Request = AUDPRC_RX0_DMA_REQUEST;
#else
  g_prc_rx_dma.Init.Request = DMA_REQUEST_53;
#endif
  g_prc_tx_dma.Parent = &g_prc;
  g_prc_rx_dma.Parent = &g_prc;
  g_prc.dest_sel = AUDPRC_TX_TO_CODEC;

  sf_dma_irq_attach();

  if (HAL_AUDPRC_Init(&g_prc) != HAL_OK)
    {
      return -EIO;
    }

  /* Note: AUDPRC automatic clock gating (enabled by HAL_AUDPRC_Init) was
   * measured and ruled out as the cause of the once-per-DMA-wrap amplitude
   * dip; disabling it changed nothing, so the vendor setting is kept. */

  /* mode selects the channel layout of the DMA source: per the vendor header
   * (bf0_hal_audprc.h) 0 is mono and 1 is stereo, 16-bit format only.  The
   * padded int16 pairs this driver feeds are consumed as L/R, which is why
   * the odd slot is kept zero. */
  tx_cfg.dma_mask = 0;
  tx_cfg.mode = 1;
  tx_cfg.format = 0;
  tx_cfg.en = 1;
  rx_cfg = tx_cfg;
  if (HAL_AUDPRC_Config_TChanel(&g_prc, 0, &tx_cfg) != HAL_OK ||
      HAL_AUDPRC_Config_RChanel(&g_prc, 0, &rx_cfg) != HAL_OK ||
      HAL_AUDPRC_Config_DACPath(&g_prc, &g_prc_dac_cfg) != HAL_OK ||
      HAL_AUDPRC_Config_ADCPath(&g_prc, &g_prc_adc_cfg) != HAL_OK)
    {
      return -EIO;
    }

  /* The vendor HAL does not write dst_sel in Config_DACPath(). */
  __HAL_AUDPRC_DAC_DST_CODEC(&g_prc);
  g_prc.dest_sel = AUDPRC_TX_TO_CODEC;
  __HAL_AUDCODEC_DAC_ENABLE(&g_codec);
  __HAL_AUDPRC_DACPATH_ENABLE(&g_prc);

  /* The capture path is deliberately left disabled here and enabled only by
   * board_audio_stream_start().  Keeping it on permanently made the ADC
   * input FIFO overflow continuously (AUDPRC IRQ bit RX_IN_FIFO_OF stayed
   * set with no reader), and that contention on the shared AUDPRC pipeline
   * was audible as a regular chop on DAC playback.  The codec's internal
   * 1 kHz generator bypasses this pipeline, which is why it stayed clean. */
  __HAL_AUDPRC_ENABLE(&g_prc);

  g_ready = true;
  sf_dump_registers();
  printf("mibot: audio setup digital paths enabled\n");
  return 0;
}

static int sf_receive(struct i2s_dev_s *d, struct ap_buffer_s *a,
                      i2s_callback_t cb, void *arg, uint32_t t)
{
  (void)d;
  (void)t;
  if (sf_setup() < 0 || a == NULL)
    {
      return -EBUSY;
    }
  if (g_rx_apb != NULL)
    {
      return -EBUSY;
    }

  g_rx_apb = a;
  g_rx_cb = cb;
  g_rx_arg = arg;
  if (HAL_AUDPRC_Receive_DMA(&g_prc, a->samp, a->nmaxbytes,
                             HAL_AUDPRC_RX_CH0) != HAL_OK)
    {
      g_rx_apb = NULL;
      g_rx_cb = NULL;
      g_rx_arg = NULL;
      return -EIO;
    }
  return 0;
}

static int sf_send(struct i2s_dev_s *d, struct ap_buffer_s *a,
                   i2s_callback_t cb, void *arg, uint32_t t)
{
  (void)d;
  (void)t;
  if (sf_setup() < 0 || a == NULL)
    {
      return -EBUSY;
    }
  if (g_tx_apb != NULL)
    {
      return -EBUSY;
    }

  g_tx_apb = a;
  g_tx_cb = cb;
  g_tx_arg = arg;
  if (board_audio_pa_start() < 0 ||
      HAL_AUDPRC_Transmit_DMA(&g_prc, a->samp, a->nbytes,
                              HAL_AUDPRC_TX_CH0) != HAL_OK)
    {
      g_tx_apb = NULL;
      g_tx_cb = NULL;
      g_tx_arg = NULL;
      (void)board_audio_pa_stop();
      return -EIO;
    }
  return 0;
}

static int sf_stop(struct i2s_dev_s *d)
{
  (void)d;
  if (!g_ready)
    {
      return 0;
    }
  HAL_AUDPRC_DMAStop(&g_prc, HAL_AUDPRC_TX_CH0);
  HAL_AUDPRC_DMAStop(&g_prc, HAL_AUDPRC_RX_CH0);
  g_prc.State[HAL_AUDPRC_TX_CH0] = HAL_AUDPRC_STATE_READY;
  g_prc.State[HAL_AUDPRC_RX_CH0] = HAL_AUDPRC_STATE_READY;
  (void)board_audio_pa_stop();
  g_tx_apb = NULL;
  g_rx_apb = NULL;
  g_tx_cb = NULL;
  g_rx_cb = NULL;
  g_tx_arg = NULL;
  g_rx_arg = NULL;
  return 0;
}

static uint32_t sf_getmclk(struct i2s_dev_s *d)
{
  (void)d;
  return 0;
}

static uint32_t sf_setmclk(struct i2s_dev_s *d, uint32_t f)
{
  (void)d;
  return f;
}

static int sf_ioctl(struct i2s_dev_s *d, int c, unsigned long a)
{
  FAR struct audio_caps_desc_s *desc =
    (FAR struct audio_caps_desc_s *)(uintptr_t)a;
  uint8_t channels;
  uint32_t rate;
  int width;

  (void)d;
  if (c != AUDIOIOC_CONFIGURE || desc == NULL)
    {
      return -ENOTTY;
    }
  channels = desc->caps.ac_channels & 0x0f;
  rate = desc->caps.ac_controls.w;
  width = desc->caps.ac_format.hw == AUDIO_FMT_PCM ? 16 : 0;
  if (channels != 1 || rate != SF32_SAMPLE_RATE || width == 0)
    {
      return -EINVAL;
    }
  return sf_setup();
}

static const struct i2s_ops_s g_ops = {
  .i2s_rxchannels = sf_rxch,
  .i2s_rxsamplerate = sf_rate,
  .i2s_rxdatawidth = sf_width,
  .i2s_receive = sf_receive,
  .i2s_txchannels = sf_rxch,
  .i2s_txsamplerate = sf_rate,
  .i2s_txdatawidth = sf_width,
  .i2s_send = sf_send,
  .i2s_getmclkfrequency = sf_getmclk,
  .i2s_setmclkfrequency = sf_setmclk,
  .i2s_ioctl = sf_ioctl
};

static struct i2s_dev_s g_i2s = { &g_ops };

void HAL_AUDPRC_TxCpltCallback(AUDPRC_HandleTypeDef *h, int cid)
{
  if (h != &g_prc || cid != HAL_AUDPRC_TX_CH0)
    {
      return;
    }

  /* Three modes share this channel and the order matters.
   *   1. streaming playback: refill the second half, never stop the DMA
   *   2. tone/one-shot with g_tx_hold: circular replay, nothing to do
   *   3. one-shot without hold: stop and notify the upper half
   */
  if (g_play_active)
    {
      sf_play_fill(&g_play_dma[SF32_STREAM_PAD_WORDS]);
      return;
    }
  if (g_tx_hold)
    {
      /* Direct-tone circular DMA: acknowledge the completion and leave the
       * channel running.  Count only here so post-play diagnostics prove the
       * transfer continued throughout the requested duration. */
      g_tone_tx_full_count++;
      return;
    }
  HAL_AUDPRC_DMAStop(h, cid);
  h->State[cid] = HAL_AUDPRC_STATE_READY;
  if (g_tx_cb != NULL && g_tx_apb != NULL)
    {
      g_tx_cb(&g_i2s, g_tx_apb, g_tx_arg, 0);
    }
  g_tx_apb = NULL;
  g_tx_cb = NULL;
  g_tx_arg = NULL;
}

void HAL_AUDPRC_TxHalfCpltCallback(AUDPRC_HandleTypeDef *h, int cid)
{
  if (h != &g_prc || cid != HAL_AUDPRC_TX_CH0)
    {
      return;
    }
  if (g_tx_hold)
    {
      g_tone_tx_half_count++;
      return;
    }
  g_tx_half_count++;
  if (g_play_active)
    {
      sf_play_fill(&g_play_dma[0]);
    }
}

void HAL_AUDPRC_RxCpltCallback(AUDPRC_HandleTypeDef *h, int cid)
{
  if (h != &g_prc || cid != HAL_AUDPRC_RX_CH0)
    {
      return;
    }

  /* Streaming mode keeps the circular transfer running and just drains the
   * second half of the buffer. */
  if (g_stream_active)
    {
      g_stream_rx_full_count++;
      sf_stream_push(&g_stream_dma[SF32_STREAM_PAD_WORDS]);
      return;
    }

  /* HAL_AUDPRC_Receive_DMA() forces DMA_CIRCULAR whenever dest_sel is not
   * AUDPRC_TX_TO_MEM, so a one-shot capture wraps and overwrites the start
   * of the buffer unless the channel is halted right here in the callback. */
  HAL_AUDPRC_DMAStop(h, cid);
  g_rx_done = true;
  h->State[cid] = HAL_AUDPRC_STATE_READY;
  if (g_rx_cb != NULL && g_rx_apb != NULL)
    {
      g_rx_cb(&g_i2s, g_rx_apb, g_rx_arg, 0);
    }
  g_rx_apb = NULL;
  g_rx_cb = NULL;
  g_rx_arg = NULL;
}

/* Take the even int16 slots of a padded buffer and write them back to back.
 * Interrupt context: keep it branch-free and allocation-free. */
void sf_compact_frame(int16_t *dst, const int16_t *padded, int samples)
{
  int i;

  for (i = 0; i < samples; i++)
    {
      dst[i] = padded[i * 2];
    }
}

/* Inverse of sf_compact_frame(): dense PCM into the even slots, zero into the
 * odd ones.  A non-zero odd slot reaches the DAC as noise. */
void sf_expand_frame(int16_t *padded, const int16_t *src, int samples)
{
  int i;

  for (i = 0; i < samples; i++)
    {
      padded[i * 2]     = src[i];
      padded[i * 2 + 1] = 0;
    }
}

/* Compact one padded half of the DMA buffer into a dense PCM frame.  Called
 * from DMA interrupt context, so it must stay short and lock-free. */
static void sf_stream_push(const int16_t *padded)
{
  uint32_t next;
  int16_t *dst;

  if (!g_stream_active)
    {
      return;
    }

  /* Drop the ADC settling transient instead of publishing it.  Counted here
   * rather than filtered later so no consumer ever sees the DC step. */
  if (g_stream_warmup > 0)
    {
      g_stream_warmup--;
      g_stream_warmup_dropped++;
      return;
    }

  next = (g_stream_head + 1) % SF32_STREAM_QUEUE_DEPTH;
  if (next == g_stream_tail)
    {
      /* Queue full.  Per the UART spec audio frames are dropped rather than
       * blocking, so discard the oldest frame and keep the newest. */
      g_stream_tail = (g_stream_tail + 1) % SF32_STREAM_QUEUE_DEPTH;
      g_stream_dropped++;
    }

  dst = g_stream_queue[g_stream_head];
  sf_compact_frame(dst, padded, SF32_STREAM_FRAME_SAMPLES);
  g_stream_head = next;

  /* nxsem_post() is safe from interrupt context; it only wakes a waiter. */
  if (g_stream_sem_ready)
    {
      nxsem_post(&g_stream_sem);
    }
}
/* Expand one queued frame into a padded half of the TX DMA buffer.  Runs in
 * DMA interrupt context.  An empty queue must not stop the transfer: the
 * vendor HAL forces DMA_CIRCULAR for AUDPRC_TX_TO_CODEC, and tearing the
 * channel down mid-stream produces an audible click.  Emit silence instead
 * and let the counter show the starvation. */
static void sf_play_fill(int16_t *padded)
{
  const int16_t *src;

  g_play_fills++;

  if (g_play_tail == g_play_head)
    {
      memset(padded, 0, SF32_STREAM_PAD_WORDS * sizeof(int16_t));
      g_play_underrun++;
      return;
    }

  src = g_play_queue[g_play_tail];
  sf_expand_frame(padded, src, SF32_STREAM_FRAME_SAMPLES);
  g_play_tail = (g_play_tail + 1) % SF32_PLAY_QUEUE_DEPTH;

  /* Level adjustment happens after expansion rather than inside
   * sf_expand_frame(), which audio_pbt checks for exact compact/expand
   * round-tripping.  The frame has already been dequeued above, so muting here
   * leaves queue depth, fill count and DAC timing exactly as they would be. */
#if SF32_PLAY_MUTE
  memset(padded, 0, SF32_STREAM_PAD_WORDS * sizeof(int16_t));
#elif SF32_PLAY_ATTEN_SHIFT > 0
  {
    int i;

    for (i = 0; i < SF32_STREAM_FRAME_SAMPLES; i++)
      {
        padded[i * 2] = (int16_t)(padded[i * 2] >> SF32_PLAY_ATTEN_SHIFT);
      }
  }
#endif
}

void HAL_AUDPRC_RxHalfCpltCallback(AUDPRC_HandleTypeDef *h, int cid)
{
  if (h != &g_prc || cid != HAL_AUDPRC_RX_CH0)
    {
      return;
    }
  g_stream_rx_half_count++;
  sf_stream_push(&g_stream_dma[0]);
}

void HAL_AUDPRC_ErrorCallback(AUDPRC_HandleTypeDef *h, int cid)
{
  if (h == &g_prc)
    {
      g_stream_error_count++;
      printf("mibot: AUDPRC DMA error channel=%d code=0x%08lx\n", cid,
             (unsigned long)h->ErrorCode);
    }
}

int board_audio_i2s_initialize(void)
{
  int ret;
  FAR struct audio_lowerhalf_s *tx;
  FAR struct audio_lowerhalf_s *rx;

  if (g_audio_registered)
    {
      return 0;
    }

#ifdef CONFIG_AUDIO_I2SCHAR
  ret = i2schar_register(&g_i2s, 0);
  if (ret < 0 && ret != -EEXIST)
    {
      return ret;
    }
#endif
  tx = audio_i2s_initialize(&g_i2s, true);
  rx = audio_i2s_initialize(&g_i2s, false);
  if (tx == NULL || rx == NULL)
    {
      return -ENOMEM;
    }
  ret = audio_register("pcm0", tx);
  if (ret < 0 && ret != -EEXIST)
    {
      return ret;
    }
  ret = audio_register("pcm_in0", rx);
  if (ret < 0 && ret != -EEXIST)
    {
      return ret;
    }
  g_audio_registered = true;
  return 0;
}

int board_audio_i2s_stop(void)
{
  return sf_stop(&g_i2s);
}

void board_audio_i2s_set_repeat(bool enable)
{
  g_tx_hold = enable;
}

/* Codec built-in 1 kHz generator, independent of the PCM DMA path. */
int board_audio_codec_hw_tone(int seconds)
{
  if (seconds <= 0 || sf_setup() < 0)
    {
      return -EIO;
    }

  /* sf_setup() intentionally leaves the DAC muted to prevent startup pops.
   * Apply the same safe ordering as the PCM path so this diagnostic is
   * actually audible: arm generator while muted, power/settle PA, unmute. */
  g_codec.Instance->DAC_CH0_CFG |= AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  g_codec.Instance->CFG |= AUDCODEC_CFG_DAC_1K_MODE;
  __HAL_AUDCODEC_DAC_ENABLE(&g_codec);
  if (board_audio_pa_start() < 0)
    {
      g_codec.Instance->CFG &= ~AUDCODEC_CFG_DAC_1K_MODE;
      return -EIO;
    }

  usleep(30000);
  g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  printf("mibot: hw tone CFG=0x%08lx DAC0=0x%08lx PA=on\n",
         (unsigned long)g_codec.Instance->CFG,
         (unsigned long)g_codec.Instance->DAC_CH0_CFG);
  usleep((useconds_t)seconds * 1000000U);

  g_codec.Instance->DAC_CH0_CFG |= AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  g_codec.Instance->CFG &= ~AUDCODEC_CFG_DAC_1K_MODE;
  (void)board_audio_pa_stop();
  g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  printf("audio: hardware 1k test complete\n");
  return 0;
}

/* 2 kHz at 16 kHz is exactly 8 samples per period, so this table keeps the
 * phase continuous across frame boundaries.  Half scale leaves headroom. */
static const int16_t g_tone_sine8[8] =
{
  0, 11314, 16000, 11314, 0, -11314, -16000, -11314
};

/* Play a test tone through the streaming playback path.
 *
 * This deliberately does NOT hand the DAC one long circular buffer.  A
 * microphone measurement of the speaker (va_test spkmic) showed that a
 * fire-and-forget circular transfer drops the output for several
 * milliseconds once per wrap - the dip period tracked the buffer length
 * exactly (200 ms buffer -> 200 ms, 50 ms buffer -> 50 ms) while every FIFO
 * status bit stayed clean, and the TX channel has no underflow flag to
 * report it.  The same measurement over the streaming path (20 ms frames
 * refilled from the DMA half/full callbacks, which is also what the vendor
 * driver does and what real TTS uses) was continuous, so the tone now reuses
 * it (va_test spkstream).
 */
int board_audio_play_tone(int seconds)
{
  struct timespec now;
  struct timespec end;
  uint32_t n = 0;
  int ret;

  if (seconds <= 0)
    {
      return -EINVAL;
    }
  if (sf_setup() < 0)
    {
      return -EIO;
    }

  ret = board_audio_play_start();
  if (ret < 0)
    {
      return ret;
    }

  clock_gettime(CLOCK_MONOTONIC, &end);
  end.tv_sec += seconds;

  for (;;)
    {
      int16_t frame[SF32_STREAM_FRAME_SAMPLES];
      int queued;

      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > end.tv_sec ||
          (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec))
        {
          break;
        }

      /* Top the queue up, then wait about one frame period.  A full queue
       * makes board_audio_play_write() drop, which paces this loop. */
      for (queued = 0; queued < SF32_PLAY_QUEUE_DEPTH; queued++)
        {
          int i;

          for (i = 0; i < SF32_STREAM_FRAME_SAMPLES; i++)
            {
              frame[i] = g_tone_sine8[(n + (uint32_t)i) & 7];
            }

          if (board_audio_play_write(frame, sizeof(frame)) !=
              (int)sizeof(frame))
            {
              break;
            }
          n += SF32_STREAM_FRAME_SAMPLES;
        }

      usleep(20000);
    }

  (void)board_audio_play_drain(500);
  ret = board_audio_play_stop();
  printf("audio: direct tone complete underrun=%lu dropped=%lu\n",
         (unsigned long)g_play_underrun, (unsigned long)g_play_dropped);
  return ret;
}

/* Legacy long-circular-buffer tone, kept only for A/B measurement against
 * the streaming path above.  Do not use for product audio: it chops once per
 * DMA wrap (see the note on board_audio_play_tone). */
static int board_audio_play_tone_circular(int seconds)
{
  int i;

  if (seconds <= 0)
    {
      return -EINVAL;
    }
  if (sf_setup() < 0)
    {
      return -EIO;
    }
  /* Match the validated streaming DMA layout: PCM in each even int16 slot,
   * zero padding in each odd slot.  A dense int16 sine is not a valid mono
   * AUDPRC DMA buffer on this board. */
  for (i = 0; i < SF32_TONE_FRAMES; i++)
    {
      /* Half scale.  Near-full-scale amplitude leaves no headroom for the
       * DAC path and clips instead of getting louder. */
      g_tone_samples[i * 2] = (int16_t)(16000.0 *
        sin(6.283185307 * 2000.0 * i / SF32_SAMPLE_RATE));
      g_tone_samples[i * 2 + 1] = 0;
    }
  /* Count DMA interrupts without logging during playback.  With 3200
   * frames, a 2 s tone should report about 10 full and 10 half cycles. */
  g_tone_tx_half_count = 0;
  g_tone_tx_full_count = 0;
  /* Vendor start-up order (example/rt_device/i2s start_tx): mute the DAC,
   * start the transfer, enable the amplifier, let the analog path settle,
   * then unmute.  Unmuting while the PA powers up produces the click that
   * was previously mistaken for the whole tone. */
  g_codec.Instance->DAC_CH0_CFG |= AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;

  g_tx_hold = true;
  if (HAL_AUDPRC_Transmit_DMA(&g_prc, (uint8_t *)g_tone_samples,
                              sizeof(g_tone_samples),
                              HAL_AUDPRC_TX_CH0) != HAL_OK)
    {
      g_tx_hold = false;
      g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
      return -EIO;
    }

  if (board_audio_pa_start() < 0)
    {
      g_tx_hold = false;
      HAL_AUDPRC_DMAStop(&g_prc, HAL_AUDPRC_TX_CH0);
      g_prc.State[HAL_AUDPRC_TX_CH0] = HAL_AUDPRC_STATE_READY;
      g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
      return -EIO;
    }

  usleep(30000);
  g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  printf("mibot: AUDPRC TX0 circular start bytes=%u\n",
         (unsigned)sizeof(g_tone_samples));

  /* Sample the FIFO status registers while the tone plays and report only
   * after it stops.  A rate mismatch between the DMA feed and the DAC shows
   * up here as a sticky AUDPRC TX0 overflow / TX_OUT underflow bit or as a
   * codec strobe FIFO count that keeps hitting 0 or its maximum.  Register
   * reads are non-blocking; no printf() may run during playback because the
   * console write is blocking and would itself starve the DAC. */
  {
    uint32_t irq_seen = 0;
    uint8_t stb_min = 0xff;
    uint8_t stb_max = 0;
    uint8_t apb_min = 0xff;
    uint8_t apb_max = 0;
    unsigned samples_taken = 0;
    struct timespec now;
    struct timespec end;

    /* Clear the sticky FIFO flags (write-1-to-clear) before measuring. */
    g_prc.Instance->IRQ = g_prc.Instance->IRQ & 0xffffu;

    /* Bound the loop by the wall clock.  The system tick is 20 ms on this
     * board, so an iteration count multiplied by a 2 ms usleep() would run
     * ten times longer than the requested tone duration. */
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += seconds;

    for (;;)
      {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > end.tv_sec ||
            (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec))
          {
            break;
          }
        uint8_t stb = (uint8_t)((g_codec.Instance->DAC_CH0_CFG &
                                 AUDCODEC_DAC_CH0_CFG_STB_FIFO_CNT_Msk) >>
                                AUDCODEC_DAC_CH0_CFG_STB_FIFO_CNT_Pos);
        uint8_t apb = (uint8_t)((g_codec.Instance->APB_STAT &
                                 AUDCODEC_APB_STAT_DAC_CH0_FIFO_CNT_Msk) >>
                                AUDCODEC_APB_STAT_DAC_CH0_FIFO_CNT_Pos);

        irq_seen |= g_prc.Instance->IRQ & 0xffffu;
        if (stb < stb_min) stb_min = stb;
        if (stb > stb_max) stb_max = stb;
        if (apb < apb_min) apb_min = apb;
        if (apb > apb_max) apb_max = apb;
        samples_taken++;
        usleep(2000);
      }

    g_tone_irq_seen = irq_seen;
    g_tone_stb_min = stb_min;
    g_tone_stb_max = stb_max;
    g_tone_apb_min = apb_min;
    g_tone_apb_max = apb_max;
  }
  g_codec.Instance->DAC_CH0_CFG |= AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  g_tx_hold = false;
  HAL_AUDPRC_DMAStop(&g_prc, HAL_AUDPRC_TX_CH0);
  g_prc.State[HAL_AUDPRC_TX_CH0] = HAL_AUDPRC_STATE_READY;
  (void)board_audio_pa_stop();
  g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  printf("audio: direct tone complete dma_full=%lu dma_half=%lu dma_errors=%lu\n",
         (unsigned long)g_tone_tx_full_count,
         (unsigned long)g_tone_tx_half_count,
         (unsigned long)g_stream_error_count);
  printf("audio: tone fifo irq=0x%04lx tx0_of=%d txout_uf=%d "
         "stb_cnt=%u..%u apb_cnt=%u..%u\n",
         (unsigned long)g_tone_irq_seen,
         (g_tone_irq_seen & AUDPRC_IRQ_TX0_FIFO_OF_Msk) != 0 ? 1 : 0,
         (g_tone_irq_seen & AUDPRC_IRQ_TX_OUT_FIFO_UF_Msk) != 0 ? 1 : 0,
         (unsigned)g_tone_stb_min, (unsigned)g_tone_stb_max,
         (unsigned)g_tone_apb_min, (unsigned)g_tone_apb_max);
  return 0;
}

/* Record from the on-board microphone, then play the capture back through
 * the speaker.  Both directions use one shot AUDPRC transfers: the RX
 * complete callback stops the channel, so poll the DMA state instead of
 * relying on a repeating buffer. */
/* Uplink capture ---------------------------------------------------------
 *
 * RX DMA runs continuously over a two-frame circular buffer.  The half and
 * full callbacks each compact one frame into the queue and post the
 * semaphore, so consumers block instead of polling: the system tick is 20 ms
 * on this board, which is exactly one frame, leaving no margin for a poll
 * loop to keep up.
 */
int board_audio_stream_start(void)
{
  if (sf_setup() < 0)
    {
      return -EIO;
    }
  if (g_stream_active)
    {
      return 0;
    }

  if (g_stream_sem_ready)
    {
      g_stream_sem_ready = false;
      nxsem_destroy(&g_stream_sem);
    }
  if (nxsem_init(&g_stream_sem, 0, 0) < 0)
    {
      return -EIO;
    }
  nxsem_set_protocol(&g_stream_sem, SEM_PRIO_NONE);
  g_stream_sem_ready = true;

  g_stream_head = 0;
  g_stream_tail = 0;
  g_stream_dropped = 0;
  g_stream_rx_half_count = 0;
  g_stream_rx_full_count = 0;
  g_stream_error_count = 0;
  g_stream_warmup = SF32_STREAM_WARMUP_FRAMES;
  g_stream_warmup_dropped = 0;
  memset(g_stream_dma, 0, sizeof(g_stream_dma));
  g_stream_active = true;

  /* Enable the capture path only for the duration of the stream; see the
   * note in sf_setup() about the ADC input FIFO overflowing when nothing
   * drains it. */
  __HAL_AUDCODEC_ADC_ENABLE(&g_codec);
  __HAL_AUDPRC_ADCPATH_ENABLE(&g_prc);

  HAL_StatusTypeDef hal_ret = HAL_AUDPRC_Receive_DMA(
      &g_prc, (uint8_t *)g_stream_dma, sizeof(g_stream_dma),
      HAL_AUDPRC_RX_CH0);
  printf("mibot: audio RX DMA start hal=%d state=%d dma_state=%d active=%d "
         "bytes=%u gain=%ddB(rough=%u,fine=%u) warmup=%u\n",
         (int)hal_ret, (int)g_prc.State[HAL_AUDPRC_RX_CH0],
         g_prc.hdma[HAL_AUDPRC_RX_CH0] != NULL
           ? (int)g_prc.hdma[HAL_AUDPRC_RX_CH0]->State : -1,
         g_stream_active ? 1 : 0, (unsigned)sizeof(g_stream_dma),
         SF32_ADC_GAIN_DB, (unsigned)SF32_ADC_ROUGH_VOL,
         (unsigned)SF32_ADC_FINE_VOL, (unsigned)SF32_STREAM_WARMUP_FRAMES);
  if (hal_ret != HAL_OK)
    {
      g_stream_active = false;
      __HAL_AUDPRC_ADCPATH_DISABLE(&g_prc);
      __HAL_AUDCODEC_ADC_DISABLE(&g_codec);
      return -EIO;
    }
  return 0;
}

/* Block until a frame is queued.  Returns 0 on wake-up, negative on timeout. */
int board_audio_stream_wait(int timeout_ms)
{
  if (!g_stream_active || !g_stream_sem_ready)
    {
      return -EPERM;
    }
  if (g_stream_tail != g_stream_head)
    {
      return 0;
    }
  return nxsem_tickwait(&g_stream_sem, MSEC2TICK(timeout_ms));
}

/* Copy one dense PCM frame out of the queue.  Returns the byte count, 0 when
 * nothing is pending, or a negative errno. */
int board_audio_stream_read(void *buf, size_t len)
{
  if (buf == NULL || len < SF32_STREAM_FRAME_BYTES)
    {
      return -EINVAL;
    }
  if (!g_stream_active)
    {
      return -EPERM;
    }
  if (g_stream_tail == g_stream_head)
    {
      return 0;
    }

  memcpy(buf, g_stream_queue[g_stream_tail], SF32_STREAM_FRAME_BYTES);
  g_stream_tail = (g_stream_tail + 1) % SF32_STREAM_QUEUE_DEPTH;
  return (int)SF32_STREAM_FRAME_BYTES;
}

int board_audio_stream_stop(void)
{
  if (!g_stream_active)
    {
      return 0;
    }
  g_stream_active = false;
  HAL_AUDPRC_DMAStop(&g_prc, HAL_AUDPRC_RX_CH0);
  g_prc.State[HAL_AUDPRC_RX_CH0] = HAL_AUDPRC_STATE_READY;
  /* Stop feeding the ADC input FIFO so it cannot overflow while idle. */
  __HAL_AUDPRC_ADCPATH_DISABLE(&g_prc);
  __HAL_AUDCODEC_ADC_DISABLE(&g_codec);
  printf("mibot: audio RX stop warmup_dropped=%lu queue_dropped=%lu "
         "errors=%lu\n",
         (unsigned long)g_stream_warmup_dropped,
         (unsigned long)g_stream_dropped,
         (unsigned long)g_stream_error_count);

  /* Wake a consumer parked in board_audio_stream_wait(). */
  if (g_stream_sem_ready)
    {
      nxsem_post(&g_stream_sem);
    }
  return 0;
}

uint32_t board_audio_stream_dropped(void)
{
  return g_stream_dropped;
}
/* Downlink playback -------------------------------------------------------
 *
 * Start-up order is the one validated by board_audio_play_tone(): mute the
 * DAC, start the DMA, enable the amplifier, let the analog path settle, then
 * unmute.  Unmuting while NS4150B powers up is audible as a click.
 */
int board_audio_play_start(void)
{
  if (sf_setup() < 0)
    {
      return -EIO;
    }
  if (g_play_active)
    {
      return 0;
    }

  g_play_head = 0;
  g_play_tail = 0;
  g_play_dropped = 0;
  g_play_underrun = 0;
  g_play_fills = 0;
  g_play_peak_queued = 0;
  g_play_started_ms = 0;
  memset(g_play_dma, 0, sizeof(g_play_dma));

  g_codec.Instance->DAC_CH0_CFG |= AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  g_play_active = true;
  /* Accept frames now, start the DAC once SF32_PLAY_PREFILL_FRAMES are banked
   * (sf_play_begin_dma(), driven from board_audio_play_write()). */
  g_play_armed = true;
  return 0;
}

/* Start the transfer + amplifier.  Split out of board_audio_play_start() so it
 * can run once the prefill has arrived.  Keeps the validated ordering: DAC
 * already muted, start DMA, enable the amplifier, let the analog path settle,
 * then unmute -- unmuting while the NS4150B powers up is an audible click. */
static int sf_play_begin_dma(void)
{
  if (HAL_AUDPRC_Transmit_DMA(&g_prc, (uint8_t *)g_play_dma,
                              sizeof(g_play_dma),
                              HAL_AUDPRC_TX_CH0) != HAL_OK)
    {
      g_play_active = false;
      g_play_armed = false;
      g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
      return -EIO;
    }

  if (board_audio_pa_start() < 0)
    {
      g_play_active = false;
      g_play_armed = false;
      HAL_AUDPRC_DMAStop(&g_prc, HAL_AUDPRC_TX_CH0);
      g_prc.State[HAL_AUDPRC_TX_CH0] = HAL_AUDPRC_STATE_READY;
      g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
      return -EIO;
    }

  /* Deliberately NOT sleeping here to let the amplifier settle before unmuting.
   * This runs from board_audio_play_write(), i.e. from inside the agent's UART
   * drain loop, so a 30 ms sleep here stops the SF32 reading the link for 30 ms
   * -- long enough to overflow the 4 KB driver RX buffer and lose the audio this
   * function exists to play.  The queue already holds SF32_PLAY_PREFILL_FRAMES
   * (480 ms) of samples, so the DAC has plenty to chew on while the NS4150B
   * comes up; unmuting immediately costs at most a faint power-up click. */
  g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  g_play_armed = false;
  /* Clock the cadence from here, not from play_start(): the prefill wait can be
   * seconds long while the cloud synthesises, and including it would swamp the
   * per-fill average. */
  g_play_started_ms = sf_now_ms();
  g_play_fills = 0;
  return 0;
}

static uint32_t sf_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

/* Frames currently banked in the playback queue. */
static uint32_t sf_play_queued(void)
{
  return (g_play_head + SF32_PLAY_QUEUE_DEPTH - g_play_tail) %
         SF32_PLAY_QUEUE_DEPTH;
}

/* Queue one 640-byte dense PCM frame.  Drops the newest frame when full so
 * that audio already committed to the DMA keeps playing without a gap. */
int board_audio_play_write(const void *buf, size_t len)
{
  uint32_t next;

  if (buf == NULL || len != SF32_STREAM_FRAME_BYTES)
    {
      return -EINVAL;
    }
  if (!g_play_active)
    {
      return -EPERM;
    }

  next = (g_play_head + 1) % SF32_PLAY_QUEUE_DEPTH;
  if (next == g_play_tail)
    {
      g_play_dropped++;
      return 0;
    }

  memcpy(g_play_queue[g_play_head], buf, SF32_STREAM_FRAME_BYTES);
  g_play_head = next;

  {
    const uint32_t queued = sf_play_queued();

    if (queued > g_play_peak_queued)
      {
        g_play_peak_queued = queued;
      }
  }

  if (g_play_armed && sf_play_queued() >= SF32_PLAY_PREFILL_FRAMES)
    {
      if (sf_play_begin_dma() < 0)
        {
          return -EIO;
        }
    }
  return (int)SF32_STREAM_FRAME_BYTES;
}

/* Wait for the queue to empty so the tail of an utterance is not cut off.
 * The tick is 20 ms on this board, so poll on that granularity. */
int board_audio_play_drain(int timeout_ms)
{
  int waited;

  if (!g_play_active)
    {
      return 0;
    }

  /* A reply shorter than the prefill never reached the threshold, so the DAC was
   * never started.  Start it now rather than discarding the audio. */
  if (g_play_armed)
    {
      if (g_play_head == g_play_tail)
        {
          return 0;               /* nothing was ever queued */
        }
      if (sf_play_begin_dma() < 0)
        {
          return -EIO;
        }
    }

  for (waited = 0; waited < timeout_ms; waited += 20)
    {
      if (g_play_tail == g_play_head)
        {
          /* One more buffer half may still be in flight. */
          usleep(40000);
          return 0;
        }
      usleep(20000);
    }
  return -ETIMEDOUT;
}

int board_audio_play_stop(void)
{
  if (!g_play_active)
    {
      return 0;
    }

  g_codec.Instance->DAC_CH0_CFG |= AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  g_play_active = false;

  /* Only tear the channel down if it was actually started; stopping a transfer
   * that never began would leave the HAL state inconsistent for the next
   * stream. */
  if (!g_play_armed)
    {
      HAL_AUDPRC_DMAStop(&g_prc, HAL_AUDPRC_TX_CH0);
      g_prc.State[HAL_AUDPRC_TX_CH0] = HAL_AUDPRC_STATE_READY;
      (void)board_audio_pa_stop();
    }
  g_play_armed = false;
  {
    const uint32_t fills = g_play_fills;
    const uint32_t elapsed = g_play_started_ms != 0
                               ? sf_now_ms() - g_play_started_ms : 0;
    /* x100 so one integer carries two decimals without pulling in floats. */
    const uint32_t per_fill_x100 = fills != 0 ? (elapsed * 100U) / fills : 0;

    printf("mibot: audio TX stop underrun=%lu dropped=%lu fills=%lu "
           "elapsed=%lums per_fill=%lu.%02lums peakq=%lu/%d\n",
           (unsigned long)g_play_underrun, (unsigned long)g_play_dropped,
           (unsigned long)fills, (unsigned long)elapsed,
           (unsigned long)(per_fill_x100 / 100U),
           (unsigned long)(per_fill_x100 % 100U),
           (unsigned long)g_play_peak_queued, SF32_PLAY_QUEUE_DEPTH);
  }
  g_codec.Instance->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DOUT_MUTE_Msk;
  return 0;
}

uint32_t board_audio_play_dropped(void)
{
  return g_play_dropped;
}

uint32_t board_audio_play_underrun(void)
{
  return g_play_underrun;
}
