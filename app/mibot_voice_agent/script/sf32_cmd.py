#!/usr/bin/env python3
"""Non-interactive SF32 NSH helper.

Open the SF32 download/console UART, optionally send a series of NSH commands,
capture everything the board prints for a bounded window, then close.  Used to
script the voice-agent bring-up test without an interactive terminal.

Usage:
  python sf32_cmd.py --port COM5 --baud 1000000 \
      --read-ms 3000 --post-ms 1500 --cmd "help" --cmd "mibot_voice_agent &"
"""

import argparse
import sys
import time

import serial


def drain(port, ms):
    # The board logs UTF-8 (ASR transcripts are Chinese).  Decoding as ASCII
    # turned every transcript byte into a replacement char, which read as
    # corruption on the device when the device was fine.  A partial multi-byte
    # sequence can also straddle two reads, so carry the tail over.
    pending = b""
    deadline = time.time() + ms / 1000.0
    while time.time() < deadline:
        n = port.in_waiting
        if n:
            pending += port.read(n)
            # Keep an incomplete trailing sequence for the next iteration.
            split = len(pending)
            for back in range(1, min(4, len(pending) + 1)):
                if pending[-back] & 0xC0 == 0xC0:
                    expected = 2
                    lead = pending[-back]
                    if lead & 0xF0 == 0xE0:
                        expected = 3
                    elif lead & 0xF8 == 0xF0:
                        expected = 4
                    if back < expected:
                        split = len(pending) - back
                    break
                if pending[-back] & 0x80 == 0:
                    break
            chunk, pending = pending[:split], pending[split:]
            if chunk:
                emit(chunk.decode("utf-8", errors="replace"))
        else:
            time.sleep(0.02)
    if pending:
        emit(pending.decode("utf-8", errors="replace"))


def emit(text):
    # A GBK console cannot encode U+FFFD and used to abort the whole capture
    # with UnicodeEncodeError, losing the rest of a crash dump.  Write through
    # the raw buffer so the console code page cannot kill the run.
    try:
        sys.stdout.write(text)
        sys.stdout.flush()
    except UnicodeEncodeError:
        sys.stdout.flush()
        sys.stdout.buffer.write(text.encode("utf-8", errors="replace"))
        sys.stdout.buffer.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--read-ms", type=int, default=3000)
    ap.add_argument("--post-ms", type=int, default=1500)
    ap.add_argument("--cmd", action="append", default=[])
    args = ap.parse_args()

    # Prefer a UTF-8 console so Chinese transcripts render instead of turning
    # into question marks under the default GBK code page.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.bytesize = serial.EIGHTBITS
    port.parity = serial.PARITY_NONE
    port.stopbits = serial.STOPBITS_ONE
    port.timeout = 0.2
    port.write_timeout = 1.0
    # Do not toggle DTR/RTS: on the CH343 that can reset/hold the SF32.
    port.dtr = False
    port.rts = False

    port.open()
    print(f"=== {args.port} opened @ {args.baud} ===", flush=True)
    try:
        drain(port, args.read_ms)
        for c in args.cmd:
            print(f"\n>>> {c}", flush=True)
            port.write((c + "\r\n").encode("ascii"))
            port.flush()
            drain(port, args.post_ms)
        print("\n=== done ===", flush=True)
    finally:
        port.close()


if __name__ == "__main__":
    main()
