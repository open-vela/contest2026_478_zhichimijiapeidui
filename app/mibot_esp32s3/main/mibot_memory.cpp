/*
 * mibot_memory.cpp — on-board short/long-term memory for the piagent gateway.
 *
 * Short-term ring: fixed SRAM arrays that hold the most recent
 * (role, text) turns so every LLM request carries conversation context.
 *
 * Long-term NVS: a JSON array of "facts".  The gateway only calls
 * mibot_memory_remember() when the LLM reply carried a "[MEM] " marker, so the
 * NVS write count is proportional to real knowledge changes, not turn volume.
 * mibot_memory_flush_nvs() performs the actual write, called at a safe point
 * after the AI_RESPONSE has been delivered to the SF32.
 *
 * The NVS namespace is owned by this component.  nvs_flash_init() is performed
 * by mibot_controller.cpp's init_wifi_sta(); the defaults partition supports
 * arbitrarily many namespaces.
 */

#include "mibot_memory.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cstring>
#include <new>
#include <string>

namespace {

constexpr char TAG[] = "mibot_mem";
constexpr char NVS_NAMESPACE[] = "piagent_mem";
constexpr char NVS_KEY_FACTS[] = "facts";

/* In-memory mirror of the long-term facts. */
struct ShortTurn {
  char role[16]; /* "user" or "assistant" (+NUL) — must fit "assistant"  */
  char text[MIBOT_MEM_TURN_TEXT_MAX + 1];
};

struct MemoryState {
  ShortTurn turns[MIBOT_MEM_MAX_TURNS];
  size_t turn_count = 0;       /* items currently in the ring         */
  size_t turn_head = 0;        /* index of the oldest item            */
  char facts[MIBOT_MEM_FACTS_MAX][MIBOT_MEM_FACT_TEXT_MAX + 1];
  size_t fact_count = 0;
  bool facts_dirty = false;    /* new facts pending NVS flush         */
  bool restored = false;       /* long-term facts loaded from flash   */
};

MemoryState g_mem;

const char *s_role(const char *role) {
  if (role == nullptr) return "user";
  return (strcmp(role, "assistant") == 0) ? "assistant" : "user";
}

/* Copy `in` to `out` (max out_cap) as well-formed UTF-8.  A MiMo reply can
 * occasionally carry malformed bytes (a reply truncated mid multi-byte char,
 * stray control bytes); if those are re-serialized verbatim into the next
 * request the API answers 400 "Invalid JSON", so anything we persist to the
 * ring or NVS is sanitized here first: invalid sequences and C0 controls
 * (other than tab/CR/LF) become '?'. */
static size_t sanitize_utf8(const char *in, char *out, size_t out_cap) {
  if (out_cap == 0) return 0;
  size_t w = 0;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(in);
  while (*p != '\0' && w + 1 < out_cap) {
    const unsigned char c = p[0];
    size_t len = 0;
    if (c < 0x80) {
      /* ASCII: let tab/CR/LF through; other C0 controls are invalid in a
       * JSON string unless escaped, so treat them as corruption. */
      len = (c < 0x20 && c != '\t' && c != '\r' && c != '\n') ? 0 : 1;
    } else if ((c & 0xE0) == 0xC0) {
      if (c >= 0xC2) len = 2; /* reject overlong C0/C1 as 0xC0/0xC1 */
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      if (c <= 0xF4) len = 4; /* reject > U+10FFFF */
    }
    bool ok = len != 0;
    if (ok) {
      for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) ok = false;
      }
      if (ok && len == 3 && (c & 0x0F) == 0x00 && p[1] < 0xA0) ok = false; /* E0 overlong */
      if (ok && len == 4 && c == 0xF0 && p[1] < 0x90) ok = false;         /* F0 overlong */
      if (ok && len == 3 && c == 0xED && p[1] > 0x9F) ok = false;         /* UTF-16 surrogate */
      if (ok && len == 4 && c == 0xF4 && p[1] > 0x8F) ok = false;         /* > U+10FFFF */
    }
    if (ok) {
      for (size_t i = 0; i < len && w < out_cap - 1; i++) out[w++] = static_cast<char>(p[i]);
      p += len;
    } else {
      if (c == '\t' || c == '\r' || c == '\n') {
        out[w++] = static_cast<char>(c);
      } else {
        out[w++] = '?';
      }
      p += 1;
    }
  }
  out[w] = '\0';
  return w;
}

bool fact_present(const char *fact) {
  for (size_t i = 0; i < g_mem.fact_count; i++) {
    if (strcmp(g_mem.facts[i], fact) == 0) return true;
  }
  return false;
}

bool load_facts_from_nvs() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGW(TAG, "NVS open (read) failed; starting empty");
    return false;
  }

  char *blob = nullptr;
  size_t length = 0;
  bool ok = false;

  if (nvs_get_str(handle, NVS_KEY_FACTS, nullptr, &length) == ESP_OK &&
      length > 8) {
    blob = new (std::nothrow) char[length];
    if (blob != nullptr &&
        nvs_get_str(handle, NVS_KEY_FACTS, blob, &length) == ESP_OK) {
      cJSON *root = cJSON_Parse(blob);
      if (cJSON_IsArray(root)) {
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, root) {
          if (!cJSON_IsString(item) || item->valuestring == nullptr) continue;
          if (g_mem.fact_count >= MIBOT_MEM_FACTS_MAX) break;
          const int len = static_cast<int>(strlen(item->valuestring));
          if (len == 0 || len > MIBOT_MEM_FACT_TEXT_MAX) continue;
          if (fact_present(item->valuestring)) continue;
          strncpy(g_mem.facts[g_mem.fact_count], item->valuestring,
                  MIBOT_MEM_FACT_TEXT_MAX);
          g_mem.facts[g_mem.fact_count][MIBOT_MEM_FACT_TEXT_MAX] = '\0';
          g_mem.fact_count++;
        }
        ok = true;
      }
      cJSON_Delete(root);
    }
    delete[] blob;
  }

  nvs_close(handle);
  if (ok && g_mem.fact_count > 0) {
    ESP_LOGI(TAG, "restored %u fact(s) from NVS", (unsigned)g_mem.fact_count);
  }
  return ok;
}

bool flush_facts_to_nvs() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "NVS open (write) failed");
    return false;
  }

  cJSON *root = cJSON_CreateArray();
  for (size_t i = 0; i < g_mem.fact_count; i++) {
    cJSON_AddItemToArray(root, cJSON_CreateString(g_mem.facts[i]));
  }
  char *serialized = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  bool written = false;
  if (serialized != nullptr) {
    const esp_err_t result =
        nvs_set_str(handle, NVS_KEY_FACTS, serialized);
    cJSON_free(serialized);
    if (result == ESP_OK) {
      written = nvs_commit(handle) == ESP_OK;
    } else {
      ESP_LOGE(TAG, "NVS set_str failed: %s", esp_err_to_name(result));
    }
  }

  nvs_close(handle);
  if (written) {
    ESP_LOGI(TAG, "persisted %u fact(s) to NVS", (unsigned)g_mem.fact_count);
  }
  return written;
}

}  // namespace

extern "C" {

bool mibot_memory_init(void) {
  std::memset(&g_mem, 0, sizeof(g_mem));
  g_mem.restored = load_facts_from_nvs();
  return g_mem.restored;
}

void mibot_memory_push(const char *role, const char *text) {
  if (text == nullptr) return;
  char clean[MIBOT_MEM_TURN_TEXT_MAX + 1];
  (void)sanitize_utf8(text, clean, sizeof(clean));
  text = clean;
  if (g_mem.turn_count < MIBOT_MEM_MAX_TURNS) {
    g_mem.turn_head = 0;
    g_mem.turn_count++;
  } else {
    /* Ring full: overwrite the oldest by advancing head past it. */
    g_mem.turn_head = (g_mem.turn_head + 1) % MIBOT_MEM_MAX_TURNS;
  }

  const size_t slot = (g_mem.turn_head + g_mem.turn_count - 1) % MIBOT_MEM_MAX_TURNS;
  strncpy(g_mem.turns[slot].role, s_role(role), sizeof(g_mem.turns[slot].role) - 1);
  g_mem.turns[slot].role[sizeof(g_mem.turns[slot].role) - 1] = '\0';
  strncpy(g_mem.turns[slot].text, text, MIBOT_MEM_TURN_TEXT_MAX);
  g_mem.turns[slot].text[MIBOT_MEM_TURN_TEXT_MAX] = '\0';
}

int mibot_memory_append_context(cJSON *messages, size_t budget_bytes) {
  if (messages == nullptr || !cJSON_IsArray(messages)) return 0;
  int appended = 0;

  /* Rough serialised-size estimate of one {role, content} object.  The
   * pessimistic x2 accounts for escaped quotes; cJSON emits non-ASCII bytes
   * raw, so plain Chinese text costs far less than this upper bound. */
  auto json_overhead = [](const char *role, const char *content) -> size_t {
    const size_t text = content != nullptr ? strlen(content) : 0;
    return 24 + (role != nullptr ? strlen(role) : 0) + text * 2;
  };

  /* Long-term facts mirror: a single system-role block, one fact per line.
   * Skipped wholesale when even it would exceed the remaining budget.
   * (system role keeps an assistant message from ever being the first
   * non-system entry, which some chat-completions APIs reject.) */
  if (g_mem.fact_count > 0) {
    std::string fact_block("关于用户，我一直记得这些事实，请据此回答：\n");
    for (size_t i = 0; i < g_mem.fact_count; i++) {
      fact_block.push_back('-');
      fact_block.push_back(' ');
      fact_block.append(g_mem.facts[i]);
      fact_block.push_back('\n');
    }
    if (json_overhead("system", fact_block.c_str()) <= budget_bytes) {
      cJSON *item = cJSON_CreateObject();
      if (item != nullptr &&
          cJSON_AddStringToObject(item, "role", "system") != nullptr &&
          cJSON_AddStringToObject(item, "content",
                                  fact_block.c_str()) != nullptr) {
        cJSON_AddItemToArray(messages, item);
        appended++;
        budget_bytes -= json_overhead("system", fact_block.c_str());
      } else {
        cJSON_Delete(item);
      }
    }
  }

  /* Short-term history: keep the NEWEST turns that fit the budget, emit them
   * oldest -> newest so the model sees a coherent timeline.  Count first, then
   * append, so trimming drops only old turns. */
  const size_t total = g_mem.turn_count;
  const size_t base = (total == 0) ? 0 : g_mem.turn_head;
  size_t keep = 0;
  size_t cost_sum = 0;
  for (size_t i = 0; i < total; i++) {
    const ShortTurn &turn =
        g_mem.turns[(base + (total - 1 - i)) % MIBOT_MEM_MAX_TURNS];
    const size_t cost = json_overhead(turn.role, turn.text);
    if (cost > budget_bytes || cost_sum + cost > budget_bytes) break;
    cost_sum += cost;
    keep++;
  }
  /* A history block must not open with an assistant turn (an assistant only
   * speaks in reply to a user).  Drop a lone leading assistant if present. */
  if (keep > 0 && strcmp(g_mem.turns[(base + (total - keep)) % MIBOT_MEM_MAX_TURNS].role,
                         "assistant") == 0) {
    keep--;
  }
  for (size_t i = total - keep; i < total; i++) {
    const ShortTurn &turn = g_mem.turns[(base + i) % MIBOT_MEM_MAX_TURNS];
    cJSON *item = cJSON_CreateObject();
    if (item == nullptr) break;
    cJSON_AddStringToObject(item, "role", turn.role);
    if (cJSON_AddStringToObject(item, "content", turn.text) == nullptr) {
      cJSON_Delete(item);
      break;
    }
    cJSON_AddItemToArray(messages, item);
    appended++;
  }

  return appended;
}

bool mibot_memory_remember(const char *fact) {
  if (fact == nullptr) return false;
  while (*fact == ' ' || *fact == '\t') fact++;
  if (*fact == '\0') return false;

  char cleaned[MIBOT_MEM_FACT_TEXT_MAX + 1];
  const size_t n = sanitize_utf8(fact, cleaned, sizeof(cleaned));
  (void)n;
  /* Trim a trailing space/tab only when sanitized clean (facts are one
   * line; remember_mem_facts already drops trailing punctuation). */
  while (cleaned[0] != '\0' &&
         (cleaned[strlen(cleaned) - 1] == ' ' ||
          cleaned[strlen(cleaned) - 1] == '\t')) {
    cleaned[strlen(cleaned) - 1] = '\0';
  }
  if (cleaned[0] == '\0') return false;

  if (fact_present(cleaned)) return false;
  if (g_mem.fact_count >= MIBOT_MEM_FACTS_MAX) {
    ESP_LOGW(TAG, "facts full (%u); dropping \"%s\"",
             (unsigned)MIBOT_MEM_FACTS_MAX, cleaned);
    return false;
  }

  strncpy(g_mem.facts[g_mem.fact_count], cleaned, MIBOT_MEM_FACT_TEXT_MAX);
  g_mem.facts[g_mem.fact_count][MIBOT_MEM_FACT_TEXT_MAX] = '\0';
  g_mem.fact_count++;
  g_mem.facts_dirty = true;
  ESP_LOGI(TAG, "new fact queued: \"%s\"", cleaned);
  return true;
}

bool mibot_memory_flush_nvs(void) {
  if (!g_mem.facts_dirty) return false;
  const bool written = flush_facts_to_nvs();
  if (written) g_mem.facts_dirty = false;
  return written;
}

void mibot_memory_clear(bool erase_facts) {
  g_mem.turn_count = 0;
  if (erase_facts) {
    g_mem.fact_count = 0;
    g_mem.facts_dirty = true; /* NVS will be rewritten empty */
    (void)flush_facts_to_nvs();
  }
}

size_t mibot_memory_turns(void) { return g_mem.turn_count; }
size_t mibot_memory_facts(void) { return g_mem.fact_count; }

}  // extern "C"