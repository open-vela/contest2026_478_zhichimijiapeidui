#!/usr/bin/env python3
"""Host-side models for the SF32 PTT and mibot-link failure paths.

These tests intentionally model only the bounded state transitions.  They do
not require LVGL, NuttX, ESP-IDF, or a connected board.
"""

from __future__ import annotations

import unittest
from dataclasses import dataclass
from typing import Any, List


AI_RESPONSE = 0x41
EVENT = 0x20


def is_critical(message_type: int, payload: Any) -> bool:
    if message_type == AI_RESPONSE:
        return True
    if message_type != EVENT or not isinstance(payload, dict):
        return False
    if payload.get("schema") == "mibot.net.v1":
        return True
    return (
        payload.get("schema") == "mibot.asr.v1"
        and payload.get("event") == "asr_text"
        and payload.get("final") is True
    )


@dataclass
class CallbackEvent:
    message_type: int
    payload: Any

    @property
    def critical(self) -> bool:
        return is_critical(self.message_type, self.payload)


class BoundedCallbackQueue:
    """Reference behavior for invoke_callback's bounded queue policy."""

    def __init__(self, depth: int) -> None:
        if depth < 1:
            raise ValueError("depth must be positive")
        self.depth = depth
        self.events: List[CallbackEvent] = []
        self.dropped = 0

    def push(self, event: CallbackEvent) -> bool:
        # Keep one slot free for a later critical event.
        if not event.critical and len(self.events) >= self.depth - 1:
            self.dropped += 1
            return False
        if len(self.events) >= self.depth:
            victim = next(
                (index for index, queued in enumerate(self.events)
                 if not queued.critical),
                None,
            )
            if victim is None:
                self.dropped += 1
                return False
            del self.events[victim]
            self.dropped += 1
        self.events.append(event)
        return True


class PttState:
    """Small model of the UI state transitions guarded by the watchdog."""

    def __init__(self, timeout_ms: int = 15000) -> None:
        self.timeout_ms = timeout_ms
        self.recording = False
        self.processing = False
        self.deadline = 0

    def start(self) -> None:
        if self.processing:
            return
        self.recording = True
        self.deadline = 0

    def stop(self, now_ms: int) -> None:
        if not self.recording or self.processing:
            return
        self.recording = False
        self.processing = True
        self.deadline = now_ms + self.timeout_ms

    def accept_final(self) -> None:
        self.processing = False
        self.deadline = 0

    def abort(self) -> None:
        self.recording = False
        self.processing = False
        self.deadline = 0

    def tick(self, now_ms: int) -> bool:
        # Signed subtraction mirrors the uint32 tick-wrap-safe C check.
        expired = (
            self.processing
            and self.deadline != 0
            and ((now_ms - self.deadline) & 0xFFFFFFFF) < 0x80000000
        )
        if expired:
            self.abort()
        return expired


class QueueResilienceTest(unittest.TestCase):
    def test_ordinary_event_cannot_consume_reserved_slot(self) -> None:
        queue = BoundedCallbackQueue(depth=2)
        self.assertTrue(queue.push(CallbackEvent(EVENT, {"kind": "telemetry"})))
        self.assertFalse(queue.push(CallbackEvent(EVENT, {"kind": "action"})))
        self.assertEqual(len(queue.events), 1)
        self.assertEqual(queue.dropped, 1)

    def test_asr_final_survives_ordinary_event_pressure(self) -> None:
        queue = BoundedCallbackQueue(depth=2)
        self.assertTrue(queue.push(CallbackEvent(EVENT, {"kind": "telemetry"})))
        final = CallbackEvent(
            EVENT,
            {"schema": "mibot.asr.v1", "event": "asr_text",
             "text": "hello", "final": True},
        )
        self.assertTrue(queue.push(final))
        self.assertEqual(queue.events[-1], final)
        self.assertEqual(len(queue.events), 2)

    def test_depth_one_keeps_only_critical_event(self) -> None:
        queue = BoundedCallbackQueue(depth=1)
        self.assertFalse(queue.push(CallbackEvent(EVENT, {"kind": "telemetry"})))
        response = CallbackEvent(AI_RESPONSE, {"answer": "ok"})
        self.assertTrue(queue.push(response))
        self.assertEqual(queue.events, [response])

    def test_network_readiness_event_is_critical(self) -> None:
        queue = BoundedCallbackQueue(depth=2)
        self.assertTrue(queue.push(CallbackEvent(EVENT, {"kind": "telemetry"})))
        readiness = CallbackEvent(EVENT, {"schema": "mibot.net.v1", "ready": False})
        self.assertTrue(readiness.critical)
        self.assertTrue(queue.push(readiness))

    def test_critical_event_evicts_oldest_ordinary_event(self) -> None:
        queue = BoundedCallbackQueue(depth=3)
        ordinary = CallbackEvent(EVENT, {"id": 1})
        old_critical = CallbackEvent(AI_RESPONSE, {"answer": "old"})
        second_ordinary = CallbackEvent(EVENT, {"id": 2})
        # Simulate a queue filled before the reserve policy was introduced;
        # the new critical event must evict the oldest ordinary entry.
        queue.events = [ordinary, old_critical, second_ordinary]
        critical = CallbackEvent(AI_RESPONSE, {"answer": "new"})
        self.assertTrue(queue.push(critical))
        self.assertEqual(queue.events, [old_critical, second_ordinary, critical])
        self.assertEqual(queue.dropped, 1)


class PttResilienceTest(unittest.TestCase):
    def test_missing_final_is_released_by_watchdog(self) -> None:
        state = PttState(timeout_ms=15000)
        state.start()
        state.stop(100)
        self.assertTrue(state.processing)
        self.assertFalse(state.tick(15099))
        self.assertTrue(state.tick(15100))
        self.assertFalse(state.processing)
        self.assertFalse(state.recording)

    def test_disconnect_abort_releases_processing(self) -> None:
        state = PttState()
        state.start()
        state.stop(100)
        state.abort()
        self.assertFalse(state.processing)
        self.assertEqual(state.deadline, 0)

    def test_valid_final_releases_processing_without_timeout(self) -> None:
        state = PttState()
        state.start()
        state.stop(100)
        state.accept_final()
        self.assertFalse(state.processing)
        self.assertFalse(state.tick(20000))

    def test_empty_final_uses_same_abort_path(self) -> None:
        state = PttState()
        state.start()
        state.stop(100)
        # agent_main treats an empty final as a terminal, unsuccessful turn.
        state.abort()
        self.assertFalse(state.processing)

    def test_tick_wrap_is_safe(self) -> None:
        state = PttState(timeout_ms=100)
        start = 0xFFFFFFF0
        state.start()
        state.stop(start)
        self.assertFalse(state.tick(0x00000030))
        self.assertTrue(state.tick(0x00000060))


if __name__ == "__main__":
    unittest.main()
