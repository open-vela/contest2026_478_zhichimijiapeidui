#!/usr/bin/env python3
"""Regression checks for the shared AI_RESPONSE/ASR contract.

The UART type is intentionally shared by two logical streams:

* canonical ``mibot.asr.v1`` envelopes are unsolicited voice input and must
  only reach the frame callback;
* all other AI_RESPONSE payloads are synchronous gateway replies and may
  complete ``mibot_link_ai_request``.

This is a dependency-free host test of that wire contract.  It mirrors the
bounded decision made by the SF32 transport and catches accidental changes to
the schema/event distinction without requiring a board or a live UART.
"""

from __future__ import annotations

import json
import unittest


ASR_SCHEMA = "mibot.asr.v1"
ASR_EVENT = "asr_text"


def is_canonical_asr_payload(payload: bytes) -> bool:
    """Return true only for a complete JSON object with the ASR identity."""

    if not payload or payload[:1] != b"{" or len(payload) > 4096:
        return False
    try:
        root = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        # The SF32 implementation fails closed for a truncated envelope when
        # both identity fields are still recoverable.  agent_main will reject
        # the malformed object, but it must not wake an AI waiter.
        return (
            b'"schema":"mibot.asr.v1"' in payload
            and b'"event":"asr_text"' in payload
        )
    return (
        isinstance(root, dict)
        and root.get("schema") == ASR_SCHEMA
        and root.get("event") == ASR_EVENT
    )


def dispatch_ai_response(waiting: bool, payload: bytes) -> tuple[bool, bool]:
    """Model ``(ai_done, callback_called)`` for one received AI_RESPONSE."""

    # Every accepted AI_RESPONSE remains observable by the callback.  ASR is
    # deliberately excluded from the synchronous waiter transition.
    return waiting and not is_canonical_asr_payload(payload), True


class MibotAiResponseContractTest(unittest.TestCase):
    def test_final_asr_does_not_complete_pending_request(self) -> None:
        payload = (
            b'{"schema":"mibot.asr.v1","event":"asr_text",'
            b'"final":true,"text":"move forward"}'
        )
        self.assertEqual(dispatch_ai_response(True, payload), (False, True))

    def test_partial_asr_does_not_complete_pending_request(self) -> None:
        payload = (
            b'{"schema":"mibot.asr.v1","event":"asr_text",'
            b'"final":false,"text":"move"}'
        )
        self.assertEqual(dispatch_ai_response(True, payload), (False, True))

    def test_asr_identity_requires_exact_schema_and_event(self) -> None:
        wrong_schema = b'{"schema":"mibot.asr.v2","event":"asr_text"}'
        wrong_event = b'{"schema":"mibot.asr.v1","event":"transcript"}'
        malformed = b'{"schema":"mibot.asr.v1","event":"asr_text"'
        self.assertFalse(is_canonical_asr_payload(wrong_schema))
        self.assertFalse(is_canonical_asr_payload(wrong_event))
        self.assertTrue(is_canonical_asr_payload(malformed))
        self.assertEqual(dispatch_ai_response(True, malformed), (False, True))

    def test_regular_ai_response_completes_pending_request(self) -> None:
        payload = b'{"ok":true,"answer":"done"}'
        self.assertEqual(dispatch_ai_response(True, payload), (True, True))

    def test_asr_callback_is_preserved_without_waiter(self) -> None:
        payload = b'{"schema":"mibot.asr.v1","event":"asr_text"}'
        self.assertEqual(dispatch_ai_response(False, payload), (False, True))


if __name__ == "__main__":
    unittest.main()
