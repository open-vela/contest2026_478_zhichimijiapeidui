/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - device-only SF32 application main (task 9).
 *
 * This file is the NuttX/SF32 hardware glue that wires the host-tested pure
 * logic core (mibot_voice_agent.c/.h, va_stubs.c/.h, va_test_args.c/.h) to
 * real hardware:
 *
 *   9.1  UART frame RX + AA55/CRC parser feeding va_on_frame / va_handle_event
 *   9.2  LCD bitmap-font rendering per Main_State (non-blocking, main loop)
 *   9.3  audio capture (AUDIO_UP) / playback (AUDIO_DOWN) tied to states
 *   9.4  THINKING DeepSeek AI_REQUEST/AI_RESPONSE + native tool_calls round-trip
 *
 * The ENTIRE file is guarded by `#if defined(__NuttX__)` so the host test
 * project (which only builds the pure-logic core) never compiles it.  Device
 * validation is compile-only in this task; the SF32 build and NSH registration
 * land with the `-VoiceAgent` build profile in task 11.
 *
 * Design contract: the dialog main state machine authority stays in the pure
 * core (va_transition/va_handle_event/va_tick).  This file only implements the
 * injectable va_hooks_t effects and the transport, and feeds abstract events
 * into the core; it never duplicates the transition logic.
 */

#if defined(__NuttX__)

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/video/fb.h>
#include <nuttx/input/buttons.h>

/* Board header carries the button bit numbering (BUTTON_KEY2).  Included
 * defensively so this file still compiles if the board does not ship one. */
#if defined(__has_include)
#  if __has_include(<arch/board/board.h>)
#    include <arch/board/board.h>
#  endif
#endif

#include "cJSON.h"

#include "mibot_voice_agent.h"
#include "va_face.h"
#include "va_stubs.h"
#include "va_test_args.h"

/* First-version acceptance is 全桩 (all-stub): the ASR/TTS stub switches
 * (MIBOT_VA_ASR_STUB / MIBOT_VA_TTS_STUB) are defined by the build system for
 * every translation unit (see CMakeLists DEFINITIONS), so va_stubs.c also
 * takes its stub branches.  Do NOT define them here per-file: that would leave
 * va_stubs.c compiled as the "real placeholder" and desync the two units. */

/* ------------------------------------------------------------------------- */
/* UART device / baud (shared with mibot_agent, /dev/ttyS0).                  */
/* ------------------------------------------------------------------------- */

/* Prefer the voice-agent Kconfig options (task 11.1); fall back to the shared
 * mibot_agent options and finally to sane defaults so the file also compiles
 * standalone. */
#if !defined(CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_UART)
#  if defined(CONFIG_EXAMPLES_MIBOT_AGENT_UART)
#    define CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_UART CONFIG_EXAMPLES_MIBOT_AGENT_UART
#  else
#    define CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_UART "/dev/ttyS0"
#  endif
#endif
#if !defined(CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_BAUD)
#  if defined(CONFIG_EXAMPLES_MIBOT_AGENT_BAUD)
#    define CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_BAUD CONFIG_EXAMPLES_MIBOT_AGENT_BAUD
#  else
#    define CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_BAUD 1000000
#  endif
#endif
#if !defined(CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_FB)
#  if defined(CONFIG_EXAMPLES_MIBOT_AGENT_FB)
#    define CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_FB CONFIG_EXAMPLES_MIBOT_AGENT_FB
#  else
#    define CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_FB "/dev/fb0"
#  endif
#endif
#if !defined(CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_BUTTON)
#  define CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_BUTTON "/dev/buttons"
#endif

#define VA_UART_DEVICE CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_UART
#define VA_UART_BAUD   CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_BAUD
#define VA_FB_DEVICE   CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_FB
#define VA_BUTTON_DEVICE CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_BUTTON

/* KEY2 sits in bit 0 of the button set (board.h defines BUTTON_KEY2 as 0 and
 * registers NUM_BUTTONS 1).  Taken from the board header when present so a
 * board that renumbers its keys stays correct without editing this file. */
#ifndef BUTTON_KEY2_BIT
#  define BUTTON_KEY2_BIT (1u << 0)
#endif

/* ------------------------------------------------------------------------- */
/* AA55/CRC frame protocol - copied verbatim from mibot_agent_main.c so this  */
/* app speaks the exact same wire protocol to the ESP32 (需求 7.1-7.4).       */
/* ------------------------------------------------------------------------- */

#define MIBOT_VERSION        1
#define MIBOT_MAX_PAYLOAD    4096
#define MIBOT_HEADER_SIZE    7
#define MIBOT_FRAME_OVERHEAD 11

#define MIBOT_TYPE_HELLO      0x01
#define MIBOT_TYPE_HELLO_ACK  0x02
#define MIBOT_TYPE_COMMAND    0x10
#define MIBOT_TYPE_ACK        0x11
#define MIBOT_TYPE_NACK       0x12
#define MIBOT_TYPE_EVENT      0x20
#define MIBOT_TYPE_TELEMETRY  0x21
#define MIBOT_TYPE_AUDIO_UP   0x30
#define MIBOT_TYPE_AUDIO_DOWN 0x31
#define MIBOT_TYPE_AI_REQUEST  0x40
#define MIBOT_TYPE_AI_RESPONSE 0x41

/* AI_REQUEST asks the ESP32 gateway to relay to DeepSeek and reply (deepseek
 * smoke uses this same ACK-request flag bit). */
#define MIBOT_FLAG_ACK_REQUEST (1u << 0)

/* Board audio: dense 16 kHz mono PCM, 20 ms frames of 640 bytes. */
#define MIBOT_AUDIO_FRAME_BYTES 640
#define MIBOT_AUDIO_SAMPLE_RATE 16000
#define MIBOT_AUDIO_CHANNELS    1
#define MIBOT_AUDIO_FRAME_MS    20

/* Timing budgets for the main-loop per-state work. */

/* Main-loop period.  This is also how often the UART is drained, which is what
 * sets it: AUDIO_DOWN arrives at 50 frames/s, so a 20 ms period let ~650 bytes
 * pile up between drains, and any loop iteration that overran the driver's
 * 4 KB RX buffer (an LCD redraw is ~30 ms, and sf_play_begin_dma() sleeps 30 ms
 * for the amplifier) simply lost bytes.  Measured cost: the ESP32 sent 480 PCM
 * frames and the parser only ever saw 164 of them -- 66% of the reply audio
 * dropped on the floor, heard as chopped speech.
 *
 * 5 ms keeps the per-drain backlog around 160 bytes and leaves the driver buffer
 * ~25 iterations of headroom.  Everything else in the loop is deadline-driven,
 * so a shorter period changes only how promptly work is noticed. */
#define VA_MAIN_TICK_US         5000

/* Capture window.
 *
 * This used to be a flat 3 s, which is a bring-up value, not a usable one: the
 * window opens the instant `va_wake` fires and there is no prompt tone, so a
 * human has not finished (often not even started) speaking before it closes.
 * The recogniser then gets room noise, returns no text, and the turn dies with
 * "cloud ASR timeout, CLOUD_FAIL" -- with nothing in the log to say the user
 * was simply too slow.
 *
 * So: a generous hard cap, plus a light end-of-speech detector that closes the
 * window early once the user has actually spoken and then gone quiet.  The
 * ESP32's own silence-based uplink eos never fires here because the mic keeps
 * producing frames for the whole window, so this SF32-side detector is what
 * ends the utterance for the recogniser. */
#define VA_LISTEN_WINDOW_MS    8000    /* hard cap on the capture window       */
/* The capture-start transient (AUDCODEC ADC / mic-bias settle) must never be
 * mistaken for speech, or it would early-close the window immediately.  The
 * driver already drops SF32_STREAM_WARMUP_FRAMES; this covers the decaying
 * tail.  Frames in the lead-in are still uploaded, just not used for detection. */
#define VA_IGNORE_LEAD_MS      800
#define VA_END_SILENCE_MS      1500    /* quiet run after speech -> early stop */
#define VA_END_MIN_MS          2500    /* never close before this elapses      */
#define VA_END_PEAK            600     /* |s16| below this counts as silence   */

/* Push-to-talk on KEY2.  Holding the button is an explicit "I am still
 * talking" signal from the user, which beats any amount of silence detection:
 * a pause mid-sentence must not end the utterance.  So while the button is
 * held the window stays open and the speech detector above is disabled; the
 * release is what closes it.
 *
 * A quick tap has no useful hold period, so it falls back to the timed window
 * (that is also the only mode available to `va_wake`, which has no release). */
#define VA_PTT_MIN_HOLD_MS     400     /* shorter press = tap -> timed window  */
#define VA_PTT_MAX_HOLD_MS     20000   /* cap if a release is never observed   */
#define VA_BUTTON_RELEASE_MS   60      /* release must persist this long       */
/* Cloud reply budget.  MUST stay above the ESP32 gateway's own HTTP timeout
 * (HTTP_TIMEOUT_MS = 20 s in deepseek_smoke/esp32/deepseek_gateway.cpp).
 *
 * It used to be 12 s on the theory that the gateway's 20 s left room for a
 * retry.  That is backwards: giving up while the gateway is still waiting makes
 * the retry land on a gateway that is single-flight (g_busy), so the retry is
 * rejected outright AND the original reply is then discarded for having the
 * previous request_id.  Observed with MiMo, which needs 8-14 s per turn:
 *   va: no AI_RESPONSE in budget, retry 1/1
 *   va: AI_RESPONSE ignored id="<missing>" expected="va-6"   <- retry rejected
 *   va: AI_RESPONSE ignored id="va-5"      expected="va-6"   <- good reply lost
 *   va: THINKING timeout, CLOUD_FAIL
 * The retry is only meant to recover a frame lost on the UART, so it has to
 * wait until the cloud attempt itself has definitely finished. */
#define VA_THINKING_TIMEOUT_MS 25000   /* LLM reply deadline -> retry/CLOUD_FAIL */
/* Cloud ASR text deadline.  Scales with how much audio we may have sent: the
 * recogniser only starts work at eos, and a 20 s push-to-talk utterance takes
 * proportionally longer to decode than the 3 s the old fixed window produced. */
#define VA_ASR_TIMEOUT_MS      15000   /* cloud ASR text deadline -> CLOUD_FAIL */
#define VA_AI_MAX_RETRIES      1       /* re-send once if the reply is lost     */
#define VA_ACTING_TIMEOUT_MS   8000    /* action_update fallback -> ACTION_DONE */
/* Contact bounce lockout for the physical wake button.  The board lower half
 * debounces nothing: it reports the raw pin level, sampled on a GPIO interrupt
 * and a 10 ms watchdog.  A press edge inside this window is treated as bounce. */
#define VA_BUTTON_DEBOUNCE_MS  250
#define VA_SPEAKING_TIMEOUT_MS 30000   /* AUDIO_DOWN eos fallback -> SPEAK_DONE */
#define VA_HEARTBEAT_MS        500
#define VA_HELLO_RETRY_MS      2000
#define VA_UART_RX_BUFFER      1024
#define VA_UART_DRAIN_MAX      (VA_UART_RX_BUFFER * 8)
/* Enough of a NACK payload to carry command_id + error code; the rest is of no
 * diagnostic value and this sits on the parser path's stack. */
#define VA_NACK_LOG_MAX        160
/* Advertise robot_perform_action through the OpenAI tools API?  Off: a model
 * that uses it returns null content, leaving nothing to speak.  Actions arrive
 * via the gateway's "[ACT]" text marker instead.  See
 * va_thinking_send_ai_request(). */
#define VA_ADVERTISE_ACTION_TOOL 0

/* Board audio driver (reused, not reimplemented; same externs as va_test.c /
 * sf32_audio_test.c / deepseek_smoke.c). */
extern int board_audio_stream_start(void);
extern int board_audio_stream_read(void *buf, size_t len);
extern int board_audio_stream_wait(int timeout_ms);
extern int board_audio_stream_stop(void);
extern uint32_t board_audio_stream_dropped(void);
extern int board_audio_play_start(void);
extern int board_audio_play_write(const void *buf, size_t len);
extern int board_audio_play_tone(int seconds);
extern int board_audio_play_drain(int timeout_ms);
extern int board_audio_play_stop(void);

/* ------------------------------------------------------------------------- */
/* Frame parser + framebuffer + app context.                                 */
/* ------------------------------------------------------------------------- */

struct va_parser_s
{
  uint8_t  stage;
  uint8_t  header[MIBOT_HEADER_SIZE];
  uint8_t  header_pos;
  /* Reserve one byte so JSON control payloads can be safely terminated
   * before cJSON_Parse(); binary audio still uses payload_len explicitly. */
  uint8_t  payload[MIBOT_MAX_PAYLOAD + 1];
  uint16_t payload_len;
  uint16_t payload_pos;
  uint8_t  crc[2];
  uint8_t  crc_pos;
};

/* Downlink receive instrumentation.
 *
 * The ESP32 reports every AI_RESPONSE and AUDIO_DOWN frame as successfully
 * handed to uart_write_bytes(), yet the SF32 acts on far fewer of them, and
 * every discard path in va_parser_feed() is silent.  That left no way to tell
 * a byte that never arrived from a byte that arrived and was thrown away.
 * These counters split those two cases:
 *
 *   rx_bytes     total bytes read off the UART, whatever they decode to
 *   resync       bytes consumed while hunting for AA55 (stage 0/1).  A healthy
 *                link is ~0; a large value means the byte stream is corrupt or
 *                has holes, so the parser keeps landing mid-frame
 *   bad_header   version mismatch or length > MIBOT_MAX_PAYLOAD
 *   crc_fail     frame assembled but the CRC did not match
 *   frames_ok    frames delivered to va_handle_frame()
 *
 * rx_bytes against the ESP32's own TX byte count says whether the link is
 * losing bytes at all; resync/bad_header/crc_fail against frames_ok says
 * whether what did arrive was intact. */
static uint32_t g_rx_bytes;
static uint32_t g_rx_resync;
static uint32_t g_rx_bad_header;
static uint32_t g_rx_crc_fail;
static uint32_t g_rx_frames_ok;

static void va_rx_stats_print(const char *tag)
{
  printf("va: rx %s bytes=%lu frames=%lu resync=%lu badhdr=%lu crcfail=%lu\n",
         tag, (unsigned long)g_rx_bytes, (unsigned long)g_rx_frames_ok,
         (unsigned long)g_rx_resync, (unsigned long)g_rx_bad_header,
         (unsigned long)g_rx_crc_fail);
}

struct va_fb_s
{
  int fd;
  uint8_t *mem;
  size_t len;
  bool mapped;
  struct fb_videoinfo_s video;
  struct fb_planeinfo_s plane;
};

/* Device app state: the pure-logic core (ctx) plus transport/hardware handles.
 * The core owns Main_State; everything here is glue. */
struct va_app_s
{
  va_context_t ctx;              /* pure-logic core                          */
  int uart_fd;
  uint16_t seq;                  /* outbound frame sequence                  */
  struct va_parser_s parser;
  struct va_fb_s fb;

  bool running;
  bool hello_acked;

  /* 9.2: LCD redraw request set by the lcd_show hook, serviced from the main
   * loop so rendering never blocks a state transition (需求 4.5, 7.5). */
  volatile bool lcd_dirty;
  va_state_t   lcd_state;        /* last state the hook asked us to render    */

  /* 9.3 audio path bookkeeping. */
  bool capture_active;
  bool play_active;
  bool speak_tone_pending;   /* stub SPEAKING: play a local placeholder tone */
  va_state_t last_serviced_state;   /* for one-shot state-entry detection    */
  uint32_t audio_stream_serial;
  bool audio_meta_sent;
  /* AUDIO_UP accounting: separates "SF32 never sent PCM" from "ESP32 dropped
   * it" when the cloud reports an empty utterance. */
  uint32_t uplink_frames_sent;
  uint32_t uplink_frames_failed;

  /* End-of-speech tracking for the early window close.  have_speech latches
   * once any frame passed VA_END_PEAK, so a leading pause cannot trigger it.
   * listen_started_ms is kept explicitly rather than derived from
   * listen_deadline_ms: the deadline is VA_PTT_MAX_HOLD_MS for a held button
   * and VA_LISTEN_WINDOW_MS otherwise, so subtracting one fixed constant from
   * it would give the wrong start time in the other mode. */
  bool     have_speech;
  uint32_t last_speech_ms;
  uint32_t listen_started_ms;

  /* Per-state deadlines (monotonic ms); 0 means "not armed". */
  uint32_t listen_deadline_ms;
  uint32_t thinking_deadline_ms;
  uint32_t acting_deadline_ms;
  uint32_t speaking_deadline_ms;

  /* Link heartbeat / hello retry. */
  uint32_t last_hello_ms;
  uint32_t last_ping_ms;

  /* 9.4: staged transcript for the DeepSeek request built on entering
   * THINKING.  Every request receives a monotonic ID so a late response from
   * an older turn cannot complete the current turn. */
  char transcript[256];
  char ai_request_id[32];
  uint32_t ai_request_serial;
  bool ai_request_sent;

  /* Task 13 real ASR: the transcript arrives asynchronously as a
   * `mibot.asr.v1` payload relayed by the ESP32 from the cloud recogniser, so
   * LISTENING cannot resolve it synchronously the way the stub does. */
  bool asr_waiting;
  bool asr_text_ready;
  uint32_t asr_deadline_ms;

  /* Retry counter for a cloud reply lost to a corrupted UART frame. */
  int ai_retries;

  /* Task 13 real TTS: `robot.speak` is sent once per SPEAKING turn and the
   * audio arrives as AUDIO_DOWN frames. */
  bool speak_request_sent;

  /* Physical wake button (KEY2 / PA11).  Polled from the main loop like every
   * other slow device so a bouncing contact can never stall a transition. */
  int btn_fd;
  btn_buttonset_t btn_last;      /* previous sample, for edge detection      */
  uint32_t btn_last_press_ms;    /* debounce lockout timestamp               */

  /* Push-to-talk.  ptt_active means the current capture window belongs to a
   * held button and is closed by its release, not by a timer. */
  bool     ptt_active;
  bool     ptt_release_pending;  /* saw a release edge, waiting out bounce    */
  uint32_t ptt_release_ms;       /* when that release edge was seen           */
  bool     ptt_close_request;    /* release committed; close the window now   */

  /* LCD face.  `face` is what was last drawn; comparing the freshly composed
   * geometry against it is what keeps the blink/breathing animation from
   * costing a redraw on every tick. */
  va_face_t      face;
  va_face_anim_t face_anim;
  bool           lcd_cleared;    /* background painted at least once         */
  bool           lcd_bar_shown;  /* speaking bar is currently on the panel    */

  /* AUDIO_DOWN accounting for one reply.  seen counts every 640-byte PCM frame
   * that reached the parser; late counts the ones that arrived with no playback
   * open (they are discarded, so they are lost audio). */
  uint32_t       downlink_frames_seen;
  uint32_t       downlink_frames_late;
  uint8_t        speak_level;    /* 0..255, drives the SPEAKING bar          */
};

static struct va_app_s g_app;

static void va_parser_reset(struct va_parser_s *parser);
static void va_lcd_render(struct va_app_s *app);
static int  va_send_frame(struct va_app_s *app, uint8_t type, uint8_t flags,
                          const void *payload, uint16_t len);
static void va_thinking_send_ai_request(struct va_app_s *app);

/* ------------------------------------------------------------------------- */
/* Small helpers.                                                            */
/* ------------------------------------------------------------------------- */

static uint32_t va_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

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

/* ------------------------------------------------------------------------- */
/* UART transport (需求 7.4): open, send framed payloads.                     */
/* ------------------------------------------------------------------------- */

static int va_uart_open(struct va_app_s *app)
{
  struct termios tio;
  speed_t speed;

  app->uart_fd = open(VA_UART_DEVICE,
                      O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (app->uart_fd < 0)
    {
      return -errno;
    }

  if (tcgetattr(app->uart_fd, &tio) < 0)
    {
      close(app->uart_fd);
      app->uart_fd = -1;
      return -errno;
    }

  speed = B921600;
#ifdef B1000000
  if (VA_UART_BAUD == 1000000)
    {
      speed = B1000000;
    }
#endif
#ifdef B115200
  if (VA_UART_BAUD == 115200)
    {
      speed = B115200;
    }
#endif
#ifdef B460800
  if (VA_UART_BAUD == 460800)
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
  if (tcsetattr(app->uart_fd, TCSANOW, &tio) < 0)
    {
      close(app->uart_fd);
      app->uart_fd = -1;
      return -errno;
    }

  return 0;
}

/* Frame layout identical to mibot_send_frame / send_frame_bytes. */
static int va_send_frame(struct va_app_s *app, uint8_t type, uint8_t flags,
                         const void *payload, uint16_t len)
{
  static uint8_t frame[MIBOT_MAX_PAYLOAD + MIBOT_FRAME_OVERHEAD];
  uint16_t crc;
  uint16_t seq;

  if (app->uart_fd < 0 || len > MIBOT_MAX_PAYLOAD)
    {
      return -ENODEV;
    }

  seq = app->seq++;
  frame[0] = 0xaa;
  frame[1] = 0x55;
  frame[2] = MIBOT_VERSION;
  frame[3] = type;
  frame[4] = flags;
  frame[5] = (uint8_t)(seq & 0xff);
  frame[6] = (uint8_t)(seq >> 8);
  frame[7] = (uint8_t)(len & 0xff);
  frame[8] = (uint8_t)(len >> 8);
  if (len != 0 && payload != NULL)
    {
      memcpy(&frame[9], payload, len);
    }

  crc = va_crc16(&frame[2], MIBOT_HEADER_SIZE + len);
  frame[9 + len]  = (uint8_t)(crc & 0xff);
  frame[10 + len] = (uint8_t)(crc >> 8);

  return va_write_all(app->uart_fd, frame, len + MIBOT_FRAME_OVERHEAD);
}

static int va_send_command_json(struct va_app_s *app, const char *json)
{
  size_t len = strlen(json);

  if (len > MIBOT_MAX_PAYLOAD)
    {
      return -E2BIG;
    }
  return va_send_frame(app, MIBOT_TYPE_COMMAND, MIBOT_FLAG_ACK_REQUEST,
                       json, (uint16_t)len);
}

static void va_send_hello(struct va_app_s *app)
{
  static const char payload[] =
    "{\"schema\":\"mibot.uart.v1\",\"node\":\"sf32-voice-agent\","
    "\"capabilities\":[\"lcd\",\"audio\",\"uart2\",\"deepseek\"]}";

  va_send_frame(app, MIBOT_TYPE_HELLO, 0, payload,
                (uint16_t)strlen(payload));
}

/* ------------------------------------------------------------------------- */
/* 9.4 helpers: extract the tool-call action and map DeepSeek tool names.     */
/* ------------------------------------------------------------------------- */

/* Map a DeepSeek native tool name to the dialog-driven ACTING action name.
 * The only motion tool for the voice agent is robot_perform_action, whose
 * "action" argument names the emotion.  Returns a heap-free pointer into the
 * caller-provided buffer, or NULL if the tool call is not an action request.
 * The whitelist is finally enforced by va_request_acting (需求 8.6, P12). */
static bool va_tool_call_action(const cJSON *tool_call,
                                char *action_out, size_t action_size)
{
  const cJSON *function;
  const cJSON *name;
  const cJSON *arguments;
  cJSON *args;
  const cJSON *action;
  bool ok = false;

  if (tool_call == NULL || action_out == NULL || action_size == 0)
    {
      return false;
    }

  function = cJSON_GetObjectItemCaseSensitive(tool_call, "function");
  name = (function != NULL)
           ? cJSON_GetObjectItemCaseSensitive(function, "name") : NULL;
  arguments = (function != NULL)
           ? cJSON_GetObjectItemCaseSensitive(function, "arguments") : NULL;

  if (!cJSON_IsString(name) || !cJSON_IsString(arguments))
    {
      return false;
    }

  if (strcmp(name->valuestring, "robot_perform_action") != 0 &&
      strcmp(name->valuestring, "robot.perform_action") != 0)
    {
      return false;
    }

  /* DeepSeek delivers arguments as a JSON *string*; parse it to read action. */
  args = cJSON_Parse(arguments->valuestring);
  action = (args != NULL)
             ? cJSON_GetObjectItemCaseSensitive(args, "action") : NULL;
  if (cJSON_IsString(action) && action->valuestring[0] != '\0')
    {
      strncpy(action_out, action->valuestring, action_size - 1);
      action_out[action_size - 1] = '\0';
      ok = true;
    }

  cJSON_Delete(args);
  return ok;
}

/* Send robot.perform_action <name> to the ESP32 once the core has entered
 * ACTING.  ESP32 owns the concrete motor/servo values and its safety_task
 * (需求 5.1, 5.4, 7.3, 7.5). */
static void va_send_perform_action(struct va_app_s *app, const char *action)
{
  char json[192];
  int len;

  len = snprintf(json, sizeof(json),
                 "{\"schema\":\"mibot.uart.v1\",\"name\":"
                 "\"robot.perform_action\",\"command_id\":\"va\","
                 "\"args\":{\"action\":\"%s\"}}",
                 action != NULL ? action : "idle");
  if (len > 0 && len < (int)sizeof(json))
    {
      va_send_command_json(app, json);
    }
}

/* Real TTS (task 13): ask the ESP32 to synthesise `text` through its cloud
 * adapter.  The audio comes back as AUDIO_DOWN PCM frames, which
 * va_on_audio_down() feeds to the speaker; an eos marker ends SPEAKING.  The
 * ESP32 rejects this with E_CLOUD_TIMEOUT when no cloud endpoint is
 * configured, which surfaces as a NACK rather than a silent stall. */
static void va_send_speak_request(struct va_app_s *app, const char *text)
{
  char json[VA_PENDING_ANSWER_SIZE + 192];
  const char *safe = (text != NULL && text[0] != '\0') ? text : "好的";
  char escaped[VA_PENDING_ANSWER_SIZE];
  size_t out = 0;
  size_t i;
  int len;

  if (app->speak_request_sent)
    {
      return;
    }

  /* Minimal JSON string escaping for the answer text. */
  for (i = 0; safe[i] != '\0' && out + 2 < sizeof(escaped); i++)
    {
      unsigned char c = (unsigned char)safe[i];

      if (c == '"' || c == '\\')
        {
          escaped[out++] = '\\';
          escaped[out++] = (char)c;
        }
      else if (c >= 0x20)
        {
          escaped[out++] = (char)c;
        }
    }
  escaped[out] = '\0';

  len = snprintf(json, sizeof(json),
                 "{\"schema\":\"mibot.uart.v1\",\"name\":\"robot.speak\","
                 "\"command_id\":\"%s-tts\",\"args\":{\"text\":\"%s\","
                 "\"voice\":\"default\",\"interruptible\":true}}",
                 app->ai_request_id[0] != '\0' ? app->ai_request_id : "va",
                 escaped);
  if (len <= 0 || len >= (int)sizeof(json))
    {
      printf("va: robot.speak too long, skipping TTS request\n");
      return;
    }

  if (va_send_command_json(app, json) < 0)
    {
      printf("va: robot.speak send failed\n");
      return;
    }

  app->speak_request_sent = true;
  printf("va: robot.speak sent (%u text bytes)\n", (unsigned)out);
}

/* Emergency stop command sent when the core enters SAFE_STOP (需求 2.2, 2.3). */
static void va_send_robot_stop(struct va_app_s *app, bool emergency)
{
  char json[160];
  int len;

  len = snprintf(json, sizeof(json),
                 "{\"schema\":\"mibot.uart.v1\",\"name\":\"robot.stop\","
                 "\"command_id\":\"va\",\"args\":{\"emergency\":%s}}",
                 emergency ? "true" : "false");
  if (len > 0 && len < (int)sizeof(json))
    {
      va_send_command_json(app, json);
    }
}

/* ------------------------------------------------------------------------- */
/* 9.3 audio path start/stop (needs board_audio_*).                           */
/* ------------------------------------------------------------------------- */

static int va_send_audio_meta(struct va_app_s *app, bool eos)
{
  char meta[256];
  int len;

  len = snprintf(meta, sizeof(meta),
                 "{\"schema\":\"mibot.audio.v1\",\"stream_id\":\"va_%lu\","
                 "\"codec\":\"pcm_s16le\",\"sample_rate\":%u,\"channels\":%u,"
                 "\"frame_ms\":%u,\"bytes\":%u,\"eos\":%s}",
                 (unsigned long)app->audio_stream_serial,
                 (unsigned)MIBOT_AUDIO_SAMPLE_RATE,
                 (unsigned)MIBOT_AUDIO_CHANNELS,
                 (unsigned)MIBOT_AUDIO_FRAME_MS,
                 (unsigned)MIBOT_AUDIO_FRAME_BYTES,
                 eos ? "true" : "false");
  if (len <= 0 || len >= (int)sizeof(meta))
    {
      return -EOVERFLOW;
    }
  return va_send_frame(app, MIBOT_TYPE_AUDIO_UP, 0, meta, (uint16_t)len);
}

static void va_capture_start(struct va_app_s *app)
{
  if (app->capture_active)
    {
      return;
    }
  if (board_audio_stream_start() == 0)
    {
      app->capture_active = true;
      app->audio_stream_serial++;
      app->audio_meta_sent = false;
      app->uplink_frames_sent = 0;
      app->uplink_frames_failed = 0;
      printf("va: AUDIO_UP capture start (stream %lu)\n",
             (unsigned long)app->audio_stream_serial);
    }
  else
    {
      printf("va: AUDIO_UP capture start failed\n");
    }
}

static void va_capture_stop(struct va_app_s *app)
{
  if (!app->capture_active)
    {
      return;
    }
  app->capture_active = false;
  board_audio_stream_stop();
  /* Tell the ESP32 the uplink window is closed so it can finalise ASR. */
  (void)va_send_audio_meta(app, true);
  printf("va: AUDIO_UP capture stop (dropped=%u sent=%lu failed=%lu)\n",
         (unsigned)board_audio_stream_dropped(),
         (unsigned long)app->uplink_frames_sent,
         (unsigned long)app->uplink_frames_failed);
}

static void va_play_start(struct va_app_s *app)
{
  if (app->play_active)
    {
      return;
    }
  if (board_audio_play_start() == 0)
    {
      app->play_active = true;
      app->downlink_frames_seen = 0;
      app->downlink_frames_late = 0;
      printf("va: AUDIO_DOWN playback start\n");
    }
}

static void va_play_stop(struct va_app_s *app)
{
  if (!app->play_active)
    {
      return;
    }
  (void)board_audio_play_drain(1000);
  board_audio_play_stop();
  app->play_active = false;
  /* Animation was frozen for the duration of the reply; ask for one redraw so
   * the face picks the schedule back up from its current geometry. */
  app->lcd_dirty = true;
  va_rx_stats_print("playback-stop");
  printf("va: AUDIO_DOWN playback stop (pcm_frames_seen=%lu late=%lu)\n",
         (unsigned long)app->downlink_frames_seen,
         (unsigned long)app->downlink_frames_late);
}

/* ------------------------------------------------------------------------- */
/* va_hooks_t implementations (9.2 LCD, 9.3 audio, 5.1 action).               */
/*                                                                            */
/* All hooks receive the va_context_t*; `user` is the struct va_app_s*.       */
/* ------------------------------------------------------------------------- */

/* 9.2: never draw from the hook (it can run in the frame/parser path).  Latch
 * a redraw request + the target state; the main loop does the framebuffer draw
 * so LCD rendering cannot block a state transition (需求 4.5, 7.5). */
static void va_hook_lcd_show(struct va_context_s *ctx, va_state_t state,
                             void *user)
{
  struct va_app_s *app = (struct va_app_s *)user;
  (void)ctx;

  if (app == NULL)
    {
      return;
    }
  app->lcd_state = state;
  app->lcd_dirty = true;
}

/* 5.1: forward the action name to the ESP32.  SAFE_STOP's entry uses
 * action_stop (below), not this. */
static void va_hook_action_start(struct va_context_s *ctx, const char *action,
                                 void *user)
{
  struct va_app_s *app = (struct va_app_s *)user;
  (void)ctx;

  if (app == NULL || action == NULL)
    {
      return;
    }
  va_send_perform_action(app, action);
}

/* action_stop is invoked on exit and, importantly, on SAFE_STOP entry.  Issue
 * an emergency robot.stop when we are entering SAFE_STOP so the ESP32 brakes;
 * for ordinary state exits a stop is implied by the next action_start, so we
 * keep this minimal and only emit the emergency stop (需求 2.2). */
static void va_hook_action_stop(struct va_context_s *ctx, void *user)
{
  struct va_app_s *app = (struct va_app_s *)user;

  if (app == NULL || ctx == NULL)
    {
      return;
    }
  if (ctx->state == VA_SAFE_STOP)
    {
      va_send_robot_stop(app, true);
    }
}

static void va_hook_audio_up_start(struct va_context_s *ctx, void *user)
{
  (void)ctx;
  va_capture_start((struct va_app_s *)user);
}

static void va_hook_audio_up_stop(struct va_context_s *ctx, void *user)
{
  (void)ctx;
  va_capture_stop((struct va_app_s *)user);
}

static void va_hook_audio_down_start(struct va_context_s *ctx, void *user)
{
  struct va_app_s *app = (struct va_app_s *)user;
  (void)ctx;

  if (app == NULL || ctx == NULL)
    {
      return;
    }

  /* Record the (stub) TTS request for observability. */
  (void)tts_request(ctx->pending_answer, ctx);

  if (va_tts_stub_active())
    {
      /* SPEAKING in all-stub mode: there is no cloud TTS PCM to stream, so
       * make the "robot is speaking" step audible with a short local
       * placeholder tone.  The blocking board_audio_play_tone() is
       * self-contained (owns DMA + amplifier), so do NOT also open the
       * streaming playback path here - that would fight over the same DAC.
       * The main-loop SPEAKING branch plays the tone and then posts
       * SPEAK_DONE (需求 9.2). */
      app->speak_tone_pending = true;
    }
  else
    {
      /* Real cloud TTS (task 13): arm the AUDIO_DOWN streaming path so the
       * ESP32-relayed PCM frames play back as they arrive, then ask the ESP32
       * to synthesise.  The playback path must be open first: the first PCM
       * frame can arrive before the command ACK. */
      va_play_start(app);
      va_send_speak_request(app, ctx->pending_answer);
    }
}

static void va_hook_audio_down_stop(struct va_context_s *ctx, void *user)
{
  (void)ctx;
  va_play_stop((struct va_app_s *)user);
}

static void va_install_hooks(va_hooks_t *hooks, struct va_app_s *app)
{
  memset(hooks, 0, sizeof(*hooks));
  hooks->lcd_show         = va_hook_lcd_show;
  hooks->action_start     = va_hook_action_start;
  hooks->action_stop      = va_hook_action_stop;
  hooks->audio_up_start   = va_hook_audio_up_start;
  hooks->audio_up_stop    = va_hook_audio_up_stop;
  hooks->audio_down_start = va_hook_audio_down_start;
  hooks->audio_down_stop  = va_hook_audio_down_stop;
  hooks->user             = app;
}

/* ------------------------------------------------------------------------- */
/* 9.2 LCD bitmap-font rendering per Main_State (需求 4.2-4.5, 7.5).          */
/*                                                                            */
/* The face itself is geometry from va_face.c; this file only turns it into    */
/* pixels.  Called only from the main loop (never the parser path) so it can   */
/* never delay a transition.                                                  */
/*                                                                            */
/* The 5x7 bitmap font and the old "solid colour + state name" screen are kept */
/* behind VA_LCD_SHOW_STATE_NAME as a bring-up aid: printing the state on the  */
/* panel is useful when no console is attached, but it is clutter on a product */
/* face, so it is off by default.                                             */
/* ------------------------------------------------------------------------- */

/* Per-frame render cost on the console.  Off by default; turn it on when the
 * animation looks wrong, because almost every way this can look wrong on the
 * panel shows up first as an unexpected frame time. */
#ifndef VA_LCD_PROFILE
#  define VA_LCD_PROFILE 0
#endif

#ifndef VA_LCD_SHOW_STATE_NAME
#  define VA_LCD_SHOW_STATE_NAME 0
#endif

static void va_lcd_close(struct va_app_s *app)
{
  if (app->fb.mapped && app->fb.mem != NULL && app->fb.mem != MAP_FAILED)
    {
      munmap(app->fb.mem, app->fb.len);
    }
  if (app->fb.fd >= 0)
    {
      close(app->fb.fd);
    }
  memset(&app->fb, 0, sizeof(app->fb));
  app->fb.fd = -1;
  app->fb.mem = NULL;
  app->fb.mapped = false;
}

static int va_lcd_open(struct va_app_s *app)
{
  int ret;

  if (app->fb.fd >= 0)
    {
      return 0;
    }
  app->fb.fd = open(VA_FB_DEVICE, O_RDWR);
  if (app->fb.fd < 0)
    {
      return -errno;
    }
  ret = ioctl(app->fb.fd, FBIOGET_VIDEOINFO,
              (unsigned long)(uintptr_t)&app->fb.video);
  if (ret < 0)
    {
      va_lcd_close(app);
      return -errno;
    }
  ret = ioctl(app->fb.fd, FBIOGET_PLANEINFO,
              (unsigned long)(uintptr_t)&app->fb.plane);
  if (ret < 0 || app->fb.plane.fblen == 0)
    {
      va_lcd_close(app);
      return ret < 0 ? -errno : -EINVAL;
    }
  app->fb.len = app->fb.plane.fblen;
  app->fb.mem = mmap(NULL, app->fb.len, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FILE, app->fb.fd, 0);
  if (app->fb.mem == MAP_FAILED)
    {
      app->fb.mem = app->fb.plane.fbmem;
    }
  else
    {
      app->fb.mapped = true;
    }
  return app->fb.mem != NULL ? 0 : -ENOMEM;
}

static uint32_t va_rgb565_to_argb8888(uint16_t color)
{
  uint32_t red = (uint32_t)((color >> 11) & 0x1f);
  uint32_t green = (uint32_t)((color >> 5) & 0x3f);
  uint32_t blue = (uint32_t)(color & 0x1f);

  red = (red << 3) | (red >> 2);
  green = (green << 2) | (green >> 4);
  blue = (blue << 3) | (blue >> 2);
  return 0xff000000U | (red << 16) | (green << 8) | blue;
}

#if VA_LCD_SHOW_STATE_NAME
static void va_lcd_putpixel(struct va_app_s *app, uint16_t x, uint16_t y,
                            uint16_t color)
{
  if (x >= app->fb.video.xres || y >= app->fb.video.yres)
    {
      return;
    }
  if (app->fb.plane.bpp == 16)
    {
      uint16_t *row = (uint16_t *)(app->fb.mem + y * app->fb.plane.stride);
      row[x] = color;
    }
  else if (app->fb.plane.bpp == 32)
    {
      uint32_t *row = (uint32_t *)(app->fb.mem + y * app->fb.plane.stride);
      row[x] = va_rgb565_to_argb8888(color);
    }
}
#endif

/* Horizontal span fill.  The inner loop of every face primitive, so it writes
 * whole rows rather than going through va_lcd_putpixel() per pixel. */
static void va_lcd_hline(struct va_app_s *app, int x0, int x1, int y,
                         uint16_t color)
{
  if (y < 0 || y >= (int)app->fb.video.yres)
    {
      return;
    }
  if (x0 < 0)
    {
      x0 = 0;
    }
  if (x1 > (int)app->fb.video.xres - 1)
    {
      x1 = (int)app->fb.video.xres - 1;
    }
  if (x0 > x1)
    {
      return;
    }

  if (app->fb.plane.bpp == 16)
    {
      uint16_t *row = (uint16_t *)(app->fb.mem + y * app->fb.plane.stride);
      int x;

      for (x = x0; x <= x1; x++)
        {
          row[x] = color;
        }
    }
  else if (app->fb.plane.bpp == 32)
    {
      uint32_t *row = (uint32_t *)(app->fb.mem + y * app->fb.plane.stride);
      uint32_t argb = va_rgb565_to_argb8888(color);
      int x;

      for (x = x0; x <= x1; x++)
        {
          row[x] = argb;
        }
    }
}

static void va_lcd_fill_rect(struct va_app_s *app, int x, int y, int w, int h,
                             uint16_t color)
{
  int row;

  for (row = y; row < y + h; row++)
    {
      va_lcd_hline(app, x, x + w - 1, row, color);
    }
}

/* Integer square root, so the corner arcs need no libm and no float. */
static uint32_t va_isqrt(uint32_t value)
{
  uint32_t rem = 0;
  uint32_t root = 0;
  int i;

  for (i = 0; i < 16; i++)
    {
      root <<= 1;
      rem = (rem << 2) | (value >> 30);
      value <<= 2;
      if (root < rem)
        {
          rem -= root | 1;
          root += 2;
        }
    }
  return root >> 1;
}

/* Rounded rectangle centred on (cx, cy): the straight band at full width, plus
 * four quarter-circle corners.  This is the only shape the face needs. */
static void va_lcd_fill_round_rect(struct va_app_s *app, int cx, int cy,
                                   int w, int h, int r, uint16_t color)
{
  const int y0 = cy - h / 2;
  int y;

  if (w <= 0 || h <= 0)
    {
      return;
    }
  if (r > w / 2)
    {
      r = w / 2;
    }
  if (r > h / 2)
    {
      r = h / 2;
    }
  if (r < 0)
    {
      r = 0;
    }

  for (y = y0; y < y0 + h; y++)
    {
      int half;

      if (y >= y0 + r && y < y0 + h - r)
        {
          half = w / 2;
        }
      else
        {
          const int dy = (y < y0 + r) ? (y0 + r) - y : y - (y0 + h - 1 - r);
          const int32_t inside = (int32_t)r * r - (int32_t)dy * dy;

          if (inside <= 0)
            {
              continue;
            }
          half = w / 2 - r + (int)va_isqrt((uint32_t)inside);
        }
      va_lcd_hline(app, cx - half, cx + half, y, color);
    }
}

#if VA_LCD_SHOW_STATE_NAME
static bool va_lcd_glyph(char c, uint8_t glyph[5])
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

static void va_lcd_draw_text(struct va_app_s *app, const char *text)
{
  const uint16_t scale = 4;
  const uint16_t glyph_width = 5 * scale;
  const uint16_t spacing = scale;
  const uint16_t text_width = (uint16_t)(strlen(text) *
                                         (glyph_width + spacing));
  const uint16_t x0 = text_width < app->fb.video.xres
                        ? (app->fb.video.xres - text_width) / 2 : 0;
  const uint16_t y0 = app->fb.video.yres > 7 * scale
                        ? (app->fb.video.yres - 7 * scale) / 2 : 0;
  size_t i;

  for (i = 0; i < strlen(text); i++)
    {
      uint8_t glyph[5];
      uint16_t col;
      uint16_t row;

      if (!va_lcd_glyph(text[i], glyph))
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
                      va_lcd_putpixel(app,
                          (uint16_t)(x0 + i * (glyph_width + spacing) +
                                     col * scale + sx),
                          (uint16_t)(y0 + row * scale + sy), 0xffff);
                    }
                }
            }
        }
    }
}
#endif /* VA_LCD_SHOW_STATE_NAME */

static void va_lcd_render(struct va_app_s *app)
{
  uint16_t color;
  bool full_repaint;
  int band_y;
  int band_h;
#if VA_LCD_PROFILE
  uint32_t t_enter = va_now_ms();
#endif

  if (va_lcd_open(app) < 0)
    {
      return;
    }

  full_repaint = !app->lcd_cleared;

  /* Rows to repaint.  The short eye band is enough unless the speaking bar is
   * on screen now or was on screen last frame -- the latter is what erases the
   * bar on the way out of SPEAKING. */
  if (app->face.bar.h > 0 || app->lcd_bar_shown)
    {
      band_y = VA_FACE_REGION_Y;
      band_h = VA_FACE_REGION_H;
    }
  else
    {
      band_y = VA_FACE_EYE_BAND_Y;
      band_h = VA_FACE_EYE_BAND_H;
    }

  /* Final display follows Main_State (需求 4.4): geometry and colour come from
   * the core's current state via the latched lcd_state.
   *
   * The panel is AMOLED, so the background stays black: unlit pixels draw no
   * current and cannot burn in.  The background is therefore painted once, on
   * the first render after open, and every later frame only rewrites the band of
   * rows the face can touch.
   *
   * The band spans the FULL panel width on purpose, even though the face only
   * occupies VA_FACE_REGION_X..+W.  lcdfb_updateearea() hands the driver the
   * sub-rectangle together with the full framebuffer stride, and
   * sf32lb_lcd_putarea() only uses its chunked 24-row DMA path when
   * row_bytes == stride.  A narrower rectangle falls into the row-at-a-time
   * path: one SetRegion + one DMA + one blocking sem_timedwait per row, i.e.
   * 228 synchronous transfers instead of 10.  That made a frame cost more than
   * the 160 ms blink, so every blink was skipped entirely.  Full-width rows cost
   * 33% more pixel writes into PSRAM and roughly an order of magnitude less
   * transfer overhead. */
  if (!app->lcd_cleared)
    {
      va_lcd_fill_rect(app, 0, 0, (int)app->fb.video.xres,
                       (int)app->fb.video.yres, 0x0000);
      app->lcd_cleared = true;
    }
  else
    {
      va_lcd_fill_rect(app, 0, band_y, (int)app->fb.video.xres, band_h,
                       0x0000);
    }

  color = app->face.color;

  va_lcd_fill_round_rect(app, VA_FACE_CX - VA_FACE_EYE_DX + app->face.left.dx,
                         VA_FACE_CY + app->face.left.dy,
                         app->face.left.w, app->face.left.h,
                         app->face.left.r, color);
  va_lcd_fill_round_rect(app, VA_FACE_CX + VA_FACE_EYE_DX + app->face.right.dx,
                         VA_FACE_CY + app->face.right.dy,
                         app->face.right.w, app->face.right.h,
                         app->face.right.r, color);
  if (app->face.bar.h > 0)
    {
      va_lcd_fill_round_rect(app, VA_FACE_CX + app->face.bar.dx,
                             VA_FACE_BAR_CY + app->face.bar.dy,
                             app->face.bar.w, app->face.bar.h,
                             app->face.bar.r, color);
    }

#if VA_LCD_SHOW_STATE_NAME
  va_lcd_draw_text(app, va_state_name(app->lcd_state));
#endif

#ifdef CONFIG_FB_UPDATE
  {
    /* Flush exactly the rows that were written, at full width so the driver
     * keeps its chunked DMA path (see the clear above). */
    struct fb_area_s area;

    area.x = 0;
    area.w = app->fb.video.xres;
    if (full_repaint)
      {
        area.y = 0;
        area.h = app->fb.video.yres;
      }
    else
      {
        area.y = band_y;
        area.h = band_h;
      }
    ioctl(app->fb.fd, FBIO_UPDATE, (unsigned long)(uintptr_t)&area);
  }
#endif

  app->lcd_bar_shown = app->face.bar.h > 0;
}

/* ------------------------------------------------------------------------- */
/* Face demo: walk every state's expression on the panel.                     */
/*                                                                            */
/* A bring-up aid, not a product path.  It drives the real renderer and the    */
/* real animation schedule rather than a second copy of them, so what you see  */
/* here is exactly what the state machine will put on screen.                  */
/* ------------------------------------------------------------------------- */

static void va_face_demo(struct va_app_s *app, uint32_t hold_ms)
{
  static const va_state_t order[] =
  {
    VA_IDLE, VA_LISTENING, VA_THINKING, VA_ACTING, VA_SPEAKING, VA_SAFE_STOP
  };
  size_t i;

  printf("va: face demo, %lums per state\n", (unsigned long)hold_ms);

  for (i = 0; i < sizeof(order) / sizeof(order[0]); i++)
    {
      uint32_t started = va_now_ms();

      app->lcd_state = order[i];
      app->lcd_dirty = true;
      printf("va: face demo -> %s\n", va_state_name(order[i]));

      for (; ; )
        {
          uint32_t now = va_now_ms();
          uint32_t elapsed = now - started;
          va_face_t next;

          if (elapsed >= hold_ms)
            {
              break;
            }

          /* Sweep the speaking level so the bar is seen moving rather than
           * parked at its floor; every other state ignores this. */
          app->speak_level = (uint8_t)((elapsed / 4) & 0xff);

          va_face_update(&app->face_anim, app->lcd_state, now,
                         app->speak_level, &next);
          if (app->lcd_dirty || !va_face_equal(&next, &app->face))
            {
              app->lcd_dirty = false;
              app->face = next;
              va_lcd_render(app);
            }
          usleep(VA_MAIN_TICK_US);
        }
    }

  app->speak_level = 0;
  printf("va: face demo done\n");

#if VA_LCD_PROFILE
  /* Frame cost matters for more than smoothness: a frame longer than
   * VA_FACE_BLINK_TOTAL_MS makes va_face_update() retire a blink before any
   * frame ever samples it closed, so the face silently stops blinking. */
  {
    static uint32_t s_render_count;
    uint32_t spent = va_now_ms() - t_enter;

    s_render_count++;
    if (s_render_count <= 12 || (s_render_count % 64) == 0)
      {
        printf("va: lcd render #%lu %lums eye_h=%d full=%d\n",
               (unsigned long)s_render_count, (unsigned long)spent,
               (int)app->face.left.h, (int)full_repaint);
      }
  }
#endif
}

/* ------------------------------------------------------------------------- */
/* 9.1 frame handling: feed decoded frames into the pure-logic core.          */
/* ------------------------------------------------------------------------- */

/* EVENT decode (需求 2.1, 5.6, 4.4).  Safety events are handed to the core via
 * va_on_frame, which latches safety_pending; va_tick performs the unconditional
 * SAFE_STOP.  An action_update "completed"/"done" phase becomes ACTION_DONE. */
static void va_on_event_frame(struct va_app_s *app, const uint8_t *payload,
                              uint16_t len)
{
  cJSON *root;
  const cJSON *event;
  const cJSON *phase;
  const cJSON *data;

  /* First let the core sniff physical Safety_Event markers directly off the
   * raw body (急停/边缘/堵转过流/低压).  This latches safety_pending without a
   * JSON parse, so a malformed safety event still preempts (需求 2.1, 2.7). */
  va_on_frame(&app->ctx, MIBOT_TYPE_EVENT, payload, len);

  root = cJSON_Parse((const char *)payload);
  if (root == NULL)
    {
      return;
    }

  event = cJSON_GetObjectItemCaseSensitive(root, "event");
  phase = cJSON_GetObjectItemCaseSensitive(root, "phase");
  data = cJSON_GetObjectItemCaseSensitive(root, "data");
  if (!cJSON_IsString(phase) && cJSON_IsObject(data))
    {
      phase = cJSON_GetObjectItemCaseSensitive(data, "phase");
    }

  /* Network-ready / link-up event flips Cloud_Available true so the next WAKE
   * can enter LISTENING (需求 3.1, 3.5). */
  if (cJSON_IsString(event) &&
      (strstr(event->valuestring, "network") != NULL ||
       strstr(event->valuestring, "cloud") != NULL))
    {
      const cJSON *ready = cJSON_GetObjectItemCaseSensitive(root, "ready");
      if (cJSON_IsBool(ready))
        {
          app->ctx.cloud_available = cJSON_IsTrue(ready);
          printf("va: cloud_available=%d (network event)\n",
                 (int)app->ctx.cloud_available);
        }
    }

  /* action_update completion drives the ACTING -> SPEAKING/IDLE decision in
   * the core (需求 5.6).  Only meaningful while in ACTING. */
  if (cJSON_IsString(event) &&
      strcmp(event->valuestring, "action_update") == 0 &&
      cJSON_IsString(phase) &&
      (strcmp(phase->valuestring, "completed") == 0 ||
       strcmp(phase->valuestring, "done") == 0))
    {
      if (app->ctx.state == VA_ACTING)
        {
          app->acting_deadline_ms = 0;
          va_handle_event(&app->ctx, VA_EVENT_ACTION_DONE);
          /* Same as the ACTING-timeout path: ACTION_DONE can land in SPEAKING
           * when the reply had a sentence as well as an action, and SPEAKING
           * needs its own deadline (需求 1.10). */
          if (app->ctx.state == VA_SPEAKING)
            {
              app->speaking_deadline_ms = va_now_ms() + VA_SPEAKING_TIMEOUT_MS;
            }
        }
    }

  cJSON_Delete(root);
}

/* AI_RESPONSE decode (需求 1.5, 1.6, 1.7, 5.1).  tool_calls -> stage action and
 * request ACTING; text-only -> stage answer and TEXT_ONLY; error/!ok ->
 * CLOUD_FAIL.  Only acted upon while in THINKING. */
/* AI_RESPONSE (0x41) carries two logically different streams: unsolicited
 * `mibot.asr.v1` transcripts relayed from the cloud recogniser, and synchronous
 * DeepSeek gateway replies.  An ASR payload must never complete a pending
 * DeepSeek turn, so it is consumed here first.  Returns true when the payload
 * was an ASR transcript. */
static bool va_on_asr_payload(struct va_app_s *app, const uint8_t *payload)
{
  cJSON *root;
  const cJSON *schema;
  const cJSON *event;
  const cJSON *final;
  const cJSON *text;
  bool is_asr;

  if (payload == NULL || payload[0] != '{')
    {
      return false;
    }

  root = cJSON_Parse((const char *)payload);
  if (root == NULL)
    {
      return false;
    }

  schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
  event = cJSON_GetObjectItemCaseSensitive(root, "event");
  is_asr = cJSON_IsString(schema) && schema->valuestring != NULL &&
           strcmp(schema->valuestring, "mibot.asr.v1") == 0 &&
           cJSON_IsString(event) && event->valuestring != NULL &&
           strcmp(event->valuestring, "asr_text") == 0;
  if (!is_asr)
    {
      cJSON_Delete(root);
      return false;
    }

  final = cJSON_GetObjectItemCaseSensitive(root, "final");
  text = cJSON_GetObjectItemCaseSensitive(root, "text");

  if (!cJSON_IsTrue(final))
    {
      /* Partial transcripts are informational only. */
      printf("va: ASR partial ignored\n");
      cJSON_Delete(root);
      return true;
    }

  if (!cJSON_IsString(text) || text->valuestring[0] == '\0')
    {
      printf("va: ASR final without text\n");
      cJSON_Delete(root);
      return true;
    }

  if (!app->asr_waiting)
    {
      printf("va: ASR text arrived outside the listening window, ignored\n");
      cJSON_Delete(root);
      return true;
    }

  strncpy(app->transcript, text->valuestring, sizeof(app->transcript) - 1);
  app->transcript[sizeof(app->transcript) - 1] = '\0';
  app->asr_text_ready = true;
  printf("va: ASR final text received (%u bytes)\n",
         (unsigned)strlen(app->transcript));
  cJSON_Delete(root);
  return true;
}

static void va_on_ai_response(struct va_app_s *app, const uint8_t *payload,
                              uint16_t len)
{
  cJSON *root;
  const cJSON *ok;
  const cJSON *tool_calls;
  const cJSON *text;
  const cJSON *request_id;
  bool handled = false;
  (void)len;

  /* ASR transcripts share this frame type and must be handled first. */
  if (va_on_asr_payload(app, payload))
    {
      return;
    }

  if (app->ctx.state != VA_THINKING)
    {
      return;
    }

  root = cJSON_Parse((const char *)payload);
  if (root == NULL)
    {
      printf("va: AI_RESPONSE invalid JSON, CLOUD_FAIL\n");
      va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
      return;
    }

  request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
  if (!cJSON_IsString(request_id) || request_id->valuestring == NULL ||
      strcmp(request_id->valuestring, app->ai_request_id) != 0)
    {
      printf("va: AI_RESPONSE ignored id=\"%s\" expected=\"%s\"\n",
             cJSON_IsString(request_id) && request_id->valuestring != NULL
               ? request_id->valuestring : "<missing>",
             app->ai_request_id);
      cJSON_Delete(root);
      return;
    }

  app->thinking_deadline_ms = 0;

  ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
  if (cJSON_IsBool(ok) && !cJSON_IsTrue(ok))
    {
      va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);   /* 需求 1.7, 3.4 */
      cJSON_Delete(root);
      return;
    }

  /* Stage the spoken sentence BEFORE dispatching on tool_calls.
   *
   * The core's ACTING + ACTION_DONE rule is "SPEAKING if pending_answer is set,
   * else IDLE" (需求 1.8, 1.9), so a reply that carries both an emotion action
   * and a sentence has to arrive in ACTING with the sentence already staged.
   * This used to read `text` only in the !handled branch, so any reply with a
   * tool call silently dropped its text: the robot performed the action and
   * then went quiet, with no error anywhere.  Observed with MiMo, which likes
   * to answer with an emotion action plus a sentence.
   *
   * THINKING entry clears pending_answer, so staging this early cannot leak an
   * answer from the previous turn. */
  text = cJSON_GetObjectItemCaseSensitive(root, "text");
  if (cJSON_IsString(text) && text->valuestring[0] != '\0')
    {
      strncpy(app->ctx.pending_answer, text->valuestring,
              VA_PENDING_ANSWER_SIZE - 1);
      app->ctx.pending_answer[VA_PENDING_ANSWER_SIZE - 1] = '\0';
    }

  tool_calls = cJSON_GetObjectItemCaseSensitive(root, "tool_calls");
  if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0)
    {
      char action[VA_PENDING_ACTION_SIZE];
      const cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);

      if (va_tool_call_action(tc, action, sizeof(action)))
        {
          /* va_request_acting enforces the 6-emotion whitelist and drives
           * THINKING -> ACTING.  A rejected action returns false and does NOT
           * enter ACTING (需求 8.6, 5.3, P8, P12); we fall through to a tool
           * error and end the turn back to IDLE. */
          if (va_request_acting(&app->ctx, action))
            {
              app->acting_deadline_ms = va_now_ms() + VA_ACTING_TIMEOUT_MS;
              handled = true;
            }
          else
            {
              printf("va: tool_call action \"%s\" rejected (not whitelisted)\n",
                     action);
              va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
              handled = true;
            }
        }
    }

  if (!handled)
    {
      /* Text-only answer -> go SPEAKING now (需求 1.6).  The text itself was
       * already staged above. */
      if (app->ctx.pending_answer[0] != '\0')
        {
          if (va_handle_event(&app->ctx, VA_EVENT_TEXT_ONLY))
            {
              app->speaking_deadline_ms = va_now_ms() + VA_SPEAKING_TIMEOUT_MS;
            }
        }
      else
        {
          /* No tool call and no usable text: treat as a failed turn. */
          va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
        }
    }

  cJSON_Delete(root);
}

/* AUDIO_DOWN decode (需求 1.10, 7.6): PCM frames go to the speaker; an eos
 * marker (empty frame or {"eos":true}) posts SPEAK_DONE (SPEAKING -> IDLE). */
static void va_on_audio_down(struct va_app_s *app, const uint8_t *payload,
                             uint16_t len)
{
  if (app->ctx.state != VA_SPEAKING)
    {
      return;
    }

  if (len == MIBOT_AUDIO_FRAME_BYTES)
    {
      /* Counted before the play_active gate so "the ESP32 sent N but we only
       * saw M" can be told apart from "we saw them all but had nowhere to put
       * them".  Compare against the ESP32's audio_down_frames_sent: a shortfall
       * there means frames are lost on the UART or in the parser. */
      app->downlink_frames_seen++;
      if (app->play_active)
        {
          board_audio_play_write(payload, len);
        }
      else
        {
          app->downlink_frames_late++;
        }
      return;
    }

  if (len == 0)
    {
      /* Legacy empty end-of-stream marker. */
      app->speaking_deadline_ms = 0;
      va_handle_event(&app->ctx, VA_EVENT_SPEAK_DONE);   /* 需求 1.10 */
      return;
    }

  if (payload != NULL && payload[0] == '{')
    {
      cJSON *meta = cJSON_Parse((const char *)payload);
      const cJSON *eos = (meta != NULL)
                           ? cJSON_GetObjectItemCaseSensitive(meta, "eos")
                           : NULL;
      bool is_eos = cJSON_IsTrue(eos);
      cJSON_Delete(meta);

      if (is_eos)
        {
          app->speaking_deadline_ms = 0;
          va_handle_event(&app->ctx, VA_EVENT_SPEAK_DONE);   /* 需求 1.10 */
        }
    }
}

static void va_handle_frame(struct va_app_s *app, uint8_t type,
                            const uint8_t *payload, uint16_t len)
{
  switch (type)
    {
      case MIBOT_TYPE_HELLO_ACK:
        /* Link up: cloud path is reachable through the ESP32 gateway
         * (需求 3.1, 3.5).  Default was false until now. */
        app->hello_acked = true;
        app->ctx.cloud_available = true;
        printf("va: HELLO_ACK, cloud_available=1\n");
        break;

      case MIBOT_TYPE_ACK:
        /* Link-health only: the core drives its own transitions.  ACTING
         * completion is authoritative via the action_update EVENT. */
        break;

      case MIBOT_TYPE_NACK:
        /* A NACK is the ESP32 rejecting a COMMAND we sent (e.g. a
         * robot.perform_action the safety layer declined).  It is NOT the
         * DeepSeek result: a failed AI_REQUEST comes back as an AI_RESPONSE
         * with an error, handled in va_on_ai_response.  So a NACK must NOT end
         * the THINKING turn - doing so would abort a perfectly good DeepSeek
         * round just because a state-entry action command was refused.  Log
         * it and let the AI_RESPONSE (or the THINKING timeout) drive the turn.
         *
         * The payload carries which command was refused and why (error code
         * such as E_TOF_INVALID / E_SAFETY_LOCK / E_INVALID_ARG).  It used to be
         * discarded, which left "NACK received" as the only trace -- useless for
         * telling "the chassis is safety-locked" apart from "the ESP32 does not
         * know that action name". */
        {
          char reason[VA_NACK_LOG_MAX];
          uint16_t n = len < (uint16_t)(sizeof(reason) - 1)
                         ? len : (uint16_t)(sizeof(reason) - 1);

          if (payload != NULL && n > 0)
            {
              memcpy(reason, payload, n);
              reason[n] = '\0';
              printf("va: NACK received (not aborting turn): %s\n", reason);
            }
          else
            {
              printf("va: NACK received (no payload; not aborting turn)\n");
            }
        }

        /* In ACTING a NACK is not just noise: the action we are waiting on has
         * failed, so the `action_update phase=completed` EVENT will never
         * arrive and the only thing left is VA_ACTING_TIMEOUT_MS of dead air.
         * Treat it as the completion signal instead (需求 5.6 keeps the timeout
         * as the fallback for a NACK that is itself lost).
         *
         * This is routine with no ToF fitted: the ESP32 accepts the action's
         * servo-only first step and then refuses the step that would drive the
         * wheels (E_TOF_INVALID), which used to cost 8 s on every single turn.
         *
         * THINKING is deliberately excluded -- there a NACK is a refused
         * state-entry action and must not abort a perfectly good cloud round. */
        if (app->ctx.state == VA_ACTING)
          {
            app->acting_deadline_ms = 0;
            va_handle_event(&app->ctx, VA_EVENT_ACTION_DONE);
            if (app->ctx.state == VA_SPEAKING)
              {
                app->speaking_deadline_ms =
                    va_now_ms() + VA_SPEAKING_TIMEOUT_MS;
              }
          }
        break;

      case MIBOT_TYPE_EVENT:
        va_on_event_frame(app, payload, len);
        break;

      case MIBOT_TYPE_TELEMETRY:
        /* The ESP32 telemetry carries the audio-link counters (frames received,
         * CRC/size/meta errors, uplink drops).  Printing it every period floods
         * the console, so it is only surfaced when explicitly enabled for an
         * integration session.  Temporarily forced on to chase the playback
         * dropouts: it is the only view of the ESP32-side downlink counters, and
         * without them "underrun on the SF32" cannot be told apart from "the
         * ESP32 never sent those frames".  Needs MIBOT_AUDIO_DIAG_TRACE=1 on the
         * ESP32 for the detailed fields.
         *
         * Off again now that the ESP32 logs "AUDIO_DOWN eos ... frames_sent"
         * once per stream, which carries the same information without printing
         * from inside the frame-parser path while the downlink is streaming. */
#ifndef VA_LOG_TELEMETRY
#  define VA_LOG_TELEMETRY 0
#endif
#if VA_LOG_TELEMETRY
        if (payload != NULL)
          {
            /* Only the downlink fields, and only when they changed.  The full
             * object is ~1 kB every second, which scrolls the lines that matter
             * off the console. */
            static uint32_t last_sent = 0xffffffffu;
            const char *sent = strstr((const char *)payload,
                                      "\"audio_down_frames_sent\":");
            const char *drop = strstr((const char *)payload,
                                      "\"downlink_dropped\":");
            const char *und  = strstr((const char *)payload,
                                      "\"downlink_underrun\":");
            const char *fift = strstr((const char *)payload,
                                      "\"audio_down_50_frames_ms\":");

            if (sent != NULL)
              {
                const uint32_t v =
                    (uint32_t)strtoul(sent + sizeof("\"audio_down_frames_sent\":") - 1,
                                      NULL, 10);

                if (v != last_sent)
                  {
                    last_sent = v;
                    printf("va: esp32 down sent=%lu dropped=%lu underrun=%lu "
                           "50f=%lums\n",
                           (unsigned long)v,
                           drop != NULL ? strtoul(drop + sizeof("\"downlink_dropped\":") - 1, NULL, 10) : 0,
                           und != NULL ? strtoul(und + sizeof("\"downlink_underrun\":") - 1, NULL, 10) : 0,
                           fift != NULL ? strtoul(fift + sizeof("\"audio_down_50_frames_ms\":") - 1, NULL, 10) : 0);
                  }
              }
          }
#endif
        break;

      case MIBOT_TYPE_AI_RESPONSE:
        printf("va: AI_RESPONSE received bytes=%u\n", (unsigned)len);
        va_on_ai_response(app, payload, len);
        break;

      case MIBOT_TYPE_AUDIO_DOWN:
        va_on_audio_down(app, payload, len);
        break;

      default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* AA55/CRC byte state machine - copied verbatim-style from mibot_agent_main. */
/* ------------------------------------------------------------------------- */

static void va_parser_reset(struct va_parser_s *parser)
{
  memset(parser, 0, sizeof(*parser));
}

static void va_parser_feed(struct va_app_s *app, const uint8_t *data,
                           size_t len)
{
  struct va_parser_s *parser = &app->parser;
  size_t i;

  for (i = 0; i < len; i++)
    {
      uint8_t byte = data[i];
      switch (parser->stage)
        {
          case 0:
            g_rx_resync++;
            parser->stage = byte == 0xaa ? 1 : 0;
            break;
          case 1:
            if (byte == 0x55)
              {
                parser->stage = 2;
                parser->header_pos = 0;
                /* The 0xaa counted as resync above opened a real frame. */
                g_rx_resync--;
              }
            else
              {
                g_rx_resync++;
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
                    g_rx_bad_header++;
                    va_parser_reset(parser);
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
                uint16_t received = (uint16_t)parser->crc[0] |
                                    ((uint16_t)parser->crc[1] << 8);
                uint16_t expected;
                uint16_t crc = 0xffff;
                size_t j;

                for (j = 0; j < MIBOT_HEADER_SIZE; j++)
                  {
                    int bit;
                    crc ^= (uint16_t)parser->header[j] << 8;
                    for (bit = 0; bit < 8; bit++)
                      {
                        crc = (crc & 0x8000) != 0
                            ? (uint16_t)((crc << 1) ^ 0x1021)
                            : (uint16_t)(crc << 1);
                      }
                  }
                for (j = 0; j < parser->payload_len; j++)
                  {
                    int bit;
                    crc ^= (uint16_t)parser->payload[j] << 8;
                    for (bit = 0; bit < 8; bit++)
                      {
                        crc = (crc & 0x8000) != 0
                            ? (uint16_t)((crc << 1) ^ 0x1021)
                            : (uint16_t)(crc << 1);
                      }
                  }
                expected = crc;

                if (expected == received)
                  {
                    /* Control payloads are JSON and cJSON_Parse() requires a
                     * C string.  The extra parser byte leaves binary frame
                     * contents untouched while making all JSON parsing
                     * deterministic. */
                    parser->payload[parser->payload_len] = '\0';
                    g_rx_frames_ok++;
                    va_handle_frame(app, parser->header[1], parser->payload,
                                    parser->payload_len);
                  }
                else if (parser->header[1] == MIBOT_TYPE_AI_RESPONSE)
                  {
                    g_rx_crc_fail++;
                    printf("va: AI_RESPONSE CRC mismatch len=%u\n",
                           (unsigned)parser->payload_len);
                  }
                else
                  {
                    g_rx_crc_fail++;
                  }
                va_parser_reset(parser);
              }
            break;
          default:
            va_parser_reset(parser);
            break;
        }
    }
}

/* Drain all currently buffered UART data in one pass (mirrors
 * mibot_uart_drain_rx: audio downlink can exceed a single 256-byte read). */
static void va_uart_drain_rx(struct va_app_s *app)
{
  uint8_t buffer[VA_UART_RX_BUFFER];
  size_t drained = 0;

  if (app->uart_fd < 0)
    {
      return;
    }

  while (drained < VA_UART_DRAIN_MAX)
    {
      ssize_t n = read(app->uart_fd, buffer, sizeof(buffer));
      if (n > 0)
        {
          g_rx_bytes += (uint32_t)n;
          va_parser_feed(app, buffer, (size_t)n);
          drained += (size_t)n;
          continue;
        }
      if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
        {
          break;
        }
      if (n < 0 && errno == EINTR)
        {
          continue;
        }
      if (n < 0)
        {
          printf("va: UART read error %d\n", errno);
        }
      break;
    }
}

/* ------------------------------------------------------------------------- */
/* 9.4 THINKING: build + send the DeepSeek AI_REQUEST with tool definitions.  */
/*                                                                            */
/* Request/tool JSON shape reused from deepseek_smoke.c: a body.messages with */
/* the transcript as the user turn, plus a robot_perform_action tool whose    */
/* "action" enum is exactly the 6 dialog-driven emotions (需求 5.1, 8.5).     */
/* ------------------------------------------------------------------------- */

static int va_add_perform_action_tool(cJSON *body)
{
  cJSON *tools;
  cJSON *tool;
  cJSON *function;
  cJSON *parameters;
  cJSON *properties;
  cJSON *field;
  cJSON *values;
  cJSON *required;
  size_t i;
  static const char *const actions[VA_ACTING_ACTION_COUNT] =
  {
    "happy", "sad", "confused", "surprised", "cute", "greeting"
  };

  tools = cJSON_AddArrayToObject(body, "tools");
  if (tools == NULL)
    {
      return -ENOMEM;
    }

  tool = cJSON_CreateObject();
  function = (tool != NULL) ? cJSON_AddObjectToObject(tool, "function") : NULL;
  parameters = (function != NULL)
                 ? cJSON_AddObjectToObject(function, "parameters") : NULL;
  properties = (parameters != NULL)
                 ? cJSON_AddObjectToObject(parameters, "properties") : NULL;
  if (properties == NULL)
    {
      cJSON_Delete(tool);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(tool, "type", "function");
  cJSON_AddStringToObject(function, "name", "robot_perform_action");
  cJSON_AddStringToObject(function, "description",
                          "Run one predefined bounded robot emotion action.");
  cJSON_AddStringToObject(parameters, "type", "object");

  field = cJSON_AddObjectToObject(properties, "action");
  cJSON_AddStringToObject(field, "type", "string");
  values = cJSON_AddArrayToObject(field, "enum");
  for (i = 0; i < VA_ACTING_ACTION_COUNT; i++)
    {
      cJSON_AddItemToArray(values, cJSON_CreateString(actions[i]));
    }

  required = cJSON_AddArrayToObject(parameters, "required");
  cJSON_AddItemToArray(required, cJSON_CreateString("action"));

  cJSON_AddItemToArray(tools, tool);
  return 0;
}

static void va_thinking_send_ai_request(struct va_app_s *app)
{
  cJSON *root;
  cJSON *body;
  cJSON *messages;
  cJSON *message;
  char *serialized;
  size_t serialized_len;
  int ret;

  snprintf(app->ai_request_id, sizeof(app->ai_request_id), "va-%lu",
           (unsigned long)++app->ai_request_serial);
  root = cJSON_CreateObject();
  body = (root != NULL) ? cJSON_AddObjectToObject(root, "body") : NULL;
  messages = (body != NULL) ? cJSON_AddArrayToObject(body, "messages") : NULL;
  message = (messages != NULL) ? cJSON_CreateObject() : NULL;
  if (root == NULL || body == NULL || messages == NULL || message == NULL ||
      cJSON_AddStringToObject(root, "schema", "mibot.deepseek.smoke.v2") == NULL ||
      cJSON_AddStringToObject(root, "request_id", app->ai_request_id) == NULL ||
      /* No "model" here on purpose: the ESP32 gateway owns the cloud model
       * choice (MiMo by default, override with MIBOT_LLM_MODEL), so the SF32
       * never has to be rebuilt when the model changes. */
      cJSON_AddNumberToObject(body, "max_completion_tokens", 256) == NULL ||
      cJSON_AddStringToObject(message, "role", "user") == NULL ||
      cJSON_AddStringToObject(message, "content",
                              app->transcript[0] != '\0'
                                ? app->transcript : "你好") == NULL)
    {
      /* message is not owned by root until it is appended below. */
      if (message != NULL)
        {
          cJSON_Delete(message);
        }
      cJSON_Delete(root);
      printf("va: AI_REQUEST build failed id=%s, CLOUD_FAIL\n",
             app->ai_request_id);
      va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
      return;
    }

  /* This cJSON version exposes AddItemToArray() as void.  All allocations
   * above have succeeded, so appending transfers message ownership to root. */
  cJSON_AddItemToArray(messages, message);

  /* The tools API is deliberately NOT advertised (需求 5.1 is still satisfied --
   * see below).
   *
   * An OpenAI-compatible model that answers with tool_calls sets content to
   * null, so the reply carries an action and nothing to say; the robot performs
   * the action and then stands there in silence.  Observed on every MiMo turn
   * that chose an action.  Prompting does not fix it: null content is the
   * documented behaviour, not a mistake.
   *
   * Instead the ESP32 gateway's persona asks for an "[ACT] <name>" line inside
   * the ordinary text answer and converts it back into this exact tool_calls
   * shape, so one round always yields speech plus an optional action and
   * va_on_ai_response() below is unchanged.  va_add_perform_action_tool() is
   * kept for a model that handles tool calls and content together. */
  if (VA_ADVERTISE_ACTION_TOOL && va_add_perform_action_tool(body) < 0)
    {
      cJSON_Delete(root);
      printf("va: AI_REQUEST tool build failed id=%s, CLOUD_FAIL\n",
             app->ai_request_id);
      va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
      return;
    }

  serialized = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (serialized == NULL)
    {
      printf("va: AI_REQUEST serialize failed id=%s, CLOUD_FAIL\n",
             app->ai_request_id);
      va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
      return;
    }

  serialized_len = strlen(serialized);
  if (serialized_len > MIBOT_MAX_PAYLOAD)
    {
      printf("va: AI_REQUEST too large id=%s bytes=%lu max=%u, CLOUD_FAIL\n",
             app->ai_request_id, (unsigned long)serialized_len,
             (unsigned)MIBOT_MAX_PAYLOAD);
      cJSON_free(serialized);
      va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
      return;
    }

  ret = va_send_frame(app, MIBOT_TYPE_AI_REQUEST, MIBOT_FLAG_ACK_REQUEST,
                      serialized, (uint16_t)serialized_len);
  cJSON_free(serialized);
  if (ret < 0)
    {
      printf("va: AI_REQUEST UART send failed id=%s ret=%d, CLOUD_FAIL\n",
             app->ai_request_id, ret);
      va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
      return;
    }

  app->ai_request_sent = true;
  app->thinking_deadline_ms = va_now_ms() + VA_THINKING_TIMEOUT_MS;
  printf("va: AI_REQUEST sent id=%s bytes=%lu transcript=\"%s\"\n",
         app->ai_request_id, (unsigned long)serialized_len, app->transcript);
  va_rx_stats_print("ai-request");
}

/* ------------------------------------------------------------------------- */
/* 9.3 audio uplink pump: while LISTENING, forward captured PCM as AUDIO_UP.  */
/* Called from the main loop; a fixed capture window (no VAD in v1) ends the  */
/* uplink and posts ASR_DONE via the transcript stub (需求 1.4, 9.1).         */
/* ------------------------------------------------------------------------- */

static void va_pump_uplink(struct va_app_s *app)
{
  uint8_t frame[MIBOT_AUDIO_FRAME_BYTES];
  int ret;
  bool vad_enabled;

  if (!app->capture_active)
    {
      return;
    }

  /* ESP32 needs a mibot.audio.v1 metadata frame before binary PCM. */
  if (!app->audio_meta_sent)
    {
      if (va_send_audio_meta(app, false) < 0)
        {
          return;
        }
      app->audio_meta_sent = true;
    }

  if (board_audio_stream_wait(0) < 0)
    {
      return;
    }

  /* Speech detection is only needed when nothing else will tell us the
   * utterance ended.  While the button is held the user is the authority, so
   * skip it entirely -- otherwise a pause for breath would close the window
   * mid-sentence.  The lead-in is excluded either way so the capture-start
   * transient can never register as speech. */
  vad_enabled = !app->ptt_active &&
                (int32_t)(va_now_ms() - app->listen_started_ms) >=
                    VA_IGNORE_LEAD_MS;

  while ((ret = board_audio_stream_read(frame, sizeof(frame))) ==
         (int)sizeof(frame))
    {
      if (vad_enabled)
        {
          const int16_t *samples = (const int16_t *)frame;
          unsigned i;

          for (i = 0; i < sizeof(frame) / sizeof(samples[0]); i++)
            {
              const int16_t v = samples[i];
              const int32_t a = v < 0 ? -(int32_t)v : (int32_t)v;

              if (a >= VA_END_PEAK)
                {
                  app->have_speech = true;
                  app->last_speech_ms = va_now_ms();
                  break;
                }
            }
        }

      /* Every captured frame goes up regardless of the detector: it only
       * decides when to stop, never what the recogniser gets to hear. */
      if (va_send_frame(app, MIBOT_TYPE_AUDIO_UP, 0, frame,
                        (uint16_t)sizeof(frame)) == 0)
        {
          app->uplink_frames_sent++;
        }
      else
        {
          app->uplink_frames_failed++;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Main-loop per-state service.                                              */
/*                                                                            */
/* The pure core owns transitions; this only observes ctx->state edges to arm */
/* per-state timers and kick off the once-per-turn work (ASR window,          */
/* DeepSeek request), and services those timers.  It never writes ctx->state  */
/* directly - it always goes through va_handle_event / va_request_acting.     */
/* ------------------------------------------------------------------------- */

static void va_service_state(struct va_app_s *app)
{
  va_state_t s = app->ctx.state;
  uint32_t now = va_now_ms();

  /* On state entry, arm the relevant per-state deadline / once-per-turn work.
   *
   * Entry is detected against a persistent last-serviced state, NOT a snapshot
   * taken by the caller before this function ran.  A transition can happen
   * *inside* this function (e.g. the LISTENING window closing posts ASR_DONE
   * which moves us to THINKING); using a caller snapshot would then miss the
   * new state's entry block on this tick and, once the caller advances its
   * snapshot to the new state, miss it forever.  A persistent field guarantees
   * every state entry runs its once-per-turn work exactly once. */
  if (s != app->last_serviced_state)
    {
      app->last_serviced_state = s;

      app->listen_deadline_ms = 0;
      app->thinking_deadline_ms = 0;
      /* acting/speaking deadlines are armed where the transition is caused
       * (AI response handler) so they survive this edge detection. */

      /* Entering anything other than LISTENING means the capture window is
       * over, however we got here (release, cap, SAFE_STOP preemption,
       * CLOUD_FAIL), so the hold no longer owns a window and a late release is
       * ignored.  LISTENING is excluded because its entry below still needs
       * ptt_active to pick the right deadline. */
      if (s != VA_LISTENING)
        {
          app->ptt_active = false;
          app->ptt_release_pending = false;
          app->ptt_close_request = false;
        }

      switch (s)
        {
          case VA_LISTENING:
            /* Push-to-talk: the deadline is only a safety net for a release we
             * never see (stuck contact, a wake from `va_wake` while the button
             * happened to be down).  The release is the real end of the
             * utterance. */
            app->listen_started_ms = now;
            app->listen_deadline_ms =
                now + (app->ptt_active ? VA_PTT_MAX_HOLD_MS
                                       : VA_LISTEN_WINDOW_MS);
            app->have_speech = false;
            app->last_speech_ms = 0;
            app->ptt_close_request = false;
            break;

          case VA_THINKING:
            /* 9.4: build+send the DeepSeek AI_REQUEST once on entering
             * THINKING (需求 5.1).  The transcript was staged when the ASR
             * window closed. */
            app->ai_request_sent = false;
            va_thinking_send_ai_request(app);
            break;

          case VA_ACTING:
            /* Sequential_Mode (default): playback waits for ACTING to finish,
             * so ACTING and SPEAKING never overlap (需求 6.1, 6.2, P7).
             * Concurrent_Mode (MIBOT_VA_CONCURRENT): if the final answer text
             * is already known, start AUDIO_DOWN playback now so it overlaps
             * the bounded action (需求 6.3, 6.4).  Either way the Main_State
             * set and transition edges are identical (需求 6.6, P10) - only
             * the playback *timing* differs, gated by the pure-logic predicate
             * va_playback_may_overlap_acting(). */
            if (va_playback_may_overlap_acting() &&
                app->ctx.pending_answer[0] != '\0' && !app->play_active)
              {
                (void)tts_request(app->ctx.pending_answer, &app->ctx);
                va_play_start(app);
                app->speaking_deadline_ms = now + VA_SPEAKING_TIMEOUT_MS;
              }
            break;

          case VA_IDLE:
            /* Reset per-turn scratch so the next turn starts clean. */
            app->transcript[0] = '\0';
            app->ctx.pending_answer[0] = '\0';
            app->ai_request_sent = false;
            app->asr_waiting = false;
            app->asr_text_ready = false;
            app->asr_deadline_ms = 0;
            app->speak_request_sent = false;
            app->ai_retries = 0;
            break;

          default:
            break;
        }
    }

  /* Serve armed deadlines. */
  switch (s)
    {
      case VA_LISTENING:
        va_pump_uplink(app);

        /* Button released -> the user says they are done.  Authoritative, so it
         * bypasses both the hard cap and the silence detector. */
        if (app->ptt_close_request)
          {
            app->ptt_close_request = false;
            app->ptt_active = false;
            app->listen_deadline_ms = now;
          }
        /* No release to wait for (tap, `va_wake`, or a wake-word EVENT): close
         * once the user has demonstrably spoken and then gone quiet, so a slow
         * speaker is not cut off and a fast one does not wait out the cap. */
        else if (!app->ptt_active && app->capture_active &&
                 app->have_speech && app->last_speech_ms != 0 &&
                 (int32_t)(now - app->last_speech_ms) >= VA_END_SILENCE_MS &&
                 (int32_t)(now - app->listen_started_ms) >= VA_END_MIN_MS)
          {
            printf("va: end of speech (%lu ms quiet)\n",
                   (unsigned long)(now - app->last_speech_ms));
            app->listen_deadline_ms = now;
          }

        if (app->listen_deadline_ms != 0 &&
            (int32_t)(now - app->listen_deadline_ms) >= 0)
          {
            app->listen_deadline_ms = 0;
            app->ptt_active = false;

            if (va_asr_stub_active())
              {
                /* Stub mode: a fixed transcript resolves immediately, so
                 * LISTENING -> THINKING happens on this tick (需求 1.4, 9.1). */
                char text[sizeof(app->transcript)];

                if (asr_transcribe(NULL, 0, text, sizeof(text)) == VA_STUB_OK)
                  {
                    strncpy(app->transcript, text, sizeof(app->transcript) - 1);
                    app->transcript[sizeof(app->transcript) - 1] = '\0';
                  }
                else
                  {
                    app->transcript[0] = '\0';
                  }
                va_handle_event(&app->ctx, VA_EVENT_ASR_DONE);
              }
            else
              {
                /* Real ASR (task 13): close the uplink window NOW, before
                 * waiting for the transcript.  The state stays LISTENING while
                 * the cloud works, so without this the capture pump would keep
                 * streaming for the whole ASR budget: the utterance would never
                 * be terminated for the recogniser and the extra frames would
                 * push the 1 Mbps UART well past the 32 kB/s audio rate. */
                va_capture_stop(app);
                app->asr_waiting = true;
                app->asr_text_ready = false;
                app->transcript[0] = '\0';
                app->asr_deadline_ms = now + VA_ASR_TIMEOUT_MS;
                printf("va: awaiting cloud ASR text (%d ms budget)\n",
                       VA_ASR_TIMEOUT_MS);
              }
          }

        /* Real-ASR wait: resolve as soon as the transcript lands, otherwise
         * fail the turn when the budget expires. */
        if (app->asr_waiting)
          {
            if (app->asr_text_ready)
              {
                app->asr_waiting = false;
                app->asr_text_ready = false;
                app->asr_deadline_ms = 0;
                va_handle_event(&app->ctx, VA_EVENT_ASR_DONE);
              }
            else if (app->asr_deadline_ms != 0 &&
                     (int32_t)(now - app->asr_deadline_ms) >= 0)
              {
                app->asr_waiting = false;
                app->asr_deadline_ms = 0;
                printf("va: cloud ASR timeout, CLOUD_FAIL\n");
                va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
              }
          }
        break;

      case VA_THINKING:
        if (app->thinking_deadline_ms != 0 &&
            (int32_t)(now - app->thinking_deadline_ms) >= 0)
          {
            app->thinking_deadline_ms = 0;

            /* The 1 Mbps link occasionally corrupts a frame (observed as
             * "AI_RESPONSE CRC mismatch"), which loses an otherwise good cloud
             * reply and would waste the whole turn.  Re-send the request once
             * before giving up; the gateway is single-flight but has already
             * released its slot by the time the reply was emitted, and the
             * per-turn request_id keeps a late duplicate from being
             * mismatched. */
            if (app->ai_retries < VA_AI_MAX_RETRIES)
              {
                app->ai_retries++;
                printf("va: no AI_RESPONSE in budget, retry %d/%d\n",
                       app->ai_retries, VA_AI_MAX_RETRIES);
                va_thinking_send_ai_request(app);
              }
            else
              {
                /* Still nothing: treat as cloud failure (需求 1.7). */
                printf("va: THINKING timeout, CLOUD_FAIL\n");
                va_rx_stats_print("thinking-timeout");
                va_handle_event(&app->ctx, VA_EVENT_CLOUD_FAIL);
              }
          }
        break;

      case VA_ACTING:
        if (app->acting_deadline_ms != 0 &&
            (int32_t)(now - app->acting_deadline_ms) >= 0)
          {
            /* Fallback if the ESP32 action_update completion is missed so the
             * turn cannot wedge in ACTING (需求 5.6). */
            app->acting_deadline_ms = 0;
            printf("va: ACTING timeout, ACTION_DONE (fallback)\n");
            va_handle_event(&app->ctx, VA_EVENT_ACTION_DONE);
            /* ACTION_DONE may move us into SPEAKING when the reply carried a
             * sentence alongside the action.  Arm the playback deadline here
             * too, otherwise a lost AUDIO_DOWN eos would wedge the turn in
             * SPEAKING with no timeout (需求 1.10). */
            if (app->ctx.state == VA_SPEAKING)
              {
                app->speaking_deadline_ms =
                    va_now_ms() + VA_SPEAKING_TIMEOUT_MS;
              }
          }
        break;

      case VA_SPEAKING:
        if (app->speak_tone_pending)
          {
            /* All-stub SPEAKING: play a short local placeholder tone so the
             * speaking step is audible, then end the turn (SPEAKING -> IDLE).
             * board_audio_play_tone() blocks for the tone's duration, which is
             * fine here: the agent is meant to be "speaking" for that moment
             * and no other state work is due (需求 9.2). */
            app->speak_tone_pending = false;
            printf("va: SPEAKING placeholder tone (1s)\n");
            (void)board_audio_play_tone(1);
            va_handle_event(&app->ctx, VA_EVENT_SPEAK_DONE);
            break;
          }
        if (app->speaking_deadline_ms != 0 &&
            (int32_t)(now - app->speaking_deadline_ms) >= 0)
          {
            app->speaking_deadline_ms = 0;
            printf("va: SPEAKING timeout, SPEAK_DONE (fallback)\n");
            va_handle_event(&app->ctx, VA_EVENT_SPEAK_DONE);
          }
        break;

      default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Trigger-dialog entry point (需求 3.5).                                     */
/*                                                                            */
/* v1 exposes wake as an internal helper.  The full `va_wake` NSH command and */
/* build-profile registration are task 11.2; here we provide the minimal      */
/* trigger so the pipeline is exercisable, plus a documented hook for a future */
/* wake-word EVENT to call.  The core enforces the cloud gate on IDLE->        */
/* LISTENING (需求 3.3), so an offline trigger is safely ignored.             */
/* ------------------------------------------------------------------------- */

static void va_post_wake(struct va_app_s *app)
{
  /* The core enforces the IDLE->LISTENING cloud gate (需求 3.3), so posting
   * WAKE while offline or mid-turn is safely ignored. */
  va_handle_event(&app->ctx, VA_EVENT_WAKE);
}

/* Trigger-dialog entry (需求 3.5).  The main loop consumes this latched flag
 * and calls va_post_wake().  It is set by:
 *   - the `va_wake` NSH command (va_wake_main below), for bring-up/联调; and
 *   - va_post_event(VA_EVENT_WAKE), the documented hook for a future offline
 *     wake-word EVENT relayed from the ESP32 (task 14).
 * A single flag is sufficient because only one wake can be pending per turn.
 * Non-static so the separate `va_wake` NSH command (va_wake.c) can latch it in
 * the NuttX flat build's shared address space. */
volatile bool g_va_wake_request;

/* Public trigger hook so a wake-word EVENT decoder (or any other source) can
 * request a dialog turn without reaching into the state machine directly.
 * Only VA_EVENT_WAKE is accepted here; other events flow through the parser. */
void va_post_event(va_event_t event)
{
  if (event == VA_EVENT_WAKE)
    {
      g_va_wake_request = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Physical wake button (需求 3.5) - KEY2 / PA11 on the SF32LB52-DevKit-LCD.  */
/*                                                                            */
/* Third source of a dialog trigger, alongside the `va_wake` NSH command and a */
/* future wake-word EVENT.  All three converge on the same latched flag, so    */
/* the state machine sees exactly one kind of wake and the cloud gate still    */
/* applies (需求 3.3).                                                        */
/*                                                                            */
/* Polled from the main loop rather than driven by the driver's notification    */
/* callback: the 20 ms tick is far below human press duration, and polling      */
/* keeps button handling out of any signal/IRQ context so a bouncing contact    */
/* can never delay a state transition (需求 4.5, 7.5).                        */
/*                                                                            */
/* PA34 is not an option here - it is the power key wired into the reset path,  */
/* the board leaves its pinmux commented out, and board.h registers a single    */
/* button (NUM_BUTTONS 1, KEY2 only).                                          */
/* ------------------------------------------------------------------------- */

static void va_button_open(struct va_app_s *app)
{
  app->btn_fd = -1;
  app->btn_last = 0;
  app->btn_last_press_ms = 0;

  if (VA_BUTTON_DEVICE[0] == '\0')
    {
      return;   /* deliberately built without a physical trigger */
    }

  app->btn_fd = open(VA_BUTTON_DEVICE, O_RDONLY | O_NONBLOCK);
  if (app->btn_fd < 0)
    {
      /* Not fatal: `va_wake` still works.  The usual cause is a build without
       * CONFIG_INPUT_BUTTONS, so name the symbol in the message. */
      printf("va: wake button %s unavailable: %d "
             "(needs CONFIG_INPUT + CONFIG_INPUT_BUTTONS)\n",
             VA_BUTTON_DEVICE, errno);
      return;
    }

  /* Seed the edge detector with the current level so a button that happens to
   * be held at boot does not read as a fresh press on the first poll. */
  if (read(app->btn_fd, &app->btn_last, sizeof(app->btn_last)) !=
      (ssize_t)sizeof(app->btn_last))
    {
      app->btn_last = 0;
    }

  printf("va: wake button %s ready (KEY2/PA11, active high)\n",
         VA_BUTTON_DEVICE);
}

static void va_button_poll(struct va_app_s *app)
{
  btn_buttonset_t sample;
  uint32_t now;

  if (app->btn_fd < 0)
    {
      return;
    }

  /* btn_read() reports the current button set rather than queued events, so a
   * short read means "no usable sample" and is simply skipped. */
  if (read(app->btn_fd, &sample, sizeof(sample)) != (ssize_t)sizeof(sample))
    {
      return;
    }

  now = va_now_ms();

  /* Rising edge: press. */
  if ((sample & BUTTON_KEY2_BIT) != 0 &&
      (app->btn_last & BUTTON_KEY2_BIT) == 0)
    {
      if (app->btn_last_press_ms != 0 &&
          now - app->btn_last_press_ms < VA_BUTTON_DEBOUNCE_MS)
        {
          /* Bounce: swallow it but still adopt the level so the next real
           * release/press pair is detected. */
          app->btn_last = sample;
          return;
        }
      app->btn_last_press_ms = now;
      app->ptt_active = true;
      app->ptt_release_pending = false;
      app->ptt_close_request = false;
      printf("va: wake button pressed (hold to talk)\n");
      va_post_event(VA_EVENT_WAKE);
    }
  /* Falling edge: start the release bounce timer rather than closing straight
   * away.  A contact bounce mid-utterance would otherwise cut the user off. */
  else if ((sample & BUTTON_KEY2_BIT) == 0 &&
           (app->btn_last & BUTTON_KEY2_BIT) != 0)
    {
      if (app->ptt_active)
        {
          app->ptt_release_pending = true;
          app->ptt_release_ms = now;
        }
    }

  app->btn_last = sample;

  /* Commit or cancel a pending release.  This runs on every poll, including
   * the ones where the level did not change -- which is the whole point: the
   * bounce window has to be timed out, not merely observed once. */
  if (app->ptt_release_pending)
    {
      if ((sample & BUTTON_KEY2_BIT) != 0)
        {
          /* Back down within the bounce window: it was not a real release. */
          app->ptt_release_pending = false;
        }
      else if (now - app->ptt_release_ms >= VA_BUTTON_RELEASE_MS)
        {
          const uint32_t held = now - app->btn_last_press_ms;

          app->ptt_release_pending = false;
          if (held >= VA_PTT_MIN_HOLD_MS)
            {
              app->ptt_close_request = true;
              printf("va: wake button released after %lu ms\n",
                     (unsigned long)held);
            }
          else
            {
              /* Too short to be a deliberate hold; treat it as a tap and let
               * the timed window / speech detector end the utterance. */
              app->ptt_active = false;
              printf("va: wake button tapped (%lu ms), using timed window\n",
                     (unsigned long)held);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Application entry.                                                         */
/*                                                                            */
/* Registered by the `-VoiceAgent` build profile in task 11 (not this task).  */
/* ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
  va_hooks_t hooks;
  int uart_ret;
  uint32_t demo_hold_ms = 0;

  /* `mibot_voice_agent face-demo [ms]` shows every state's expression and
   * exits, without opening the UART or the button.  Anything else starts the
   * agent as usual. */
  if (argc >= 2 && strcmp(argv[1], "face-demo") == 0)
    {
      demo_hold_ms = argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 5000u;
      if (demo_hold_ms < 500u)
        {
          demo_hold_ms = 500u;
        }
    }

  memset(&g_app, 0, sizeof(g_app));
  g_app.uart_fd = -1;
  g_app.fb.fd = -1;
  /* 0 is a valid descriptor, so the memset above is not enough. */
  g_app.btn_fd = -1;
  g_app.running = true;
  g_app.seq = 1;
  va_parser_reset(&g_app.parser);

  /* Wire the injectable hardware effects, then init the pure-logic core.
   * cloud_available defaults false until HELLO_ACK / a network-ready EVENT
   * brings the link up (需求 3.1). */
  va_install_hooks(&hooks, &g_app);
  va_init(&g_app.ctx, &hooks);
  g_app.lcd_state = g_app.ctx.state;
  g_app.lcd_dirty = true;
  va_face_anim_init(&g_app.face_anim, va_now_ms());
  va_face_base(g_app.lcd_state, &g_app.face);
  /* Boot IDLE entry effects are kicked manually below, so mark IDLE as already
   * serviced; va_service_state then only fires on the next real state change. */
  g_app.last_serviced_state = g_app.ctx.state;

  if (va_asr_stub_active() || va_tts_stub_active())
    {
      printf("va: stub mode active (asr=%d tts=%d) - phase-1 all-stub\n",
             (int)va_asr_stub_active(), (int)va_tts_stub_active());
    }

  /* Let the board LCD worker finish panel power-up before the first frame. */
  sleep(2);

  if (demo_hold_ms > 0)
    {
      va_face_demo(&g_app, demo_hold_ms);
      va_lcd_close(&g_app);
      return 0;
    }

  uart_ret = va_uart_open(&g_app);
  if (uart_ret < 0)
    {
      printf("va: UART %s unavailable: %d\n",
             VA_UART_DEVICE, -uart_ret);
    }
  else
    {
      printf("va: UART %s @ %d 8N1 ready\n",
             VA_UART_DEVICE, VA_UART_BAUD);
      va_send_hello(&g_app);
      g_app.last_hello_ms = va_now_ms();
    }

  /* Physical dialog trigger.  Optional: a missing device only means wake has to
   * come from `va_wake` instead. */
  va_button_open(&g_app);

  /* va_init leaves the core resting in IDLE but does not fire IDLE's entry
   * effects (a self-transition is rejected), so kick the idle action + first
   * LCD frame explicitly at boot (需求 8.2, 4.2). */
  va_send_perform_action(&g_app, "idle");
  va_lcd_render(&g_app);
  g_app.lcd_dirty = false;

  while (g_app.running)
    {
      uint32_t now;

      usleep(VA_MAIN_TICK_US);

      /* 9.1: pull all buffered UART bytes and decode frames into the core. */
      va_uart_drain_rx(&g_app);

      /* Unconditional SAFE_STOP preemption + core main-loop step (需求 2.7). */
      va_tick(&g_app.ctx);

      /* Physical wake button; latches the same flag the NSH command does, so it
       * must run before the flag is consumed to react within this tick. */
      va_button_poll(&g_app);

      /* Wake trigger, from `va_wake`, the button, or a wake-word EVENT. */
      if (g_va_wake_request)
        {
          g_va_wake_request = false;
          va_post_wake(&g_app);
        }

      /* Observe the state edge and service per-state work.  Entry detection is
       * internal (app->last_serviced_state), so a transition that happens
       * inside this call is still serviced on the next tick. */
      va_service_state(&g_app);

      now = va_now_ms();

      /* 9.2: render the LCD only from the main loop, never the parser path,
       * so it cannot block a transition (需求 4.5, 7.5).
       *
       * Blink and breathing mean the target geometry changes without any state
       * change, so the redraw trigger is "the composed face differs from what
       * is on screen" rather than the dirty flag alone.  During the 160 ms of a
       * blink that is a handful of frames; the rest of the time the composition
       * is identical and nothing is drawn. */
      {
        va_face_t next;

        va_face_update(&g_app.face_anim, g_app.lcd_state, now,
                       g_app.speak_level, &next);

        /* Hold the face still while audio is playing.
         *
         * A redraw costs ~30 ms of this loop, and this loop is also what drains
         * the UART and hands AUDIO_DOWN frames to the DAC.  A blink is ~5
         * redraws inside 160 ms, i.e. most of the 240 ms playback queue spent
         * not feeding it -- heard as chopped speech.  The state's resting face
         * is still drawn (lcd_dirty from the state change is honoured); only the
         * blink/breathing animation waits for the reply to finish. */
        if (g_app.lcd_dirty ||
            (!g_app.play_active && !va_face_equal(&next, &g_app.face)))
          {
            g_app.lcd_dirty = false;
            g_app.face = next;
            va_lcd_render(&g_app);
          }
      }

      /* Retry HELLO until the ESP32 acknowledges the link. */
      if (g_app.uart_fd >= 0 && !g_app.hello_acked &&
          now - g_app.last_hello_ms >= VA_HELLO_RETRY_MS)
        {
          va_send_hello(&g_app);
          g_app.last_hello_ms = now;
        }
    }

  va_lcd_close(&g_app);
  if (g_app.btn_fd >= 0)
    {
      close(g_app.btn_fd);
    }
  if (g_app.uart_fd >= 0)
    {
      close(g_app.uart_fd);
    }
  return 0;
}

#endif /* __NuttX__ */
