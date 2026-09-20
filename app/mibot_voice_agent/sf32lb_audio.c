/* SF32LB52 on-chip audio analog bring-up.  Kept in the board layer because
 * the SiFli HAL depends on the chip CMSIS register environment. */
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "bf0_hal.h"

#define AUDIO_PA_GPIO       hwp_gpio1
#define AUDIO_PA_PIN        10
/* The DevKit-LCD amplifier is an NS4150B.  Its CTRL input is a normal
 * active-high enable; it is not the pulse-count interface used by AW8155. */
#define NS4150B_STARTUP_US  5000
#define NS4150B_SHUTDOWN_US 1000

static bool g_audio_analog_ready;
static bool g_audio_pa_initialized;
static bool g_audio_pa_enabled;

/* Configure the DevKit-LCD Class-D amplifier control pin in its safe state.
 * Keep it low until the codec path is configured, then hold it high for the
 * entire DMA transfer. */
static void board_audio_pa_init(void)
{
  GPIO_InitTypeDef gpio;

  if (g_audio_pa_initialized)
    {
      return;
    }

  memset(&gpio, 0, sizeof(gpio));
  gpio.Pin = AUDIO_PA_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT;
  gpio.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(AUDIO_PA_GPIO, &gpio);
  HAL_GPIO_WritePin(AUDIO_PA_GPIO, AUDIO_PA_PIN, GPIO_PIN_RESET);
  /* Let the shutdown state settle before any codec activity. */
  HAL_Delay_us(NS4150B_SHUTDOWN_US);
  g_audio_pa_initialized = true;
  g_audio_pa_enabled = false;
}

int board_audio_pa_start(void)
{
  board_audio_pa_init();
  if (g_audio_pa_enabled)
    {
      return 0;
    }

  /* NS4150B CTRL is level-sensitive.  Do not generate a short pulse: the
   * enable level must remain asserted while DAC samples are being sent. */
  HAL_GPIO_WritePin(AUDIO_PA_GPIO, AUDIO_PA_PIN, GPIO_PIN_SET);
  HAL_Delay_us(NS4150B_STARTUP_US);

  g_audio_pa_enabled = true;
  return 0;
}

int board_audio_pa_stop(void)
{
  board_audio_pa_init();
  if (!g_audio_pa_enabled)
    {
      return 0;
    }

  HAL_GPIO_WritePin(AUDIO_PA_GPIO, AUDIO_PA_PIN, GPIO_PIN_RESET);
  HAL_Delay_us(NS4150B_SHUTDOWN_US);
  g_audio_pa_enabled = false;
  return 0;
}

int board_audio_analog_ready(void)
{
  return g_audio_analog_ready ? 1 : 0;
}

int board_audio_analog_init(void)
{
  AUDCODE_DAC_CLK_CONFIG_TYPE dac;
  AUDCODE_ADC_CLK_CONFIG_TYPE adc;

  if (g_audio_analog_ready)
    {
      return 0;
    }

  /* Match the vendor SDK bring-up order: power and clock gates must be
   * enabled before touching AUDCODEC analog registers. */
  HAL_PMU_EnableAudio(1);
  printf("mibot: audio analog PMU done\n");
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC_HP);
  printf("mibot: audio analog HP clock done\n");
  HAL_RCC_EnableModule(RCC_MOD_AUDCODEC_LP);
  printf("mibot: audio analog LP clock done\n");

  /* Config_Analog_* only programs the per-channel clocks and bias.  The
   * shared bandgap, reference generator and audio PLL are enabled by this
   * separate SiFli HAL entry and must be started first. */
  HAL_TURN_ON_PLL();
  printf("mibot: audio analog reference/PLL done\n");

  /* Vendor production table row for SF32LB52X / XTAL / 16 kHz, taken from
   * codec_dac_clk_config_xtal[] in drv_audcodec_m.c:
   *   {16000, 0, clk_div 1, osr_sel 4, 0x14D, 0, 5, 4, 2, 20, 20, 0}
   * Must stay identical to g_dac_cfg in sf32lb_i2s.c: a clk_div/osr_sel pair
   * that disagrees with the AUDPRC 16 kHz strobe makes the DAC drain the
   * stream at the wrong rate and the PCM output drops out periodically. */
  memset(&dac, 0, sizeof(dac));
  dac.samplerate = 16000;
  dac.clk_src_sel = 0;
  dac.clk_div = 1;
  dac.osr_sel = 4;
  dac.sinc_gain = 0x14d;
  dac.sel_clk_dac_source = 0;
  dac.diva_clk_dac = 5;
  dac.diva_clk_chop_dac = 4;
  dac.divb_clk_chop_dac = 2;
  dac.diva_clk_chop_bg = 20;
  dac.diva_clk_chop_refgen = 20;
  dac.sel_clk_dac = 0;
  if (HAL_AUDCODEC_Config_Analog_DACPath(&dac) != HAL_OK)
    {
      return -EIO;
    }
  printf("mibot: audio analog DAC path done\n");

  /* Official SDK XTAL table entry for 16 kHz. */
  memset(&adc, 0, sizeof(adc));
  adc.samplerate = 16000;
  adc.clk_src_sel = 0;
  adc.clk_div = 10;
  adc.osr_sel = 1;
  adc.sel_clk_adc_source = 0;
  adc.sel_clk_adc = 0;
  adc.diva_clk_adc = 5;
  adc.fsp = 2;
  HAL_AUDCODEC_Config_Analog_ADCPath(&adc);
  printf("mibot: audio analog ADC path done\n");

  /* AU_PA_EN is PA10 on the DevKit-LCD.  Initialise it low; NS4150B is
   * enabled with a stable high level only when playback begins. */
  board_audio_pa_init();
  g_audio_analog_ready = true;
  return 0;
}
