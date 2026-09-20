/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mibot Voice Agent - `va_wake` NSH command (需求 3.5).
 *
 * Requests one dialog turn from the running mibot_voice_agent task.  In the
 * NuttX flat build the command shares the address space with the agent task,
 * so it simply latches the agent's wake-request flag; the agent's main loop
 * consumes it and posts VA_EVENT_WAKE (the core enforces the IDLE->LISTENING
 * cloud gate, so an offline/mid-turn trigger is safely ignored).
 *
 * This is the first-version trigger entry point.  A future offline wake-word
 * detected on the ESP32 relays an EVENT that the agent turns into the same
 * va_post_event(VA_EVENT_WAKE) (task 14); this manual command remains useful
 * for bring-up and 联调.
 *
 * Device-only: guarded by `#if defined(__NuttX__)` so the host test project
 * never compiles it.
 */

#if defined(__NuttX__)

#include <stdbool.h>
#include <stdio.h>

/* Latched by this command, polled by the agent main loop (mibot_voice_agent_
 * main.c).  Shared in the flat-build address space. */
extern volatile bool g_va_wake_request;

int va_wake_main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  g_va_wake_request = true;
  printf("va_wake: dialog trigger requested\n");
  return 0;
}

#endif /* __NuttX__ */
