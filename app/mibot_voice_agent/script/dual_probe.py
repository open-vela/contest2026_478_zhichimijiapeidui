#!/usr/bin/env python3
"""Drive the SF32 (COM5) while capturing the ESP32 (COM7) reaction.

Opens both serial ports in one process, sends a series of SF32 NSH commands,
and interleaves everything both boards print so we can see the ESP32-side
handling of AI_REQUEST / commands that the SF32 voice agent emits.
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf-port", default="COM5")
    ap.add_argument("--sf-baud", type=int, default=1000000)
    ap.add_argument("--esp-port", default="COM7")
    ap.add_argument("--esp-baud", type=int, default=115200)
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--pre-ms", type=int, default=1500)
    ap.add_argument("--post-ms", type=int, default=6000)
    args = ap.parse_args()

    sf = serial.Serial(args.sf_port, args.sf_baud, timeout=0.1)
    sf.dtr = False
    sf.rts = False
    esp = serial.Serial(args.esp_port, args.esp_baud, timeout=0.1)
    # Do not reset the ESP32; just observe.
    esp.dtr = False
    esp.rts = False

    def pump(ms):
        deadline = time.time() + ms / 1000.0
        while time.time() < deadline:
            got = False
            n = sf.in_waiting
            if n:
                sys.stdout.write("[SF] " + sf.read(n).decode("ascii", "replace"))
                got = True
            n = esp.in_waiting
            if n:
                sys.stdout.write("[ESP] " + esp.read(n).decode("utf-8", "replace"))
                got = True
            sys.stdout.flush()
            if not got:
                time.sleep(0.03)

    try:
        pump(args.pre_ms)
        for c in args.cmd:
            sys.stdout.write("\n>>> " + c + "\n")
            sys.stdout.flush()
            sf.write((c + "\r\n").encode("ascii"))
            sf.flush()
            pump(args.post_ms)
        sys.stdout.write("\n=== done ===\n")
    finally:
        sf.close()
        esp.close()


if __name__ == "__main__":
    main()
