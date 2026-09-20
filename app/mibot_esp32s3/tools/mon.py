#!/usr/bin/env python3
"""Minimal ESP32 serial log capture to a file (non-interactive).

Reads COM7 (default) for a bounded number of seconds and appends everything to
a log file, so a second process can trigger the SF32 while we capture the
ESP32-side reaction.
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM7")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    p = serial.Serial(args.port, args.baud, timeout=0.2)
    out = open(args.out, "w", encoding="utf-8") if args.out else None
    deadline = time.time() + args.seconds
    try:
        while time.time() < deadline:
            n = p.in_waiting
            if n:
                data = p.read(n).decode("utf-8", errors="replace")
                sys.stdout.write(data)
                sys.stdout.flush()
                if out:
                    out.write(data)
                    out.flush()
            else:
                time.sleep(0.05)
    finally:
        p.close()
        if out:
            out.close()


if __name__ == "__main__":
    main()
