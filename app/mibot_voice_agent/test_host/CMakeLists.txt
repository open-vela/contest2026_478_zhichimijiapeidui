# SPDX-License-Identifier: Apache-2.0
#
# Host test project for the Mibot Voice Agent pure-logic core.
#
# This is a standalone CMake project (not part of the NuttX build). It compiles
# the hardware-agnostic state machine core with plain gcc/clang so the logic can
# be exercised off-target, and registers CTest targets.
#
# Only the pure-logic sources are built here. Device-only files (va_test.c and
# the future UART/LCD/board_audio glue) are excluded because they are guarded
# behind `#if defined(__NuttX__)` and pull in NuttX/board headers.
#
# Usage:
#   cmake -S . -B build
#   cmake --build build
#   ctest --test-dir build --output-on-failure

cmake_minimum_required(VERSION 3.13)
project(mibot_voice_agent_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

# Path to the app sources one directory up.
set(VA_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Pure-logic sources that compile on host without NuttX/board dependencies.
set(VA_CORE_SOURCES
    "${VA_APP_DIR}/mibot_voice_agent.c"
    "${VA_APP_DIR}/va_stubs.c"
    "${VA_APP_DIR}/va_test_args.c"
)

# Library holding the portable core so tests can link against it.
add_library(mibot_va_core STATIC ${VA_CORE_SOURCES})
target_include_directories(mibot_va_core PUBLIC "${VA_APP_DIR}")

# The host build runs the full all-stub pipeline: define both stub switches so
# asr_transcribe()/tts_request() take their stub branches (需求 9.1, 9.2).
target_compile_definitions(mibot_va_core PUBLIC MIBOT_VA_ASR_STUB MIBOT_VA_TTS_STUB)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mibot_va_core PRIVATE -Wall -Wextra)
endif()

# --- Tests ----------------------------------------------------------------

add_executable(test_va_state_machine test_va_state_machine.c)
target_link_libraries(test_va_state_machine PRIVATE mibot_va_core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_va_state_machine PRIVATE -Wall -Wextra)
endif()

add_test(NAME va_state_machine COMMAND test_va_state_machine)

add_executable(test_va_stubs test_va_stubs.c)
target_link_libraries(test_va_stubs PRIVATE mibot_va_core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_va_stubs PRIVATE -Wall -Wextra)
endif()

add_test(NAME va_stubs COMMAND test_va_stubs)

add_executable(test_va_properties test_va_properties.c)
target_link_libraries(test_va_properties PRIVATE mibot_va_core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_va_properties PRIVATE -Wall -Wextra)
endif()

add_test(NAME va_properties COMMAND test_va_properties)

add_executable(test_va_test_args test_va_test_args.c)
target_link_libraries(test_va_test_args PRIVATE mibot_va_core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_va_test_args PRIVATE -Wall -Wextra)
endif()

add_test(NAME va_test_args COMMAND test_va_test_args)

# ToF safety-gate model (P14).  This does NOT link mibot_va_core: it exercises a
# host-portable transcription of the ESP32 ToF safety gate, which is otherwise
# ESP-IDF-only and cannot be compiled on host.
add_executable(test_tof_safety test_tof_safety.c tof_safety_model.c)
target_include_directories(test_tof_safety PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_tof_safety PRIVATE -Wall -Wextra)
endif()

add_test(NAME tof_safety COMMAND test_tof_safety)

# TOF050C (VL6180X) decode logic.  This one compiles the *production* source
# from the ESP32 tree rather than a transcription: mibot_vl6180x.c is kept free
# of ESP-IDF includes precisely so the shipped decode logic can be tested here.
# It also links tof_safety_model so a decoded sample can be pushed through the
# gate, checking the driver and the gate agree on what an edge means.
set(ESP32_MAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../esp32s3/mibot_esp32s3/main")
add_executable(test_vl6180x_decode
    test_vl6180x_decode.c
    tof_safety_model.c
    "${ESP32_MAIN_DIR}/mibot_vl6180x.c"
)
target_include_directories(test_vl6180x_decode PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ESP32_MAIN_DIR}"
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_vl6180x_decode PRIVATE -Wall -Wextra)
endif()

add_test(NAME vl6180x_decode COMMAND test_vl6180x_decode)

# LCD face geometry + blink/breathing schedule.  va_face.c is deliberately free
# of NuttX and framebuffer headers so it can be compiled here unchanged.
add_executable(test_va_face test_va_face.c "${VA_APP_DIR}/va_face.c")
target_include_directories(test_va_face PRIVATE "${VA_APP_DIR}")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_va_face PRIVATE -Wall -Wextra)
endif()

add_test(NAME va_face COMMAND test_va_face)

# Execution-mode properties (P7/P10).  The mode is a compile-time switch
# (MIBOT_VA_CONCURRENT), so the same test source is built and run twice: once
# against the default (Sequential) core, once against a Concurrent core, to
# genuinely exercise both modes and prove their state set / edges are identical.
add_executable(test_va_modes_sequential test_va_modes.c)
target_link_libraries(test_va_modes_sequential PRIVATE mibot_va_core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_va_modes_sequential PRIVATE -Wall -Wextra)
endif()
add_test(NAME va_modes_sequential COMMAND test_va_modes_sequential)

# Concurrent core: same portable sources, compiled with MIBOT_VA_CONCURRENT.
add_library(mibot_va_core_concurrent STATIC ${VA_CORE_SOURCES})
target_include_directories(mibot_va_core_concurrent PUBLIC "${VA_APP_DIR}")
target_compile_definitions(mibot_va_core_concurrent PUBLIC
    MIBOT_VA_ASR_STUB MIBOT_VA_TTS_STUB MIBOT_VA_CONCURRENT)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mibot_va_core_concurrent PRIVATE -Wall -Wextra)
endif()

add_executable(test_va_modes_concurrent test_va_modes.c)
target_link_libraries(test_va_modes_concurrent PRIVATE mibot_va_core_concurrent)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_va_modes_concurrent PRIVATE -Wall -Wextra)
endif()
add_test(NAME va_modes_concurrent COMMAND test_va_modes_concurrent)
