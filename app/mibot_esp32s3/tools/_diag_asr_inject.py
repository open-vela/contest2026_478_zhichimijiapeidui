"""Drive one cloud ASR text into the ESP32 session via the gateway asr_inject hook.

Connects to the local gateway (ws://127.0.0.1:8765), sends
{"type":"asr_inject","text":<TEXT>}; the gateway relays it to the live ESP32
session exactly as a real local-ASR result.  Used to deterministically drive
the SF32 ASR->LLM->TTS chain without the microphone.
"""
import argparse
import asyncio
import json
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

try:
    import websockets
except ImportError:  # pragma: no cover
    print("error: pip install websockets", file=sys.stderr)
    raise SystemExit(2)


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", default="你好，你是谁呀？")
    ap.add_argument("--uri", default="ws://127.0.0.1:8765")
    ap.add_argument("--hold", type=float, default=2.0)
    args = ap.parse_args()

    async with websockets.connect(args.uri) as ws:
        await ws.send(json.dumps({"type": "asr_inject", "text": args.text},
                                 ensure_ascii=False))
        print(f"asr_inject sent: {args.text!r}", flush=True)
        await asyncio.sleep(args.hold)


if __name__ == "__main__":
    asyncio.run(main())