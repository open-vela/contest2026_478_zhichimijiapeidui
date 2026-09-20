#!/usr/bin/env python3
"""Host-side checks for the shared SF32/ESP32 AA55 transport.

The test intentionally has no project dependencies.  It is useful before a
board is connected: both firmware parsers must agree on byte order, CRC and
the raw IPv4 message types.
"""

from __future__ import annotations

import random
import unittest


SOF = b"\xaa\x55"
VERSION = 1
MAX_PAYLOAD = 4096
IP_TX = 0x70
IP_RX = 0x71


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(message_type: int, payload: bytes, sequence: int = 1, flags: int = 0) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    header = bytes((VERSION, message_type, flags, sequence & 0xFF, sequence >> 8,
                    len(payload) & 0xFF, len(payload) >> 8))
    checksum = crc16(header + payload)
    return SOF + header + payload + bytes((checksum & 0xFF, checksum >> 8))


class Parser:
    """Small reference parser mirroring the two firmware state machines."""

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.frames: list[tuple[int, int, bytes]] = []
        self.errors = 0

    def feed(self, data: bytes) -> None:
        self.buffer.extend(data)
        while True:
            start = self.buffer.find(SOF)
            if start < 0:
                # Keep a possible first SOF byte for the next chunk.
                self.buffer[:] = self.buffer[-1:] if self.buffer[-1:] == SOF[:1] else b""
                return
            if start:
                del self.buffer[:start]
            if len(self.buffer) < 2 + 7 + 2:
                return
            header = self.buffer[2:9]
            length = header[5] | (header[6] << 8)
            if header[0] != VERSION or length > MAX_PAYLOAD:
                del self.buffer[:2]
                self.errors += 1
                continue
            total = 2 + 7 + length + 2
            if len(self.buffer) < total:
                return
            payload = bytes(self.buffer[9:9 + length])
            received = self.buffer[9 + length] | (self.buffer[10 + length] << 8)
            del self.buffer[:total]
            if crc16(bytes(header) + payload) != received:
                self.errors += 1
                continue
            self.frames.append((header[1], header[3] | (header[4] << 8), payload))


class MibotLinkProtocolTest(unittest.TestCase):
    def test_crc_and_partial_reads(self) -> None:
        parser = Parser()
        frames = [
            encode(0x10, b'{"name":"robot.stop"}', 7),
            encode(IP_TX, bytes((0x45, 0x00)) + bytes(38), 8),
            encode(IP_RX, bytes((0x45, 0x00)) + bytes(38), 9),
        ]
        stream = b"noise" + b"".join(frames)
        random.seed(20260913)
        offset = 0
        while offset < len(stream):
            size = random.randint(1, 23)
            parser.feed(stream[offset:offset + size])
            offset += size
        self.assertEqual([frame[0] for frame in parser.frames], [0x10, IP_TX, IP_RX])
        self.assertEqual([frame[1] for frame in parser.frames], [7, 8, 9])
        self.assertEqual(parser.errors, 0)

    def test_bad_crc_is_dropped_and_parser_resynchronizes(self) -> None:
        parser = Parser()
        bad = bytearray(encode(0x10, b"bad", 10))
        bad[-1] ^= 0x80
        good = encode(0x11, b"ok", 11)
        parser.feed(bytes(bad) + good)
        self.assertEqual(parser.frames, [(0x11, 11, b"ok")])
        self.assertEqual(parser.errors, 1)

    def test_length_limit(self) -> None:
        parser = Parser()
        header = bytes((VERSION, 0x10, 0, 0, 0, (MAX_PAYLOAD + 1) & 0xFF,
                        (MAX_PAYLOAD + 1) >> 8))
        parser.feed(SOF + header + b"\x00\x00" + encode(0x11, b"ok", 2))
        self.assertEqual(parser.frames, [(0x11, 2, b"ok")])
        self.assertGreaterEqual(parser.errors, 1)

    def test_payload_limit_is_enforced_by_encoder(self) -> None:
        with self.assertRaises(ValueError):
            encode(0x70, b"x" * (MAX_PAYLOAD + 1))


if __name__ == "__main__":
    unittest.main()
