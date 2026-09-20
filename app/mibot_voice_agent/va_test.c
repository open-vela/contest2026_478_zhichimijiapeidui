/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - standalone hardware test command (`va_test`).
 *
 * This provides a NSH entry point to exercise mic / speaker / motor / servo
 * one at a time, independent of the dialog main state machine and without
 * requiring the cloud (需求 10.x).  Task 5.1 implements the mic / speaker
 * branches; task 5.2 adds the motor / servo branches, which send robot.*
 * commands over the AA55/CRC UART link and let the ESP32 enforce range checks
 * and the safety_task (需求 10.3-10.7).
 *
 * The whole file is device-only: it touches board_audio_* and the UART link,
 * so it is compiled out of the host build via `#if defined(__NuttX__)`.  The
 * host test project builds only the pure-logic core + stubs.
 */

#if defined(__NuttX__)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>

#include "mibot_voice_agent.h"
#include "va_test_args.h"

/* One capture frame is 640 bytes of 16 kHz mono PCM, i.e. 320 samples and a
 * 20 ms period, so a healthy stream produces ~50 frames per second. */
#define VA_FRAME_BYTES   640
#define VA_FRAMES_PER_SEC 50

/* Default test duration when no [seconds] argument is supplied. */
#define VA_TEST_DEFAULT_SECONDS 3

/* UART to the ESP32.  Prefer the voice-agent Kconfig option, fall back to the
 * shared mibot_agent option, then to /dev/ttyS0. */
#if defined(CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_UART)
#  define VA_TEST_UART CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_UART
#elif defined(CONFIG_EXAMPLES_MIBOT_AGENT_UART)
#  define VA_TEST_UART CONFIG_EXAMPLES_MIBOT_AGENT_UART
#else
#  define VA_TEST_UART "/dev/ttyS0"
#endif

/* AA55/CRC frame constants, copied verbatim from mibot_agent_main.c so the
 * standalone test speaks the exact same wire protocol to the ESP32. */
#define MIBOT_VERSION        1
#define MIBOT_MAX_PAYLOAD    4096
#define MIBOT_HEADER_SIZE    7
#define MIBOT_FRAME_OVERHEAD 11
#define MIBOT_TYPE_COMMAND   0x10
#define MIBOT_TYPE_ACK       0x11
#define MIBOT_TYPE_NACK      0x12

/* Wall-clock budget for waiting on the ESP32 ACK/NACK reply. */
#define VA_REPLY_TIMEOUT_MS  1500
#define VA_REPLY_BUF_SIZE    512

/* Board audio capture / playback API (implemented by the SF32 board layer).
 * Declared extern here exactly as sf32_audio_test.c does; this file only
 * reuses the existing driver, it does not reimplement it. */
extern int board_audio_play_tone(int seconds);
extern int board_audio_codec_hw_tone(int seconds);
extern int board_audio_play_start(void);
extern int board_audio_play_write(const void *buf, size_t len);
extern int board_audio_play_drain(int timeout_ms);
extern int board_audio_play_stop(void);
extern int board_audio_stream_start(void);
extern int board_audio_stream_read(void *buf, size_t len);
extern int board_audio_stream_wait(int timeout_ms);
extern int board_audio_stream_stop(void);
extern uint32_t board_audio_stream_dropped(void);

static uint32_t va_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* CRC16 (init 0xffff, poly 0x1021), computed from the version byte through the
 * end of the payload.  Copied verbatim from mibot_agent_main.c's mibot_crc16. */
static uint16_t va_crc16(const uint8_t *data, size_t len)
{
  uint16_t crc = 0xffff;
  size_t i;

  for (i = 0; i < len; i++)
    {
      int bit;
      crc ^= (uint16_t)data[i] << 8;
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc & 0x8000) != 0 ? (uint16_t)((crc << 1) ^ 0x1021)
                                    : (uint16_t)(crc << 1);
        }
    }

  return crc;
}

/* Retry on EINTR and EAGAIN/EWOULDBLOCK, mirroring mibot_write_all so a
 * non-blocking UART fd still drains the whole frame. */
static int va_write_all(int fd, const uint8_t *data, size_t len)
{
  while (len != 0)
    {
      ssize_t written = write(fd, data, len);
      if (written < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              usleep(1000);
              continue;
            }
          return -errno;
        }
      if (written == 0)
        {
          return -EIO;
        }
      data += written;
      len -= (size_t)written;
    }

  return 0;
}

/* Open the shared UART and put it in raw 8N1 mode at the mibot baud, matching
 * mibot_uart_open.  This standalone tool owns its own fd and does not depend on
 * any mibot_agent globals. */
static int va_uart_open(void)
{
  struct termios tio;
  speed_t speed;
  int fd;

  fd = open(VA_TEST_UART,
            O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0)
    {
      return -errno;
    }

  if (tcgetattr(fd, &tio) < 0)
    {
      close(fd);
      return -errno;
    }

  speed = B921600;
#ifdef B1000000
  speed = B1000000;
#endif
  cfmakeraw(&tio);
  cfsetspeed(&tio, speed);
  tio.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
  tio.c_cflag &= ~CRTSCTS;
#endif
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSANOW, &tio) < 0)
    {
      close(fd);
      return -errno;
    }

  return fd;
}

/* Build and send a COMMAND frame carrying a JSON payload.  The caller owns the
 * fd.  Frame layout matches mibot_send_frame exactly. */
/* motor/servo commands are short JSON strings (<200 bytes); a small stack
 * frame keeps this well inside the standalone command's task stack.  The old
 * MIBOT_MAX_PAYLOAD (4096) buffer overflowed the 3976-byte va_test stack and
 * hardfaulted, so cap the command payload at a sane bound here. */
#define VA_TEST_CMD_MAX 256

static int va_send_command(int fd, const char *json)
{
  uint8_t frame[VA_TEST_CMD_MAX + MIBOT_FRAME_OVERHEAD];
  size_t len = strlen(json);
  uint16_t crc;

  if (len > VA_TEST_CMD_MAX)
    {
      return -E2BIG;
    }

  frame[0] = 0xaa;
  frame[1] = 0x55;
  frame[2] = MIBOT_VERSION;
  frame[3] = MIBOT_TYPE_COMMAND;
  frame[4] = 0;
  frame[5] = 0;   /* seq low  (standalone tool uses a fixed seq) */
  frame[6] = 0;   /* seq high */
  frame[7] = (uint8_t)(len & 0xff);
  frame[8] = (uint8_t)(len >> 8);
  memcpy(&frame[9], json, len);

  crc = va_crc16(&frame[2], MIBOT_HEADER_SIZE + (uint16_t)len);
  frame[9 + len] = (uint8_t)(crc & 0xff);
  frame[10 + len] = (uint8_t)(crc >> 8);

  return va_write_all(fd, frame, len + MIBOT_FRAME_OVERHEAD);
}

/* Best-effort reply reader for a manual test tool: poll raw bytes off the UART
 * up to VA_REPLY_TIMEOUT_MS and print whatever arrives.  We do not fully parse
 * the framed reply; showing the raw ACK/NACK payload bytes is enough to judge
 * the ESP32 result (e.g. an "ok" or an "E_SERVO_LIMIT" substring). */
static void va_read_reply(int fd)
{
  uint8_t buf[VA_REPLY_BUF_SIZE];
  size_t total = 0;
  uint32_t t0 = va_now_ms();

  while ((va_now_ms() - t0) < VA_REPLY_TIMEOUT_MS &&
         total < sizeof(buf) - 1)
    {
      ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
      if (n > 0)
        {
          total += (size_t)n;
          /* Keep draining so a multi-frame reply is captured; the outer
           * deadline still bounds the total wait. */
          continue;
        }

      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
          printf("va_test: reply read error %d\n", errno);
          break;
        }

      usleep(10 * 1000);
    }

  if (total == 0)
    {
      printf("va_test: no reply within %dms\n", VA_REPLY_TIMEOUT_MS);
      return;
    }

  /* Print the raw reply bytes.  The ACK/NACK payload is JSON text; any framing
   * bytes render as escapes, which is acceptable for a manual test tool. */
  buf[total] = '\0';
  printf("va_test: reply (%u bytes): ", (unsigned)total);

  {
    size_t i;
    for (i = 0; i < total; i++)
      {
        uint8_t c = buf[i];
        if (c >= 0x20 && c < 0x7f)
          {
            putchar((int)c);
          }
        else
          {
            printf("\\x%02x", c);
          }
      }
  }

  putchar('\n');
}

static void va_test_usage(void)
{
  printf("usage: va_test mic|spk|hw|spkmic [seconds] | "
         "motor <speed> <ms> [a|b|both] | "
         "servo <deg> | action <name> [intensity_pct]\n");
  printf("  motor: speed [-50,50] !=0, ms [50,30000]; channel defaults to a.\n");
  printf("         e.g. `va_test motor 50 30000 both` holds both bridges on\n");
  printf("         for 30 s, long enough to probe STBY/AIN/BIN with a meter.\n");
  printf("  action names: idle listening thinking happy sad confused "
         "surprised cute greeting warning\n");
  printf("  both motors + both servos: cute happy sad confused surprised "
         "thinking warning\n");
  printf("  servos only (motors stay at 0): idle listening greeting\n");
}

/* `va_test motor <speed> <ms>`: send robot.test_motor_a over UART and let the
 * ESP32 do the range validation (speed in [-50,50] and !=0, duration_ms in
 * [50,1000]); out-of-range returns an error rather than executing (需求 10.3,
 * 10.6).  simulate_tof=true is required by the ESP32 or it returns
 * E_SIMULATE_TOF_REQUIRED, so it is always set here. */
static int va_test_motor(int speed, int ms, const char *channel)
{
  char json[256];
  int fd;
  int ret;

  fd = va_uart_open();
  if (fd < 0)
    {
      printf("va_test: UART %s open failed %d\n", VA_TEST_UART, -fd);
      return 1;
    }

  snprintf(json, sizeof(json),
           "{\"schema\":\"mibot.uart.v1\",\"name\":\"robot.test_motor_a\","
           "\"command_id\":\"vat\",\"args\":{\"simulate_tof\":true,"
           "\"speed\":%d,\"duration_ms\":%d,\"channel\":\"%s\"}}",
           speed, ms, channel);

  printf("va_test: motor ch=%s speed=%d duration_ms=%d (simulate_tof=true "
         "auto-set; ESP32 requires speed in [-50,50] !=0, duration_ms in "
         "[50,30000])\n",
         channel, speed, ms);

  ret = va_send_command(fd, json);
  if (ret < 0)
    {
      printf("va_test: motor send failed %d\n", -ret);
      close(fd);
      return 1;
    }

  va_read_reply(fd);
  close(fd);
  return 0;
}

/* `va_test servo <deg>`: send robot.set_arm_pose with the single angle applied
 * to both arms and let the ESP32 validate left/right in [0,180]; out of range
 * returns E_SERVO_LIMIT rather than executing (需求 10.4, 10.6). */
static int va_test_servo(int deg)
{
  char json[256];
  int fd;
  int ret;

  fd = va_uart_open();
  if (fd < 0)
    {
      printf("va_test: UART %s open failed %d\n", VA_TEST_UART, -fd);
      return 1;
    }

  snprintf(json, sizeof(json),
           "{\"schema\":\"mibot.uart.v1\",\"name\":\"robot.set_arm_pose\","
           "\"command_id\":\"vat\",\"args\":{\"left_deg\":%d,"
           "\"right_deg\":%d}}",
           deg, deg);

  printf("va_test: servo deg=%d (both arms; ESP32 requires [0,180], "
         "else E_SERVO_LIMIT)\n", deg);

  ret = va_send_command(fd, json);
  if (ret < 0)
    {
      printf("va_test: servo send failed %d\n", -ret);
      close(fd);
      return 1;
    }

  va_read_reply(fd);
  close(fd);
  return 0;
}

/* `va_test action <name> [intensity_pct]`: send robot.perform_action and let the
 * ESP32 play the bounded sequence behind that name.
 *
 * This is the one command that exercises both wheel motors and both arm servos
 * in a single run: every step of an ACTION_PLANS entry carries left_motor,
 * right_motor and the two servo angles, and the ESP32 owns those values -- only
 * the name and intensity travel over the link.  robot.test_motor_a drives motor
 * A alone and forces B to 0, so it cannot do this.
 *
 * Intensity scales the step values; it is sent as a fraction in [0,1] because
 * that is what the ESP32 validates, but taken on the command line as a
 * percentage so the shell does not have to pass a decimal point. */
static int va_test_action(const char *name, int intensity_pct)
{
  char json[256];
  int fd;
  int ret;

  fd = va_uart_open();
  if (fd < 0)
    {
      printf("va_test: UART %s open failed %d\n", VA_TEST_UART, -fd);
      return 1;
    }

  snprintf(json, sizeof(json),
           "{\"schema\":\"mibot.uart.v1\",\"name\":\"robot.perform_action\","
           "\"command_id\":\"vat\",\"args\":{\"action\":\"%s\","
           "\"intensity\":%d.%02d}}",
           name, intensity_pct / 100, intensity_pct % 100);

  printf("va_test: action %s intensity=%d.%02d (%s)\n",
         name, intensity_pct / 100, intensity_pct % 100,
         va_test_action_moves_motors(name)
             ? "drives both motors and both servos"
             : "servos only -- this action holds both motors at 0");

  ret = va_send_command(fd, json);
  if (ret < 0)
    {
      printf("va_test: action send failed %d\n", -ret);
      close(fd);
      return 1;
    }

  /* The ACK only says the action was accepted; the ESP32 then steps through the
   * sequence and emits action_update events plus a final completed reply.  Read
   * for a while so those land in the log next to the command. */
  va_read_reply(fd);
  va_read_reply(fd);
  close(fd);
  return 0;
}

/* `va_test mic [seconds]`: capture from the microphone via board_audio_stream_*
 * and report the number of frames captured plus the driver drop count, so the
 * mic data path can be verified without the dialog state machine (需求 10.1). */
static int va_test_mic(int seconds)
{
  uint8_t frame[VA_FRAME_BYTES];
  int frames = 0;
  int empty = 0;
  uint32_t t0;
  uint32_t elapsed_ms;
  int ret;

  if (board_audio_stream_start() < 0)
    {
      printf("va_test: mic stream start failed\n");
      return 1;
    }

  printf("va_test: mic capturing %ds (~%d frames expected)\n",
         seconds, seconds * VA_FRAMES_PER_SEC);
  t0 = va_now_ms();

  /* Block on the frame semaphore rather than polling: the frame period is
   * 20 ms, so a busy loop would just burn wake-ups.  A run of empty waits
   * (no data for ~10s) breaks out so a dead mic does not hang forever. */
  while (empty < 50 && (va_now_ms() - t0) < (uint32_t)seconds * 1000U)
    {
      if (board_audio_stream_wait(200) < 0)
        {
          empty++;
          continue;
        }

      while ((ret = board_audio_stream_read(frame, sizeof(frame))) ==
             (int)sizeof(frame))
        {
          frames++;
        }

      if (ret < 0)
        {
          printf("va_test: mic read error %d\n", ret);
        }
    }

  /* Drain frames the DMA queued while the loop was exiting so the tail is not
   * dropped from the reported count. */
  while (board_audio_stream_read(frame, sizeof(frame)) == (int)sizeof(frame))
    {
      frames++;
    }

  elapsed_ms = va_now_ms() - t0;
  board_audio_stream_stop();

  printf("va_test: mic done frames=%d dropped=%u elapsed=%ums empty=%d\n",
         frames, (unsigned)board_audio_stream_dropped(),
         (unsigned)elapsed_ms, empty);
  return 0;
}

/* `va_test spk [seconds]`: play a test tone via board_audio_play_tone so the
 * speaker output path can be verified without the dialog state machine
 * (需求 10.2).  board_audio_play_tone is the simplest reuse of the existing
 * playback driver. */
static int va_test_spk(int seconds)
{
  int ret;

  printf("va_test: spk playing test tone %ds\n", seconds);
  ret = board_audio_play_tone(seconds);
  if (ret < 0)
    {
      printf("va_test: spk tone failed %d\n", ret);
      return 1;
    }

  printf("va_test: spk done\n");
  return 0;
}

/* `va_test spkmic [seconds]`: objective measurement of the speaker output.
 *
 * The tone is played from a helper task while this task captures the on-board
 * microphone, which sits centimetres from the speaker.  The captured PCM is
 * reduced to one energy value per 5 ms and printed as a compact envelope
 * AFTER playback, so a periodic dropout shows up as a repeating dip with a
 * measurable period instead of relying on someone listening.  Nothing is
 * printed while the tone plays: console writes are blocking. */

#define VA_SPKMIC_BLOCK      80      /* 5 ms at 16 kHz                      */
#define VA_SPKMIC_MAX_BLOCKS 800     /* 4 s of envelope                     */

/* Raw waveform window captured around the first dip, so the artefact can be
 * identified as silence, attenuation, a DC step or a phase discontinuity. */
#define VA_SPKMIC_WINDOW  640        /* 40 ms at 16 kHz                     */
#define VA_SPKMIC_HISTORY 320        /* 20 ms kept before the dip           */

static volatile bool g_spkmic_tone_done;
static int g_spkmic_seconds;
static bool g_spkmic_use_stream;

/* 2 kHz at 16 kHz is exactly 8 samples per period, so a fixed table keeps the
 * phase continuous across frame boundaries. */
static const int16_t g_spkmic_sine8[8] =
{
  0, 11314, 16000, 11314, 0, -11314, -16000, -11314
};

/* Feed the streaming playback path (the one real TTS uses): 20 ms dense PCM
 * frames refilled from the DMA half/full callbacks, instead of one long
 * circular buffer. */
static void va_spkmic_stream_play(int seconds)
{
  /* Static, not automatic: this runs on the va_tone task, whose stack cannot
   * absorb a 640-byte frame on top of the board_audio_play_write() call chain.
   * A stack automatic here hard-faulted with a corrupted backtrace
   * (PC == LR pointing at an illegal address).  Only one spkmic run exists at
   * a time, so a single shared buffer is safe. */
  static int16_t frame[VA_FRAME_BYTES / 2];
  uint32_t n = 0;
  uint32_t t0;

  if (board_audio_play_start() < 0)
    {
      return;
    }

  t0 = va_now_ms();
  while ((va_now_ms() - t0) < (uint32_t)seconds * 1000U)
    {
      int queued;

      /* Top the queue up, then wait roughly one frame period.  play_write
       * drops (returns 0) once the queue is full, which paces this loop. */
      for (queued = 0; queued < 12; queued++)
        {
          int i;

          for (i = 0; i < (int)(sizeof(frame) / sizeof(frame[0])); i++)
            {
              frame[i] = g_spkmic_sine8[(n + i) & 7];
            }

          if (board_audio_play_write(frame, sizeof(frame)) !=
              (int)sizeof(frame))
            {
              break;
            }
          n += sizeof(frame) / sizeof(frame[0]);
        }

      usleep(20000);
    }

  (void)board_audio_play_drain(500);
  board_audio_play_stop();
}

static int va_spkmic_tone_task(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  if (g_spkmic_use_stream)
    {
      va_spkmic_stream_play(g_spkmic_seconds);
    }
  else
    {
      (void)board_audio_play_tone(g_spkmic_seconds);
    }
  g_spkmic_tone_done = true;
  return 0;
}

static int va_test_spkmic(int seconds, bool use_stream)
{
  static uint8_t envelope[VA_SPKMIC_MAX_BLOCKS];
  static int16_t history[VA_SPKMIC_HISTORY];
  static int16_t window[VA_SPKMIC_WINDOW];
  int history_pos = 0;
  int history_filled = 0;
  int window_pos = -1;              /* -1 = not triggered yet              */
  int16_t frame[VA_FRAME_BYTES / 2];
  int blocks = 0;
  int pid;
  uint32_t t0;
  int i;

  if (board_audio_stream_start() < 0)
    {
      printf("va_test: spkmic capture start failed\n");
      return 1;
    }

  g_spkmic_seconds = seconds;
  g_spkmic_use_stream = use_stream;
  g_spkmic_tone_done = false;
  printf("va_test: spkmic source=%s\n", use_stream ? "stream" : "direct-tone");
  /* 4 KB: the stream branch descends into board_audio_play_write() and the
   * AUDPRC/DMA glue.  2 KB overflowed and corrupted the task's stack. */
  pid = task_create("va_tone", 120, 4096, va_spkmic_tone_task, NULL);
  if (pid < 0)
    {
      printf("va_test: spkmic tone task failed %d\n", pid);
      board_audio_stream_stop();
      return 1;
    }

  t0 = va_now_ms();
  while (blocks < VA_SPKMIC_MAX_BLOCKS &&
         (va_now_ms() - t0) < (uint32_t)(seconds + 1) * 1000U)
    {
      if (board_audio_stream_wait(50) < 0)
        {
          if (g_spkmic_tone_done)
            {
              break;
            }
          continue;
        }

      while (board_audio_stream_read(frame, sizeof(frame)) ==
             (int)sizeof(frame))
        {
          int b;

          for (b = 0; b + VA_SPKMIC_BLOCK <= (int)(sizeof(frame) /
                                                   sizeof(frame[0]));
               b += VA_SPKMIC_BLOCK)
            {
              uint32_t peak = 0;
              int s;

              for (s = 0; s < VA_SPKMIC_BLOCK; s++)
                {
                  int v = frame[b + s];
                  uint32_t a = (uint32_t)(v < 0 ? -v : v);
                  if (a > peak)
                    {
                      peak = a;
                    }
                }
              (void)peak;

              uint8_t level = 0;

              /* Coarse log-ish scale 0-9 so the envelope is readable. */
              while (level < 9 && peak > (uint32_t)(64u << level))
                {
                  level++;
                }

              if (blocks < VA_SPKMIC_MAX_BLOCKS)
                {
                  envelope[blocks] = level;
                }

              /* Trigger the raw capture on the first dip that occurs after
               * the tone is well established (>=100 blocks = 500 ms). */
              if (window_pos < 0 && blocks > 100 && level <= 3 &&
                  history_filled >= VA_SPKMIC_HISTORY)
                {
                  int h;

                  for (h = 0; h < VA_SPKMIC_HISTORY; h++)
                    {
                      window[h] = history[(history_pos + h) %
                                          VA_SPKMIC_HISTORY];
                    }
                  window_pos = VA_SPKMIC_HISTORY;
                }

              /* Keep the rolling pre-dip history and fill the post-dip half. */
              for (s = 0; s < VA_SPKMIC_BLOCK; s++)
                {
                  int16_t v = frame[b + s];

                  if (window_pos >= 0 && window_pos < VA_SPKMIC_WINDOW)
                    {
                      window[window_pos++] = v;
                    }
                  history[history_pos] = v;
                  history_pos = (history_pos + 1) % VA_SPKMIC_HISTORY;
                  if (history_filled < VA_SPKMIC_HISTORY)
                    {
                      history_filled++;
                    }
                }

              blocks++;
            }
        }
    }

  board_audio_stream_stop();

  printf("va_test: spkmic blocks=%d (5ms each)\n", blocks);
  printf("va_test: spkmic envelope=");
  for (i = 0; i < blocks && i < VA_SPKMIC_MAX_BLOCKS; i++)
    {
      putchar('0' + envelope[i]);
    }
  putchar('\n');

  if (window_pos > 0)
    {
      printf("va_test: spkmic dip window (%d samples, dip starts at %d):\n",
             window_pos, VA_SPKMIC_HISTORY);
      for (i = 0; i < window_pos; i++)
        {
          printf("%d ", (int)window[i]);
          if ((i % 20) == 19)
            {
              putchar('\n');
            }
        }
      putchar('\n');
    }
  else
    {
      printf("va_test: spkmic no dip captured\n");
    }
  return 0;
}

/* `va_test hw [seconds]`: route the AUDCODEC's internal 1 kHz generator to
 * the existing PA.  It intentionally bypasses the PCM source and AUDPRC DMA,
 * making it a hardware-isolation diagnostic for speaker interruption issues. */
static int va_test_hw_tone(int seconds)
{
  int ret;

  printf("va_test: hw playing DMA-bypass tone %ds\n", seconds);
  ret = board_audio_codec_hw_tone(seconds);
  if (ret < 0)
    {
      printf("va_test: hw tone failed %d\n", ret);
      return 1;
    }

  printf("va_test: hw done\n");
  return 0;
}

int va_test_main(int argc, char *argv[])
{
  va_test_kind_t kind;
  int seconds;

  if (argc < 2)
    {
      va_test_usage();
      return 1;
    }

  /* Flat command dispatch via the shared pure-logic contract (va_test_args):
   * no cloud, no dialog state machine (需求 10.5, P15). */
  kind = va_test_classify(argv[1]);

  if (kind == VA_TEST_MOTOR)
    {
      int speed;
      int ms;
      const char *channel = "a";

      if (argc < 4)
        {
          printf("va_test: motor needs <speed> <ms>\n");
          va_test_usage();
          return 1;
        }

      speed = atoi(argv[2]);
      ms = atoi(argv[3]);
      if (argc >= 5)
        {
          channel = argv[4];
        }

      if (!va_test_motor_channel_valid(channel))
        {
          printf("va_test: unknown motor channel \"%s\"; use a, b or both\n",
                 channel);
          va_test_usage();
          return 1;
        }

      /* The ESP32 is the authority on range checks (需求 10.6, P16); we still
       * hint locally so an out-of-range request is easy to spot in the log. */
      if (!va_test_motor_params_valid(speed, ms))
        {
          printf("va_test: motor speed=%d ms=%d out of ESP32 range "
                 "(speed [-50,50] !=0, ms [50,30000]); ESP32 will reject\n",
                 speed, ms);
        }

      return va_test_motor(speed, ms, channel);
    }

  if (kind == VA_TEST_SERVO)
    {
      int deg;

      if (argc < 3)
        {
          printf("va_test: servo needs <deg>\n");
          va_test_usage();
          return 1;
        }

      deg = atoi(argv[2]);

      if (!va_test_servo_params_valid(deg))
        {
          printf("va_test: servo deg=%d out of ESP32 range [0,180]; "
                 "ESP32 will reject with E_SERVO_LIMIT\n", deg);
        }

      return va_test_servo(deg);
    }

  if (kind == VA_TEST_ACTION)
    {
      const char *name;
      int intensity_pct = 100;

      if (argc < 3)
        {
          printf("va_test: action needs <name>\n");
          va_test_usage();
          return 1;
        }

      name = argv[2];
      if (argc >= 4)
        {
          intensity_pct = atoi(argv[3]);
        }

      /* The ESP32 is the authority (it returns E_INVALID_ARG for an unknown
       * action or an intensity outside [0,1]); check locally so a typo is
       * obvious here rather than as a rejection code. */
      if (!va_test_action_name_valid(name))
        {
          printf("va_test: unknown action \"%s\"; ESP32 will reject with "
                 "E_INVALID_ARG\n", name);
          va_test_usage();
          return 1;
        }

      if (intensity_pct < 0 || intensity_pct > 100)
        {
          printf("va_test: intensity %d out of range [0,100]; ESP32 requires "
                 "a fraction in [0,1]\n", intensity_pct);
          return 1;
        }

      return va_test_action(name, intensity_pct);
    }

  seconds = (argc > 2) ? atoi(argv[2]) : VA_TEST_DEFAULT_SECONDS;
  if (seconds <= 0)
    {
      seconds = VA_TEST_DEFAULT_SECONDS;
    }

  if (kind == VA_TEST_MIC)
    {
      return va_test_mic(seconds);
    }

  if (kind == VA_TEST_SPK)
    {
      return va_test_spk(seconds);
    }

  if (kind == VA_TEST_HW_TONE)
    {
      return va_test_hw_tone(seconds);
    }

  if (kind == VA_TEST_SPKMIC)
    {
      return va_test_spkmic(seconds, false);
    }

  if (kind == VA_TEST_SPKSTREAM)
    {
      return va_test_spkmic(seconds, true);
    }

  va_test_usage();
  return 1;
}

#endif /* __NuttX__ */
