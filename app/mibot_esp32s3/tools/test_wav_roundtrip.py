#!/usr/bin/env python3
"""Small dependency-free WAV round-trip check for CI and development."""
import io
import wave


def encode(pcm: bytes) -> bytes:
    stream = io.BytesIO()
    with wave.open(stream, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(pcm)
    return stream.getvalue()


def main() -> int:
    # PCM16 streams contain complete two-byte samples; arbitrary frame counts
    # still exercise odd 640-byte frame boundaries while keeping valid WAV.
    for size in (0, 2, 638, 640, 642, 4096):
        pcm = bytes((index * 17) & 0xFF for index in range(size))
        encoded = encode(pcm)
        with wave.open(io.BytesIO(encoded), "rb") as wav:
            assert wav.getnchannels() == 1
            assert wav.getsampwidth() == 2
            assert wav.getframerate() == 16000
            assert wav.readframes(wav.getnframes()) == pcm
    print("wav round-trip passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
