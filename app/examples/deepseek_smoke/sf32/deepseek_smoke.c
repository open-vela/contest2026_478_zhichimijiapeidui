#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "cJSON.h"

/* The standalone voice smoke links the board's existing DMA audio lower-half. */
extern int board_audio_i2s_initialize(void);
extern int board_audio_stream_start(void);
extern int board_audio_stream_read(void *buf, size_t len);
extern int board_audio_stream_wait(int timeout_ms);
extern int board_audio_stream_stop(void);
extern int board_audio_play_start(void);
extern int board_audio_play_write(const void *buf, size_t len);
extern int board_audio_play_drain(int timeout_ms);
extern int board_audio_play_stop(void);

#ifndef CONFIG_EXAMPLES_DEEPSEEK_SMOKE_UART
#define CONFIG_EXAMPLES_DEEPSEEK_SMOKE_UART "/dev/ttyS0"
#endif
#ifndef CONFIG_EXAMPLES_DEEPSEEK_SMOKE_BAUD
#define CONFIG_EXAMPLES_DEEPSEEK_SMOKE_BAUD 1000000
#endif

#define SMOKE_VERSION 1
#define SMOKE_HEADER_SIZE 7
#define SMOKE_OVERHEAD 11
#define SMOKE_MAX_PAYLOAD 4096
#define SMOKE_AI_REQUEST 0x40
#define SMOKE_AI_RESPONSE 0x41
#define SMOKE_AUDIO_UP 0x30
#define SMOKE_AUDIO_DOWN 0x31
#define SMOKE_COMMAND 0x10
#define SMOKE_ACK 0x11
#define SMOKE_NACK 0x12
#define SMOKE_HELLO_ACK 0x02
#define SMOKE_FLAG_ACK_REQUEST (1u << 0)
#define SMOKE_AUDIO_FRAME_BYTES 640
#define SMOKE_AUDIO_RECORD_SECONDS 4
#define SMOKE_MAX_TOOL_CALLS 4

enum parser_state_e {
  PARSER_SOF0 = 0,
  PARSER_SOF1,
  PARSER_HEADER,
  PARSER_PAYLOAD,
  PARSER_CRC,
};

struct parser_s {
  enum parser_state_e state;
  uint8_t header[SMOKE_HEADER_SIZE];
  uint8_t header_pos;
  uint8_t payload[SMOKE_MAX_PAYLOAD];
  uint16_t payload_len;
  uint16_t payload_pos;
  uint8_t crc[2];
  uint8_t crc_pos;
};

/* The application stack is intentionally small.  Keep protocol-sized
 * buffers in static storage instead of placing several kilobytes in main(). */
static uint8_t g_tx_frame[SMOKE_MAX_PAYLOAD + SMOKE_OVERHEAD];
static struct parser_s g_parser;
static uint8_t g_response[SMOKE_MAX_PAYLOAD + 1];

static uint16_t crc16_append(uint16_t crc, const uint8_t *data, size_t length)
{
  size_t index;
  int bit;
  for (index = 0; index < length; index++) {
    crc ^= (uint16_t)data[index] << 8;
    for (bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) != 0 ? (uint16_t)((crc << 1) ^ 0x1021)
                                : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static uint16_t crc16(const uint8_t *data, size_t length)
{
  return crc16_append(0xffff, data, length);
}

static int write_all(int fd, const uint8_t *data, size_t length)
{
  while (length > 0) {
    const ssize_t written = write(fd, data, length);
    if (written < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (written == 0) return -EIO;
    data += written;
    length -= (size_t)written;
  }
  return 0;
}

static int send_frame_bytes(int fd, uint8_t type, uint8_t flags, uint16_t seq,
                            const uint8_t *payload, size_t length)
{
  uint16_t crc;
  if (length > SMOKE_MAX_PAYLOAD || (length > 0 && payload == NULL))
    return -E2BIG;

  g_tx_frame[0] = 0xaa;
  g_tx_frame[1] = 0x55;
  g_tx_frame[2] = SMOKE_VERSION;
  g_tx_frame[3] = type;
  g_tx_frame[4] = flags;
  g_tx_frame[5] = (uint8_t)(seq & 0xff);
  g_tx_frame[6] = (uint8_t)(seq >> 8);
  g_tx_frame[7] = (uint8_t)(length & 0xff);
  g_tx_frame[8] = (uint8_t)(length >> 8);
  if (length > 0) memcpy(&g_tx_frame[9], payload, length);
  crc = crc16(&g_tx_frame[2], SMOKE_HEADER_SIZE + length);
  g_tx_frame[9 + length] = (uint8_t)(crc & 0xff);
  g_tx_frame[10 + length] = (uint8_t)(crc >> 8);
  return write_all(fd, g_tx_frame, length + SMOKE_OVERHEAD);
}

static int send_frame(int fd, uint8_t type, uint8_t flags, uint16_t seq,
                      const char *payload)
{
  return send_frame_bytes(fd, type, flags, seq,
                          (const uint8_t *)payload,
                          payload != NULL ? strlen(payload) : 0);
}


static int open_uart(void)
{
  struct termios tio;
  speed_t speed = B921600;
  int fd = open(CONFIG_EXAMPLES_DEEPSEEK_SMOKE_UART,
                O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -errno;
  if (tcgetattr(fd, &tio) < 0) {
    const int error = errno;
    close(fd);
    return -error;
  }
#ifdef B1000000
  if (CONFIG_EXAMPLES_DEEPSEEK_SMOKE_BAUD == 1000000) speed = B1000000;
#endif
#ifdef B115200
  if (CONFIG_EXAMPLES_DEEPSEEK_SMOKE_BAUD == 115200) speed = B115200;
#endif
#ifdef B460800
  if (CONFIG_EXAMPLES_DEEPSEEK_SMOKE_BAUD == 460800) speed = B460800;
#endif
  cfmakeraw(&tio);
  cfsetspeed(&tio, speed);
  tio.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
  tio.c_cflag &= ~CRTSCTS;
#endif
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSANOW, &tio) < 0) {
    const int error = errno;
    close(fd);
    return -error;
  }
  return fd;
}

static void parser_reset(struct parser_s *parser)
{
  memset(parser, 0, sizeof(*parser));
  parser->state = PARSER_SOF0;
}

/* Return 1 for a complete frame, 0 for more bytes, and -1 for a bad CRC. */
static int parser_feed(struct parser_s *parser, uint8_t byte, uint8_t *type,
                       uint8_t **payload, uint16_t *length)
{
  uint16_t expected;
  uint16_t received;
  if (parser == NULL || type == NULL || payload == NULL || length == NULL)
    return 0;
  switch (parser->state) {
    case PARSER_SOF0:
      if (byte == 0xaa) parser->state = PARSER_SOF1;
      break;
    case PARSER_SOF1:
      if (byte == 0x55) {
        parser->header_pos = 0;
        parser->state = PARSER_HEADER;
      } else {
        parser->state = byte == 0xaa ? PARSER_SOF1 : PARSER_SOF0;
      }
      break;
    case PARSER_HEADER:
      parser->header[parser->header_pos++] = byte;
      if (parser->header_pos == SMOKE_HEADER_SIZE) {
        parser->payload_len = (uint16_t)parser->header[5] |
                              ((uint16_t)parser->header[6] << 8);
        parser->payload_pos = 0;
        if (parser->header[0] != SMOKE_VERSION ||
            parser->payload_len > SMOKE_MAX_PAYLOAD) {
          parser_reset(parser);
        } else if (parser->payload_len == 0) {
          parser->crc_pos = 0;
          parser->state = PARSER_CRC;
        } else {
          parser->state = PARSER_PAYLOAD;
        }
      }
      break;
    case PARSER_PAYLOAD:
      parser->payload[parser->payload_pos++] = byte;
      if (parser->payload_pos == parser->payload_len) {
        parser->crc_pos = 0;
        parser->state = PARSER_CRC;
      }
      break;
    case PARSER_CRC:
      parser->crc[parser->crc_pos++] = byte;
      if (parser->crc_pos == 2) {
        expected = crc16(parser->header, SMOKE_HEADER_SIZE);
        expected = crc16_append(expected, parser->payload,
                                parser->payload_len);
        received = (uint16_t)parser->crc[0] | ((uint16_t)parser->crc[1] << 8);
        if (expected != received) {
          parser_reset(parser);
          return -1;
        }
        *type = parser->header[1];
        *payload = parser->payload;
        *length = parser->payload_len;
        return 1;
      }
      break;
  }
  return 0;
}

static int wait_for_frame(int fd, struct parser_s *parser, int timeout_ms,
                          uint8_t wanted_type, uint8_t *payload,
                          uint16_t *length)
{
  const int64_t deadline = (int64_t)timeout_ms;
  int elapsed = 0;
  uint8_t input[256];
  while (elapsed < deadline) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    const int wait_ms = deadline - elapsed > 250 ? 250 : deadline - elapsed;
    const int poll_result = poll(&pfd, 1, wait_ms);
    elapsed += wait_ms;
    if (poll_result < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (poll_result == 0) continue;
    const ssize_t count = read(fd, input, sizeof(input));
    if (count < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      return -errno;
    }
    for (ssize_t index = 0; index < count; index++) {
      uint8_t type;
      uint8_t *frame_payload;
      uint16_t frame_length;
      const int result = parser_feed(parser, input[index], &type,
                                     &frame_payload, &frame_length);
      if (result < 0) {
        fprintf(stderr, "deepseek_smoke: CRC error\n");
        continue;
      }
      if (result == 1) {
        if (type == wanted_type) {
          if (frame_length > SMOKE_MAX_PAYLOAD) {
            parser_reset(parser);
            return -E2BIG;
          }
          /* parser_feed returns a pointer into its working buffer.  Copy it
           * before resetting that buffer for the next frame. */
          memcpy(payload, frame_payload, frame_length);
          payload[frame_length] = '\0';
          *length = frame_length;
          parser_reset(parser);
          return 0;
        }
        parser_reset(parser);
      }
    }
  }
  return -ETIMEDOUT;
}

static int wait_next_frame(int fd, struct parser_s *parser, int timeout_ms,
                           uint8_t *type, uint8_t *payload, uint16_t *length);
static int run_ai_conversation(int fd, uint16_t *sequence, const char *prompt,
                               bool with_tools, bool allow_motion,
                               char *answer, size_t answer_size);
static int capture_prompt(int fd, uint16_t *sequence, int seconds,
                          char *transcript, size_t transcript_size);
static int speak_and_play(int fd, uint16_t *sequence, const char *text);

int main(int argc, char **argv)
{
  char prompt[512] = "Reply with exactly DEEPSEEK_SMOKE_OK";
  char answer[1024] = {};
  char transcript[512] = {};
  bool tool_mode = false;
  bool voice_mode = false;
  bool allow_motion = false;
  uint16_t sequence = 1;
  int first_prompt_arg = 1;
  int fd;
  int ret;

  while (first_prompt_arg < argc) {
    if (strcmp(argv[first_prompt_arg], "--tool") == 0) tool_mode = true;
    else if (strcmp(argv[first_prompt_arg], "--voice") == 0) {
      voice_mode = true;
      tool_mode = true;
    } else if (strcmp(argv[first_prompt_arg], "--allow-motion") == 0) {
      allow_motion = true;
      tool_mode = true;
    } else {
      break;
    }
    first_prompt_arg++;
  }

  if (first_prompt_arg < argc) {
    size_t used = 0;
    prompt[0] = '\0';
    for (int index = first_prompt_arg; index < argc; index++) {
      const size_t part = strlen(argv[index]);
      if (used != 0 && used + 1 < sizeof(prompt)) prompt[used++] = ' ';
      if (used + part >= sizeof(prompt)) {
        fprintf(stderr, "deepseek_smoke: prompt too long\n");
        return 2;
      }
      memcpy(prompt + used, argv[index], part);
      used += part;
    }
    prompt[used] = '\0';
  } else if (tool_mode && !voice_mode) {
    snprintf(prompt, sizeof(prompt),
             "Turn the robot status LED on using the available tool, then confirm briefly.");
  }

  fd = open_uart();
  if (fd < 0) {
    fprintf(stderr, "deepseek_smoke: open %s failed: %d\n",
            CONFIG_EXAMPLES_DEEPSEEK_SMOKE_UART, -fd);
    return 1;
  }
  parser_reset(&g_parser);
  ret = send_frame(fd, 0x01, 0, sequence++,
                   "{\"schema\":\"mibot.uart.v1\",\"node\":\"sf32-deepseek-smoke\"}");
  if (ret < 0) goto fail;
  uint16_t hello_length = 0;
  ret = wait_for_frame(fd, &g_parser, 3000, SMOKE_HELLO_ACK, g_response,
                       &hello_length);
  if (ret < 0) goto fail;
  printf("deepseek_smoke: HELLO_ACK received\n");

  if (voice_mode) {
    ret = board_audio_i2s_initialize();
    if (ret < 0) {
      fprintf(stderr, "deepseek_smoke: audio init failed: %d\n", ret);
      goto fail;
    }
    printf("deepseek_smoke: recording %d seconds...\n",
           SMOKE_AUDIO_RECORD_SECONDS);
    ret = capture_prompt(fd, &sequence, SMOKE_AUDIO_RECORD_SECONDS,
                         transcript, sizeof(transcript));
    if (ret < 0) {
      fprintf(stderr, "deepseek_smoke: ASR failed: %d\n", ret);
      goto fail;
    }
    snprintf(prompt, sizeof(prompt), "%s", transcript);
    printf("deepseek_smoke: transcript=%s\n", prompt);
  }

  ret = run_ai_conversation(fd, &sequence, prompt, tool_mode, allow_motion,
                            answer, sizeof(answer));
  if (ret < 0) {
    fprintf(stderr, "deepseek_smoke: AI conversation failed: %d\n", ret);
    goto fail;
  }
  printf("deepseek_smoke: answer=%s\n", answer);

  if (voice_mode) {
    ret = speak_and_play(fd, &sequence, answer);
    if (ret < 0) {
      fprintf(stderr, "deepseek_smoke: TTS/playback failed: %d\n", ret);
      goto fail;
    }
  }

  close(fd);
  printf("deepseek_smoke: PASS\n");
  return 0;

fail:
  close(fd);
  fprintf(stderr, "deepseek_smoke: FAIL (%d)\n", ret);
  return 1;
}
struct tool_result_s {
  char id[96];
  char content[768];
};

static char g_request[SMOKE_MAX_PAYLOAD + 1];

static int wait_next_frame(int fd, struct parser_s *parser, int timeout_ms,
                           uint8_t *type, uint8_t *payload, uint16_t *length)
{
  int elapsed = 0;
  uint8_t input[256];
  while (elapsed < timeout_ms) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    const int wait_ms = timeout_ms - elapsed > 250 ? 250 : timeout_ms - elapsed;
    const int poll_result = poll(&pfd, 1, wait_ms);
    elapsed += wait_ms;
    if (poll_result < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (poll_result == 0) continue;
    const ssize_t count = read(fd, input, sizeof(input));
    if (count < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      return -errno;
    }
    for (ssize_t index = 0; index < count; index++) {
      uint8_t frame_type;
      uint8_t *frame_payload;
      uint16_t frame_length;
      const int result = parser_feed(parser, input[index], &frame_type,
                                     &frame_payload, &frame_length);
      if (result < 0) {
        fprintf(stderr, "deepseek_smoke: CRC error\n");
        continue;
      }
      if (result == 1) {
        if (frame_length > SMOKE_MAX_PAYLOAD) {
          parser_reset(parser);
          return -E2BIG;
        }
        memcpy(payload, frame_payload, frame_length);
        payload[frame_length] = '\0';
        *type = frame_type;
        *length = frame_length;
        parser_reset(parser);
        return 0;
      }
    }
  }
  return -ETIMEDOUT;
}

static cJSON *add_tool(cJSON *tools, const char *name,
                       const char *description)
{
  cJSON *tool = cJSON_CreateObject();
  cJSON *function = tool != NULL ? cJSON_AddObjectToObject(tool, "function") : NULL;
  cJSON *parameters = function != NULL
                        ? cJSON_AddObjectToObject(function, "parameters") : NULL;
  if (tool == NULL || function == NULL || parameters == NULL) {
    cJSON_Delete(tool);
    return NULL;
  }
  cJSON_AddStringToObject(tool, "type", "function");
  cJSON_AddStringToObject(function, "name", name);
  cJSON_AddStringToObject(function, "description", description);
  cJSON_AddStringToObject(parameters, "type", "object");
  cJSON_AddObjectToObject(parameters, "properties");
  cJSON_AddItemToArray(tools, tool);
  return function;
}

static int add_tool_definitions(cJSON *body, bool allow_motion)
{
  cJSON *tools = cJSON_AddArrayToObject(body, "tools");
  cJSON *function;
  cJSON *parameters;
  cJSON *properties;
  cJSON *field;
  cJSON *required;
  if (tools == NULL) return -ENOMEM;

  function = add_tool(tools, "robot_get_status", "Read the robot status.");
  if (function == NULL) return -ENOMEM;

  function = add_tool(tools, "robot_set_led", "Turn the status LED on or off.");
  if (function == NULL) return -ENOMEM;
  parameters = cJSON_GetObjectItemCaseSensitive(function, "parameters");
  properties = cJSON_GetObjectItemCaseSensitive(parameters, "properties");
  field = cJSON_AddObjectToObject(properties, "on");
  cJSON_AddStringToObject(field, "type", "boolean");
  required = cJSON_AddArrayToObject(parameters, "required");
  cJSON_AddItemToArray(required, cJSON_CreateString("on"));

  function = add_tool(tools, "robot_stop", "Stop robot motion immediately.");
  if (function == NULL) return -ENOMEM;
  parameters = cJSON_GetObjectItemCaseSensitive(function, "parameters");
  properties = cJSON_GetObjectItemCaseSensitive(parameters, "properties");
  field = cJSON_AddObjectToObject(properties, "emergency");
  cJSON_AddStringToObject(field, "type", "boolean");

  if (!allow_motion) return 0;

  function = add_tool(tools, "robot_perform_action",
                      "Run one predefined bounded robot action.");
  if (function == NULL) return -ENOMEM;
  parameters = cJSON_GetObjectItemCaseSensitive(function, "parameters");
  properties = cJSON_GetObjectItemCaseSensitive(parameters, "properties");
  field = cJSON_AddObjectToObject(properties, "action");
  cJSON_AddStringToObject(field, "type", "string");
  cJSON *values = cJSON_AddArrayToObject(field, "enum");
  const char *actions[] = {"happy", "sad", "confused", "greeting",
                           "thinking", "warning"};
  for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
    cJSON_AddItemToArray(values, cJSON_CreateString(actions[i]));
  field = cJSON_AddObjectToObject(properties, "intensity");
  cJSON_AddStringToObject(field, "type", "number");
  cJSON_AddNumberToObject(field, "minimum", 0.0);
  cJSON_AddNumberToObject(field, "maximum", 1.0);
  required = cJSON_AddArrayToObject(parameters, "required");
  cJSON_AddItemToArray(required, cJSON_CreateString("action"));
  return 0;
}

static char *serialize_bounded(cJSON *root)
{
  char *text = cJSON_PrintUnformatted(root);
  if (text == NULL) return NULL;
  if (strlen(text) > SMOKE_MAX_PAYLOAD) {
    cJSON_free(text);
    return NULL;
  }
  return text;
}

static int build_initial_request(const char *prompt, bool with_tools,
                                 bool allow_motion)
{
  cJSON *root = cJSON_CreateObject();
  cJSON *body = root != NULL ? cJSON_AddObjectToObject(root, "body") : NULL;
  cJSON *messages = body != NULL ? cJSON_AddArrayToObject(body, "messages") : NULL;
  cJSON *message = messages != NULL ? cJSON_CreateObject() : NULL;
  if (root == NULL || body == NULL || messages == NULL || message == NULL) {
    cJSON_Delete(message);
    cJSON_Delete(root);
    return -ENOMEM;
  }
  cJSON_AddStringToObject(root, "schema", "mibot.deepseek.smoke.v2");
  cJSON_AddStringToObject(root, "request_id", "sf32-smoke-1");
  cJSON_AddStringToObject(body, "model", "deepseek-chat");
  cJSON_AddNumberToObject(body, "max_tokens", 256);
  cJSON_AddStringToObject(message, "role", "user");
  cJSON_AddStringToObject(message, "content", prompt);
  cJSON_AddItemToArray(messages, message);
  if (with_tools && add_tool_definitions(body, allow_motion) < 0) {
    cJSON_Delete(root);
    return -ENOMEM;
  }
  char *serialized = serialize_bounded(root);
  cJSON_Delete(root);
  if (serialized == NULL) return -E2BIG;
  strcpy(g_request, serialized);
  cJSON_free(serialized);
  return 0;
}

static int send_ai_request(int fd, uint16_t *sequence, cJSON **response_root)
{
  uint16_t response_length = 0;
  int ret = send_frame(fd, SMOKE_AI_REQUEST, SMOKE_FLAG_ACK_REQUEST,
                       (*sequence)++, g_request);
  if (ret < 0) return ret;
  ret = wait_for_frame(fd, &g_parser, 25000, SMOKE_AI_RESPONSE, g_response,
                       &response_length);
  if (ret < 0) return ret;
  g_response[response_length] = '\0';
  *response_root = cJSON_Parse((const char *)g_response);
  if (*response_root == NULL) return -EINVAL;
  cJSON *ok = cJSON_GetObjectItemCaseSensitive(*response_root, "ok");
  return cJSON_IsTrue(ok) ? 0 : -EIO;
}

static bool tool_name_to_command(const char *tool_name, bool allow_motion,
                                 const char **command_name)
{
  if (tool_name == NULL || command_name == NULL) return false;
  if (strcmp(tool_name, "robot_get_status") == 0)
    *command_name = "robot.get_status";
  else if (strcmp(tool_name, "robot_set_led") == 0)
    *command_name = "robot.set_led";
  else if (strcmp(tool_name, "robot_stop") == 0)
    *command_name = "robot.stop";
  else if (allow_motion && strcmp(tool_name, "robot_perform_action") == 0)
    *command_name = "robot.perform_action";
  else
    return false;
  return true;
}

static int wait_command_reply(int fd, const char *command_id,
                              char *out, size_t out_size)
{
  int elapsed = 0;
  while (elapsed < 10000) {
    uint8_t type;
    uint16_t length;
    int ret = wait_next_frame(fd, &g_parser, 1000, &type, g_response, &length);
    elapsed += 1000;
    if (ret == -ETIMEDOUT) continue;
    if (ret < 0) return ret;
    if (type != SMOKE_ACK && type != SMOKE_NACK) continue;
    cJSON *reply = cJSON_Parse((const char *)g_response);
    const cJSON *id = reply != NULL
                        ? cJSON_GetObjectItemCaseSensitive(reply, "command_id") : NULL;
    const bool match = cJSON_IsString(id) &&
                       strcmp(id->valuestring, command_id) == 0;
    cJSON_Delete(reply);
    if (!match) continue;
    if ((size_t)length >= out_size) return -E2BIG;
    memcpy(out, g_response, length);
    out[length] = '\0';
    return type == SMOKE_ACK ? 0 : -ECANCELED;
  }
  return -ETIMEDOUT;
}

static int execute_tool_call(int fd, uint16_t *sequence, const cJSON *tool_call,
                             bool allow_motion, unsigned index,
                             struct tool_result_s *result)
{
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(tool_call, "id");
  const cJSON *function = cJSON_GetObjectItemCaseSensitive(tool_call, "function");
  const cJSON *name = function != NULL
                        ? cJSON_GetObjectItemCaseSensitive(function, "name") : NULL;
  const cJSON *arguments = function != NULL
                        ? cJSON_GetObjectItemCaseSensitive(function, "arguments") : NULL;
  const char *command_name = NULL;
  if (!cJSON_IsString(id) || !cJSON_IsString(name) ||
      !cJSON_IsString(arguments)) return -EINVAL;
  snprintf(result->id, sizeof(result->id), "%s", id->valuestring);
  if (!tool_name_to_command(name->valuestring, allow_motion, &command_name)) {
    snprintf(result->content, sizeof(result->content),
             "{\"ok\":false,\"error\":{\"code\":\"E_UNSUPPORTED\"}}");
    return 0;
  }

  cJSON *args = cJSON_Parse(arguments->valuestring);
  cJSON *command = cJSON_CreateObject();
  char command_id[64];
  snprintf(command_id, sizeof(command_id), "smoke-tool-%u", index);
  if (!cJSON_IsObject(args) || command == NULL) {
    cJSON_Delete(args);
    cJSON_Delete(command);
    snprintf(result->content, sizeof(result->content),
             "{\"ok\":false,\"error\":{\"code\":\"E_INVALID_ARG\"}}");
    return 0;
  }
  cJSON_AddStringToObject(command, "schema", "mibot.uart.v1");
  cJSON_AddStringToObject(command, "command_id", command_id);
  cJSON_AddStringToObject(command, "trace_id", "deepseek-smoke");
  cJSON_AddStringToObject(command, "name", command_name);
  cJSON_AddItemToObject(command, "args", args);
  char *wire = serialize_bounded(command);
  cJSON_Delete(command);
  if (wire == NULL) return -E2BIG;
  int ret = send_frame(fd, SMOKE_COMMAND, SMOKE_FLAG_ACK_REQUEST,
                       (*sequence)++, wire);
  cJSON_free(wire);
  if (ret < 0) return ret;
  ret = wait_command_reply(fd, command_id, result->content,
                           sizeof(result->content));
  if (ret == -ECANCELED) return 0;
  return ret;
}

static int build_followup_request(const char *prompt, const cJSON *first_response,
                                  const struct tool_result_s *results,
                                  size_t result_count, bool allow_motion)
{
  const cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(first_response,
                                                             "tool_calls");
  cJSON *root = cJSON_CreateObject();
  cJSON *body = root != NULL ? cJSON_AddObjectToObject(root, "body") : NULL;
  cJSON *messages = body != NULL ? cJSON_AddArrayToObject(body, "messages") : NULL;
  if (root == NULL || body == NULL || messages == NULL ||
      !cJSON_IsArray(tool_calls)) {
    cJSON_Delete(root);
    return -EINVAL;
  }
  cJSON_AddStringToObject(root, "schema", "mibot.deepseek.smoke.v2");
  cJSON_AddStringToObject(root, "request_id", "sf32-smoke-2");
  cJSON_AddStringToObject(body, "model", "deepseek-chat");
  cJSON_AddNumberToObject(body, "max_tokens", 256);

  cJSON *user = cJSON_CreateObject();
  cJSON_AddStringToObject(user, "role", "user");
  cJSON_AddStringToObject(user, "content", prompt);
  cJSON_AddItemToArray(messages, user);
  cJSON *assistant = cJSON_CreateObject();
  cJSON_AddStringToObject(assistant, "role", "assistant");
  cJSON_AddNullToObject(assistant, "content");
  cJSON_AddItemToObject(assistant, "tool_calls", cJSON_Duplicate(tool_calls, true));
  cJSON_AddItemToArray(messages, assistant);
  for (size_t i = 0; i < result_count; i++) {
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "role", "tool");
    cJSON_AddStringToObject(tool, "tool_call_id", results[i].id);
    cJSON_AddStringToObject(tool, "content", results[i].content);
    cJSON_AddItemToArray(messages, tool);
  }
  if (add_tool_definitions(body, allow_motion) < 0) {
    cJSON_Delete(root);
    return -ENOMEM;
  }
  char *serialized = serialize_bounded(root);
  cJSON_Delete(root);
  if (serialized == NULL) return -E2BIG;
  strcpy(g_request, serialized);
  cJSON_free(serialized);
  return 0;
}

static int run_ai_conversation(int fd, uint16_t *sequence, const char *prompt,
                               bool with_tools, bool allow_motion,
                               char *answer, size_t answer_size)
{
  cJSON *response = NULL;
  int ret = build_initial_request(prompt, with_tools, allow_motion);
  if (ret < 0) return ret;
  ret = send_ai_request(fd, sequence, &response);
  if (ret < 0) {
    if (response != NULL) {
      char *error = cJSON_PrintUnformatted(response);
      if (error != NULL) {
        fprintf(stderr, "deepseek_smoke: response=%s\n", error);
        cJSON_free(error);
      }
    }
    cJSON_Delete(response);
    return ret;
  }

  const cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(response,
                                                             "tool_calls");
  if (with_tools && cJSON_IsArray(tool_calls) &&
      cJSON_GetArraySize(tool_calls) > 0) {
    const int count = cJSON_GetArraySize(tool_calls);
    if (count > SMOKE_MAX_TOOL_CALLS) {
      cJSON_Delete(response);
      return -E2BIG;
    }
    struct tool_result_s results[SMOKE_MAX_TOOL_CALLS];
    memset(results, 0, sizeof(results));
    for (int i = 0; i < count; i++) {
      ret = execute_tool_call(fd, sequence, cJSON_GetArrayItem(tool_calls, i),
                              allow_motion, (unsigned)i + 1, &results[i]);
      if (ret < 0) {
        cJSON_Delete(response);
        return ret;
      }
    }
    ret = build_followup_request(prompt, response, results, (size_t)count,
                                 allow_motion);
    cJSON_Delete(response);
    response = NULL;
    if (ret < 0) return ret;
    ret = send_ai_request(fd, sequence, &response);
    if (ret < 0) {
      cJSON_Delete(response);
      return ret;
    }
    if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(response,
                                                       "tool_calls"))) {
      cJSON_Delete(response);
      return -ELOOP;
    }
  }

  const cJSON *text = cJSON_GetObjectItemCaseSensitive(response, "text");
  if (!cJSON_IsString(text) || text->valuestring[0] == '\0' ||
      strlen(text->valuestring) >= answer_size) {
    cJSON_Delete(response);
    return -E2BIG;
  }
  strcpy(answer, text->valuestring);
  cJSON_Delete(response);
  return 0;
}

static int send_audio_meta(int fd, uint16_t *sequence, unsigned stream,
                           bool eos)
{
  char meta[256];
  const int length = snprintf(meta, sizeof(meta),
      "{\"schema\":\"mibot.audio.v1\",\"stream_id\":\"smoke-%u\","
      "\"codec\":\"pcm_s16le\",\"sample_rate\":16000,\"channels\":1,"
      "\"frame_ms\":20,\"bytes\":640,\"eos\":%s}",
      stream, eos ? "true" : "false");
  if (length <= 0 || (size_t)length >= sizeof(meta)) return -EOVERFLOW;
  return send_frame(fd, SMOKE_AUDIO_UP, 0, (*sequence)++, meta);
}

static int capture_prompt(int fd, uint16_t *sequence, int seconds,
                          char *transcript, size_t transcript_size)
{
  uint8_t frame[SMOKE_AUDIO_FRAME_BYTES];
  const int target_frames = seconds * 50;
  int sent = 0;
  int ret = send_audio_meta(fd, sequence, 1, false);
  if (ret < 0) return ret;
  ret = board_audio_stream_start();
  if (ret < 0) return ret;
  while (sent < target_frames) {
    if (board_audio_stream_wait(200) < 0) continue;
    while (sent < target_frames &&
           board_audio_stream_read(frame, sizeof(frame)) == (int)sizeof(frame)) {
      ret = send_frame_bytes(fd, SMOKE_AUDIO_UP, 0, (*sequence)++, frame,
                             sizeof(frame));
      if (ret < 0) {
        board_audio_stream_stop();
        return ret;
      }
      sent++;
    }
  }
  board_audio_stream_stop();
  ret = send_audio_meta(fd, sequence, 1, true);
  if (ret < 0) return ret;

  for (int elapsed = 0; elapsed < 15000; elapsed += 1000) {
    uint8_t type;
    uint16_t length;
    ret = wait_next_frame(fd, &g_parser, 1000, &type, g_response, &length);
    if (ret == -ETIMEDOUT) continue;
    if (ret < 0) return ret;
    if (type != SMOKE_AI_RESPONSE) continue;
    cJSON *root = cJSON_Parse((const char *)g_response);
    const cJSON *schema = root != NULL
                            ? cJSON_GetObjectItemCaseSensitive(root, "schema") : NULL;
    const cJSON *text = root != NULL
                            ? cJSON_GetObjectItemCaseSensitive(root, "text") : NULL;
    const bool asr = cJSON_IsString(schema) &&
                     strcmp(schema->valuestring, "mibot.asr.v1") == 0 &&
                     cJSON_IsString(text) && text->valuestring[0] != '\0';
    if (asr && strlen(text->valuestring) < transcript_size)
      strcpy(transcript, text->valuestring);
    cJSON_Delete(root);
    if (asr) return 0;
  }
  return -ETIMEDOUT;
}

static int speak_and_play(int fd, uint16_t *sequence, const char *text)
{
  cJSON *command = cJSON_CreateObject();
  cJSON *args = command != NULL ? cJSON_AddObjectToObject(command, "args") : NULL;
  if (command == NULL || args == NULL) {
    cJSON_Delete(command);
    return -ENOMEM;
  }
  cJSON_AddStringToObject(command, "schema", "mibot.uart.v1");
  cJSON_AddStringToObject(command, "command_id", "smoke-speak-1");
  cJSON_AddStringToObject(command, "trace_id", "deepseek-voice-smoke");
  cJSON_AddStringToObject(command, "name", "robot.speak");
  cJSON_AddStringToObject(args, "text", text);
  cJSON_AddStringToObject(args, "voice", "default");
  cJSON_AddBoolToObject(args, "interruptible", true);
  char *wire = serialize_bounded(command);
  cJSON_Delete(command);
  if (wire == NULL) return -E2BIG;
  int ret = send_frame(fd, SMOKE_COMMAND, SMOKE_FLAG_ACK_REQUEST,
                       (*sequence)++, wire);
  cJSON_free(wire);
  if (ret < 0) return ret;

  bool playing = false;
  bool eos = false;
  for (int elapsed = 0; elapsed < 30000; elapsed += 1000) {
    uint8_t type;
    uint16_t length;
    ret = wait_next_frame(fd, &g_parser, 1000, &type, g_response, &length);
    if (ret == -ETIMEDOUT) continue;
    if (ret < 0) break;
    if (type == SMOKE_AUDIO_DOWN) {
      if (length == SMOKE_AUDIO_FRAME_BYTES) {
        if (!playing) {
          ret = board_audio_play_start();
          if (ret < 0) break;
          playing = true;
        }
        if (board_audio_play_write(g_response, length) < 0) {
          ret = -EIO;
          break;
        }
      } else if (length > 0 && g_response[0] == '{') {
        cJSON *meta = cJSON_Parse((const char *)g_response);
        const cJSON *end = meta != NULL
                             ? cJSON_GetObjectItemCaseSensitive(meta, "eos") : NULL;
        if (cJSON_IsTrue(end)) eos = true;
        cJSON_Delete(meta);
        if (eos && playing) {
          board_audio_play_drain(1000);
          board_audio_play_stop();
          playing = false;
        }
      }
      continue;
    }
    if (type == SMOKE_NACK) {
      ret = -ECANCELED;
      break;
    }
    if (type == SMOKE_ACK) {
      cJSON *reply = cJSON_Parse((const char *)g_response);
      const cJSON *id = reply != NULL
                          ? cJSON_GetObjectItemCaseSensitive(reply, "command_id") : NULL;
      const cJSON *state = reply != NULL
                          ? cJSON_GetObjectItemCaseSensitive(reply, "state") : NULL;
      const bool done = cJSON_IsString(id) &&
                        strcmp(id->valuestring, "smoke-speak-1") == 0 &&
                        cJSON_IsString(state) &&
                        strcmp(state->valuestring, "completed") == 0;
      cJSON_Delete(reply);
      if (done) {
        ret = 0;
        if (!playing) return ret;
      }
    }
  }
  if (playing) {
    board_audio_play_drain(1000);
    board_audio_play_stop();
  }
  return ret < 0 ? ret : (eos ? 0 : -ETIMEDOUT);
}
