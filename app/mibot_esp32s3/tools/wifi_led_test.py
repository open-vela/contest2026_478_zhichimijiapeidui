#!/usr/bin/env python3
"""Test the Mibot Wi-Fi TCP transport and the onboard WS2812B LED.

Protocol: AA 55 | version | type | flags | seq (LE) | length (LE) |
payload | CRC16-CCITT-FALSE (LE).
"""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import json
import os
import socket
import sys
import time
from typing import BinaryIO


SOF = b"\xAA\x55"
VERSION = 1
MSG_HELLO = 0x01
MSG_HELLO_ACK = 0x02
MSG_COMMAND = 0x10
MSG_ACK = 0x11
MSG_NACK = 0x12
MAX_PAYLOAD = 4096
ACTION_NAMES = {"happy", "sad", "confused", "greeting", "thinking", "warning"}


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def make_frame(message_type: int, flags: int, sequence: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload is larger than 4096 bytes")
    header = bytes((VERSION, message_type, flags)) + sequence.to_bytes(2, "little") + len(payload).to_bytes(2, "little")
    crc = crc16_ccitt_false(header + payload)
    return SOF + header + payload + crc.to_bytes(2, "little")


def read_exact(stream: BinaryIO, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = stream.read(length - len(data))
        if not chunk:
            raise ConnectionError("ESP32 closed the TCP connection")
        data.extend(chunk)
    return bytes(data)


def read_frame(stream: BinaryIO) -> tuple[int, int, int, bytes]:
    # Resynchronize on AA55 in case a previous test left bytes in the stream.
    previous = None
    while True:
        value = read_exact(stream, 1)[0]
        if previous == 0xAA and value == 0x55:
            break
        previous = value

    header = read_exact(stream, 7)
    version, message_type, flags = header[:3]
    sequence = int.from_bytes(header[3:5], "little")
    length = int.from_bytes(header[5:7], "little")
    if version != VERSION or length > MAX_PAYLOAD:
        raise ValueError(f"invalid frame header: version={version}, length={length}")
    payload = read_exact(stream, length)
    received_crc = int.from_bytes(read_exact(stream, 2), "little")
    actual_crc = crc16_ccitt_false(header + payload)
    if received_crc != actual_crc:
        raise ValueError(f"CRC mismatch: received 0x{received_crc:04x}, expected 0x{actual_crc:04x}")
    return message_type, flags, sequence, payload


def read_json_response(stream: BinaryIO) -> tuple[int, dict]:
    message_type, _flags, sequence, payload = read_frame(stream)
    try:
        body = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"response payload is not JSON (seq={sequence})") from exc
    return message_type, body


def send_json(sock: socket.socket, message_type: int, flags: int, sequence: int, body: dict) -> None:
    payload = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    sock.sendall(make_frame(message_type, flags, sequence, payload))


def timestamp_after(seconds: int = 0) -> str:
    value = datetime.now(timezone.utc) + timedelta(seconds=seconds)
    return value.astimezone().isoformat(timespec="milliseconds")


def run(host: str, port: int, state: str, intensity: float, speed: int, duration_ms: int) -> int:
    if state not in {"on", "off", "toggle", "motor_a"} and state not in ACTION_NAMES:
        raise ValueError("state must be on, off, toggle, or an action name")

    print(f"Connecting to {host}:{port} ...")
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(5)
        stream = sock.makefile("rb")
        try:
            send_json(
                sock,
                MSG_HELLO,
                0,
                1,
                {"schema": "mibot.uart.v1", "msg_id": "pc-hello-1", "protocol_version": VERSION, "device": "pc-test"},
            )
            message_type, hello = read_json_response(stream)
            if message_type != MSG_HELLO_ACK:
                raise ValueError(f"expected HELLO_ACK (0x02), got 0x{message_type:02x}: {hello}")
            print("HELLO_ACK:", json.dumps(hello, ensure_ascii=False))

            command_id = f"pc_test_{int(time.time())}"
            if state == "motor_a":
                command_name = "robot.test_motor_a"
                args = {"speed": speed, "duration_ms": duration_ms, "simulate_tof": True}
            elif state in ACTION_NAMES:
                command_name = "robot.perform_action"
                args = {"action": state, "intensity": intensity}
            else:
                command_name = "robot.set_led"
                if state == "toggle":
                    # Toggle is implemented client-side so the device protocol stays
                    # deterministic: each COMMAND always carries an explicit bool.
                    current = bool(hello.get("status_led_on", False))
                    led_on = not current
                else:
                    led_on = state == "on"
                args = {"on": led_on}
            send_json(
                sock,
                MSG_COMMAND,
                1,
                2,
                {
                    "schema": "mibot.uart.v1",
                    "msg_id": f"pc-msg-{int(time.time())}",
                    "command_id": command_id,
                    "trace_id": "pc-wifi-test",
                    "created_at": timestamp_after(),
                    "expires_at": timestamp_after(3),
                    "name": command_name,
                    "args": args,
                },
            )
            deadline = time.monotonic() + (8 if state in ACTION_NAMES else 5)
            accepted = False
            while time.monotonic() < deadline:
                message_type, response = read_json_response(stream)
                label = "ACK" if message_type == MSG_ACK else "NACK" if message_type == MSG_NACK else f"TYPE_0x{message_type:02x}"
                print(f"{label}:", json.dumps(response, ensure_ascii=False))
                if message_type == MSG_NACK or response.get("ok") is False:
                    return 2
                if message_type == MSG_ACK:
                    if response.get("state") == "completed":
                        return 0
                    accepted = accepted or response.get("state") == "accepted"
                    if state not in ACTION_NAMES and state != "motor_a" and accepted:
                        return 0
            raise TimeoutError("timed out waiting for action completion")
        finally:
            stream.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Test Mibot ESP32-S3 commands over Wi-Fi")
    parser.add_argument(
        "state",
        choices=("on", "off", "toggle", "motor_a", *sorted(ACTION_NAMES)),
        help="LED state or predefined action name",
    )
    parser.add_argument("--intensity", type=float, default=1.0, help="action intensity, 0.0..1.0")
    parser.add_argument("--host", default=None, help="ESP32 STA address (required unless MIBOT_ESP32_HOST is set)")
    parser.add_argument("--port", type=int, default=3333, help="Mibot TCP test port")
    parser.add_argument("--speed", type=int, default=30, help="motor_a speed -50..50")
    parser.add_argument("--duration-ms", type=int, default=500, help="motor_a duration 50..1000 ms")
    args = parser.parse_args()
    if args.host is None:
        args.host = os.environ.get("MIBOT_ESP32_HOST")
    if not args.host:
        parser.error("--host is required in STA mode, for example --host 192.168.1.120")
    try:
        return run(args.host, args.port, args.state, args.intensity, args.speed, args.duration_ms)
    except (ConnectionError, OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
