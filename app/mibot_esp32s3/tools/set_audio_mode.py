#!/usr/bin/env python3
"""Set the ESP32 Audio_Link mode over the control TCP connection."""
import argparse
import json
import socket
import struct


SOF = b"\xaa\x55"


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(kind, flags, sequence, payload):
    header = bytes((1, kind, flags)) + struct.pack("<HH", sequence, len(payload))
    return SOF + header + payload + struct.pack("<H", crc16(header + payload))


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("ESP32 closed the control connection")
        data.extend(chunk)
    return bytes(data)


def read_frame(sock):
    window = bytearray()
    while bytes(window) != SOF:
        window.extend(read_exact(sock, 1))
        del window[:-2]
    header = read_exact(sock, 7)
    length = struct.unpack_from("<H", header, 5)[0]
    payload = read_exact(sock, length)
    received = struct.unpack("<H", read_exact(sock, 2))[0]
    if received != crc16(header + payload):
        raise RuntimeError("CRC mismatch")
    return header[1], json.loads(payload.decode("utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--mode", choices=("loopback", "capture", "cloud"), required=True)
    args = parser.parse_args()

    command_id = "pc-set-audio-mode"
    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        sock.settimeout(5)
        hello = json.dumps({"schema": "mibot.uart.v1", "msg_id": "pc-mode"},
                           separators=(",", ":")).encode("utf-8")
        sock.sendall(frame(0x01, 0, 1, hello))
        read_frame(sock)
        command = json.dumps({
            "schema": "mibot.uart.v1",
            "command_id": command_id,
            "name": "robot.set_audio_mode",
            "args": {"mode": args.mode},
        }, separators=(",", ":")).encode("utf-8")
        sock.sendall(frame(0x10, 1, 2, command))
        while True:
            kind, body = read_frame(sock)
            if kind in (0x11, 0x12) and body.get("command_id") == command_id:
                print(json.dumps(body, ensure_ascii=False, separators=(",", ":")))
                return 0 if kind == 0x11 and body.get("ok") else 2


if __name__ == "__main__":
    raise SystemExit(main())
