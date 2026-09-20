#!/usr/bin/env python3
"""Full gateway loop test — emulate the ESP32 WS audio client.

Drives the exact device-side protocol (mibot_audio.cpp) against any gateway
serving ws://host:port, and asserts the whole loop works:

  A. uplink PCM (16k s16le mono, whole 640B/20ms frames) + {"eos": true}
     -> gateway runs real ASR -> downlink {"type":"asr_result","text",...}
  B. downlink TTS: {"type":"tts_request","text","voice","stream_id"}
     -> gateway streams TTS PCM frames -> {"eos": true}

Usage:
    python gateway_loop_test.py --pcm _selftest/sample.pcm --host 127.0.0.1
"""
from __future__ import annotations

import argparse
import json
import sys
import time

BUSY_WAIT = 0.4

try:
    import websockets
except ImportError:  # pragma: no cover
    print("error: pip install websockets", file=sys.stderr)
    raise SystemExit(2)


async def loop_test(args: argparse.Namespace) -> int:
    import asyncio

    pcm = open(args.pcm, "rb").read()
    print(f"[t] sample {len(pcm)} bytes ({len(pcm) // 640} frames / "
          f"{len(pcm) / 32000:.2f}s)")
    pcm = pcm[: len(pcm) - (len(pcm) % 640)]

    tts_text = "你好，我是小美，我记住你了。"
    ok = [False, False]

    def say(msg: str) -> None:
        print(f"[t] {msg}", flush=True)

    async with websockets.connect(f"ws://{args.host}:{args.port}",
                                  max_size=None, ping_interval=None) as ws:
        # A. stream utterance
        say(f"A1: streaming {len(pcm)} bytes of PCM")
        for off in range(0, len(pcm), 640):
            await ws.send(pcm[off:off + 640])
        await ws.send(json.dumps({"eos": True}))

        # read until asr_result
        deadline = time.monotonic() + args.timeout
        asr_text = ""
        while time.monotonic() < deadline:
            msg = await asyncio.wait_for(ws.recv(), timeout=min(10.0, args.timeout))
            if isinstance(msg, str):
                root = json.loads(msg)
                if root.get("type") == "asr_result":
                    asr_text = root.get("text", "")
                    ok[0] = True
                    say(f"A2: ASR result -> {asr_text!r}")
                    break
        if not ok[0]:
            say("A: FAIL no asr_result")
            return 1

        # B. request TTS
        say("B1: tts_request")
        await ws.send(json.dumps({
            "type": "tts_request", "text": tts_text,
            "voice": "default", "stream_id": "aud_0001"}))
        frames = 0
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            msg = await asyncio.wait_for(ws.recv(), timeout=min(15.0, args.timeout))
            if isinstance(msg, (bytes, bytearray)):
                frames += len(msg) // 640
            else:
                root = json.loads(msg)
                if root.get("eos") is True:
                    ok[1] = True
                    say(f"B2: TTS downlink {frames} frames + eos")
                    break
        if not ok[1]:
            say("B: FAIL no TTS stream")
            return 1

    say("ALL PASS" if all(ok) else "FAIL")
    return 0 if all(ok) else 1


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pcm", required=True,
                        help="raw 16k s16le mono PCM file to stream (headerless; "
                             "strip the 44-byte header from a WAV first)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()
    import asyncio
    raise SystemExit(asyncio.run(loop_test(args)))