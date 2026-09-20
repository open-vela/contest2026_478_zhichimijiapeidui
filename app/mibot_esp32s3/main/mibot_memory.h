#pragma once

/*
 * mibot_memory.h — on-board short-term and long-term memory for the board-side
 * piagent gateway (方案 A of board-piagent-design.md).
 *
 * Short-term memory : a small SRAM ring of recent (role, text) turns that is
 *                     spliced into every LLM request so MiMo sees context.
 * Long-term memory   : facts persisted to NVS, only written when a *new* fact
 *                      is identified by the model ("[MEM] " marker), which keeps
 *                      flash wear proportional to real knowledge, not to turns.
 *
 * This component is deliberately independent of the DeepSeek/MiMo gateway:
 * it exposes plain C strings and a cJSON append helper, and owns the NVS
 * namespace.  The gateway (deepseek_gateway.cpp) is the only caller.
 */

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Short-term memory sizing.  ~20 turns x 256 B of text ≈ 5 KB of SRAM, well
 * within the ESP32-S3 budget, and small enough to fit alongside the 4 KiB
 * request frame after history is spliced. */
#define MIBOT_MEM_MAX_TURNS      20
#define MIBOT_MEM_TURN_TEXT_MAX  256
#define MIBOT_MEM_FACTS_MAX      64   /* max persisted facts in NVS        */
#define MIBOT_MEM_FACT_TEXT_MAX  192  /* max bytes per single fact         */

/* Initialise memory: clears short-term ring, loads persisted facts from NVS.
 * Returns true when long-term state was restored from flash. */
bool mibot_memory_init(void);

/* Push one spoken turn (role "user" or "assistant") into the short-term ring.
 * The oldest turn is dropped when the ring is full. */
void mibot_memory_push(const char *role, const char *text);

/* Append the memory context to an existing cJSON "messages" array.  The items
 * are appended in order: {facts block}, then {short-term turns oldest -> newest}.
 * The gateway places this block between the system/persona message and the
 * current user turn, so the final order is
 *   [ {system: persona}, {facts + turns}, {current request} ].
 *
 * budget_bytes bounds a pessimistic serialised-size estimate of the appended
 * block so the rebuilt request body stays inside REQUEST_MAX (4 KiB).  When
 * there is not room for everything, the oldest turns are dropped first
 * (newest turns and the facts block are kept).  Returns items appended. */
int mibot_memory_append_context(cJSON *messages, size_t budget_bytes);

/* Suggest a fact for long-term persistence.  The caller (gateway) feeds this
 * with text extracted from an LLM reply that carried a "[MEM] " marker.  The
 * fact is deduplicated against the in-memory mirror; when it is genuinely new
 * the dirty flag is raised and the next NVS flush persists it.  Returns true
 * when the fact was accepted as new. */
bool mibot_memory_remember(const char *fact);

/* Flush any pending new facts to NVS (incremental, only writes when dirty).
 * Prunes to MIBOT_MEM_FACTS_MAX / MIBOT_MEM_FACT_TEXT_MAX.  Called from the
 * gateway's LLM worker at a safe point after response delivery.  Returns true
 * when a write actually happened. */
bool mibot_memory_flush_nvs(void);

/* Drop all short-term memory (a "clear session" affordance; long-term facts
 * are kept unless erase_facts is true). */
void mibot_memory_clear(bool erase_facts);

/* Diagnostics for telemetry / debug logs. */
size_t mibot_memory_turns(void);
size_t mibot_memory_facts(void);

#ifdef __cplusplus
}
#endif