#!/usr/bin/env python3
"""Check audio API identifiers and the provider-neutral cloud contract."""
import re
import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    api = (root / "ACTION_API.md").read_text(encoding="utf-8")
    source = "\n".join(p.read_text(encoding="utf-8", errors="ignore")
                       for p in (root / "main").glob("*.[ch]*"))
    documented = set(re.findall(r"`([A-Za-z][A-Za-z0-9_.-]*)`", api))
    required = {"robot.speak", "robot.stop_audio", "robot.get_audio_stats",
                "robot.reset_audio_stats", "robot.set_audio_mode",
                "robot.play_test_tone", "audio_up_frames_received",
                "downlink_underrun", "mibot.audio.v1"}
    missing = sorted(item for item in required if item not in documented or item not in source)
    forbidden = sorted(item for item in ("Authorization", "AppKey", "wss://")
                       if item in (root / "main" / "mibot_cloud.h").read_text(encoding="utf-8"))
    if missing or forbidden:
        if missing:
            print("missing contract identifiers:", ", ".join(missing), file=sys.stderr)
        if forbidden:
            print("provider-specific terms in mibot_cloud.h:", ", ".join(forbidden), file=sys.stderr)
        return 2
    print("audio contract check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
