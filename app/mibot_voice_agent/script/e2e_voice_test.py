#!/usr/bin/env python3
"""End-to-end voice pipeline test for SF32 <-> ESP32 <-> cloud gateway.

Drives one or more dialog turns on the SF32 while capturing BOTH boards'
consoles from a single process, then prints a pass/fail verdict per checkpoint.
Running both serial ports from one process avoids the port contention you get
when two shells poll COM5/COM7 at the same time.

Usage:
    python e2e_voice_test.py --sf32 COM5 --esp32 COM7 --turns 2
    python e2e_voice_test.py --sf32 COM5 --turns 1 --stub      # all-stub build

Exit code is 0 only when every required checkpoint passed.
"""

from __future__ import annotations

import argparse
import re
import sys
import threading
import time

try:
    import serial
except ImportError:  # pragma: no cover
    print("error: pip install pyserial", file=sys.stderr)
    raise SystemExit(2)


class Reader(threading.Thread):
    """Continuously drain a serial port into a text buffer."""

    def __init__(self, port: str, baud: int, label: str, echo: bool) -> None:
        super().__init__(daemon=True)
        self.label = label
        self.echo = echo
        self.text = ""
        # NOTE: do not name this `_stop`; threading.Thread already defines
        # `_stop()` internally and shadowing it breaks join().
        self._stop_event = threading.Event()
        self._ser = serial.Serial(port, baud, timeout=0.2)

    def run(self) -> None:
        while not self._stop_event.is_set():
            try:
                data = self._ser.read(8192)
            except Exception:  # noqa: BLE001 - port may close during shutdown
                break
            if data:
                chunk = data.decode("utf-8", errors="replace")
                self.text += chunk
                if self.echo:
                    sys.stdout.write(chunk)
                    sys.stdout.flush()

    def reset_board(self) -> None:
        """Pulse the USB-serial control lines to reboot an ESP32."""
        self._ser.dtr = False
        self._ser.rts = True
        time.sleep(0.15)
        self._ser.rts = False
        self._ser.dtr = True

    def write_line(self, line: str) -> None:
        self._ser.write((line + "\r\n").encode("ascii", errors="replace"))
        self._ser.flush()

    def close(self) -> None:
        self._stop_event.set()
        self.join(timeout=2)
        try:
            self._ser.close()
        except Exception:  # noqa: BLE001
            pass


def count(pattern: str, text: str) -> int:
    return len(re.findall(pattern, text))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sf32", default="COM5")
    parser.add_argument("--sf32-baud", type=int, default=1000000)
    parser.add_argument("--esp32", default=None,
                        help="ESP32 console port; omit to skip ESP32 capture")
    parser.add_argument("--esp32-baud", type=int, default=115200)
    parser.add_argument("--turns", type=int, default=1)
    parser.add_argument("--turn-timeout", type=float, default=30.0,
                        help="seconds to wait for a turn to finish")
    parser.add_argument("--stub", action="store_true",
                        help="expect the all-stub build (local transcript + tone)")
    parser.add_argument("--echo", action="store_true", help="stream raw logs")
    parser.add_argument("--reset-esp32", action="store_true",
                        help="reboot the ESP32 first so its log starts clean")
    parser.add_argument("--esp32-boot-s", type=float, default=12.0,
                        help="seconds to wait after an ESP32 reset")
    args = parser.parse_args()

    sf32 = Reader(args.sf32, args.sf32_baud, "SF32", args.echo)
    esp32 = Reader(args.esp32, args.esp32_baud, "ESP32", False) if args.esp32 else None
    sf32.start()
    if esp32:
        esp32.start()
        if args.reset_esp32:
            # Start from a known state so a crash log from an earlier run cannot
            # be mistaken for a crash in this one.
            esp32.reset_board()
            print("ESP32 reset; waiting for Wi-Fi and gateway connect")
            time.sleep(args.esp32_boot_s)
            esp32.text = ""

    try:
        time.sleep(1.0)
        # One agent instance only: repeated launches leave stale tasks that
        # fight over the mic, the DAC and the UART.
        if "stub mode active" not in sf32.text and "UART /dev/ttyS0" not in sf32.text:
            print("launching mibot_voice_agent")
            sf32.write_line("mibot_voice_agent &")
            time.sleep(3.0)

        for turn in range(1, args.turns + 1):
            print(f"--- turn {turn} ---")
            mark = len(sf32.text)
            sf32.write_line("va_wake")
            deadline = time.time() + args.turn_timeout
            while time.time() < deadline:
                tail = sf32.text[mark:]
                done = ("SPEAK_DONE" in tail or "CLOUD_FAIL" in tail or
                        "tone complete" in tail or "playback stop" in tail)
                if done:
                    break
                time.sleep(0.5)
            time.sleep(2.0)

        sf_text = sf32.text
        esp_text = esp32.text if esp32 else ""

        checks: list[tuple[str, bool, str]] = []
        checks.append(("agent linked (HELLO_ACK)",
                       "HELLO_ACK" in sf_text, ""))
        # One "8N1 ready" line is printed per agent instance, so this counts
        # instances rather than log lines.  Stale instances fight over the mic,
        # the DAC and the UART and invalidate a run.
        checks.append(("single agent instance",
                       count(r"8N1 ready", sf_text) <= 1,
                       "multiple agents are running; power-cycle the SF32"))
        checks.append(("mic uplink sent",
                       count(r"capture stop \(dropped=\d+ sent=[1-9]", sf_text) >= 1,
                       "no AUDIO_UP frames were sent"))

        if args.stub:
            checks.append(("stub transcript used",
                           "ASR stub mode active" in sf_text, ""))
        else:
            checks.append(("cloud ASR text received",
                           "ASR final text received" in sf_text,
                           "gateway never returned a final mibot.asr.v1 text"))

        checks.append(("LLM request sent",
                       count(r"AI_REQUEST sent", sf_text) >= 1, ""))
        checks.append(("LLM response received",
                       count(r"AI_RESPONSE received", sf_text) >= 1, ""))

        if args.stub:
            checks.append(("placeholder tone played",
                           "tone complete" in sf_text, ""))
        else:
            checks.append(("TTS requested",
                           "robot.speak sent" in sf_text, ""))
            checks.append(("TTS audio played",
                           "AUDIO_DOWN playback" in sf_text,
                           "no AUDIO_DOWN playback was opened"))

        checks.append(("no cloud failure",
                       "CLOUD_FAIL" not in sf_text,
                       "a turn ended as CLOUD_FAIL"))

        if esp32:
            checks.append(("ESP32 did not crash",
                           "stack overflow" not in esp_text and "Guru Meditation" not in esp_text,
                           "ESP32 panic/reset during the run"))

        print("\n=== verdict ===")
        failures = 0
        for name, ok, hint in checks:
            print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f"  <- {hint}" if not ok and hint else ""))
            if not ok:
                failures += 1

        if esp32 and ("stack overflow" in esp_text or "Guru Meditation" in esp_text):
            print("\n=== ESP32 crash context ===")
            for line in esp_text.splitlines():
                if any(k in line for k in ("stack overflow", "Guru Meditation",
                                           "Backtrace", "rst:", "abort()",
                                           "assert", "Core  ", "watchdog")):
                    print("  " + line.strip())

        print(f"\nturns requested : {args.turns}")
        with open("e2e_sf32.log", "w", encoding="utf-8") as handle:
            handle.write(sf_text)
        if esp32:
            with open("e2e_esp32.log", "w", encoding="utf-8") as handle:
                handle.write(esp_text)
        print("logs written: e2e_sf32.log" + (", e2e_esp32.log" if esp32 else ""))

        print(f"AI_REQUEST count: {count(r'AI_REQUEST sent', sf_text)}")
        print(f"AI_RESPONSE cnt : {count(r'AI_RESPONSE received', sf_text)}")
        if not args.stub:
            print(f"ASR text count  : {count(r'ASR final text received', sf_text)}")
        return 0 if failures == 0 else 1
    finally:
        sf32.close()
        if esp32:
            esp32.close()


if __name__ == "__main__":
    raise SystemExit(main())
