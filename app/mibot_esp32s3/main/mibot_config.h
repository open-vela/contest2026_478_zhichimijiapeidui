#pragma once

#if __has_include("mibot_secrets.h")
#include "mibot_secrets.h"
#endif

#include "mibot_protocol.h"

// Recommended map for an ESP32-S3-WROOM-1-N16R8 module.
// Confirm that the particular carrier board exposes these pins.
#define MIBOT_I2C_PORT I2C_NUM_0
#define MIBOT_I2C_SDA GPIO_NUM_8
#define MIBOT_I2C_SCL GPIO_NUM_9

#define MIBOT_MOTOR_AIN1 GPIO_NUM_4
#define MIBOT_MOTOR_AIN2 GPIO_NUM_5
#define MIBOT_MOTOR_PWMA GPIO_NUM_6
#define MIBOT_MOTOR_BIN1 GPIO_NUM_7
/* Confirmed on hardware, both wheels turn on this map.  Motor B was dead for a
 * while with motor A working, which looked like a wrong pin number here (the
 * old Arduino sketch used 15/16 for BIN2/PWMB while agreeing with 4/5/6 for the
 * A channel).  It was the wiring, not these values -- do not "fix" them to
 * 15/16.  The A channel running proves STBY, VM, VCC and ground are all fine,
 * so if one wheel stops again, suspect that channel's three signal wires or its
 * BO1/BO2 pair before touching this table. */
#define MIBOT_MOTOR_BIN2 GPIO_NUM_10
#define MIBOT_MOTOR_PWMB GPIO_NUM_11
#define MIBOT_MOTOR_STBY GPIO_NUM_12

#define MIBOT_SERVO_LEFT GPIO_NUM_13
#define MIBOT_SERVO_RIGHT GPIO_NUM_14

#define MIBOT_SF32_UART UART_NUM_2
#define MIBOT_SF32_RX GPIO_NUM_2
#define MIBOT_SF32_TX GPIO_NUM_1
#define MIBOT_SF32_BAUD 1000000

// The carrier board routes GPIO48 to its onboard WS2812B status LED.
#define MIBOT_STATUS_LED_GPIO GPIO_NUM_48

// Wi-Fi credentials belong in the ignored mibot_secrets.h. Empty defaults
// keep source-only and CI builds reproducible without embedding credentials.
#ifndef MIBOT_WIFI_STA_SSID
#define MIBOT_WIFI_STA_SSID ""
#endif
#ifndef MIBOT_WIFI_STA_PASSWORD
#define MIBOT_WIFI_STA_PASSWORD ""
#endif
#define MIBOT_WIFI_TCP_PORT 3333

// --- TOF050C (VL6180X) ranging front end ---------------------------------
//
// Four identical modules share one I2C bus.  They all boot at the factory
// address, and the VL6180X address register is volatile, so each module needs
// its own XSHUT line: bring-up holds the others in reset while it reassigns one.
// Confirm the carrier board actually breaks these out before wiring.
#define MIBOT_TOF_XSHUT_0 GPIO_NUM_15  // front_left
#define MIBOT_TOF_XSHUT_1 GPIO_NUM_16  // front_right
#define MIBOT_TOF_XSHUT_2 GPIO_NUM_17  // rear_left
#define MIBOT_TOF_XSHUT_3 GPIO_NUM_18  // rear_right

// Runtime 7-bit addresses.  The factory default 0x29 is deliberately left out:
// a module that browns out reverts to it, so keeping it unassigned lets the
// recovery path tell "lost its address" apart from "healthy".
#define MIBOT_TOF_ADDR_0 0x2A
#define MIBOT_TOF_ADDR_1 0x2B
#define MIBOT_TOF_ADDR_2 0x2C
#define MIBOT_TOF_ADDR_3 0x2D

// Continuous ranging period.  Must stay below the safety loop's 50 ms so every
// poll finds a fresh sample without blocking on convergence.
#define MIBOT_TOF_RANGE_PERIOD_MS 30
// A sample older than this is reported as unusable rather than reused.
#define MIBOT_TOF_SAMPLE_MAX_AGE_MS 200
// Lower bound between recovery attempts for a channel that stopped answering.
#define MIBOT_TOF_RECOVERY_INTERVAL_MS 2000
#define MIBOT_TOF_I2C_TIMEOUT_MS 20

// Downward-facing front sensors: a reading past this distance means the surface
// fell away.  A VL6180X saturates at 255 mm, which is well clear of the desk
// standoff, so a drop-off is unambiguous.
#define MIBOT_EDGE_THRESHOLD_MM 50
#define MIBOT_TOF_TIMEOUT_MS 250
#define MIBOT_MAX_FRAME_PAYLOAD 4096

// Audio_Link wire format and bounded buffering.
#define MIBOT_AUDIO_SAMPLE_RATE 16000
#define MIBOT_AUDIO_CHANNELS 1
#define MIBOT_AUDIO_FRAME_MS 20
#define MIBOT_AUDIO_FRAME_SAMPLES 320
#define MIBOT_AUDIO_FRAME_BYTES 640
#define MIBOT_AUDIO_FRAME_OVERHEAD 11
#define MIBOT_AUDIO_FPS 50
#define MIBOT_AUDIO_UP_DEPTH_PSRAM 25
#define MIBOT_AUDIO_UP_DEPTH_SRAM 10
#define MIBOT_AUDIO_DN_DEPTH_PSRAM 25
#define MIBOT_AUDIO_DN_DEPTH_SRAM 15
#define MIBOT_AUDIO_PREFILL_DEPTH 3
// Loopback uses a deeper prefill to absorb host-side scheduling jitter while
// leaving headroom in the 25-frame downlink ring for incoming PCM.
#define MIBOT_AUDIO_LOOPBACK_PREFILL_DEPTH 15
#define MIBOT_AUDIO_CLOUD_CHUNK_QUEUE_DEPTH 4
#define MIBOT_AUDIO_UPLINK_CHUNK_FRAMES 5
#define MIBOT_AUDIO_UPLINK_CHUNK_BYTES 3200
#define MIBOT_AUDIO_UP_SILENCE_MS 500
// WebSocket send timeout.  This is NOT just "how long we are willing to wait":
// esp_websocket_client passes it to transport_poll_write(), and a poll that
// times out makes esp_transport_write() return 0, which the client treats as a
// fatal transport error and tears the whole session down (observed as the
// gateway logging close 1006 mid-utterance, with errno=0 and
// transport_error=ESP_OK -- nothing was actually broken).
//
// The uplink pushes 3200 B every 100 ms, so ordinary TCP backpressure is enough
// to trip it: the measured failure was a send that needed 231 ms against the old
// 200 ms budget.  Keep this well above normal Wi-Fi jitter.  The ceiling is the
// SF32's ASR budget (VA_ASR_TIMEOUT_MS, 15 s), which a stalled uplink eats into
// before eos arrives.
//
// 1500 ms still lost sessions, and only just: the failure logged "wrote 0/3200
// in 1531 ms (timeout 1500 ms)", i.e. it gave up 31 ms short.  The gateway was
// fixed not to block its receive loop during TTS, which cut how often this
// happens, but a blocked write is a normal consequence of TCP backpressure and
// should be waited out rather than escalated into dropping the session.  4000 ms
// leaves a 2.6x margin over the longest stall seen and still fits inside the ASR
// budget alongside an 8 s capture.
// Must match the duplicate definition in mibot_audio.h.
#define MIBOT_AUDIO_CLOUD_SEND_TIMEOUT_MS 4000
// How long to wait for the first TTS chunk before aborting the stream.
// 5 s was too tight: edge-tts needs a network round trip plus an ffmpeg decode,
// and a long reply measured 6 s from request to first audio.  The stream was
// aborted with E_CLOUD_TIMEOUT and the whole reply thrown away moments before it
// arrived -- which also faked a clean underrun=0, because nothing ever played.
#define MIBOT_AUDIO_TTS_FIRST_CHUNK_TIMEOUT_MS 20000
#define MIBOT_AUDIO_CLOUD_CHUNK_MAX_PSRAM 4096
#define MIBOT_AUDIO_CLOUD_CHUNK_MAX_SRAM 2048
#define MIBOT_AUDIO_RECONNECT_MIN_MS 1000
#define MIBOT_AUDIO_RECONNECT_MAX_MS 30000
#define MIBOT_AUDIO_SEQ_GAP_MAX 64
#define MIBOT_AUDIO_SPEAK_TEXT_MAX_CP 500
#define MIBOT_AUDIO_STREAM_ID_MAX 31
#define MIBOT_AUDIO_TX_LOCK_BUDGET_MS 8
#define MIBOT_AUDIO_ABORT_BUDGET_MS 40
#define MIBOT_AUDIO_DOWN_BYTES_PER_SEC 32550
#define MIBOT_AUDIO_FIRST_FRAME_BUDGET_MS 150
#define MIBOT_AUDIO_CMD_CACHE_SLOTS 8
// Keep high-cardinality audio diagnostics out of periodic TELEMETRY in the
// production image.  Enable this temporarily for stage-1 timing/heap traces.
/* Was temporarily 1 to chase the playback dropouts: it puts the detailed audio
 * counters (downlink_dropped, audio_down_frames_sent, audio_down_write_error,
 * downlink_underrun) into the periodic TELEMETRY frame, which the SF32 prints
 * when built with VA_LOG_TELEMETRY.  Back to 0: those counters now come from the
 * one-per-stream "AUDIO_DOWN eos" log line instead, which keeps the periodic
 * TELEMETRY frame small.  TELEMETRY shares the downlink UART with the 50 fps
 * audio stream, so its size is not free while a reply is playing. */
#define MIBOT_AUDIO_DIAG_TRACE 0
#define MIBOT_COMMAND_ID_MAX 64
// Decoder and resampler are optional in the first firmware image.
#define MIBOT_AUDIO_ENABLE_DECODER 0
#define MIBOT_AUDIO_ENABLE_RESAMPLER 0

// Cloud credentials are read from mibot_secrets.h and never arrive over UART.

// Simulated ToF.  DANGEROUS as a "disable" switch: it makes tof_read_mm()
// return ESP_OK with a fixed 30 mm / quality 90, i.e. the safety layer believes
// all four channels see clear floor and forward/turn motion is UNLOCKED on
// fabricated data.  Only for bench work on the decision logic itself.
#define MIBOT_SIMULATE_TOF 0

// Is the ToF hardware actually wired up?
//
// Set to 0 when the four TOF050C modules are not fitted.  That skips bring-up
// and the 2 s recovery retries, whose per-channel warnings otherwise flood the
// console and bury everything else in the log.  Readings stay INVALID, so
// forward/turn motion stays locked (需求 9.4) exactly as it would with four
// dead sensors -- this is a "stop polling absent hardware" switch, not a
// "pretend the path is clear" switch (that is MIBOT_SIMULATE_TOF above).
#define MIBOT_TOF_PRESENT 0

// ###########################################################################
// # DANGER: set to 1 to restore obstacle/edge safety.  At 0 the chassis will #
// # accept motion with NO obstacle detection and NO drop-off detection.      #
// ###########################################################################
//
// Turned off deliberately for voice-loop bring-up on a bench with no ToF
// modules fitted: without this, every bounded action's motor step is refused
// with E_TOF_INVALID, which is correct but noisy while working on the dialog.
//
// What stops working when this is 0:
//   * E_TOF_INVALID is never returned, so motion runs on unknown surroundings.
//   * front_edge_locked() is ignored, so driving off a desk is not prevented.
//   * safety_task no longer brakes a running motor when readings go invalid.
// The SAFE_STOP path, robot.stop and the Fault/Braking locks are NOT affected.
//
// Put this back to 1 before the robot runs anywhere it can fall or hit
// something.
#define MIBOT_TOF_SAFETY_ENFORCE 0
