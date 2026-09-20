"""Inject one AI_REQUEST on COM4 and capture the ESP32's LLM reply/error line."""
import serial, sys, time, json, threading

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def crc16_append(crc: int, data: bytes) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def build_ai_request_frame(text: str, seq: int, request_id: str) -> bytes:
    payload = json.dumps(
        {"request_id": request_id,
         "body": {"messages": [{"role": "user", "content": text}]}},
        ensure_ascii=False,
    ).encode("utf-8")
    header = bytes([0x01, 0x40, 0x00, seq & 0xFF, (seq >> 8) & 0xFF,
                    len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    crc = crc16_append(0xFFFF, header)
    crc = crc16_append(crc, payload)
    return b"\xAA\x55" + header + payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    text = sys.argv[2] if len(sys.argv) > 2 else "你好"
    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.reset_input_buffer()

    buf = []
    done = threading.Event()
    def drain():
        while not done.is_set():
            r = ser.read(8192)
            if r:
                buf.append(r)
    t = threading.Thread(target=drain, daemon=True)
    t.start()

    frame = build_ai_request_frame(text, seq=0xB302, request_id="diag_llm_probe")
    print(f"inject AI_REQUEST '{text}' ({len(frame)}B)", flush=True)
    ser.write(frame)
    ser.flush()

    time.sleep(35)  # LLM round trip typically 2-10s; give retries room
    done.set()
    time.sleep(0.3)
    ser.close()
    data = b"".join(buf).decode("utf-8", errors="replace")
    print(data[-2000:])


if __name__ == "__main__":
    main()