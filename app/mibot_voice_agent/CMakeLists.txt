# SPDX-License-Identifier: Apache-2.0

if(CONFIG_EXAMPLES_MIBOT_AGENT)
  nuttx_add_application(
    MODULE ${CONFIG_EXAMPLES_MIBOT_AGENT}
    NAME ${CONFIG_EXAMPLES_MIBOT_AGENT_PROGNAME}
    SRCS mibot_agent_main.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}
    STACKSIZE ${CONFIG_EXAMPLES_MIBOT_AGENT_STACKSIZE}
    PRIORITY ${CONFIG_EXAMPLES_MIBOT_AGENT_PRIORITY})

  nuttx_add_application(
    MODULE sf32_audio_test
    NAME sf32_audio_test
    SRCS sf32_audio_test.c
    STACKSIZE 4096
    PRIORITY 100)

  # Hardware-free checks for the padded/dense frame helpers in sf32lb_i2s.c.
  nuttx_add_application(
    MODULE audio_pbt
    NAME audio_pbt
    SRCS audio_pbt.c
    STACKSIZE 4096
    PRIORITY 100)

  if(CONFIG_EXAMPLES_MIBOT_ROUTE_A_PPP)
    nuttx_add_application(
      MODULE mibot_pppd
      NAME mibot_pppd
      SRCS mibot_pppd.c
      STACKSIZE 8192
      PRIORITY 110)
  endif()
endif()

if(CONFIG_EXAMPLES_DEEPSEEK_SMOKE)
  nuttx_add_application(
    MODULE deepseek_smoke
    NAME deepseek_smoke
    SRCS deepseek_smoke.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}
                        ${CMAKE_SOURCE_DIR}/../nuttx-apps/netutils/cjson/cJSON
    DEPENDS cjson
    STACKSIZE 12288
    PRIORITY 110)
endif()

if(CONFIG_EXAMPLES_MIBOT_VOICE_AGENT)
  # Main voice-agent task: pure-logic core + device glue + stubs + arg contract.
  # The ASR/TTS stub switches must be defined for EVERY translation unit (not
  # just the main), so va_stubs.c takes its stub branches too (asr_transcribe
  # returns the fixed transcript, tts_request accepts).  Defining them only in
  # the main file leaves va_stubs.c compiled as the "real placeholder" that
  # returns E_UNIMPLEMENTED / empty transcript.
  #
  # MIBOT_VA_REAL_SPEECH=y selects the real speech path instead: the transcript
  # comes from the cloud recogniser relayed by the ESP32 as `mibot.asr.v1`, and
  # SPEAKING sends `robot.speak` and plays the returned AUDIO_DOWN PCM.  That
  # build requires a configured cloud audio endpoint on the ESP32.
  if(MIBOT_VA_REAL_SPEECH)
    set(VA_SPEECH_DEFINITIONS "")
    message(STATUS "mibot voice agent: real cloud ASR/TTS path")
  else()
    set(VA_SPEECH_DEFINITIONS MIBOT_VA_ASR_STUB MIBOT_VA_TTS_STUB)
    message(STATUS "mibot voice agent: all-stub ASR/TTS path")
  endif()

  nuttx_add_application(
    MODULE ${CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_PROGNAME}
    NAME ${CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_PROGNAME}
    SRCS mibot_voice_agent_main.c
         mibot_voice_agent.c
         va_face.c
         va_stubs.c
         va_test_args.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}
                        ${CMAKE_SOURCE_DIR}/../nuttx-apps/netutils/cjson/cJSON
    DEFINITIONS ${VA_SPEECH_DEFINITIONS}
    DEPENDS cjson
    STACKSIZE ${CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_STACKSIZE}
    PRIORITY ${CONFIG_EXAMPLES_MIBOT_VOICE_AGENT_PRIORITY})

  # Standalone single-hardware test command (mic/spk/motor/servo).  Needs a
  # comfortable stack for the frame/reply/mic buffers held on the stack.
  nuttx_add_application(
    MODULE va_test
    NAME va_test
    SRCS va_test.c
         va_test_args.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}
    STACKSIZE 8192
    PRIORITY 100)

  # `va_wake` trigger command: requests one dialog turn.  Its own tiny source
  # latches the agent's shared wake flag, so it must not recompile the agent
  # main (which would duplicate main()).
  nuttx_add_application(
    MODULE va_wake
    NAME va_wake
    SRCS va_wake.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}
    STACKSIZE 2048
    PRIORITY 100)
endif()
