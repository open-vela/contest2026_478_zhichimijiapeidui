#!/usr/bin/env python3
"""Real cloud audio gateway for the Mibot voice loop.

Exactly the same WebSocket contract the ESP32 audio link speaks as
``mock_cloud_audio.py``, but with REAL speech recognition:

  * ASR:   Volcengine「大模型流式自动语音识别」(BigModel SAUC streaming WS)
           over wss://openspeech.bytedance.com/api/v3/sauc/bigmodel
  * TTS:   Microsoft Edge TTS (free, real Chinese speech; already verified
           end-to-end through the SF32 speaker). No cloud TTS key needed.

Credentials live in ``volc.env`` next to this file (see volc.env.example).
The file must NOT be committed (it is already in tools/.gitignore).

Device-side protocol (unchanged, see mibot_audio.cpp):
  Uplink:   binary=raw PCM s16le 16k mono (whole 640B/20ms frames);
            text {"eos": true} closes the utterance.
  Downlink: {"type":"asr_result","text":...,"final":true};
            TTS PCM binary frames + {"eos": true}.

Usage (gateway, choose --host/--port the ESP32 reaches):
    python volc_gateway.py --host 0.0.0.0 --port 8765 --tts-engine edge

Self-test (ASR only, no server):
    python volc_gateway.py --recognize-file sample.pcm

Requires: pip install websockets edge-tts  (+ ffmpeg on PATH for edge TTS)
"""

from __future__ import annotations

import argparse
import asyncio
import gzip
import json
import os
import struct
import sys
import threading
import time
import uuid

try:
    import websockets
except ImportError:  # pragma: no cover - dependency hint only
    print("error: pip install websockets", file=sys.stderr)
    raise SystemExit(2)

SAMPLE_RATE = 16000
CHANNELS = 1
FRAME_SAMPLES = 320          # 20 ms
FRAME_BYTES = FRAME_SAMPLES * 2
# How far ahead of real time the TTS downlink is allowed to run.  Must stay
# under the ESP32's downlink ring (MIBOT_AUDIO_DN_DEPTH_PSRAM = 25 frames =
# 500 ms) or the surplus is dropped there.  300 ms leaves the ring comfortably
# topped up with headroom for Wi-Fi jitter.
PACE_LEAD_MS = 300
CHUNK_MS = 200               # optimal Volc SAUC audio chunk
CHUNK_BYTES = int(SAMPLE_RATE * 2 * CHUNK_MS / 1000)  # 6400 bytes

VOLC_WS_URI = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel"

# byte0: version(1)<<4 | header_size(1); byte2: serialization(1,json)<<4 | compression(1,gzip)
HEADER_FULL_SEND = 0x11      # byte1: message_type<<4 | flags
MT_FULL_REQUEST = 0x01
MT_AUDIO_ONLY = 0x02
FLAG_SEQ = 0x01
FLAG_LAST = 0x02
# X-Api-Sequence: -1 => sequence travels in each binary message header (flags & FLAG_SEQ)


def _load_env(env_path: str) -> dict[str, str]:
    env: dict[str, str] = {}
    try:
        with open(env_path, "r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                value = value.split("#", 1)[0].strip()  # 剥行尾注释
                env[key.strip()] = value
    except OSError:
        pass
    return env


# ---- Local offline ASR (sherpa-onnx streaming Paraformer, zh) ----
#
# Simplest no-key path: ``--asr-engine local``.  The model sits in
# ``models/sherpa-onnx-streaming-paraformer-zh/`` (encoder.int8.onnx,
# decoder.int8.onnx, tokens.txt) next to this script.  Run recognition in a
# worker thread because ONNX decode blocks the event loop.

def _default_model_dir() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "models", "sherpa-onnx-streaming-paraformer-zh")


# Recognisers are cached per model directory: constructing one loads ~236 MB of
# int8 ONNX weights, which took seconds on every single utterance and came
# straight out of the SF32's ASR budget.  The object is reusable across
# utterances -- only the stream is per-utterance.  Guarded by a lock because
# local_recognize() runs on a worker thread via asyncio.to_thread().
_RECOGNIZERS: dict[str, object] = {}
_RECOGNIZER_LOCK = threading.Lock()


def local_recognize(pcm: bytes, model_dir: str, log) -> str | None:
    """Synchronous: recognise a whole 16k s16le mono utterance offline."""
    try:
        import numpy as np
        from sherpa_onnx import OnlineRecognizer
    except ImportError as exc:
        log(f"local-asr: missing dependency ({exc}); pip install sherpa-onnx numpy")
        return None

    encoder = os.path.join(model_dir, "encoder.int8.onnx")
    decoder = os.path.join(model_dir, "decoder.int8.onnx")
    tokens = os.path.join(model_dir, "tokens.txt")
    for path in (encoder, decoder, tokens):
        if not os.path.exists(path):
            log(f"local-asr: model file missing: {path}")
            return None

    with _RECOGNIZER_LOCK:
        recognizer = _RECOGNIZERS.get(model_dir)
        if recognizer is None:
            load_started = time.monotonic()
            recognizer = OnlineRecognizer.from_paraformer(
                tokens=tokens, encoder=encoder, decoder=decoder,
                num_threads=2, decoding_method="greedy_search",
                enable_endpoint_detection=False)
            _RECOGNIZERS[model_dir] = recognizer
            log(f"local-asr: model loaded in {time.monotonic() - load_started:.1f}s "
                f"(cached for later utterances)")
    stream = recognizer.create_stream()

    samples = (np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0)
    stream.accept_waveform(16000, samples)
    # tail padding: streaming models drop the last chars without trailing silence
    stream.accept_waveform(16000, np.zeros(int(0.5 * SAMPLE_RATE), dtype=np.float32))
    while recognizer.is_ready(stream):
        recognizer.decode_stream(stream)
    text = (recognizer.get_result_all(stream).text or "").strip()
    log(f"local-asr: {len(pcm) / (SAMPLE_RATE * 2):.2f}s audio -> {text!r}")
    return text or None


async def volc_asr_stream(pcm: bytes, env: dict[str, str], log) -> str | None:
    """Stream ``pcm`` to Volcengine BigModel SAUC and return the final text.

    Returns None on failure (caller decides how to proceed).  ``log`` is a
    callable str -> None used for session-level logging.
    """
    api_key = env.get("VOLC_API_KEY", "").strip()
    resource_id = env.get("VOLC_ASR_RESOURCE_ID", "volc.bigasr.sauc.duration").strip()
    if not api_key:
        log("volc: VOLC_API_KEY missing in volc.env (or --volc-key); cannot run real ASR")
        return None

    headers = {
        "X-Api-Key": api_key,
        "X-Api-Resource-Id": resource_id,
        "X-Api-Request-Id": str(uuid.uuid4()),
        "X-Api-Sequence": "-1",
    }
    request_id = headers["X-Api-Request-Id"]
    log(f"volc: connecting ({resource_id}) req={request_id} audio={len(pcm)}B/{len(pcm) // FRAME_BYTES}frames")

    def wrap(msg_type: int, flags: int, payload: bytes = b"") -> bytes:
        body = gzip.compress(payload, compresslevel=6, mtime=0)
        header = struct.pack(">BBBB",
                             HEADER_FULL_SEND, (msg_type << 4) | flags,
                             0x11, 0)
        return header + struct.pack(">I", len(body)) + body

    full_request = {
        "user": {
            "uid": "mibot-esp32s3", "did": "mibot", "platform": "embedded",
            "sdk_version": "0.0.1", "app_version": "0.0.1",
        },
        "audio": {
            "format": "pcm", "codec": "raw", "rate": SAMPLE_RATE,
            "bits": 16, "channel": 1, "language": "zh-CN",
        },
        "request": {
            "model_name": env.get("VOLC_ASR_MODEL", "bigmodel").strip() or "bigmodel",
            "enable_itn": True, "enable_punc": True, "enable_ddc": False,
            "result_type": "full", "source_lang": "zh-CN",
        },
    }

    async with websockets.connect(VOLC_WS_URI, additional_headers=headers,
                                  max_size=None, ping_interval=None) as ws:
        await ws.send(wrap(MT_FULL_REQUEST, 0, json.dumps(full_request).encode("utf-8")))
        # 200 ms chunks, paced 10x faster than real time
        seq = 1
        for offset in range(0, len(pcm) - CHUNK_BYTES + 1, CHUNK_BYTES):
            await ws.send(wrap(MT_AUDIO_ONLY, FLAG_SEQ, pcm[offset:offset + CHUNK_BYTES]))
            seq += 1
            await asyncio.sleep(0.02)
        if len(pcm) % CHUNK_BYTES:
            tail = pcm[-(len(pcm) % CHUNK_BYTES):]
            await ws.send(wrap(MT_AUDIO_ONLY, FLAG_SEQ, tail))
            seq += 1
        await ws.send(wrap(MT_AUDIO_ONLY, FLAG_LAST, b""))  # close utterance
        log(f"volc: sent {seq - 1} audio chunks, awaiting result")

        text = ""
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=20.0)
            except asyncio.TimeoutError:
                log("volc: timeout waiting for final result")
                break
            except websockets.exceptions.ConnectionClosed as exc:
                log(f"volc: connection closed mid-result: {exc!r} ({exc.code})")
                break
            if isinstance(raw, str):
                data = json.loads(raw)
            else:
                if len(raw) < 8:
                    continue
                payload_len = struct.unpack(">I", raw[4:8])[0]
                data = json.loads(gzip.decompress(raw[8:8 + payload_len]))
            code = data.get("code", 20000000)
            if code != 20000000:
                log(f"volc: server error code={code} msg={data.get('message', '')} "
                    f"seq={data.get('audio_info', {}).get('seq', '?')} "
                    f"logid={data.get('logid', '?')}")
                if code == 45000001:
                    log("volc: 资源 ID 未开通/错误,尝试换 volc.bigasr.sauc.concurrent "
                        "(或 2.0 用 volc.seedasr.sauc.*)")
                return None
            result = data.get("result") or {}
            current = result.get("text") or ""
            if current:
                text = current
            utterances = result.get("utterances") or []
            if utterances and utterances[-1].get("definite"):
                text = utterances[-1].get("text") or text
                log(f"volc: final definite -> {text!r}")
                return text.strip()
        if text:
            log(f"volc: final (timeout) -> {text!r}")
            return text.strip()
        log("volc: no transcript in response")
        return None


# ---- Edge TTS downlink (reused from mock gateway, already verified) ----

def _ffmpeg_mp3_to_pcm16k(mp3: bytes) -> bytes | None:
    import shutil
    import subprocess

    if shutil.which("ffmpeg") is None:
        return None
    try:
        proc = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error",
             "-f", "mp3", "-i", "-", "-f", "s16le", "-ac", "1",
             "-ar", str(SAMPLE_RATE), "-"],
            input=mp3, capture_output=True, timeout=120,
        )
    except (subprocess.SubprocessError, OSError):
        return None
    return proc.stdout if proc.returncode == 0 else None


async def synth_edge_pcm(text: str, voice: str) -> bytes | None:
    try:
        import edge_tts
    except ImportError:
        print("error: real TTS needs `pip install edge-tts`", file=sys.stderr)
        return None
    try:
        communicate = edge_tts.Communicate(text, voice)
        chunks: list[bytes] = []
        async for chunk in communicate.stream():
            if chunk.get("type") == "audio":
                chunks.append(chunk["data"])
    except Exception as exc:  # noqa: BLE001 - network failures fall back
        print(f"edge_tts failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        return None
    mp3 = b"".join(chunks)
    if not mp3:
        return None
    return await asyncio.to_thread(_ffmpeg_mp3_to_pcm16k, mp3)


def pcm_speech_like(duration_ms: int) -> bytes:
    import math
    total = int(SAMPLE_RATE * duration_ms / 1000)
    out = bytearray()
    for n in range(total):
        t = n / SAMPLE_RATE
        freq = 440.0 if int(t * 6) % 2 == 0 else 660.0
        envelope = 0.6 + 0.4 * math.sin(2.0 * math.pi * 3.0 * t)
        value = int(12000 * envelope * math.sin(2.0 * math.pi * freq * n / SAMPLE_RATE))
        out += struct.pack("<h", max(-32768, min(32767, value)))
    return bytes(out)


# ---- Device-facing session (protocol identical to mock_cloud_audio.py) ----

class Session:
    def __init__(self, args: argparse.Namespace, env: dict[str, str]) -> None:
        self.args = args
        self.env = env
        self.uplink = bytearray()
        self.uplink_bytes = 0
        self.uplink_frames = 0
        self.asr_sent = 0
        self.tts_streams = 0
        # Monotonic timestamp of the last uplink audio frame, used to keep the
        # keepalive off the wire while the ESP32 is transmitting.
        self.last_uplink_at = 0.0
        # In-flight TTS synthesis + downlink, run off the receive path.
        self.tts_task: asyncio.Task | None = None

    def log(self, message: str) -> None:
        print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def dump_uplink_pcm(out_dir: str, pcm: bytes, session: Session) -> None:
    """Write the raw captured uplink PCM (16k s16le mono) to out_dir for
    offline mic-level/quality analysis.  Diagnostic aid for \"mic sounds
    wrong\" reports; opt-in via --dump-uplink DIR."""
    try:
        os.makedirs(out_dir, exist_ok=True)
        name = time.strftime("uplink_%H%M%S") + f"_{session.uplink_frames}f.pcm"
        path = os.path.join(out_dir, name)
        with open(path, "wb") as fh:
            fh.write(pcm)
        session.log(f"uplink PCM dumped: {path} ({len(pcm)} bytes)")
    except OSError as exc:
        session.log(f"uplink PCM dump failed: {exc}")


async def send_pcm_stream(ws, session: Session, pcm: bytes, pace: bool) -> None:
    # Pace against a wall clock, not a per-frame sleep.
    #
    # `await asyncio.sleep(0.01)` per 640 B frame is nominally 2x real time, and
    # measured far worse: 519 frames (10.4 s of audio) went out in 3 s, i.e. ~6x.
    # The ESP32 forwards to the SF32 at 20 ms/frame and its downlink ring holds
    # only MIBOT_AUDIO_DN_DEPTH_PSRAM = 25 frames (500 ms), so everything beyond
    # that was dropped on the ESP32 -- observed as underrun=52 on the SF32 and
    # heard as chopped speech.  Sending in real time with a small lead keeps the
    # ring topped up without ever overflowing it.
    lead_s = PACE_LEAD_MS / 1000.0
    frame_s = FRAME_SAMPLES / SAMPLE_RATE          # 0.02
    started = time.monotonic()
    sent = 0
    frames = 0
    for offset in range(0, len(pcm) - FRAME_BYTES + 1, FRAME_BYTES):
        await ws.send(pcm[offset:offset + FRAME_BYTES])
        sent += FRAME_BYTES
        frames += 1
        if pace:
            # Stay at most `lead_s` ahead of real time.
            ahead = (started + frames * frame_s - lead_s) - time.monotonic()
            if ahead > 0:
                await asyncio.sleep(ahead)
    await ws.send(json.dumps({"eos": True}))
    session.tts_streams += 1
    elapsed = time.monotonic() - started
    session.log(f"TTS downlink complete: {sent} bytes ({frames} frames) + eos "
                f"in {elapsed:.1f}s for {frames * frame_s:.1f}s of audio")


class _SelfTestLog:
    def __call__(self, msg: str) -> None:
        print(f"[tt] {msg}", flush=True)


async def _self_test(args: argparse.Namespace, env: dict[str, str]) -> None:
    with open(args.recognize_file, "rb") as fh:
        pcm = fh.read()
    print(f"self-test: recognized file {args.recognize_file} "
          f"({len(pcm)} bytes / {len(pcm) // FRAME_BYTES} frames / "
          f"{len(pcm) / (SAMPLE_RATE * 2):.2f} s)", flush=True)

    text = None
    if args.asr_engine == "local":
        text = await asyncio.to_thread(local_recognize, pcm, args.asr_model_dir, _SelfTestLog())
    elif args.asr_engine == "volc":
        text = await volc_asr_stream(pcm, env, _SelfTestLog())
    else:
        text = args.transcript
    print(f"self-test: ASR text = {text!r}", flush=True)
    raise SystemExit(0 if text else 2)


async def run_tts_downlink(ws, session: Session, text: str) -> None:
    """Synthesise and stream one reply, off the receive path.  See handle_text."""
    try:
        pcm = await synth_edge_pcm(text or " ", session.args.tts_voice)
        if pcm is None:
            pcm = pcm_speech_like(session.args.tts_ms)
            session.log("edge TTS failed; fell back to synthetic warble")
        else:
            session.log(
                f"edge synth: {len(pcm)} bytes "
                f"({len(pcm) // FRAME_BYTES} frames) voice={session.args.tts_voice}"
            )
        await send_pcm_stream(ws, session, pcm, pace=not session.args.tts_no_pace)
    except asyncio.CancelledError:
        session.log("TTS downlink cancelled")
        raise
    except websockets.exceptions.ConnectionClosed:
        session.log("TTS downlink stopped: connection closed")
    except Exception as exc:  # noqa: BLE001 - diagnostics
        session.log(f"TTS downlink error: {type(exc).__name__}: {exc}")


async def handle_text(ws, session: Session, message: str) -> None:
    message = message.strip().rstrip("\x00").strip()
    try:
        root = json.loads(message)
    except json.JSONDecodeError:
        session.log(f"uplink text (not JSON): {message[:120]!r}")
        return
    if not isinstance(root, dict):
        return

    if root.get("eos") is True and "type" not in root:
        seconds = session.uplink_bytes / (SAMPLE_RATE * 2)
        session.log(
            f"uplink eos after {session.uplink_bytes} bytes "
            f"({session.uplink_frames} frames, {seconds:.2f} s of audio)"
        )
        pcm = bytes(session.uplink)
        if session.args.dump_uplink:
            dump_uplink_pcm(session.args.dump_uplink, pcm, session)
        if session.uplink_bytes == 0 and not session.args.always_asr:
            session.log("no audio received; not sending an ASR result")
            return
        if session.args.asr_engine == "local":
            text = await asyncio.to_thread(local_recognize, pcm,
                                           session.args.asr_model_dir, session.log)
        elif session.args.asr_engine == "volc":
            text = await volc_asr_stream(pcm, session.env, session.log)
        else:
            text = session.args.transcript if session.uplink_bytes or session.args.always_asr else ""
        if not text:
            session.log("ASR produced no text; not sending an asr_result "
                        "(ESP32 keeps the session open for the next turn)")
        else:
            payload = {"type": "asr_result", "text": text, "final": True}
            await ws.send(json.dumps(payload, ensure_ascii=False))
            session.asr_sent += 1
            session.log(f"ASR result sent: {text!r}")
        session.uplink.clear()
        session.uplink_bytes = 0
        session.uplink_frames = 0
        return

    if root.get("type") == "tts_request":
        text = root.get("text", "")
        session.log(
            f"TTS request: stream_id={root.get('stream_id')!r} "
            f"voice={root.get('voice')!r} text={text!r}"
        )
        # Synthesis and the paced downlink must not run on the receive path.
        #
        # This handler is awaited from inside "async for message in ws", so
        # doing the work here stops the gateway reading the socket for the whole
        # of it -- a couple of seconds of edge synthesis plus a downlink paced to
        # real time, so 12+ seconds for a normal reply.  The ESP32 keeps sending
        # during that window, its writes back up once the TCP window fills, and
        # it tears the session down: "esp_transport_write() returned 0" after
        # "wrote 0/3200 in 1531 ms", which arrives here as close code 1006 and
        # loses the turn.
        #
        # Run it as a task so the receive loop keeps draining the socket.  Only
        # one downlink is meaningful at a time, so a new request supersedes an
        # unfinished one.
        if session.tts_task is not None and not session.tts_task.done():
            session.log("TTS request arrived while one was still streaming; "
                        "cancelling the previous downlink")
            session.tts_task.cancel()
        session.tts_task = asyncio.create_task(run_tts_downlink(ws, session, text))
        return

    if root.get("type") == "asr_inject":
        text = str(root.get("text", "")).strip()
        if not text:
            session.log("asr_inject: empty text, ignored")
            return
        # Relay to the most recently connected session other than the injector
        # itself (that is the real ESP32 when the ESP32 reconnected last).
        target_ws, target_session = None, None
        for saved_ws, saved_session in reversed(list(_ESP32_SESSIONS.items())):
            if saved_ws is not ws:
                target_ws, target_session = saved_ws, saved_session
                break
        if target_ws is None:
            session.log("asr_inject: no other ESP32 session connected")
            return
        payload = {"type": "asr_result", "text": text, "final": True}
        await target_ws.send(json.dumps(payload, ensure_ascii=False))
        target_session.asr_sent += 1
        session.log(f"asr_inject: relayed asr_result {text!r} -> ESP32")
        return

    session.log(f"uplink text: {message[:200]}")


# Live WS sessions by socket, insertion-ordered.  A PC-side diagnostic client
# can inject a fake cloud ASR result with {"type":"asr_inject","text":"..."}
# via any connection and the gateway relays it to the most recent *other*
# session (the real ESP32) exactly as a local-ASR result would arrive.  This
# drives the full SF32 voice-agent turn deterministically from the PC without
# the microphone, for repeatable bring-up of the ASR->LLM->TTS chain.
_ESP32_SESSIONS: dict = {}


async def handler(ws, args: argparse.Namespace, env: dict[str, str]) -> None:
    session = Session(args, env)
    peer = getattr(ws, "remote_address", None)
    _ESP32_SESSIONS[ws] = session
    session.log(f"ESP32 connected from {peer}")
    keepalive_task = asyncio.create_task(keepalive(ws, session, args.keepalive_s))
    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                session.uplink.extend(message)
                session.uplink_bytes += len(message)
                session.uplink_frames += len(message) // FRAME_BYTES
                session.last_uplink_at = time.monotonic()
                if len(message) % FRAME_BYTES != 0:
                    session.log(
                        f"warning: binary frame {len(message)} bytes not multiple of {FRAME_BYTES}"
                    )
            else:
                await handle_text(ws, session, message)
    except websockets.exceptions.ConnectionClosed as exc:
        session.log(f"connection closed: {exc!r}")
    except Exception as exc:  # noqa: BLE001 - diagnostics
        session.log(f"handler error: {type(exc).__name__}: {exc}")
    finally:
        keepalive_task.cancel()
        if session.tts_task is not None and not session.tts_task.done():
            session.tts_task.cancel()
        _ESP32_SESSIONS.pop(ws, None)
        session.log(
            f"ESP32 disconnected (asr_sent={session.asr_sent} "
            f"tts_streams={session.tts_streams} "
            f"close_code={getattr(ws, 'close_code', None)} "
            f"uplink_bytes={session.uplink_bytes})"
        )


async def keepalive(ws, session: Session, period_s: float) -> None:
    """Send a periodic noop, but never while the ESP32 is streaming uplink audio.

    This is hygiene, not a fix for anything measured.  Raising --keepalive-s to
    600 did coincide with three clean turns while the 3 s default was losing
    roughly every second turn to close code 1006, but skipping the tick during
    captures did not reproduce that improvement, so the keepalive is not the
    cause and those three turns were luck.  The real cause was the gateway
    blocking its own receive loop during TTS (see handle_text).

    Kept anyway because a connection with audio flowing on it is self-evidently
    alive and has nothing to gain from a keepalive, and because writing into an
    active uplink is pointless work on both ends.
    """
    quiet_s = min(1.0, period_s / 2)
    try:
        while True:
            await asyncio.sleep(period_s)
            since_uplink = time.monotonic() - session.last_uplink_at
            if session.last_uplink_at and since_uplink < quiet_s:
                continue
            await ws.send(json.dumps({"type": "noop"}))
    except (asyncio.CancelledError, websockets.exceptions.ConnectionClosed):
        return


async def main_async(args: argparse.Namespace, env: dict[str, str]) -> None:
    async def route(ws, *_):
        await handler(ws, args, env)

    async with websockets.serve(route, args.host, args.port, max_size=None,
                                ping_interval=None):
        if args.asr_engine == "local":
            asr = ("local sherpa-onnx Paraformer (offline, no key)"
                   f" model={os.path.basename(args.asr_model_dir)}")
        elif args.asr_engine == "volc":
            asr = ("Volcengine BigModel SAUC" if env.get("VOLC_API_KEY")
                   else f"mock transcript {args.transcript!r} (no VOLC_API_KEY in volc.env)")
        else:
            asr = f"mock transcript {args.transcript!r}"
        print(
            f"volc audio gateway listening on ws://{args.host}:{args.port}\n"
            f"  asr        : {asr}\n"
            f"  tts        : edge {args.tts_voice} (real Chinese speech)\n"
            "Point the ESP32 at this URI via MIBOT_CLOUD_WS_URI, then trigger a turn.",
            flush=True,
        )
        await asyncio.Future()


def parse_args() -> argparse.Namespace:
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--env", default=os.path.join(here, "volc.env"),
                        help="path to the volc.env credential file")
    parser.add_argument("--asr-engine", choices=["local", "volc", "mock"], default="local",
                        help="local = offline sherpa-onnx (no key, default); "
                             "volc = Volcengine BigModel streaming (needs volc.env key); "
                             "mock = canned --transcript")
    parser.add_argument("--asr-model-dir", default=_default_model_dir(),
                        help="sherpa-onnx streaming-paraformer-zh model directory "
                             "(encoder/decoder int8 onnx + tokens.txt)")
    parser.add_argument("--transcript", default="你好，请用一句话介绍你自己",
                        help="mock-ASR canned text (used with --asr-engine mock)")
    parser.add_argument("--recognize-file", default="",
                        help="self-test mode: run ASR on a raw 16k s16le mono PCM file, "
                             "print the recognized text, then exit")
    parser.add_argument("--tts-ms", type=int, default=2000)
    parser.add_argument("--tts-no-pace", action="store_true")
    parser.add_argument("--tts-voice", default="zh-CN-XiaoxiaoNeural")
    parser.add_argument("--always-asr", action="store_true")
    parser.add_argument("--keepalive-s", type=float, default=3.0)
    parser.add_argument("--dump-uplink", default="",
                        help="directory: dump every captured uplink PCM chunk "
                             "(16k s16le mono) there for offline mic analysis")
    return parser.parse_args()


if __name__ == "__main__":
    # GBK 控制台保护:中文 print 绝不因编码崩
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    parsed = parse_args()
    env = _load_env(parsed.env)
    try:
        if parsed.recognize_file:
            asyncio.run(_self_test(parsed, env))
        else:
            asyncio.run(main_async(parsed, env))
    except KeyboardInterrupt:
        pass