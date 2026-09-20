#!/usr/bin/env python3
"""Send robot.set_text through ESP32 Wi-Fi TCP to the SF32 LCD."""
import argparse, json, socket, struct, time

SOF = b"\xaa\x55"

def crc16(data):
    crc = 0xffff
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc

def frame(kind, flags, seq, payload):
    header = bytes((1, kind, flags)) + struct.pack('<HH', seq, len(payload))
    return SOF + header + payload + struct.pack('<H', crc16(header + payload))

def read_exact(sock, n):
    out = b''
    while len(out) < n:
        part = sock.recv(n - len(out))
        if not part: raise RuntimeError('ESP32 closed connection')
        out += part
    return out

def read_frame(sock):
    window = b''
    while window != SOF:
        window = (window + read_exact(sock, 1))[-2:]
    header = read_exact(sock, 7)
    payload = read_exact(sock, struct.unpack_from('<H', header, 5)[0])
    received = struct.unpack('<H', read_exact(sock, 2))[0]
    if received != crc16(header + payload): raise RuntimeError('CRC mismatch')
    return header[1], json.loads(payload.decode())

def send_audio_stream(sock, frame_count):
    meta = {"schema":"mibot.audio.v1", "stream_id":"aud_pc_1",
            "codec":"pcm_s16le", "sample_rate":16000, "channels":1,
            "frame_ms":20, "bytes":640, "eos":False}
    seq = 100
    raw = json.dumps(meta, separators=(',', ':')).encode()
    sock.sendall(frame(0x30, 0, seq, raw))
    for index in range(frame_count):
        payload = bytes(((index + offset) & 0xff) for offset in range(640))
        sock.sendall(frame(0x30, 0, seq + index + 1, payload))
        time.sleep(0.02)
    print(f'sent {frame_count} AUDIO_UP frames')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host'); ap.add_argument('text', nargs='?', default='PC->ESP32->SF32')
    ap.add_argument('--port', type=int, default=3333)
    ap.add_argument('--audio-frames', type=int, default=0,
                    help='also send this many 20 ms AUDIO_UP frames')
    args = ap.parse_args()
    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        sock.settimeout(5)
        hello = {"schema":"mibot.uart.v1", "msg_id":"pc-hello", "protocol_version":1}
        sock.sendall(frame(0x01, 0, 1, json.dumps(hello,separators=(',',':')).encode()))
        print('HELLO_ACK:', read_frame(sock)[1])
        if args.audio_frames > 0:
            sock.sendall(frame(0x10, 1, 50, json.dumps({
                "schema":"mibot.uart.v1", "command_id":"pc-audio-mode",
                "name":"robot.set_audio_mode", "args":{"mode":"loopback"}},
                separators=(',', ':')).encode()))
            print('MODE_ACK:', read_frame(sock)[1])
            send_audio_stream(sock, args.audio_frames)
        command = {"schema":"mibot.uart.v1", "command_id":"pc-lcd-1",
                   "name":"robot.set_text", "args":{"text":args.text}}
        sock.sendall(frame(0x10, 1, 2, json.dumps(command,separators=(',',':'),ensure_ascii=False).encode()))
        while True:
            kind, body = read_frame(sock)
            print(('ACK' if kind == 0x11 else 'NACK' if kind == 0x12 else f'TYPE_0x{kind:02x}') + ':', body)
            if kind in (0x11, 0x12) and body.get('command_id') == 'pc-lcd-1':
                return 0 if kind == 0x11 and body.get('ok', False) else 2

if __name__ == '__main__':
    raise SystemExit(main())
