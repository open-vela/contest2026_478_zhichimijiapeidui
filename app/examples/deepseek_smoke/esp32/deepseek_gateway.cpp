#include "deepseek_gateway.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mibot_config.h"
#include "mibot_memory.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

/* mibot_secrets.h (included through mibot_config.h) may define either
 * MIBOT_MIMO_API_KEY or the legacy MIBOT_DEEPSEEK_API_KEY. */

namespace {

constexpr char TAG[] = "mibot_llm";

/* Cloud LLM endpoint.  Default target is Xiaomi MiMo, whose API is
 * OpenAI-Chat-Completions compatible:
 *   base URL : https://api.xiaomimimo.com/v1
 *   model    : mimo-v2.5-pro
 * Override any of these from mibot_secrets.h (for example to use a Token Plan
 * base URL, or to point at a self-hosted OpenAI-compatible server). */
#ifndef MIBOT_LLM_URL
#  define MIBOT_LLM_URL "https://api.xiaomimimo.com/v1/chat/completions"
#endif
#ifndef MIBOT_LLM_MODEL
#  define MIBOT_LLM_MODEL "mimo-v2.5-pro"
#endif
/* Persona, injected only when the caller did not supply a system message.
 *
 * This is NOT just the vendor-recommended identity line.  Two things here are
 * load-bearing and the rest of the pipeline is broken without them:
 *
 *  1. Brevity.  The reply is spoken through TTS and has to survive
 *     max_completion_tokens, the 4 KiB AA55 wire frame and the SF32's
 *     pending_answer buffer.  Markdown and emoji would be read out loud.
 *  2. The "[MEM]" contract.  Long-term memory is driven entirely by the model
 *     emitting that marker: remember_mem_facts() below scans replies for it and
 *     strip_mem_lines() keeps it out of the speech.  Nothing else ever calls
 *     mibot_memory_remember(), so a persona without this clause means facts are
 *     never persisted and the NVS store stays empty forever -- silently.
 *
 * The marker format matches remember_mem_facts()'s parser: the fact runs from
 * the marker to the end of that line.
 *
 * Override from mibot_secrets.h only if you want a different character; keep
 * clauses 1 and 2 if you do. */
/* The identity line is the MiMo vendor-recommended wording.  Note it becomes a
 * false self-description if MIBOT_LLM_URL is pointed at another provider (this
 * gateway is OpenAI-compatible, so that is easy to do) -- override
 * MIBOT_LLM_SYSTEM_PROMPT from mibot_secrets.h when you switch. */
#ifndef MIBOT_LLM_SYSTEM_PROMPT
#  define MIBOT_LLM_SYSTEM_PROMPT \
     "你是MiMo（中文名称也是MiMo），是小米公司研发的AI智能助手，" \
     "现在运行在一台桌面机器人里，你的回答会被语音合成后念出来。\n" \
     "回答要求：用口语，简短，一到三句话，不超过80个字；" \
     "不要使用Markdown、列表、表情符号或颜文字。\n" \
     "记忆：当用户告诉你值得长期记住的事情（名字、称呼、喜好、习惯、" \
     "重要日期等），在回答的最后另起一行追加一条记录，格式为\n" \
     "[MEM] 事实内容\n" \
     "每条事实单独一行，只写事实本身，不要写解释。这一行不会被念出来。" \
     "如果本轮没有值得长期记住的新信息，就不要输出[MEM]。\n" \
     "动作：如果这句回答适合配一个表情动作，在最后另起一行写\n" \
     "[ACT] 动作名\n" \
     "动作名只能是 happy、sad、confused、surprised、cute、greeting 之一，" \
     "每轮最多一个，这一行同样不会被念出来。" \
     "不要用函数调用的方式做动作，务必用这一行标记，" \
     "否则机器人会做完动作却一句话都不说。"
#endif

/* API key: prefer a MiMo-specific key, fall back to the legacy DeepSeek macro
 * so existing local secrets keep working. */
#if defined(MIBOT_MIMO_API_KEY)
#  define MIBOT_LLM_API_KEY MIBOT_MIMO_API_KEY
#elif defined(MIBOT_DEEPSEEK_API_KEY)
#  define MIBOT_LLM_API_KEY MIBOT_DEEPSEEK_API_KEY
#else
#  define MIBOT_LLM_API_KEY ""
#endif

constexpr char LLM_URL[] = MIBOT_LLM_URL;
constexpr char DEFAULT_MODEL[] = MIBOT_LLM_MODEL;
constexpr size_t REQUEST_MAX = 4096;
constexpr size_t HTTP_RESPONSE_MAX = 8192;
constexpr size_t WIRE_RESPONSE_MAX = 4096;
constexpr uint32_t HTTP_TIMEOUT_MS = 20000;
// run_request keeps bounded JSON and HTTP buffers on its stack.  ESP-IDF
// interprets the xTaskCreate stack argument in bytes, so 8 KiB is not enough
// for the two buffers to coexist safely during an HTTPS request.
constexpr uint32_t REQUEST_TASK_STACK_BYTES = 24 * 1024;

std::atomic<bool> g_busy{false};

struct Request {
  uint16_t length;
  mibot_deepseek_response_fn callback;
  void *context;
  uint8_t payload[REQUEST_MAX + 1];
};

struct HttpResponse {
  size_t length = 0;
  bool overflow = false;
  char data[HTTP_RESPONSE_MAX] = {};
};

bool append_http_data(HttpResponse *response, const char *data, size_t length) {
  if (response == nullptr || data == nullptr) return false;
  if (length > sizeof(response->data) - 1 - response->length) {
    const size_t available = sizeof(response->data) - 1 - response->length;
    if (available > 0) {
      memcpy(response->data + response->length, data, available);
      response->length += available;
      response->data[response->length] = '\0';
    }
    response->overflow = true;
    return false;
  }
  memcpy(response->data + response->length, data, length);
  response->length += length;
  response->data[response->length] = '\0';
  return true;
}

esp_err_t http_event_handler(esp_http_client_event_t *event) {
  if (event == nullptr || event->user_data == nullptr) return ESP_OK;
  auto *response = static_cast<HttpResponse *>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr &&
      event->data_len > 0) {
    append_http_data(response, static_cast<const char *>(event->data),
                     static_cast<size_t>(event->data_len));
  }
  return ESP_OK;
}

bool copy_json_string(const cJSON *object, const char *name, char *out,
                      size_t out_size) {
  if (object == nullptr || name == nullptr || out == nullptr || out_size == 0)
    return false;
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
  strncpy(out, item->valuestring, out_size - 1);
  out[out_size - 1] = '\0';
  return true;
}

bool make_error_response(const char *request_id, const char *code,
                         const char *message, char *out, size_t out_size,
                         size_t *out_length) {
  if (out == nullptr || out_size == 0 || out_length == nullptr) return false;
  cJSON *root = cJSON_CreateObject();
  cJSON *error = root != nullptr ? cJSON_AddObjectToObject(root, "error") : nullptr;
  if (root == nullptr || error == nullptr ||
      cJSON_AddStringToObject(root, "schema", "mibot.ai.response.v1") == nullptr ||
      cJSON_AddBoolToObject(root, "ok", false) == nullptr ||
      cJSON_AddStringToObject(root, "request_id", request_id != nullptr ? request_id : "") == nullptr ||
      cJSON_AddStringToObject(error, "code", code != nullptr ? code : "E_GATEWAY") == nullptr ||
      cJSON_AddStringToObject(error, "message", message != nullptr ? message : "") == nullptr) {
    cJSON_Delete(root);
    return false;
  }
  char *serialized = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (serialized == nullptr) return false;
  const size_t length = strlen(serialized);
  if (length >= out_size || length > WIRE_RESPONSE_MAX) {
    cJSON_free(serialized);
    return false;
  }
  memcpy(out, serialized, length + 1);
  cJSON_free(serialized);
  *out_length = length;
  return true;
}

bool make_success_response(const char *request_id, const char *model,
                           const cJSON *message, const char *finish_reason,
                           const char *text_override, char *out,
                           size_t out_size, size_t *out_length) {
  if (out == nullptr || out_size == 0 || out_length == nullptr ||
      !cJSON_IsObject(message)) {
    return false;
  }

  const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
  const cJSON *tool_calls =
      cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
  /* text_override carries the reply with the "[MEM]" metadata lines already
   * stripped; fall back to the raw content when no override is supplied. */
  const char *text = (text_override != nullptr)
                         ? text_override
                         : (cJSON_IsString(content) ? content->valuestring : "");
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr ||
      cJSON_AddStringToObject(root, "schema", "mibot.ai.response.v1") == nullptr ||
      cJSON_AddBoolToObject(root, "ok", true) == nullptr ||
      cJSON_AddStringToObject(root, "request_id",
                              request_id != nullptr ? request_id : "") == nullptr ||
      cJSON_AddStringToObject(root, "model",
                              model != nullptr ? model : DEFAULT_MODEL) == nullptr ||
      cJSON_AddStringToObject(root, "finish_reason",
                              finish_reason != nullptr ? finish_reason : "stop") == nullptr ||
      cJSON_AddStringToObject(root, "text", text != nullptr ? text : "") == nullptr) {
    cJSON_Delete(root);
    return false;
  }

  if (cJSON_IsArray(tool_calls)) {
    cJSON *copy = cJSON_Duplicate(tool_calls, true);
    if (copy == nullptr) {
      cJSON_Delete(root);
      return false;
    }
    cJSON_AddItemToObject(root, "tool_calls", copy);
  }

  char *serialized = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (serialized == nullptr) return false;
  const size_t length = strlen(serialized);
  if (length >= out_size || length > WIRE_RESPONSE_MAX) {
    cJSON_free(serialized);
    return false;
  }
  memcpy(out, serialized, length + 1);
  cJSON_free(serialized);
  *out_length = length;
  return true;
}

/* Where the next metadata marker starts, or nullptr.  Both markers are stripped
 * by the same pass so a reply carrying one of each still yields clean speech. */
const char *next_meta_marker(const char *p) {
  const char *mem = strstr(p, "[MEM]");
  const char *act = strstr(p, "[ACT]");
  if (mem == nullptr) return act;
  if (act == nullptr) return mem;
  return mem < act ? mem : act;
}

/* Copy `in` into `out` with every "[MEM]"/"[ACT]" metadata segment removed.  A
 * segment runs from the marker to the end of its line (newline included), so
 * both standalone marker lines and markers inlined at the end of a reply are
 * stripped before the text reaches the SF32 for speech. */
void strip_mem_lines(const char *in, char *out, size_t out_size) {
  if (out == nullptr || out_size == 0) return;
  out[0] = '\0';
  if (in == nullptr) return;
  size_t written = 0;
  const char *p = in;
  while (*p != '\0' && written + 1 < out_size) {
    const char *marker = next_meta_marker(p);
    const char *segment = (marker != nullptr) ? marker : (p + strlen(p));
    while (p < segment && written + 1 < out_size) out[written++] = *p++;
    if (marker == nullptr) break;
    while (*p != '\0' && *p != '\n') p++; /* drop marker up to line end      */
    if (*p == '\n') p++;                  /* and the newline itself           */
  }
  out[written] = '\0';
  /* A whole-line marker leaves its preceding newline behind; trim trailing
   * whitespace so only clean speech survives. */
  while (written > 0 &&
         (out[written - 1] == ' ' || out[written - 1] == '\t' ||
          out[written - 1] == '\n' || out[written - 1] == '\r')) {
    out[--written] = '\0';
  }
}

/* Scan an assistant reply for "[MEM] " markers and queue the text following
 * each marker (one per line, up to the first line break) as a long-term fact.
 * The persona above tells MiMo to emit exactly this; persistence is deferred
 * to mibot_memory_flush_nvs(), so flash wear stays proportional to real
 * knowledge changes, not to conversation volume. */
void remember_mem_facts(const char *text) {
  if (text == nullptr) return;
  const char *scan = text;
  while ((scan = strstr(scan, "[MEM]")) != nullptr) {
    scan += 5;
    while (*scan == ' ' || *scan == '\t' || *scan == '-' || *scan == ':')
      scan++;
    const char *end = scan;
    while (*end != '\0' && *end != '\n' && *end != '\r') end++;
    size_t len = static_cast<size_t>(end - scan);
    if (len > MIBOT_MEM_FACT_TEXT_MAX) len = MIBOT_MEM_FACT_TEXT_MAX;
    char fact[MIBOT_MEM_FACT_TEXT_MAX + 1];
    memcpy(fact, scan, len);
    fact[len] = '\0';
    while (len > 0) {
      const char last = fact[len - 1];
      if (last != ' ' && last != '\t' && last != '.' && last != ';') break;
      fact[len - 1] = '\0';
      len--;
    }
    if (len > 0) (void)mibot_memory_remember(fact);
    scan = end;
  }
}

/* The bounded-action set the SF32 will accept (mibot_voice_agent.c's whitelist).
 * Validated here so a hallucinated action name never reaches the state machine
 * as a tool call it would only reject. */
constexpr const char *ACTION_NAMES[] = {
    "happy", "sad", "confused", "surprised", "cute", "greeting"};

/* Pull the action out of an "[ACT] <name>" line.
 *
 * Why a text marker instead of the tools API: an OpenAI-compatible model that
 * answers with tool_calls sets content to null, so the reply carries an action
 * and NOTHING to say.  The robot then performs the action and stands there
 * silently, which is not a usable voice assistant.  The marker keeps the action
 * inside the ordinary text completion, so a single round always yields speech
 * and optionally an action.  Same trick as "[MEM]", which is already proven on
 * this model.  The gateway converts it back into the tool_calls shape the SF32
 * already understands, so the wire contract is unchanged. */
bool extract_act_marker(const char *text, char *out, size_t out_size) {
  if (text == nullptr || out == nullptr || out_size == 0) return false;
  out[0] = '\0';
  const char *scan = strstr(text, "[ACT]");
  if (scan == nullptr) return false;
  scan += 5;
  while (*scan == ' ' || *scan == '\t' || *scan == ':' || *scan == '-') scan++;
  const char *end = scan;
  while (*end != '\0' && *end != '\n' && *end != '\r' && *end != ' ' &&
         *end != '.' && *end != ',' && *end != ';') {
    end++;
  }
  const size_t len = static_cast<size_t>(end - scan);
  if (len == 0 || len >= out_size) return false;
  memcpy(out, scan, len);
  out[len] = '\0';
  for (const char *name : ACTION_NAMES) {
    if (strcmp(out, name) == 0) return true;
  }
  ESP_LOGW(TAG, "ignoring unknown [ACT] \"%s\"", out);
  out[0] = '\0';
  return false;
}

/* Wrap an action name in the tool_calls shape the SF32 decoder expects, so the
 * marker path and a real tools-API reply are indistinguishable downstream. */
cJSON *make_synthetic_tool_calls(const char *action) {
  cJSON *array = cJSON_CreateArray();
  cJSON *call = array != nullptr ? cJSON_CreateObject() : nullptr;
  cJSON *function = call != nullptr ? cJSON_AddObjectToObject(call, "function")
                                    : nullptr;
  char args[96];

  if (function == nullptr) {
    cJSON_Delete(call);
    cJSON_Delete(array);
    return nullptr;
  }
  snprintf(args, sizeof(args), "{\"action\":\"%s\"}", action);
  if (cJSON_AddStringToObject(call, "id", "act_marker") == nullptr ||
      cJSON_AddStringToObject(call, "type", "function") == nullptr ||
      cJSON_AddStringToObject(function, "name", "robot_perform_action") == nullptr ||
      cJSON_AddStringToObject(function, "arguments", args) == nullptr) {
    cJSON_Delete(call);
    cJSON_Delete(array);
    return nullptr;
  }
  cJSON_AddItemToArray(array, call);
  return array;
}

bool build_request_body(const cJSON *request_root, cJSON **body_out,
                        char *request_id, size_t request_id_size,
                        char *model, size_t model_size, char *error,
                        size_t error_size) {
  if (request_root == nullptr || body_out == nullptr || request_id == nullptr ||
      model == nullptr || error == nullptr) {
    return false;
  }
  *body_out = nullptr;
  request_id[0] = '\0';
  strncpy(model, DEFAULT_MODEL, model_size - 1);
  model[model_size - 1] = '\0';

  (void)copy_json_string(request_root, "request_id", request_id,
                         request_id_size);
  (void)copy_json_string(request_root, "model", model, model_size);

  const cJSON *provided_body =
      cJSON_GetObjectItemCaseSensitive(request_root, "body");
  if (cJSON_IsObject(provided_body)) {
    *body_out = cJSON_Duplicate(provided_body, true);
    if (*body_out == nullptr) {
      strncpy(error, "cannot copy request body", error_size - 1);
      error[error_size - 1] = '\0';
      return false;
    }
    if (cJSON_GetObjectItemCaseSensitive(*body_out, "model") == nullptr)
      cJSON_AddStringToObject(*body_out, "model", model);
    return true;
  }

  char prompt[1024] = {};
  if (!copy_json_string(request_root, "prompt", prompt, sizeof(prompt)) ||
      prompt[0] == '\0') {
    strncpy(error, "request needs a non-empty prompt or body", error_size - 1);
    error[error_size - 1] = '\0';
    return false;
  }

  cJSON *body = cJSON_CreateObject();
  cJSON *messages = body != nullptr ? cJSON_AddArrayToObject(body, "messages") : nullptr;
  cJSON *message = messages != nullptr ? cJSON_CreateObject() : nullptr;
  if (body == nullptr || messages == nullptr || message == nullptr ||
      cJSON_AddStringToObject(body, "model", model) == nullptr ||
      cJSON_AddNumberToObject(body, "max_tokens", 64) == nullptr ||
      cJSON_AddStringToObject(message, "role", "user") == nullptr ||
      cJSON_AddStringToObject(message, "content", prompt) == nullptr) {
    cJSON_Delete(message);
    cJSON_Delete(body);
    strncpy(error, "cannot build DeepSeek request", error_size - 1);
    error[error_size - 1] = '\0';
    return false;
  }
  cJSON_AddItemToArray(messages, message);
  *body_out = body;
  return true;
}

void run_request(Request *request) {
  /* The wire response (4 KiB) and the HTTP receive buffer (8 KiB) used to live
   * on this task's stack.  Together with the TLS handshake below that overflowed
   * the task stack and corrupted neighbouring memory, which surfaced as a
   * LoadProhibited panic inside the lwIP thread right after a successful
   * completion.  Keep both on the heap. */
  auto *response = static_cast<char *>(calloc(1, WIRE_RESPONSE_MAX + 1));
  auto *http_response = new (std::nothrow) HttpResponse();
  /* Owns the raw reply text for the whole function.  Heap for the same reason as
   * the two buffers above: this task's stack has no room for another 4 KiB. */
  auto *assistant_copy = static_cast<char *>(calloc(1, WIRE_RESPONSE_MAX + 1));
  size_t response_length = 0;
  char request_id[96] = {};
  char model[96] = {};
  char error[160] = {};
  bool ok = false;
  char request_text[1024] = {}; /* current user utterance, for short memory   */
  const char *assistant_text = nullptr; /* raw reply, for short memory/facts */

  if (response == nullptr || http_response == nullptr ||
      assistant_copy == nullptr) {
    ESP_LOGE(TAG, "out of memory for the LLM request buffers");
    free(response);
    free(assistant_copy);
    delete http_response;
    g_busy.store(false, std::memory_order_release);
    return;
  }

  /* One-time boot: restore long-term facts from NVS and clear the short-term
   * ring.  mibot_memory_init() memsets the module state, so it must run
   * exactly once. */
  static std::once_flag memory_once;
  std::call_once(memory_once, [] { mibot_memory_init(); });

  cJSON *request_root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(request->payload), request->length);
  cJSON *body = nullptr;
  if (request_root == nullptr || !cJSON_IsObject(request_root)) {
    make_error_response("", "E_INVALID_JSON", "request is not a JSON object",
                        response, WIRE_RESPONSE_MAX + 1, &response_length);
  } else if (!build_request_body(request_root, &body, request_id,
                                 sizeof(request_id), model, sizeof(model),
                                 error, sizeof(error))) {
    make_error_response(request_id, "E_INVALID_REQUEST", error, response,
                        WIRE_RESPONSE_MAX + 1, &response_length);
  } else {
    /* MiMo follows the newer OpenAI field name. */
    if (cJSON_GetObjectItemCaseSensitive(body, "max_tokens") == nullptr &&
        cJSON_GetObjectItemCaseSensitive(body, "max_completion_tokens") == nullptr) {
      cJSON_AddNumberToObject(body, "max_completion_tokens", 256);
    }
    /* Inject the vendor-recommended system prompt when the caller did not
     * supply a system message of its own. */
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(body, "messages");
    if (cJSON_IsArray(messages) && strlen(MIBOT_LLM_SYSTEM_PROMPT) != 0) {
      bool has_system = false;
      const cJSON *entry = nullptr;
      cJSON_ArrayForEach(entry, messages) {
        const char *role = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(entry, "role"));
        if (role != nullptr && strcmp(role, "system") == 0) {
          has_system = true;
          break;
        }
      }
      if (!has_system) {
        cJSON *system_message = cJSON_CreateObject();
        if (system_message != nullptr &&
            cJSON_AddStringToObject(system_message, "role", "system") != nullptr &&
            cJSON_AddStringToObject(system_message, "content",
                                    MIBOT_LLM_SYSTEM_PROMPT) != nullptr) {
          cJSON_InsertItemInArray(messages, 0, system_message);
        } else {
          cJSON_Delete(system_message);
        }
      }
    }
    /* --- piagent memory splice --------------------------------------------
     * Rebuild the message order as
     *   [system/persona] + [facts + short-term turns] + [current request]
     * so the model first reads who it is, then what it remembers, then what
     * the user just said.  Budget the memory block against the 4 KiB wire
     * frame (design §6: drop the oldest turns first, error out only as a
     * last resort). */
    if (cJSON_IsArray(messages) && cJSON_GetArraySize(messages) > 0) {
      cJSON *entry = nullptr;
      cJSON_ArrayForEach(entry, messages) {
        const char *role = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(entry, "role"));
        if (role != nullptr && strcmp(role, "system") == 0) continue;
        const char *content = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(entry, "content"));
        if (content != nullptr) {
          strncpy(request_text, content, sizeof(request_text) - 1);
          request_text[sizeof(request_text) - 1] = '\0';
          break;
        }
      }

      /* Serialise the request-only body once: the memory block gets whatever
       * room is left inside the wire frame. */
      size_t budget = 0;
      char *base = cJSON_PrintUnformatted(body);
      if (base != nullptr) {
        const size_t base_len = strlen(base);
        budget = (base_len < REQUEST_MAX) ? (REQUEST_MAX - base_len) : 0;
        cJSON_free(base);
      }

      cJSON *system_block = cJSON_CreateArray();
      cJSON *request_block = cJSON_CreateArray();
      cJSON *memory_block = cJSON_CreateArray();
      cJSON *final_messages = cJSON_CreateArray();
      if (system_block != nullptr && request_block != nullptr &&
          memory_block != nullptr && final_messages != nullptr) {
        while (cJSON_GetArraySize(messages) > 0) {
          cJSON *item = cJSON_GetArrayItem(messages, 0);
          const char *role = cJSON_GetStringValue(
              cJSON_GetObjectItemCaseSensitive(item, "role"));
          const bool is_system =
              (role != nullptr && strcmp(role, "system") == 0);
          cJSON_DetachItemFromArray(messages, 0);
          cJSON_AddItemToArray(is_system ? system_block : request_block, item);
        }
        (void)mibot_memory_append_context(memory_block, budget);
        while (cJSON_GetArraySize(system_block) > 0) {
          cJSON *item = cJSON_GetArrayItem(system_block, 0);
          cJSON_DetachItemFromArray(system_block, 0);
          cJSON_AddItemToArray(final_messages, item);
        }
        while (cJSON_GetArraySize(memory_block) > 0) {
          cJSON *item = cJSON_GetArrayItem(memory_block, 0);
          cJSON_DetachItemFromArray(memory_block, 0);
          cJSON_AddItemToArray(final_messages, item);
        }
        while (cJSON_GetArraySize(request_block) > 0) {
          cJSON *item = cJSON_GetArrayItem(request_block, 0);
          cJSON_DetachItemFromArray(request_block, 0);
          cJSON_AddItemToArray(final_messages, item);
        }
        cJSON_ReplaceItemInObject(body, "messages", final_messages);
      } else {
        cJSON_Delete(system_block);
        cJSON_Delete(request_block);
        cJSON_Delete(memory_block);
        cJSON_Delete(final_messages);
      }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    if (post_data == nullptr) {
      make_error_response(request_id, "E_NO_MEMORY", "cannot serialize request",
                          response, WIRE_RESPONSE_MAX + 1, &response_length);
    } else if (strlen(post_data) >= REQUEST_MAX) {
      ESP_LOGE(TAG, "request body (%u bytes) exceeds the wire frame; dropping",
               (unsigned)strlen(post_data));
      make_error_response(request_id, "E_REQUEST_TOO_LARGE",
                          "request body exceeds the wire frame", response,
                          WIRE_RESPONSE_MAX + 1, &response_length);
    } else if (strlen(MIBOT_LLM_API_KEY) == 0) {
      make_error_response(request_id, "E_NO_API_KEY",
                          "no LLM API key configured (MIBOT_MIMO_API_KEY)",
                          response, WIRE_RESPONSE_MAX + 1, &response_length);
    } else {
      esp_http_client_config_t config = {};
      config.url = LLM_URL;
      config.timeout_ms = HTTP_TIMEOUT_MS;
      config.buffer_size = 2048;
      config.buffer_size_tx = 2048;
      config.crt_bundle_attach = esp_crt_bundle_attach;
      config.event_handler = http_event_handler;
      config.user_data = http_response;

      esp_http_client_handle_t client = esp_http_client_init(&config);
      if (client == nullptr) {
        make_error_response(request_id, "E_HTTP_INIT", "HTTP client init failed",
                            response, WIRE_RESPONSE_MAX + 1, &response_length);
      } else {
        char auth[256] = {};
        snprintf(auth, sizeof(auth), "Bearer %s", MIBOT_LLM_API_KEY);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        /* MiMo accepts the OpenAI-style bearer token; its own curl examples use
         * an `api-key` header.  Send both so either gateway flavour works. */
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "api-key", MIBOT_LLM_API_KEY);
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
        const esp_err_t http_result = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        /* Kept at INFO on purpose: during integration the one line that tells
         * "the HTTPS round trip happened at all" is worth more than a quiet
         * console, and it is one line per turn, not per frame. */
        if (http_result != ESP_OK || status != 200) {
          ESP_LOGE(TAG, "LLM HTTP result=%s status=%d body_len=%u",
                   esp_err_to_name(http_result), status,
                   static_cast<unsigned>(http_response->length));
        } else {
          ESP_LOGI(TAG, "LLM HTTP ok status=%d body_len=%u",
                   status, static_cast<unsigned>(http_response->length));
        }

        if (http_result != ESP_OK) {
          make_error_response(request_id, "E_HTTP_REQUEST",
                              esp_err_to_name(http_result), response,
                              WIRE_RESPONSE_MAX + 1, &response_length);
        } else if (status != 200) {
          ESP_LOGE(TAG, "LLM non-200 body(%u)=%.300s",
                   static_cast<unsigned>(http_response->length),
                   http_response->data);
          char message[256] = {};
          snprintf(message, sizeof(message), "LLM HTTP %d: %.180s", status,
                   http_response->data);
          make_error_response(request_id, "E_LLM_HTTP", message, response,
                              WIRE_RESPONSE_MAX + 1, &response_length);
        } else if (http_response->overflow) {
          make_error_response(request_id, "E_RESPONSE_TOO_LARGE",
                              "LLM reply exceeded the receive buffer", response,
                              WIRE_RESPONSE_MAX + 1, &response_length);
        } else {
          cJSON *api_root = cJSON_ParseWithLength(http_response->data,
                                                  http_response->length);
          const cJSON *choices = api_root != nullptr
                                     ? cJSON_GetObjectItemCaseSensitive(api_root, "choices")
                                     : nullptr;
          const cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
          const cJSON *message = choice != nullptr
                                     ? cJSON_GetObjectItemCaseSensitive(choice, "message")
                                     : nullptr;
          const cJSON *finish_reason = choice != nullptr
                                           ? cJSON_GetObjectItemCaseSensitive(choice, "finish_reason")
                                           : nullptr;
          const cJSON *tool_calls = message != nullptr
                                        ? cJSON_GetObjectItemCaseSensitive(message, "tool_calls")
                                        : nullptr;
          const char *text = message != nullptr
                                 ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "content"))
                                 : nullptr;
          /* Copy, do not alias.  `text` points into api_root's allocation, which
           * is deleted at the end of this block, while the short-term ring and
           * the "[MEM]" fact scan both run *after* that -- reading the alias
           * there is a use-after-free.  It usually looks fine because the freed
           * block still holds the reply, which is exactly what makes it nasty:
           * observed as a fact persisted to NVS with a corrupted tail
           * ("小美喜欢蓝色H+??+") once the allocator reused the end of it. */
          if (text != nullptr) {
            strncpy(assistant_copy, text, WIRE_RESPONSE_MAX);
            assistant_copy[WIRE_RESPONSE_MAX] = '\0';
            assistant_text = assistant_copy;
          }
          if (message == nullptr || (text == nullptr && !cJSON_IsArray(tool_calls))) {
            make_error_response(request_id, "E_BAD_RESPONSE",
                                "LLM reply has neither content nor tool_calls",
                                response, WIRE_RESPONSE_MAX + 1, &response_length);
          } else {
            /* [MEM] lines are gateway metadata; keep them out of the spoken text but
             * remember the raw reply for the short-term ring + facts. */
            /* An "[ACT] <name>" line becomes tool_calls on the wire.  Only when
             * the model did not already use the tools API, so a real tool call
             * always wins and the two mechanisms cannot fight. */
            if (!cJSON_IsArray(tool_calls) && text != nullptr) {
              char action[32];

              if (extract_act_marker(text, action, sizeof(action))) {
                cJSON *synthetic = make_synthetic_tool_calls(action);

                if (synthetic != nullptr) {
                  /* message is owned by api_root; adding to it is fine and
                   * make_success_response() re-reads tool_calls from it. */
                  cJSON_AddItemToObject(const_cast<cJSON *>(message),
                                        "tool_calls", synthetic);
                  ESP_LOGI(TAG, "[ACT] %s -> tool_calls", action);
                }
              }
            }

            char *clean_text =
                static_cast<char *>(calloc(1, WIRE_RESPONSE_MAX + 1));
            if (clean_text != nullptr) {
              if (text != nullptr)
                strip_mem_lines(text, clean_text, WIRE_RESPONSE_MAX + 1);
              /* Reply-text marker is DEBUG-only: default firmware talks errors
               * only.  A DEBUG build re-enables it for M3/M5 content proofs. */
              ESP_LOGD(TAG, "LLM reply text (%.300s)",
                       text != nullptr ? clean_text : "<tool_calls>");
              ok = make_success_response(
                  request_id, model, message,
                  cJSON_IsString(finish_reason)
                      ? finish_reason->valuestring
                      : nullptr,
                  text != nullptr ? clean_text : nullptr, response,
                  WIRE_RESPONSE_MAX + 1, &response_length);
              free(clean_text);
            } else {
              ok = make_success_response(
                  request_id, model, message,
                  cJSON_IsString(finish_reason) ? finish_reason->valuestring
                                                : nullptr,
                  nullptr, response, WIRE_RESPONSE_MAX + 1, &response_length);
            }
            if (!ok) {
              make_error_response(request_id, "E_RESPONSE_TOO_LARGE",
                                  "response does not fit one AA55 frame", response,
                                  WIRE_RESPONSE_MAX + 1, &response_length);
            }
          }
          cJSON_Delete(api_root);
        }
        esp_http_client_cleanup(client);
      }
    }
    cJSON_free(post_data);
  }
  cJSON_Delete(body);
  cJSON_Delete(request_root);

  /* The single-flight gate protects only LLM request execution.  UART delivery
   * is downstream work and can block on the controller TX mutex; holding
   * g_busy until that callback returns made a congested UART turn a permanent
   * E_GATEWAY_BUSY condition.  Release the HTTP slot before the callback so a
   * completed request never prevents the next turn. */
  g_busy.store(false, std::memory_order_release);

  /* Remember the spoken turn only on success, so failures never pollute the
   * short-term ring.  Long-term "[MEM]" facts are read from the raw reply
   * (the marker lines are already stripped from the relayed text above). */
  if (ok) {
    if (request_text[0] != '\0') mibot_memory_push("user", request_text);
    if (assistant_text != nullptr && assistant_text[0] != '\0') {
      mibot_memory_push("assistant", assistant_text);
      remember_mem_facts(assistant_text);
    }
  }

  if (request->callback != nullptr && response_length > 0) {
    ESP_LOGI(TAG, "LLM response ready ok=%d len=%u turns=%u facts=%u",
             ok ? 1 : 0, static_cast<unsigned>(response_length),
             static_cast<unsigned>(mibot_memory_turns()),
             static_cast<unsigned>(mibot_memory_facts()));
    request->callback(ok, response, static_cast<uint16_t>(response_length),
                      request->context);
    /* Persist long-term facts only after the AI_RESPONSE reached the SF32, so
     * a slow UART never blocks the already-released HTTP slot. */
    (void)mibot_memory_flush_nvs();
  }

  free(response);
  free(assistant_copy);
  delete http_response;
}

void request_task(void *arg) {
  auto *request = static_cast<Request *>(arg);
  run_request(request);
  free(request);
  /* run_request releases the slot before response delivery; retain this
   * idempotent cleanup for any future early-return path. */
  g_busy.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

}  // namespace

extern "C" esp_err_t mibot_deepseek_gateway_submit(
    const uint8_t *request, uint16_t length,
    mibot_deepseek_response_fn callback, void *context) {
  if (request == nullptr || length == 0 || length > REQUEST_MAX ||
      callback == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true,
                                      std::memory_order_acq_rel)) {
    return ESP_ERR_INVALID_STATE;
  }

  auto *pending = static_cast<Request *>(calloc(1, sizeof(Request)));
  if (pending == nullptr) {
    g_busy.store(false, std::memory_order_release);
    return ESP_ERR_NO_MEM;
  }
  pending->length = length;
  pending->callback = callback;
  pending->context = context;
  memcpy(pending->payload, request, length);
  pending->payload[length] = '\0';

  const BaseType_t task_result = xTaskCreate(
      request_task, "mibot_deepseek", REQUEST_TASK_STACK_BYTES, pending, 5,
      nullptr);
  if (task_result != pdPASS) {
    free(pending);
    g_busy.store(false, std::memory_order_release);
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "LLM request accepted (%u bytes) model=%s",
           static_cast<unsigned>(length), DEFAULT_MODEL);
  return ESP_OK;
}
