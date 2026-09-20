#pragma once

/* Shared AA55 message identifiers and flag bits used by every ESP32 link
 * producer.  Keep these values in one header so audio, AI responses and
 * robot-control traffic cannot silently drift apart. */
enum {
  MIBOT_MSG_HELLO = 0x01,
  MIBOT_MSG_HELLO_ACK = 0x02,
  MIBOT_MSG_COMMAND = 0x10,
  MIBOT_MSG_ACK = 0x11,
  MIBOT_MSG_NACK = 0x12,
  MIBOT_MSG_EVENT = 0x20,
  MIBOT_MSG_TELEMETRY = 0x21,
  MIBOT_MSG_AUDIO_UP = 0x30,
  MIBOT_MSG_AUDIO_DOWN = 0x31,
  MIBOT_MSG_AI_REQUEST = 0x40,
  MIBOT_MSG_AI_RESPONSE = 0x41,
  MIBOT_MSG_PING = 0x60,
  MIBOT_MSG_PONG = 0x61,
};

enum {
  MIBOT_FLAG_ACK_REQUEST = 1u << 0,
  MIBOT_FLAG_RESPONSE = 1u << 1,
  MIBOT_FLAG_ERROR = 1u << 2,
};

#define MIBOT_SCHEMA_ASR_V1 "mibot.asr.v1"
#define MIBOT_ASR_EVENT_TEXT "asr_text"

