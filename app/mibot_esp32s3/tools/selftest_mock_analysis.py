#!/usr/bin/env python3
"""Self-test for mock_cloud_audio.py's uplink analysis.

Feeds synthetic PCM that imitates each failure mode we are trying to tell
apart, so the analysis is trusted before it is pointed at real hardware.
"""

import math
import struct
import sys

from mock_cloud_audio import FRAME_SAMPLES, SAMPLE_RATE, analyse_pcm

failures = 0
checks = 0


def check(condition, message):
    global failures, checks
    checks += 1
    if not condition:
        print(f"FAIL: {message}")
        failures += 1


def pcm(values):
    return b"".join(struct.pack("<h", max(-32768, min(32767, int(v))))
                    for v in values)


def sine(n, freq, amp, dc=0):
    return [amp * math.sin(2 * math.pi * freq * i / SAMPLE_RATE) + dc
            for i in range(n)]


# --- healthy 2 kHz tone, the shape the board actually produced --------------
good = analyse_pcm(pcm(sine(SAMPLE_RATE, 2000, 500)))
check(abs(good["peak"] - 500) <= 2, f"peak ~500, got {good['peak']}")
check(abs(good["dc"]) < 5, f"dc ~0, got {good['dc']}")
check(good["silent_frames"] == 0, f"no silent frames, got {good['silent_frames']}")
check(abs(good["duration_s"] - 1.0) < 0.01, "duration 1 s")
check(good["even_odd_ratio"] > 0.5, "even/odd energy balanced on dense PCM")
check(good["clipped"] == 0, "nothing clipped")

# --- silence ---------------------------------------------------------------
silent = analyse_pcm(pcm([0] * SAMPLE_RATE))
check(silent["peak"] == 0, "silence has zero peak")
check(silent["silent_frames"] == len(silent["frame_peaks"]),
      "every frame counted silent")

# --- padded PCM leaking through (odd slots zero) ---------------------------
padded_values = []
for value in sine(SAMPLE_RATE // 2, 2000, 500):
    padded_values += [value, 0]
padded = analyse_pcm(pcm(padded_values))
check(padded["even_odd_ratio"] is not None and padded["even_odd_ratio"] < 0.05,
      f"padded PCM detected, ratio={padded['even_odd_ratio']}")

# --- large DC offset (HPF not working) -------------------------------------
dc_off = analyse_pcm(pcm(sine(SAMPLE_RATE, 2000, 500, dc=3000)))
check(abs(dc_off["dc"] - 3000) < 50, f"dc ~3000, got {dc_off['dc']}")

# --- cold start: first 3 frames silent, rest fine --------------------------
cold = [0] * (FRAME_SAMPLES * 3) + [int(v) for v in sine(FRAME_SAMPLES * 20, 2000, 900)]
cold_stats = analyse_pcm(pcm(cold))
check(all(p == 0 for p in cold_stats["frame_peaks"][:3]),
      "first three frames silent")
check(any(p > 512 for p in cold_stats["frame_peaks"][3:]),
      "later frames loud")

# --- clipping --------------------------------------------------------------
clip = analyse_pcm(pcm(sine(SAMPLE_RATE, 500, 32767)))
check(clip["clipped"] > 0, "clipping detected")

# --- degenerate input ------------------------------------------------------
check(analyse_pcm(b"")["samples"] == 0, "empty input handled")
check(analyse_pcm(b"\x01")["samples"] == 0, "odd trailing byte handled")

if failures:
    print(f"{failures} of {checks} checks failed")
    sys.exit(1)
print(f"all {checks} uplink-analysis checks passed")
