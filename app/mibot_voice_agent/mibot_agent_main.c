/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot SF32 agent for the SF32LB52-DevKit-LCD.
 *
 * The application deliberately keeps the protocol and hardware policy local:
 * ESP32 remains the motion-safety authority, while SF32 owns UI, audio and
 * the high-level UART link.
 */

#include <nuttx/config.h>
#include <nuttx/sched.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/video/fb.h>

#include "mibot_lcd.h"

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO_HW
extern int board_audio_analog_init(void);
extern int board_audio_analog_ready(void);
#endif
#ifdef CONFIG_AUDIO_I2SCHAR
extern int board_audio_i2s_initialize(void);
#endif

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
#  include <nuttx/audio/audio.h>
#endif

#ifndef CONFIG_EXAMPLES_MIBOT_AGENT_UART
#  define CONFIG_EXAMPLES_MIBOT_AGENT_UART "/dev/ttyS0"
#endif

#ifndef CONFIG_EXAMPLES_MIBOT_AGENT_BAUD
#  define CONFIG_EXAMPLES_MIBOT_AGENT_BAUD 1000000
#endif

#ifndef CONFIG_EXAMPLES_MIBOT_AGENT_FB
#  define CONFIG_EXAMPLES_MIBOT_AGENT_FB "/dev/fb0"
#endif

#define MIBOT_VERSION             1
#define MIBOT_MAX_PAYLOAD         4096
#define MIBOT_HEADER_SIZE         7
#define MIBOT_FRAME_OVERHEAD      11
#define MIBOT_HEARTBEAT_MS        500
#define MIBOT_HELLO_RETRY_MS      2000
#define MIBOT_LINK_TIMEOUT_MS     1500
#define MIBOT_UART_RECONNECT_MS   2000
#define MIBOT_UART_RX_BUFFER_BYTES 1024
#define MIBOT_UART_DRAIN_MAX_BYTES (MIBOT_UART_RX_BUFFER_BYTES * 8)
#define MIBOT_AUDIO_FRAME_BYTES   640
#define MIBOT_AUDIO_SAMPLE_RATE   16000
#define MIBOT_AUDIO_CHANNELS      1
#define MIBOT_AUDIO_FRAME_MS      20

/* Board audio driver: dense 16 kHz mono PCM frames of 20 ms.  The driver owns
 * the padded/dense conversion and the circular DMA, so the agent never sees
 * the AUDPRC layout.  Capture blocks on a semaphore fed from the DMA
 * callback; polling cannot pace a 20 ms frame because the tick is 20 ms. */
extern int board_audio_stream_start(void);
extern int board_audio_stream_read(void *buf, size_t len);
extern int board_audio_stream_wait(int timeout_ms);
extern int board_audio_stream_stop(void);
extern uint32_t board_audio_stream_dropped(void);
extern int board_audio_play_start(void);
extern int board_audio_play_write(const void *buf, size_t len);
extern int board_audio_play_drain(int timeout_ms);
extern int board_audio_play_stop(void);
extern uint32_t board_audio_play_dropped(void);
extern uint32_t board_audio_play_underrun(void);

#define MIBOT_TYPE_HELLO          0x01
#define MIBOT_TYPE_HELLO_ACK      0x02
#define MIBOT_TYPE_COMMAND        0x10
#define MIBOT_TYPE_ACK            0x11
#define MIBOT_TYPE_NACK           0x12
#define MIBOT_TYPE_EVENT          0x20
#define MIBOT_TYPE_TELEMETRY      0x21
#define MIBOT_TYPE_AUDIO_UP       0x30
#define MIBOT_TYPE_AUDIO_DOWN     0x31
#define MIBOT_TYPE_PING           0x60
#define MIBOT_TYPE_PONG           0x61

enum mibot_state_e
{
  MIBOT_BOOT = 0,
  MIBOT_SELF_CHECK,
  MIBOT_READY,
  MIBOT_LISTENING,
  MIBOT_THINKING,
  MIBOT_EXECUTING,
  MIBOT_SPEAKING,
  MIBOT_DEGRADED,
  MIBOT_SAFE_STOP,
  MIBOT_FAULT
};

struct mibot_parser_s
{
  uint8_t stage;
  uint8_t header[MIBOT_HEADER_SIZE];
  uint8_t header_pos;
  uint8_t payload[MIBOT_MAX_PAYLOAD];
  uint16_t payload_len;
  uint16_t payload_pos;
  uint8_t crc[2];
  uint8_t crc_pos;
};

struct mibot_fb_s
{
  int fd;
  FAR uint8_t *mem;
  size_t len;
  bool mapped;
  struct fb_videoinfo_s video;
  struct fb_planeinfo_s plane;
};

struct mibot_ctx_s
{
  int uart_fd;
  pthread_mutex_t tx_lock;
  pthread_mutex_t audio_lock;
  struct mibot_parser_s parser;
  struct mibot_fb_s fb;
  enum mibot_state_e state;
  uint32_t seq;
  uint32_t last_peer_ms;
  uint32_t last_hello_ms;
  uint32_t last_ping_ms;
  uint32_t last_lcd_refresh_ms;
  uint32_t last_uart_attempt_ms;
  bool hello_acked;
  bool running;
#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
  int audio_fd;
  pthread_t audio_thread;
  bool audio_running;
  bool play_active;
  bool capture_active;
#endif
};

static struct mibot_ctx_s g_mibot;
static mibot_lcd_animation_cb_t g_lcd_animation;
static char g_lcd_text[32] = "BOOT";

static void mibot_parser_reset(struct mibot_parser_s *parser);
static void mibot_lcd_render(void);

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO_HW
static int mibot_audio_hw_init(void)
{
  return board_audio_analog_init();
}
#endif

static uint32_t mibot_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

static const char *mibot_state_name(enum mibot_state_e state)
{
  static const char *const names[] =
  {
    "BOOT", "SELF_CHECK", "READY", "LISTENING", "THINKING",
    "EXECUTING", "SPEAKING", "DEGRADED", "SAFE_STOP", "FAULT"
  };

  return state <= MIBOT_FAULT ? names[state] : "FAULT";
}

static uint16_t mibot_crc16(const uint8_t *data, size_t len)
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

static int mibot_write_all(int fd, const uint8_t *data, size_t len)
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
      len -= written;
    }

  return 0;
}

static void mibot_uart_close(void)
{
  if (g_mibot.uart_fd >= 0)
    {
      close(g_mibot.uart_fd);
      g_mibot.uart_fd = -1;
    }
  g_mibot.hello_acked = false;
  mibot_parser_reset(&g_mibot.parser);
}

static int mibot_send_frame(uint8_t type, const void *payload, uint16_t len)
{
  uint8_t frame[MIBOT_MAX_PAYLOAD + MIBOT_FRAME_OVERHEAD];
  uint16_t crc;
  uint16_t seq;
  int ret;

  if (g_mibot.uart_fd < 0 || len > MIBOT_MAX_PAYLOAD)
    {
      return -ENODEV;
    }

  seq = (uint16_t)g_mibot.seq++;
  frame[0] = 0xaa;
  frame[1] = 0x55;
  frame[2] = MIBOT_VERSION;
  frame[3] = type;
  frame[4] = 0;
  frame[5] = (uint8_t)(seq & 0xff);
  frame[6] = (uint8_t)(seq >> 8);
  frame[7] = (uint8_t)(len & 0xff);
  frame[8] = (uint8_t)(len >> 8);
  if (len != 0 && payload != NULL)
    {
      memcpy(&frame[9], payload, len);
    }

  crc = mibot_crc16(&frame[2], MIBOT_HEADER_SIZE + len);
  frame[9 + len] = (uint8_t)(crc & 0xff);
  frame[10 + len] = (uint8_t)(crc >> 8);

  pthread_mutex_lock(&g_mibot.tx_lock);
  ret = mibot_write_all(g_mibot.uart_fd, frame, len + MIBOT_FRAME_OVERHEAD);
  pthread_mutex_unlock(&g_mibot.tx_lock);
  return ret;
}

static bool mibot_json_string(const char *json, const char *key,
                              char *out, size_t outlen)
{
  char needle[48];
  const char *p;
  const char *q;
  size_t n = 0;

  if (snprintf(needle, sizeof(needle), "\"%s\"", key) >=
      (int)sizeof(needle))
    {
      return false;
    }

  p = strstr(json, needle);
  if (p == NULL)
    {
      return false;
    }
  p = strchr(p + strlen(needle), ':');
  if (p == NULL)
    {
      return false;
    }
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    {
      p++;
    }
  if (*p != '"')
    {
      return false;
    }
  p++;
  q = p;
  while (*q != '\0' && *q != '"')
    {
      if (*q == '\\' && q[1] != '\0')
        {
          q++;
        }
      q++;
    }
  if (*q != '"' || outlen == 0)
    {
      return false;
    }

  while (p < q && n + 1 < outlen)
    {
      if (*p == '\\' && p + 1 < q)
        {
          p++;
        }
      out[n++] = *p++;
    }
  out[n] = '\0';
  return true;
}

static bool mibot_json_number(const char *json, const char *key,
                              int *value)
{
  char needle[48];
  const char *p;
  char *end;
  long parsed;

  if (snprintf(needle, sizeof(needle), "\"%s\"", key) >=
      (int)sizeof(needle))
    {
      return false;
    }
  p = strstr(json, needle);
  if (p == NULL)
    {
      return false;
    }
  p = strchr(p + strlen(needle), ':');
  if (p == NULL)
    {
      return false;
    }
  parsed = strtol(p + 1, &end, 10);
  if (end == p + 1)
    {
      return false;
    }
  *value = (int)parsed;
  return true;
}

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
static bool mibot_json_bool(const char *json, const char *key, bool *value)
{
  char needle[48];
  const char *p;

  if (json == NULL || key == NULL || value == NULL ||
      snprintf(needle, sizeof(needle), "\"%s\"", key) >=
        (int)sizeof(needle))
    {
      return false;
    }

  p = strstr(json, needle);
  if (p == NULL)
    {
      return false;
    }
  p = strchr(p + strlen(needle), ':');
  if (p == NULL)
    {
      return false;
    }
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    {
      p++;
    }

  if (strncmp(p, "true", 4) == 0)
    {
      *value = true;
      return true;
    }
  if (strncmp(p, "false", 5) == 0)
    {
      *value = false;
      return true;
    }
  return false;
}
#endif

static bool mibot_json_object_string(const char *json, const char *object,
                                     const char *key, char *out,
                                     size_t outlen)
{
  char needle[48];
  const char *p;

  if (snprintf(needle, sizeof(needle), "\"%s\"", object) >=
      (int)sizeof(needle))
    {
      return false;
    }
  p = strstr(json, needle);
  if (p == NULL)
    {
      return false;
    }
  p = strchr(p + strlen(needle), '{');
  return p != NULL && mibot_json_string(p, key, out, outlen);
}

static bool mibot_json_object_number(const char *json, const char *object,
                                     const char *key, int *value)
{
  char needle[48];
  const char *p;

  if (snprintf(needle, sizeof(needle), "\"%s\"", object) >=
      (int)sizeof(needle))
    {
      return false;
    }
  p = strstr(json, needle);
  if (p == NULL)
    {
      return false;
    }
  p = strchr(p + strlen(needle), '{');
  return p != NULL && mibot_json_number(p, key, value);
}

static void mibot_safe_token(const char *input, char *output, size_t size)
{
  size_t i;

  if (size == 0)
    {
      return;
    }
  for (i = 0; input != NULL && input[i] != '\0' && i + 1 < size; i++)
    {
      char c = input[i];
      output[i] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'
                    ? c : '_';
    }
  output[i] = '\0';
}

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
static int mibot_send_audio_meta(uint32_t stream_serial, bool eos)
{
  char payload[256];
  int len;

  len = snprintf(payload, sizeof(payload),
                 "{\"schema\":\"mibot.audio.v1\","
                 "\"stream_id\":\"sf32_%lu\","
                 "\"codec\":\"pcm_s16le\","
                 "\"sample_rate\":%u,\"channels\":%u,"
                 "\"frame_ms\":%u,\"bytes\":%u,\"eos\":%s}",
                 (unsigned long)stream_serial,
                 (unsigned)MIBOT_AUDIO_SAMPLE_RATE,
                 (unsigned)MIBOT_AUDIO_CHANNELS,
                 (unsigned)MIBOT_AUDIO_FRAME_MS,
                 (unsigned)MIBOT_AUDIO_FRAME_BYTES,
                 eos ? "true" : "false");
  if (len <= 0 || len >= (int)sizeof(payload))
    {
      return -EOVERFLOW;
    }
  return mibot_send_frame(MIBOT_TYPE_AUDIO_UP, payload, (uint16_t)len);
}

/* Drive the two audio paths from the single state transition point.
 *
 *   LISTENING  uplink only
 *   SPEAKING   downlink only
 *   DEGRADED   uplink off, local prompts only
 *   others     both off
 *
 * The first release is half duplex, so the old path is always torn down
 * before the new one starts: capture and playback must never hold the codec
 * at the same time. */
static void mibot_audio_apply_state(enum mibot_state_e state)
{
  bool want_up = (state == MIBOT_LISTENING);
  bool want_down = (state == MIBOT_SPEAKING);

  if (!want_up && g_mibot.capture_active)
    {
      g_mibot.capture_active = false;
      board_audio_stream_stop();
    }

  if (!want_down && g_mibot.play_active)
    {
      g_mibot.play_active = false;
      board_audio_play_stop();
    }

  if (want_up && !g_mibot.capture_active)
    {
      int ret = board_audio_stream_start();
      printf("mibot: audio capture start %s (%d)\n",
             ret == 0 ? "ok" : "failed", ret);
      if (ret == 0)
        {
          g_mibot.capture_active = true;
        }
    }
}
#endif

static void mibot_set_state(enum mibot_state_e state)
{
  if (g_mibot.state != state)
    {
      g_mibot.state = state;
      snprintf(g_lcd_text, sizeof(g_lcd_text), "%s", mibot_state_name(state));
      printf("mibot: state=%s\n", mibot_state_name(state));
#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
      mibot_audio_apply_state(state);
#endif
    }
}

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
static void mibot_audio_finish_downlink(void)
{
  /* EOS metadata is sent after the final PCM frame.  Drain the board queue
   * before stopping DMA so the tail of the utterance is not cut off. */
  (void)board_audio_play_drain(1000);
  g_mibot.play_active = false;
  (void)board_audio_play_stop();
  mibot_set_state(MIBOT_READY);
  mibot_lcd_render();
}
#endif

int mibot_lcd_set_animation_callback(mibot_lcd_animation_cb_t callback)
{
  g_lcd_animation = callback;
  mibot_lcd_render();

  return 0;
}

int mibot_lcd_set_text(const char *text)
{
  if (text == NULL)
    {
      return -EINVAL;
    }
  mibot_safe_token(text, g_lcd_text, sizeof(g_lcd_text));
  mibot_lcd_render();
  return 0;
}

static uint16_t mibot_state_color(enum mibot_state_e state)
{
  switch (state)
    {
      case MIBOT_READY:      return 0x07e0;
      case MIBOT_LISTENING:  return 0x07ff;
      case MIBOT_THINKING:   return 0xffe0;
      case MIBOT_EXECUTING:  return 0xfd20;
      case MIBOT_SPEAKING:   return 0x841f;
      case MIBOT_DEGRADED:   return 0xfbe0;
      case MIBOT_SAFE_STOP:
      case MIBOT_FAULT:      return 0xf800;
      default:               return 0x39e7;
    }
}

static void mibot_lcd_close(void)
{
  if (g_mibot.fb.mapped && g_mibot.fb.mem != NULL &&
      g_mibot.fb.mem != MAP_FAILED)
    {
      munmap(g_mibot.fb.mem, g_mibot.fb.len);
    }
  if (g_mibot.fb.fd >= 0)
    {
      close(g_mibot.fb.fd);
    }
  memset(&g_mibot.fb, 0, sizeof(g_mibot.fb));
  g_mibot.fb.fd = -1;
  g_mibot.fb.mem = NULL;
  g_mibot.fb.mapped = false;
}

static int mibot_lcd_open(void)
{
  int ret;

  if (g_mibot.fb.fd >= 0)
    {
      return 0;
    }
  g_mibot.fb.fd = open(CONFIG_EXAMPLES_MIBOT_AGENT_FB, O_RDWR);
  if (g_mibot.fb.fd < 0)
    {
      return -errno;
    }
  ret = ioctl(g_mibot.fb.fd, FBIOGET_VIDEOINFO,
              (unsigned long)(uintptr_t)&g_mibot.fb.video);
  if (ret < 0)
    {
      mibot_lcd_close();
      return -errno;
    }
  ret = ioctl(g_mibot.fb.fd, FBIOGET_PLANEINFO,
              (unsigned long)(uintptr_t)&g_mibot.fb.plane);
  if (ret < 0 || g_mibot.fb.plane.fblen == 0)
    {
      mibot_lcd_close();
      return ret < 0 ? -errno : -EINVAL;
    }
  g_mibot.fb.len = g_mibot.fb.plane.fblen;
  g_mibot.fb.mem = mmap(NULL, g_mibot.fb.len, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_FILE, g_mibot.fb.fd, 0);
  if (g_mibot.fb.mem == MAP_FAILED)
    {
      /* Some flat builds expose the framebuffer pointer directly. */
      g_mibot.fb.mem = g_mibot.fb.plane.fbmem;
    }
  else
    {
      g_mibot.fb.mapped = true;
    }
  return g_mibot.fb.mem != NULL ? 0 : -ENOMEM;
}

static uint32_t mibot_rgb565_to_argb8888(uint16_t color)
{
  uint32_t red = (uint32_t)((color >> 11) & 0x1f);
  uint32_t green = (uint32_t)((color >> 5) & 0x3f);
  uint32_t blue = (uint32_t)(color & 0x1f);
  red = (red << 3) | (red >> 2);
  green = (green << 2) | (green >> 4);
  blue = (blue << 3) | (blue >> 2);
  return 0xff000000U | (red << 16) | (green << 8) | blue;
}

static void mibot_lcd_putpixel(uint16_t x, uint16_t y, uint16_t color)
{
  if (x >= g_mibot.fb.video.xres || y >= g_mibot.fb.video.yres)
    {
      return;
    }
  if (g_mibot.fb.plane.bpp == 16)
    {
      uint16_t *row = (uint16_t *)(g_mibot.fb.mem +
                                   y * g_mibot.fb.plane.stride);
      row[x] = color;
    }
  else if (g_mibot.fb.plane.bpp == 32)
    {
      uint32_t *row = (uint32_t *)(g_mibot.fb.mem +
                                   y * g_mibot.fb.plane.stride);
      row[x] = mibot_rgb565_to_argb8888(color);
    }
}

static bool mibot_lcd_glyph(char c, uint8_t glyph[5])
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:";
  static const uint8_t glyphs[][5] =
  {
    {0x7e, 0x09, 0x09, 0x09, 0x7e}, /* A */
    {0x7f, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3e, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, /* D */
    {0x7f, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7f, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, /* G */
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, /* H */
    {0x00, 0x41, 0x7f, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3f, 0x01}, /* J */
    {0x7f, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7f, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, /* M */
    {0x7f, 0x04, 0x08, 0x10, 0x7f}, /* N */
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, /* O */
    {0x7f, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, /* Q */
    {0x7f, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7f, 0x01, 0x01}, /* T */
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, /* U */
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, /* V */
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x3e, 0x45, 0x49, 0x51, 0x3e}, /* 0 */
    {0x00, 0x21, 0x7f, 0x01, 0x00}, /* 1 */
    {0x23, 0x45, 0x49, 0x51, 0x21}, /* 2 */
    {0x42, 0x41, 0x49, 0x49, 0x36}, /* 3 */
    {0x0c, 0x14, 0x24, 0x7f, 0x04}, /* 4 */
    {0x72, 0x51, 0x51, 0x51, 0x4e}, /* 5 */
    {0x1e, 0x29, 0x49, 0x49, 0x06}, /* 6 */
    {0x40, 0x47, 0x48, 0x50, 0x60}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x30, 0x49, 0x49, 0x4a, 0x3c}, /* 9 */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* _ */
    {0x00, 0x36, 0x36, 0x00, 0x00}  /* : */
  };
  const char *p;

  if (c == ' ')
    {
      memset(glyph, 0, 5);
      return true;
    }
  p = strchr(alphabet, c);
  if (p == NULL)
    {
      return false;
    }
  memcpy(glyph, glyphs[p - alphabet], 5);
  return true;
}

static void mibot_lcd_draw_text(void)
{
  const uint16_t scale = 4;
  const uint16_t glyph_width = 5 * scale;
  const uint16_t spacing = scale;
  const uint16_t text_width = (uint16_t)(strlen(g_lcd_text) *
                                         (glyph_width + spacing));
  const uint16_t x0 = text_width < g_mibot.fb.video.xres
                        ? (g_mibot.fb.video.xres - text_width) / 2 : 0;
  const uint16_t y0 = g_mibot.fb.video.yres > 7 * scale
                        ? (g_mibot.fb.video.yres - 7 * scale) / 2 : 0;
  size_t i;

  for (i = 0; i < strlen(g_lcd_text); i++)
    {
      uint8_t glyph[5];
      uint16_t col;
      uint16_t row;
      if (!mibot_lcd_glyph(g_lcd_text[i], glyph))
        {
          continue;
        }
      for (col = 0; col < 5; col++)
        {
          for (row = 0; row < 7; row++)
            {
              uint16_t sx;
              uint16_t sy;
              if ((glyph[col] & (1U << row)) == 0)
                {
                  continue;
                }
              for (sx = 0; sx < scale; sx++)
                {
                  for (sy = 0; sy < scale; sy++)
                    {
                      mibot_lcd_putpixel((uint16_t)(x0 +
                          i * (glyph_width + spacing) + col * scale + sx),
                          (uint16_t)(y0 + row * scale + sy), 0xffff);
                    }
                }
            }
        }
    }
}

static void mibot_lcd_render(void)
{
  uint16_t color = mibot_state_color(g_mibot.state);
  uint16_t width;
  uint16_t height;
  uint16_t y;
  uint16_t x;

  if (mibot_lcd_open() < 0)
    {
      return;
    }
  width = g_mibot.fb.video.xres;
  height = g_mibot.fb.video.yres;

  if (g_mibot.fb.plane.bpp == 16)
    {
      for (y = 0; y < height; y++)
        {
          uint16_t *row = (uint16_t *)(g_mibot.fb.mem +
                                       y * g_mibot.fb.plane.stride);
          for (x = 0; x < width; x++)
            {
              row[x] = color;
            }
        }
      /* White bars make the state change visible even on a dim backlight. */
      for (y = 20; y < 32 && y < height; y++)
        {
          uint16_t *row = (uint16_t *)(g_mibot.fb.mem +
                                       y * g_mibot.fb.plane.stride);
          for (x = 20; x + 20 < width; x++)
            {
              row[x] = 0xffff;
            }
        }
    }
  else if (g_mibot.fb.plane.bpp == 32)
    {
      uint32_t argb = mibot_rgb565_to_argb8888(color);
      for (y = 0; y < height; y++)
        {
          uint32_t *row = (uint32_t *)(g_mibot.fb.mem +
                                       y * g_mibot.fb.plane.stride);
          for (x = 0; x < width; x++)
            {
              row[x] = argb;
            }
        }
    }

  mibot_lcd_draw_text();
  if (g_lcd_animation != NULL)
    {
      g_lcd_animation(g_mibot.fb.mem, width, height,
                      g_mibot.fb.plane.stride, g_mibot.fb.plane.bpp,
                      mibot_now_ms(), (uint8_t)g_mibot.state);
    }

#ifdef CONFIG_FB_UPDATE
  {
    struct fb_area_s area = { 0, 0, width, height };
    ioctl(g_mibot.fb.fd, FBIO_UPDATE, (unsigned long)(uintptr_t)&area);
  }
#endif
}

static int mibot_uart_open(void)
{
  struct termios tio;
  speed_t speed;

  g_mibot.uart_fd = open(CONFIG_EXAMPLES_MIBOT_AGENT_UART,
                         O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (g_mibot.uart_fd < 0)
    {
      return -errno;
    }

  if (tcgetattr(g_mibot.uart_fd, &tio) < 0)
    {
      close(g_mibot.uart_fd);
      g_mibot.uart_fd = -1;
      return -errno;
    }
  speed = B921600;
#ifdef B1000000
  if (CONFIG_EXAMPLES_MIBOT_AGENT_BAUD == 1000000)
    {
      speed = B1000000;
    }
#endif
#ifdef B115200
  if (CONFIG_EXAMPLES_MIBOT_AGENT_BAUD == 115200)
    {
      speed = B115200;
    }
#endif
#ifdef B460800
  if (CONFIG_EXAMPLES_MIBOT_AGENT_BAUD == 460800)
    {
      speed = B460800;
    }
#endif
  cfmakeraw(&tio);
  cfsetspeed(&tio, speed);
  tio.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
  tio.c_cflag &= ~CRTSCTS;
#endif
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (tcsetattr(g_mibot.uart_fd, TCSANOW, &tio) < 0)
    {
      close(g_mibot.uart_fd);
      g_mibot.uart_fd = -1;
      return -errno;
    }
  return 0;
}

static void mibot_uart_mark_disconnected(void)
{
  if (g_mibot.uart_fd >= 0)
    {
      printf("mibot: UART link disconnected\n");
    }
  mibot_uart_close();
  mibot_set_state(MIBOT_DEGRADED);
  mibot_lcd_render();
}

static void mibot_send_hello(void)
{
#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
  static const char payload[] =
    "{\"schema\":\"mibot.uart.v1\",\"node\":\"sf32lb52\","
    "\"capabilities\":[\"lcd\",\"touch\",\"uart2\",\"audio\"]}";
#else
  static const char payload[] =
    "{\"schema\":\"mibot.uart.v1\",\"node\":\"sf32lb52\","
    "\"capabilities\":[\"lcd\",\"touch\",\"uart2\",\"audio_probe\"]}";
#endif
  mibot_send_frame(MIBOT_TYPE_HELLO, payload, (uint16_t)strlen(payload));
}

static void mibot_send_ping(void)
{
  char payload[128];
  int len = snprintf(payload, sizeof(payload),
                     "{\"schema\":\"mibot.uart.v1\",\"state\":\"%s\","
                     "\"uptime_ms\":%lu}", mibot_state_name(g_mibot.state),
                     (unsigned long)mibot_now_ms());
  if (len > 0 && len < (int)sizeof(payload))
    {
      mibot_send_frame(MIBOT_TYPE_PING, payload, (uint16_t)len);
    }
}

static void mibot_send_ack(bool ok, const char *command_id,
                           const char *state, const char *error)
{
  char id[80];
  char payload[256];
  int len;

  mibot_safe_token(command_id != NULL ? command_id : "", id, sizeof(id));
  if (ok)
    {
      len = snprintf(payload, sizeof(payload),
                     "{\"schema\":\"mibot.uart.v1\",\"ok\":true,"
                     "\"command_id\":\"%s\",\"state\":\"%s\"}",
                     id, state != NULL ? state : "accepted");
    }
  else
    {
      len = snprintf(payload, sizeof(payload),
                     "{\"schema\":\"mibot.uart.v1\",\"ok\":false,"
                     "\"command_id\":\"%s\",\"state\":\"rejected\","
                     "\"error\":{\"code\":\"%s\"}}", id,
                     error != NULL ? error : "E_INVALID_COMMAND");
    }
  if (len > 0 && len < (int)sizeof(payload))
    {
      mibot_send_frame(ok ? MIBOT_TYPE_ACK : MIBOT_TYPE_NACK,
                       payload, (uint16_t)len);
    }
}

static void mibot_handle_command(const char *json)
{
  char name[64];
  char command_id[80];
  char expression[32];
  char action[32];
  int duration = 0;

  command_id[0] = '\0';
  expression[0] = '\0';
  action[0] = '\0';
  mibot_json_string(json, "command_id", command_id, sizeof(command_id));
  if (!mibot_json_string(json, "name", name, sizeof(name)))
    {
      mibot_send_ack(false, command_id, NULL, "E_INVALID_COMMAND");
      return;
    }

  if (strcmp(name, "robot.stop") == 0)
    {
      mibot_set_state(MIBOT_SAFE_STOP);
      mibot_lcd_render();
      mibot_send_ack(true, command_id, "accepted", NULL);
      return;
    }

  if (strcmp(name, "robot.set_expression") == 0)
    {
      if (mibot_json_object_string(json, "args", "name", expression,
                                   sizeof(expression)))
        {
          if (strcmp(expression, "listening") == 0)
            mibot_set_state(MIBOT_LISTENING);
          else if (strcmp(expression, "thinking") == 0)
            mibot_set_state(MIBOT_THINKING);
          else if (strcmp(expression, "speaking") == 0)
            mibot_set_state(MIBOT_SPEAKING);
          else if (strcmp(expression, "warning") == 0)
            mibot_set_state(MIBOT_DEGRADED);
          else
            mibot_set_state(MIBOT_READY);
          mibot_lcd_render();
        }
      mibot_json_object_number(json, "args", "duration_ms", &duration);
      (void)duration;
      mibot_send_ack(true, command_id, "accepted", NULL);
      return;
    }

  if (strcmp(name, "robot.set_text") == 0 ||
      strcmp(name, "display.show_text") == 0)
    {
      if (!mibot_json_object_string(json, "args", "text", expression,
                                    sizeof(expression)))
        {
          mibot_send_ack(false, command_id, NULL, "E_INVALID_ARGUMENT");
          return;
        }
      mibot_lcd_set_text(expression);
      /* The SiFli UART lower-half is not safe for a synchronous write from
       * the receive/command path.  The ESP32 already acknowledges the PC
       * command; keep this local display operation one-way until TX can be
       * queued from a dedicated worker. */
      return;
    }

  if (strcmp(name, "robot.perform_action") == 0)
    {
      if (mibot_json_object_string(json, "args", "action", action,
                                   sizeof(action)))
        {
          (void)action;
        }
      mibot_set_state(MIBOT_EXECUTING);
      mibot_lcd_render();
      mibot_send_ack(true, command_id, "accepted", NULL);
      /* The command is already on the SF32-to-ESP32 link; only local UI is
       * updated here. ESP32's completed EVENT is authoritative. */
      return;
    }

  if (strcmp(name, "robot.get_status") == 0)
    {
      mibot_send_ack(true, command_id, "completed", NULL);
      return;
    }

  mibot_send_ack(false, command_id, NULL, "E_UNSUPPORTED_COMMAND");
}

static void mibot_handle_event(const char *json)
{
  char event[48];
  char phase[32];
  char expression[32];
  char data_phase[32];
  char data_expression[32];
  char data_name[32];

  event[0] = '\0';
  phase[0] = '\0';
  expression[0] = '\0';
  data_phase[0] = '\0';
  data_expression[0] = '\0';
  data_name[0] = '\0';
  mibot_json_string(json, "event", event, sizeof(event));
  mibot_json_string(json, "phase", phase, sizeof(phase));
  mibot_json_string(json, "expression", expression, sizeof(expression));
  mibot_json_object_string(json, "data", "phase", data_phase,
                           sizeof(data_phase));
  mibot_json_object_string(json, "data", "expression", data_expression,
                           sizeof(data_expression));
  mibot_json_object_string(json, "data", "name", data_name,
                           sizeof(data_name));

  /* ESP32 action/expression events put their payload under data.  Accept
   * both the historical top-level form and the current event.v1 form so a
   * listening transition reliably starts the capture DMA. */
  if (expression[0] == '\0')
    {
      if (data_expression[0] != '\0')
        {
          snprintf(expression, sizeof(expression), "%s", data_expression);
        }
      else if (data_name[0] != '\0')
        {
          snprintf(expression, sizeof(expression), "%s", data_name);
        }
    }
  if (phase[0] == '\0' && data_phase[0] != '\0')
    {
      snprintf(phase, sizeof(phase), "%s", data_phase);
    }

  if (expression[0] != '\0')
    {
      if (strcmp(expression, "listening") == 0)
        mibot_set_state(MIBOT_LISTENING);
      else if (strcmp(expression, "thinking") == 0)
        mibot_set_state(MIBOT_THINKING);
      else if (strcmp(expression, "speaking") == 0)
        mibot_set_state(MIBOT_SPEAKING);
    }
  if (strcmp(phase, "completed") == 0)
    mibot_set_state(MIBOT_READY);
  else if (strcmp(phase, "action_aborted") == 0)
    mibot_set_state(MIBOT_DEGRADED);
  else if (strcmp(phase, "started") == 0 || strcmp(phase, "step") == 0)
    mibot_set_state(MIBOT_EXECUTING);
  mibot_lcd_render();
  (void)event;
}

static void mibot_handle_frame(uint8_t type, const uint8_t *payload,
                               uint16_t len)
{
  char text[MIBOT_MAX_PAYLOAD + 1];
  uint32_t now = mibot_now_ms();

  g_mibot.last_peer_ms = now;
  if (len > MIBOT_MAX_PAYLOAD)
    {
      return;
    }
  memcpy(text, payload, len);
  text[len] = '\0';

  switch (type)
    {
      case MIBOT_TYPE_HELLO_ACK:
        g_mibot.hello_acked = true;
        mibot_set_state(MIBOT_READY);
        mibot_lcd_render();
        break;
      case MIBOT_TYPE_PING:
        /* Do not write synchronously from the UART receive path.  The SiFli
         * lower-half can hard-fault when a response is queued while RX is
         * active (visible in local UART loopback).  Heartbeats are diagnostic
         * only; command/ACK traffic remains fully bidirectional. */
        break;
      case MIBOT_TYPE_PONG:
        break;
      case MIBOT_TYPE_COMMAND:
        mibot_handle_command(text);
        break;
      case MIBOT_TYPE_EVENT:
        mibot_handle_event(text);
        break;
      case MIBOT_TYPE_ACK:
        if (strstr(text, "\"state\":\"completed\"") != NULL)
          mibot_set_state(MIBOT_READY);
        else if (strstr(text, "\"state\":\"accepted\"") != NULL)
          mibot_set_state(MIBOT_EXECUTING);
        mibot_lcd_render();
        break;
      case MIBOT_TYPE_NACK:
        mibot_set_state(MIBOT_DEGRADED);
        mibot_lcd_render();
        break;
      case MIBOT_TYPE_AUDIO_DOWN:
#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
        {
          bool eos = false;
          bool is_eos_meta = len != MIBOT_AUDIO_FRAME_BYTES && len != 0 &&
                             payload != NULL && payload[0] == '{' &&
                             mibot_json_bool(text, "eos", &eos) && eos;

          /* ESP32 sends the stream boundary as mibot.audio.v1 JSON with a
           * non-zero length.  It is distinct from PCM and the legacy empty
           * marker, but has the same drain/stop semantics on this endpoint. */
          if (is_eos_meta)
            {
              mibot_audio_finish_downlink();
              break;
            }

          /* Entering SPEAKING starts the DAC through
           * mibot_audio_apply_state(); the driver keeps the DMA running and
           * fills silence on underrun, so a gap in the link never tears the
           * channel down. */
          if (g_mibot.state != MIBOT_SPEAKING)
            {
              mibot_set_state(MIBOT_SPEAKING);
              if (board_audio_play_start() == 0)
                {
                  g_mibot.play_active = true;
                }
              mibot_lcd_render();
            }

          if (len == MIBOT_AUDIO_FRAME_BYTES && g_mibot.play_active)
            {
              board_audio_play_write(payload, len);
            }
          else if (len == 0)
            {
              /* Zero-length frame marks end of stream: flush, then let the
               * state transition stop the channel. */
              mibot_audio_finish_downlink();
            }
        }
#endif
        break;
      default:
        break;
    }
}

static void mibot_parser_reset(struct mibot_parser_s *parser)
{
  memset(parser, 0, sizeof(*parser));
}

static void mibot_parser_feed(struct mibot_parser_s *parser,
                              const uint8_t *data, size_t len)
{
  size_t i;

  for (i = 0; i < len; i++)
    {
      uint8_t byte = data[i];
      switch (parser->stage)
        {
          case 0:
            parser->stage = byte == 0xaa ? 1 : 0;
            break;
          case 1:
            if (byte == 0x55)
              {
                parser->stage = 2;
                parser->header_pos = 0;
              }
            else
              {
                parser->stage = byte == 0xaa ? 1 : 0;
              }
            break;
          case 2:
            parser->header[parser->header_pos++] = byte;
            if (parser->header_pos == MIBOT_HEADER_SIZE)
              {
                uint16_t length = (uint16_t)parser->header[5] |
                                  ((uint16_t)parser->header[6] << 8);
                if (parser->header[0] != MIBOT_VERSION ||
                    length > MIBOT_MAX_PAYLOAD)
                  {
                    mibot_parser_reset(parser);
                  }
                else
                  {
                    parser->payload_len = length;
                    parser->payload_pos = 0;
                    parser->crc_pos = 0;
                    parser->stage = length == 0 ? 4 : 3;
                  }
              }
            break;
          case 3:
            parser->payload[parser->payload_pos++] = byte;
            if (parser->payload_pos == parser->payload_len)
              {
                parser->stage = 4;
              }
            break;
          case 4:
            parser->crc[parser->crc_pos++] = byte;
            if (parser->crc_pos == 2)
              {
                uint8_t frame_header[MIBOT_HEADER_SIZE];
                uint16_t expected;
                uint16_t received = (uint16_t)parser->crc[0] |
                                    ((uint16_t)parser->crc[1] << 8);
                memcpy(frame_header, parser->header, MIBOT_HEADER_SIZE);
                expected = mibot_crc16(frame_header, MIBOT_HEADER_SIZE);
                if (parser->payload_len != 0)
                  {
                    uint16_t crc = 0xffff;
                    size_t j;
                    for (j = 0; j < MIBOT_HEADER_SIZE; j++)
                      {
                        int bit;
                        crc ^= (uint16_t)frame_header[j] << 8;
                        for (bit = 0; bit < 8; bit++)
                          crc = (crc & 0x8000) != 0
                              ? (uint16_t)((crc << 1) ^ 0x1021)
                              : (uint16_t)(crc << 1);
                      }
                    for (j = 0; j < parser->payload_len; j++)
                      {
                        int bit;
                        crc ^= (uint16_t)parser->payload[j] << 8;
                        for (bit = 0; bit < 8; bit++)
                          crc = (crc & 0x8000) != 0
                              ? (uint16_t)((crc << 1) ^ 0x1021)
                              : (uint16_t)(crc << 1);
                      }
                    expected = crc;
                  }
                if (expected == received)
                  {
                    mibot_handle_frame(parser->header[1], parser->payload,
                                       parser->payload_len);
                  }
                mibot_parser_reset(parser);
              }
            break;
          default:
            mibot_parser_reset(parser);
            break;
        }
    }
}

/* Consume all currently buffered UART data in one scheduler pass.  Audio
 * frames are about 651 bytes on the wire, so a 256-byte single read cannot
 * keep up with the 50 Hz downlink.  The byte budget handles a pathological
 * continuous backlog while the normal path runs until the nonblocking read
 * reports EAGAIN. */
static void mibot_uart_drain_rx(void)
{
  uint8_t buffer[MIBOT_UART_RX_BUFFER_BYTES];
  size_t drained = 0;

  if (g_mibot.uart_fd < 0)
    {
      return;
    }

  while (drained < MIBOT_UART_DRAIN_MAX_BYTES)
    {
      ssize_t n = read(g_mibot.uart_fd, buffer, sizeof(buffer));
      if (n > 0)
        {
          mibot_parser_feed(&g_mibot.parser, buffer, (size_t)n);
          drained += (size_t)n;
          continue;
        }

      if (n == 0 || (n < 0 &&
                     (errno == EAGAIN || errno == EWOULDBLOCK)))
        {
          break;
        }
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n < 0)
        {
          /* SiFli's nonblocking UART may report transient read errors.
           * Closing this lower-half is unsafe on this BSP and can hard fault,
           * so retain the descriptor and retry on the next pass. */
          printf("mibot: UART read error: %d\n", errno);
        }
      break;
    }
}

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
static void *mibot_audio_task(void *arg)
{
  FAR struct mibot_ctx_s *ctx = arg;
  uint8_t frame[MIBOT_AUDIO_FRAME_BYTES];
  uint32_t sent = 0;
  uint32_t stream_serial = 0;
  bool meta_sent = false;

  printf("mibot: audio uplink thread ready\n");

  /* No printf() inside this loop.  Console output goes to UART2 as a blocking
   * write and delays the DMA interrupt long enough to starve the codec, which
   * is audible as periodic gaps.  Counters are reported on exit instead. */
  while (ctx->running && ctx->audio_running)
    {
      int ret;

      /* Capture is owned by mibot_audio_apply_state(); idle until the state
       * machine enters LISTENING. */
      if (!ctx->capture_active)
        {
          if (meta_sent)
            {
              /* Let the ESP32 close its uplink aggregation window before the
               * next LISTENING transition.  This is best effort because the
               * UART may already be down during shutdown. */
              (void)mibot_send_audio_meta(stream_serial, true);
              meta_sent = false;
            }
          usleep(20000);
          continue;
        }

      /* The ESP32 parser requires a mibot.audio.v1 metadata frame before it
       * accepts binary PCM.  Send it once per LISTENING session, before
       * waiting for the first DMA frame. */
      if (!meta_sent)
        {
          stream_serial++;
          if (mibot_send_audio_meta(stream_serial, false) < 0)
            {
              usleep(20000);
              continue;
            }
          meta_sent = true;
        }

      if (board_audio_stream_wait(200) < 0)
        {
          continue;
        }

      while ((ret = board_audio_stream_read(frame, sizeof(frame))) ==
             (int)sizeof(frame))
        {
          /* Audio frames are never retransmitted: a failed write is dropped
           * so the link cannot stall the capture path. */
          if (mibot_send_frame(MIBOT_TYPE_AUDIO_UP, frame,
                               (uint16_t)sizeof(frame)) == 0)
            {
              sent++;
            }
        }
    }

  printf("mibot: audio uplink stopped sent=%lu dropped=%lu\n",
         (unsigned long)sent, (unsigned long)board_audio_stream_dropped());
  ctx->audio_fd = -1;
  return NULL;
}
static void mibot_audio_start(void)
{
  g_mibot.audio_fd = -1;
  g_mibot.audio_running = true;
  if (pthread_create(&g_mibot.audio_thread, NULL, mibot_audio_task,
                     &g_mibot) != 0)
    {
      g_mibot.audio_running = false;
      printf("mibot: audio thread unavailable\n");
    }
}
#endif

#ifdef CONFIG_AUDIO_I2SCHAR
static int mibot_audio_i2s_worker(int argc, FAR char *argv[])
{
  int ret;
  (void)argc;
  (void)argv;
  printf("mibot: audio_i2s_init analog/hal begin\n");
  ret = board_audio_i2s_initialize();
  printf("mibot: audio_i2s_init %s (%d)\n", ret == 0 ? "ok" : "failed", ret);
  return ret;
}
#endif

int main(int argc, FAR char *argv[])
{
  uint32_t now;
  int uart_ret;
  (void)argc;

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO_HW
  /* Manual diagnostic mode keeps vendor analog bring-up out of the normal
   * daemon startup path, where a HAL clock wait could affect NSH. */
  if (argc > 1 && argv != NULL && strcmp(argv[1], "audio_init") == 0)
    {
      int ret = mibot_audio_hw_init();
      printf("mibot: audio_init %s (%d), analog_ready=%d\n",
             ret == 0 ? "ok" : "failed", ret,
             board_audio_analog_ready());
      return ret == 0 ? 0 : 1;
    }
#else
  (void)argv;
#endif

#ifdef CONFIG_AUDIO_I2SCHAR
  /* Audio HAL/PLL initialization is intentionally an explicit command.  A
   * vendor clock wait must never prevent the NSH console from starting. */
  if (argc > 1 && argv != NULL && strcmp(argv[1], "audio_i2s_init") == 0)
    {
      /* Keep the diagnostic worker below nsh_main.  Some HAL revisions can
       * wait for an audio PLL indefinitely; that must not starve the shell. */
      int pid = task_create("audio_init", 100, 4096,
                            mibot_audio_i2s_worker, NULL);
      printf("mibot: audio_i2s_init started (pid=%d)\n", pid);
      return pid < 0 ? 1 : 0;
    }
#endif

  memset(&g_mibot, 0, sizeof(g_mibot));
  g_mibot.uart_fd = -1;
  g_mibot.fb.fd = -1;
#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
  g_mibot.audio_fd = -1;
#endif
  pthread_mutex_init(&g_mibot.tx_lock, NULL);
  pthread_mutex_init(&g_mibot.audio_lock, NULL);
  g_mibot.running = true;
  mibot_parser_reset(&g_mibot.parser);
  /* Let the board LCD worker finish panel power-up before the first frame. */
  sleep(2);
  mibot_set_state(MIBOT_BOOT);
  mibot_lcd_render();
  mibot_set_state(MIBOT_SELF_CHECK);
  mibot_lcd_render();

  uart_ret = mibot_uart_open();
  if (uart_ret < 0)
    {
      printf("mibot: UART %s unavailable: %d\n",
             CONFIG_EXAMPLES_MIBOT_AGENT_UART, -uart_ret);
      mibot_set_state(MIBOT_DEGRADED);
      g_mibot.last_uart_attempt_ms = mibot_now_ms();
    }
  else
    {
      printf("mibot: UART2 %s @ %d 8N1 ready\n",
             CONFIG_EXAMPLES_MIBOT_AGENT_UART,
             CONFIG_EXAMPLES_MIBOT_AGENT_BAUD);
      mibot_send_hello();
      g_mibot.last_hello_ms = mibot_now_ms();
    }
  mibot_lcd_render();

#ifdef CONFIG_AUDIO_I2SCHAR
  printf("mibot: audio I2S registration deferred; run 'mibot_agent audio_i2s_init'\n");
#endif

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO_HW
  /* Do not run the vendor PLL/analog sequence on the main startup path.
   * Some SDK revisions wait indefinitely for an audio PLL lock. The audio
   * bring-up must be invoked from a dedicated test worker with a timeout. */
  printf("mibot: audio hardware test is disabled on startup\n");
#endif

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
  mibot_audio_start();
#else
  printf("mibot: audio bridge disabled; register the SF32 AUDCODEC/AUDPRC "
         "lower-half first\n");
#endif

  while (g_mibot.running)
    {
      /* This SiFli UART lower-half has an unstable poll()/semaphore path.
       * Use a short sleep and direct nonblocking reads instead. */
      usleep(20000);
      mibot_uart_drain_rx();

      now = mibot_now_ms();
      /* The panel registers asynchronously after /dev/fb0 is created.  Keep
       * refreshing during early boot so the first frame is delivered after
       * CO5300 becomes ready. */
      if (now < 15000 && now - g_mibot.last_lcd_refresh_ms >= 500)
        {
          mibot_lcd_render();
          g_mibot.last_lcd_refresh_ms = now;
        }
      if (g_mibot.uart_fd < 0 &&
          now - g_mibot.last_uart_attempt_ms >= MIBOT_UART_RECONNECT_MS)
        {
          g_mibot.last_uart_attempt_ms = now;
          uart_ret = mibot_uart_open();
          if (uart_ret == 0)
            {
              printf("mibot: UART2 %s reconnected @ %d 8N1\n",
                     CONFIG_EXAMPLES_MIBOT_AGENT_UART,
                     CONFIG_EXAMPLES_MIBOT_AGENT_BAUD);
              mibot_send_hello();
              g_mibot.last_hello_ms = now;
              mibot_set_state(MIBOT_SELF_CHECK);
              mibot_lcd_render();
            }
        }
      if (g_mibot.uart_fd >= 0 && now - g_mibot.last_ping_ms >=
          MIBOT_HEARTBEAT_MS)
        {
          mibot_send_ping();
          g_mibot.last_ping_ms = now;
        }
      if (g_mibot.uart_fd >= 0 && !g_mibot.hello_acked &&
          now - g_mibot.last_hello_ms >= MIBOT_HELLO_RETRY_MS)
        {
          mibot_send_hello();
          g_mibot.last_hello_ms = now;
        }
      if (g_mibot.hello_acked && now - g_mibot.last_peer_ms >
          MIBOT_LINK_TIMEOUT_MS && g_mibot.state != MIBOT_DEGRADED)
        {
          mibot_set_state(MIBOT_DEGRADED);
          mibot_lcd_render();
        }
    }

#ifdef CONFIG_EXAMPLES_MIBOT_AGENT_AUDIO
  g_mibot.audio_running = false;
  if (g_mibot.audio_thread != 0)
    pthread_join(g_mibot.audio_thread, NULL);
#endif
  mibot_lcd_close();
  if (g_mibot.uart_fd >= 0)
    {
      close(g_mibot.uart_fd);
    }
  return 0;
}
