#!/usr/bin/env python3
"""Capture raw Local_Capture PCM from TCP/3333 and write a WAV file.

The setup connection puts the ESP32 in capture mode and asks the SF32 to
enter LISTENING before the raw PCM connection is opened.  This keeps the
capture command useful after a reboot instead of requiring two manual NSH or
TCP setup steps.
"""
import argparse
import json
import socket
import struct
import sys
import wave

SOF = b"\xaa\x55"


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(kind: int, flags: int, sequence: int, payload: bytes) -> bytes:
    header = bytes((1, kind, flags)) + struct.pack("<HH", sequence, len(payload))
    return SOF + header + payload + struct.pack("<H", crc16(header + payload))


def read_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("ESP32 closed the setup connection")
        data.extend(chunk)
    return bytes(data)


def read_frame(sock: socket.socket):
    window = bytearray()
    while bytes(window) != SOF:
        window.extend(read_exact(sock, 1))
        del window[:-2]
    header = read_exact(sock, 7)
    payload = read_exact(sock, struct.unpack_from("<H", header, 5)[0])
    received = struct.unpack("<H", read_exact(sock, 2))[0]
    if received != crc16(header + payload):
        raise RuntimeError("CRC mismatch in setup response")
    return header[1], json.loads(payload.decode("utf-8"))


def send_command(sock: socket.socket, sequence: int, command_id: str,
                 name: str, args: dict) -> None:
    payload = json.dumps({
        "schema": "mibot.uart.v1",
        "command_id": command_id,
        "name": name,
        "args": args,
    }, separators=(",", ":")).encode("utf-8")
    sock.sendall(frame(0x10, 1, sequence, payload))
    while True:
        kind, body = read_frame(sock)
        if kind in (0x11, 0x12) and body.get("command_id") == command_id:
            if kind != 0x11 or not body.get("ok"):
                error = body.get("error", {}).get("code", "unknown")
                raise RuntimeError(f"{name} rejected by ESP32 ({error})")
            return


def prepare_capture(host: str, port: int) -> None:
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.settimeout(5)
        hello = json.dumps({
            "schema": "mibot.uart.v1", "msg_id": "pc-capture-hello",
        }, separators=(",", ":")).encode("utf-8")
        sock.sendall(frame(0x01, 0, 1, hello))
        kind, _ = read_frame(sock)
        if kind != 0x02:
            raise RuntimeError("ESP32 did not acknowledge the setup HELLO")
        send_command(sock, 2, "pc-capture-mode", "robot.set_audio_mode",
                     {"mode": "capture"})
        send_command(sock, 3, "pc-capture-listening", "robot.set_expression",
                     {"name": "listening", "duration_ms": 10000})


def capture(host: str, port: int, output: str, startup_timeout: float) -> int:
    with socket.create_connection((host, port), timeout=10) as sock:
        sock.settimeout(startup_timeout)
        with wave.open(output, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            received_any = False
            while True:
                try:
                    chunk = sock.recv(4096)
                except KeyboardInterrupt:
                    break
                except socket.timeout as exc:
                    if not received_any:
                        raise TimeoutError(
                            f"no PCM received within {startup_timeout:g}s; "
                            "check SF32 audio DMA and UART wiring") from exc
                    break
                if not chunk:
                    break
                received_any = True
                sock.settimeout(None)
                wav.writeframesraw(chunk)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--out", required=True)
    parser.add_argument("--startup-timeout", type=float, default=15.0,
                        help="seconds to wait for the first PCM bytes")
    args = parser.parse_args()
    try:
        prepare_capture(args.host, args.port)
        return capture(args.host, args.port, args.out, args.startup_timeout)
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError, TimeoutError) as exc:
        print(f"capture failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
