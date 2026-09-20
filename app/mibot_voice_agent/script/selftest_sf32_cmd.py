#!/usr/bin/env python3
"""Self-test for sf32_cmd.py's UTF-8 handling (no serial port needed).

Two real bugs are covered:
  1. transcripts decoded as ASCII became a row of replacement chars, which
     looked like device-side corruption when the device was fine;
  2. a GBK console raised UnicodeEncodeError on U+FFFD and aborted the capture,
     truncating a crash dump.
"""

import io
import sys
import time
import types

# sf32_cmd imports pyserial at module scope; stub it so this runs anywhere.
sys.modules.setdefault("serial", types.SimpleNamespace(
    Serial=object, EIGHTBITS=8, PARITY_NONE="N", STOPBITS_ONE=1))

import sf32_cmd  # noqa: E402

failures = 0
checks = 0


def check(condition, message):
    global failures, checks
    checks += 1
    if not condition:
        print(f"FAIL: {message}")
        failures += 1


class FakePort:
    """Feeds a scripted list of byte chunks, then reports nothing available."""

    def __init__(self, chunks):
        self.chunks = list(chunks)

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def read(self, _n):
        return self.chunks.pop(0) if self.chunks else b""


def capture(chunks, ms=120):
    buffer = io.StringIO()
    saved = sys.stdout
    sys.stdout = buffer
    try:
        sf32_cmd.drain(FakePort(chunks), ms)
    finally:
        sys.stdout = saved
    return buffer.getvalue()


TRANSCRIPT = "你好，请用一句话介绍你自己"
RAW = TRANSCRIPT.encode("utf-8")

# The exact byte count the board reported, which is what made the display bug
# diagnosable in the first place.
check(len(RAW) == 39, f"transcript is 39 UTF-8 bytes, got {len(RAW)}")

# --- whole transcript in one read ------------------------------------------
out = capture([b'transcript="' + RAW + b'"\n'])
check(TRANSCRIPT in out, f"transcript decoded intact, got {out!r}")
check("\ufffd" not in out, "no replacement chars for valid UTF-8")

# --- transcript split mid-character across two reads -----------------------
out = capture([RAW[:7], RAW[7:]])
check(TRANSCRIPT in out, f"split multi-byte sequence reassembled, got {out!r}")
check("\ufffd" not in out, "split sequence produced no replacement chars")

# --- split at every possible offset ---------------------------------------
for cut in range(1, len(RAW)):
    out = capture([RAW[:cut], RAW[cut:]])
    if TRANSCRIPT not in out:
        check(False, f"split at byte {cut} lost data: {out!r}")
        break
else:
    check(True, "every split offset reassembles")

# --- genuinely invalid bytes must not raise -------------------------------
out = capture([b"\xff\xfe ok\n"])
check("ok" in out, "invalid bytes do not discard the rest of the line")

# --- a console that cannot encode U+FFFD must not abort the capture -------
class HostileStdout:
    """Raises on non-ASCII the way a GBK console does."""

    def __init__(self):
        self.text = ""
        self.buffer = io.BytesIO()

    def write(self, text):
        text.encode("ascii")   # raises UnicodeEncodeError on U+FFFD
        self.text += text

    def flush(self):
        pass


hostile = HostileStdout()
saved = sys.stdout
sys.stdout = hostile
try:
    sf32_cmd.emit("head \ufffd tail")
    raised = False
except UnicodeEncodeError:
    raised = True
finally:
    sys.stdout = saved
check(not raised, "emit() survives a console that cannot encode U+FFFD")
check(b"tail" in hostile.buffer.getvalue(),
      "fallback wrote the text to the raw buffer")

if failures:
    print(f"{failures} of {checks} checks failed")
    sys.exit(1)
print(f"all {checks} sf32_cmd encoding checks passed")
