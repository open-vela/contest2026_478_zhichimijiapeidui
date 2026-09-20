#!/usr/bin/env python3
"""Send a JSON LCD command through ESP32 to SF32 and print the returned ACK."""
import argparse
import json
import serial
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="ESP32 USB serial port, e.g. COM6")
    ap.add_argument("text", nargs="?", default="PC->ESP32->SF32")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    command = {
        "command_id": "pc-lcd-1",
        "name": "robot.set_text",
        "args": {"text": args.text},
    }
    with serial.Serial(args.port, args.baud, timeout=2) as ser:
        ser.reset_input_buffer()
        ser.write((json.dumps(command, separators=(",", ":")) + "\n").encode())
        ser.flush()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            line = ser.readline()
            if line:
                print(line.decode(errors="replace").rstrip())
                if b'"command_id":"pc-lcd-1"' in line:
                    return
    raise SystemExit("timeout waiting for SF32 ACK")


if __name__ == "__main__":
    main()
