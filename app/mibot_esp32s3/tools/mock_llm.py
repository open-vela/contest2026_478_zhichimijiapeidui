#!/usr/bin/env python3
"""Mock OpenAI-compatible chat-completions server (stands in for Xiaomi MiMo).

Lets the ESP32 LLM gateway be exercised end to end without a real MiMo API key.
It speaks the subset the gateway uses:

  POST /v1/chat/completions
    request : {"model": ..., "messages": [...], "max_completion_tokens": N,
               "tools": [...]}
    response: {"choices": [{"message": {"role": "assistant",
                                        "content": "..."},
                            "finish_reason": "stop"}], "model": ...}

With --tool-call it answers with a native tool_calls reply instead, so the
robot-action branch (THINKING -> ACTING) can be tested too.

Usage:
    python mock_llm.py --host 0.0.0.0 --port 8766
    python mock_llm.py --tool-call --action happy

Point the ESP32 at it from mibot_secrets.h:
    #define MIBOT_LLM_URL "http://<this-host-ip>:8766/v1/chat/completions"

Standard library only.
"""

from __future__ import annotations

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ARGS: argparse.Namespace


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        print(f"[{time.strftime('%H:%M:%S')}] {fmt % args}", flush=True)

    def _reply(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802 - required name
        length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            request = json.loads(raw.decode("utf-8"))
        except Exception:  # noqa: BLE001
            self._reply(400, {"error": {"message": "invalid JSON"}})
            return

        messages = request.get("messages", [])
        user_text = ""
        has_system = False
        for message in messages:
            if message.get("role") == "system":
                has_system = True
            if message.get("role") == "user":
                user_text = str(message.get("content", ""))
        tools = request.get("tools", [])

        auth = "yes" if (self.headers.get("Authorization") or
                         self.headers.get("api-key")) else "NO"
        self.log_message(
            "chat model=%s msgs=%d system=%s tools=%d auth=%s user=%r",
            request.get("model"), len(messages), has_system, len(tools), auth,
            user_text[:60])

        if ARGS.status != 200:
            self._reply(ARGS.status, {"error": {"message": "mock failure"}})
            return

        if ARGS.tool_call and tools:
            message = {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": "call_mock_1",
                    "type": "function",
                    "function": {
                        "name": "robot_perform_action",
                        "arguments": json.dumps({"action": ARGS.action}),
                    },
                }],
            }
            finish = "tool_calls"
        else:
            message = {"role": "assistant", "content": ARGS.reply}
            finish = "stop"

        self._reply(200, {
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "created": int(time.time()),
            "model": request.get("model", "mock-model"),
            "choices": [{"index": 0, "message": message,
                         "finish_reason": finish}],
            "usage": {"prompt_tokens": 0, "completion_tokens": 0,
                      "total_tokens": 0},
        })

    def do_GET(self) -> None:  # noqa: N802 - required name
        self._reply(200, {"status": "ok"})


def main() -> None:
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--reply", default="我是MiMo，很高兴认识你。",
                        help="assistant text returned for a normal turn")
    parser.add_argument("--tool-call", action="store_true",
                        help="answer with a robot_perform_action tool call")
    parser.add_argument("--action", default="happy",
                        help="action name used with --tool-call")
    parser.add_argument("--status", type=int, default=200,
                        help="HTTP status to return (for error-path testing)")
    ARGS = parser.parse_args()

    server = ThreadingHTTPServer((ARGS.host, ARGS.port), Handler)
    print(f"mock LLM (OpenAI compatible) on http://{ARGS.host}:{ARGS.port}/v1/chat/completions",
          flush=True)
    print(f"  reply     : {ARGS.reply!r}", flush=True)
    print(f"  tool_call : {ARGS.tool_call} ({ARGS.action})", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
