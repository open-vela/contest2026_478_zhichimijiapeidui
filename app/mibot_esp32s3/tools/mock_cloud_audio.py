#!/usr/bin/env python3
"""Mock cloud audio gateway for the Mibot ESP32 <-> SF32 voice pipeline.

This implements exactly the WebSocket contract the ESP32 audio link already
speaks, so the whole chain (SF32 mic -> ESP32 -> cloud ASR -> DeepSeek ->
cloud TTS -> ESP32 -> SF32 speaker) can be validated before the real AI
backend exists.  It is a test double, not a product component.

Protocol as implemented by esp32s3/mibot_esp32s3/main/mibot_audio.cpp:

  Uplink (ESP32 -> cloud)
    * binary frames: raw PCM, 16 kHz, mono, signed 16-bit little endian,
      aggregated as a whole number of 640-byte (20 ms) frames
    * text frame ``{"eos": true}`` when the capture window closes
    * text frame ``{"type": "tts_request", "text": ..., "voice": ...,
      "stream_id": ...}`` when the robot wants speech synthesised

  Downlink (cloud -> ESP32)
    * text frame ``{"type": "asr_result", "text": ..., "final": true}``
      (``{"schema": "mibot.asr.v1", "event": "asr_text", ...}`` is also
      accepted by the ESP32) -> relayed to the SF32 as AI_RESPONSE
    * binary frames: raw PCM in the same format as uplink -> played on the
      SF32 speaker via AUDIO_DOWN
    * text frame ``{"eos": true}`` to end the synthesised stream

Usage:
    python mock_cloud_audio.py --host 0.0.0.0 --port 8765 \
        --transcript "你好，请介绍一下你自己"

Then point the ESP32 at it:
    #define MIBOT_CLOUD_WS_URI "ws://<this-host-ip>:8765"

Requires: pip install websockets
"""

from __future__ import annotations

import argparse
import array
import asyncio
import json
import math
import os
import struct
import sys
import time
import wave

try:
    import websockets
except ImportError:  # pragma: no cover - dependency hint only
    print("error: pip install websockets", file=sys.stderr)
    raise SystemExit(2)


SAMPLE_RATE = 16000
CHANNELS = 1
FRAME_SAMPLES = 320          # 20 ms
FRAME_BYTES = FRAME_SAMPLES * 2


def pcm_tone(duration_ms: int, freq_hz: int, amplitude: int = 12000) -> bytes:
    """Signed 16-bit little-endian mono PCM sine, phase-continuous."""
    total = int(SAMPLE_RATE * duration_ms / 1000)
    out = bytearray()
    for n in range(total):
        value = int(amplitude * math.sin(2.0 * math.pi * freq_hz * n / SAMPLE_RATE))
        out += struct.pack("<h", value)
    return bytes(out)


def pcm_speech_like(duration_ms: int) -> bytes:
    """A two-tone warble so a successful TTS downlink is obvious by ear and
    clearly distinguishable from the SF32 local 2 kHz test tone."""
    total = int(SAMPLE_RATE * duration_ms / 1000)
    out = bytearray()
    for n in range(total):
        t = n / SAMPLE_RATE
        freq = 440.0 if int(t * 6) % 2 == 0 else 660.0
        envelope = 0.6 + 0.4 * math.sin(2.0 * math.pi * 3.0 * t)
        value = int(12000 * envelope * math.sin(2.0 * math.pi * freq * n / SAMPLE_RATE))
        out += struct.pack("<h", max(-32768, min(32767, value)))
    return bytes(out)


class Session:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.uplink_bytes = 0
        self.uplink_frames = 0
        self.asr_sent = 0
        self.tts_streams = 0
        # Whole utterance kept so it can be written out and measured on eos.
        self.uplink_pcm = bytearray()
        self.utterance = 0

    def log(self, message: str) -> None:
        print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def analyse_pcm(pcm: bytes) -> dict:
    """Signal statistics for one captured utterance.

    Deliberately done here rather than in firmware: the gateway sees exactly
    the bytes an ASR engine would receive, so measuring at this point proves
    what actually left the device instead of what the device believed it sent.
    """
    samples = array.array("h")
    usable = len(pcm) - (len(pcm) % 2)
    samples.frombytes(pcm[:usable])
    if sys.byteorder != "little":
        samples.byteswap()

    total = len(samples)
    stats = {
        "samples": total,
        "duration_s": total / SAMPLE_RATE,
        "peak": 0,
        "rms": 0.0,
        "dc": 0.0,
        "clipped": 0,
        "silent_frames": 0,
        "frame_peaks": [],
        "head": [],
        "even_odd_ratio": None,
    }
    if total == 0:
        return stats

    peak = 0
    total_sq = 0
    total_sum = 0
    clipped = 0
    for value in samples:
        magnitude = -value if value < 0 else value
        if magnitude > peak:
            peak = magnitude
        total_sq += value * value
        total_sum += value
        if magnitude >= 32000:
            clipped += 1

    stats["peak"] = peak
    stats["rms"] = math.sqrt(total_sq / total)
    stats["dc"] = total_sum / total
    stats["clipped"] = clipped
    stats["head"] = list(samples[:16])

    # Per-frame peaks: a cold-start artefact shows up as a few bad frames at
    # the head while the rest is fine, which a whole-utterance peak would hide.
    silent = 0
    for offset in range(0, total - FRAME_SAMPLES + 1, FRAME_SAMPLES):
        frame_peak = 0
        for value in samples[offset:offset + FRAME_SAMPLES]:
            magnitude = -value if value < 0 else value
            if magnitude > frame_peak:
                frame_peak = magnitude
        stats["frame_peaks"].append(frame_peak)
        if frame_peak <= 64:
            silent += 1
    stats["silent_frames"] = silent

    # Padding-misalignment detector.  The AUDPRC mono FIFO puts one 16-bit
    # sample in every 32-bit word, so a compaction that picks the wrong slot
    # yields alternating zeros.  Compare energy in even vs odd positions.
    even = sum(abs(v) for v in samples[0::2])
    odd = sum(abs(v) for v in samples[1::2])
    if even + odd > 0:
        stats["even_odd_ratio"] = odd / (even + 1e-9)
    return stats


def report_pcm(session: Session, pcm: bytes) -> None:
    stats = analyse_pcm(pcm)
    session.log(
        f"uplink analysis: {stats['samples']} samples "
        f"({stats['duration_s']:.2f} s) peak={stats['peak']} "
        f"rms={stats['rms']:.1f} dc={stats['dc']:+.1f} "
        f"clipped={stats['clipped']} silent_frames={stats['silent_frames']}"
        f"/{len(stats['frame_peaks'])}"
    )
    if stats["samples"] == 0:
        session.log("  verdict: NO AUDIO reached the gateway")
        return

    session.log(f"  head samples: {stats['head']}")
    peaks = stats["frame_peaks"]
    if peaks:
        head = " ".join(str(p) for p in peaks[:10])
        session.log(f"  first 10 frame peaks: {head}")
        session.log(f"  last 10 frame peaks : "
                    f"{' '.join(str(p) for p in peaks[-10:])}")

    # Interpretation hints, kept conservative: these flag a suspicion, they do
    # not prove a cause.
    ratio = stats["even_odd_ratio"]
    if ratio is not None and ratio < 0.05:
        session.log("  suspicion: odd-position samples are near zero -> the "
                    "device may be sending padded rather than dense PCM")
    if stats["peak"] <= 64:
        session.log("  verdict: silence (mic dead, muted, or gain far too low)")
    elif stats["peak"] < 512:
        session.log("  suspicion: very low level; ASR will struggle")
    if abs(stats["dc"]) > 500:
        session.log("  suspicion: large DC offset -> ADC high-pass filter not "
                    "settled or bypassed")
    if peaks and len(peaks) > 6:
        head_quiet = all(p <= 64 for p in peaks[:3])
        body_loud = any(p > 512 for p in peaks[3:])
        if head_quiet and body_loud:
            session.log("  suspicion: first frames are silent but later frames "
                        "are not -> ADC cold-start on capture enable")


def write_wav(session: Session, pcm: bytes) -> str | None:
    directory = session.args.dump_dir
    if not directory:
        return None
    os.makedirs(directory, exist_ok=True)
    session.utterance += 1
    path = os.path.join(
        directory,
        f"uplink-{time.strftime('%Y%m%d-%H%M%S')}-{session.utterance:03d}.wav",
    )
    with wave.open(path, "wb") as handle:
        handle.setnchannels(CHANNELS)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(pcm)
    return path


async def send_pcm_stream(ws, session: Session, pcm: bytes, pace: bool) -> None:
    """Send PCM as whole 640-byte frames, then end the stream with eos."""
    sent = 0
    for offset in range(0, len(pcm) - FRAME_BYTES + 1, FRAME_BYTES):
        await ws.send(pcm[offset:offset + FRAME_BYTES])
        sent += FRAME_BYTES
        if pace:
            # Real gateways stream faster than real time; a light pace keeps the
            # ESP32 downlink ring from dropping frames in this test double.
            await asyncio.sleep(0.01)
    await ws.send(json.dumps({"eos": True}))
    session.tts_streams += 1
    session.log(f"TTS downlink complete: {sent} bytes ({sent // FRAME_BYTES} frames) + eos")


async def handle_text(ws, session: Session, message: str) -> None:
    # Be liberal about trailing NUL/whitespace: embedded senders sometimes
    # include the string terminator in the frame length.
    message = message.strip().rstrip("\x00").strip()
    try:
        root = json.loads(message)
    except json.JSONDecodeError:
        session.log(f"uplink text (not JSON): {message[:120]!r}")
        return

    if not isinstance(root, dict):
        return

    if root.get("eos") is True and "type" not in root:
        session.log(
            f"uplink eos after {session.uplink_bytes} bytes "
            f"({session.uplink_frames} frames, "
            f"{session.uplink_bytes / (SAMPLE_RATE * 2):.2f} s of audio)"
        )
        captured = bytes(session.uplink_pcm)
        session.uplink_pcm.clear()
        report_pcm(session, captured)
        path = write_wav(session, captured)
        if path:
            session.log(f"  wrote {path}")

        if session.uplink_bytes == 0 and not session.args.always_asr:
            session.log("no audio received; not sending an ASR result")
            return
        payload = {
            "type": "asr_result",
            "text": session.args.transcript,
            "final": True,
        }
        await ws.send(json.dumps(payload, ensure_ascii=False))
        session.asr_sent += 1
        session.log(f"ASR result sent: {session.args.transcript!r}")
        session.uplink_bytes = 0
        session.uplink_frames = 0
        return

    if root.get("type") == "tts_request":
        text = root.get("text", "")
        session.log(
            f"TTS request: stream_id={root.get('stream_id')!r} "
            f"voice={root.get('voice')!r} text={text!r}"
        )
        pcm = (
            pcm_tone(session.args.tts_ms, session.args.tts_freq)
            if session.args.tts_plain_tone
            else pcm_speech_like(session.args.tts_ms)
        )
        await send_pcm_stream(ws, session, pcm, pace=not session.args.tts_no_pace)
        return

    session.log(f"uplink text: {message[:200]}")


async def handler(ws, args: argparse.Namespace) -> None:
    session = Session(args)
    peer = getattr(ws, "remote_address", None)
    session.log(f"ESP32 connected from {peer}")
    keepalive_task = asyncio.create_task(keepalive(ws, session, args.keepalive_s))
    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                session.uplink_bytes += len(message)
                session.uplink_frames += len(message) // FRAME_BYTES
                session.uplink_pcm += message
                if len(message) % FRAME_BYTES != 0:
                    session.log(
                        f"warning: binary frame {len(message)} bytes is not a "
                        f"multiple of {FRAME_BYTES}"
                    )
                if session.uplink_frames and session.uplink_frames % 25 == 0:
                    session.log(
                        f"uplink {session.uplink_bytes} bytes "
                        f"({session.uplink_frames} frames)"
                    )
            else:
                await handle_text(ws, session, message)
    except websockets.exceptions.ConnectionClosed as exc:
        session.log(f"connection closed: {exc!r}")
    except Exception as exc:  # noqa: BLE001 - diagnostics for a test double
        session.log(f"handler error: {type(exc).__name__}: {exc}")
    finally:
        keepalive_task.cancel()
        session.log(
            f"ESP32 disconnected (asr_sent={session.asr_sent} "
            f"tts_streams={session.tts_streams} "
            f"close_code={getattr(ws, 'close_code', None)} "
            f"close_reason={getattr(ws, 'close_reason', None)!r} "
            f"uplink_bytes={session.uplink_bytes})"
        )


async def keepalive(ws, session: Session, period_s: float) -> None:
    """Keep the session warm with a harmless application-level text frame.

    The ESP32 websocket client tears an idle session down (observed as close
    1006 roughly every 10 s with no traffic), so a real gateway should keep the
    link busy.  The ESP32 ignores a text frame that carries neither audio
    metadata nor an eos marker, so this is safe.
    """
    try:
        while True:
            await asyncio.sleep(period_s)
            await ws.send(json.dumps({"type": "noop"}))
    except (asyncio.CancelledError, websockets.exceptions.ConnectionClosed):
        return


async def main_async(args: argparse.Namespace) -> None:
    async def route(ws, *_):
        await handler(ws, args)

    # ping_interval=None disables the library's own WebSocket pings so session
    # lifetime is governed only by the application-level keepalive above.
    async with websockets.serve(route, args.host, args.port, max_size=None,
                                ping_interval=None):
        print(
            f"mock cloud audio gateway listening on ws://{args.host}:{args.port}\n"
            f"  transcript : {args.transcript!r}\n"
            f"  tts        : {args.tts_ms} ms "
            f"{'plain tone' if args.tts_plain_tone else 'warble'}\n"
            f"  dump dir   : {args.dump_dir or '(disabled)'}\n"
            "Point the ESP32 at this URI via MIBOT_CLOUD_WS_URI, then trigger a "
            "dialog turn on the SF32.",
            flush=True,
        )
        await asyncio.Future()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--transcript", default="你好，请用一句话介绍你自己",
                        help="ASR text returned after each uplink eos")
    parser.add_argument("--tts-ms", type=int, default=1500,
                        help="length of the synthesised PCM reply")
    parser.add_argument("--tts-freq", type=int, default=440)
    parser.add_argument("--tts-plain-tone", action="store_true",
                        help="send a steady tone instead of the warble")
    parser.add_argument("--tts-no-pace", action="store_true",
                        help="send PCM as fast as possible")
    parser.add_argument("--always-asr", action="store_true",
                        help="return an ASR result even if no audio arrived")
    parser.add_argument("--keepalive-s", type=float, default=3.0,
                        help="application-level keepalive period (0 disables)")
    parser.add_argument("--dump-dir", default="uplink-dump",
                        help="directory for per-utterance WAV files "
                             "(empty string disables)")
    return parser.parse_args()


if __name__ == "__main__":
    try:
        asyncio.run(main_async(parse_args()))
    except KeyboardInterrupt:
        pass
