from __future__ import annotations

import unittest

from editor.plugins.AITool.services.model_call_budget import ModelCallLedger


class ModelCallLedgerTests(unittest.TestCase):
    def test_one_visible_reasoning_call_is_allowed_and_second_is_blocked(self) -> None:
        ledger = ModelCallLedger()

        first = ledger.claim(
            room_id="room-1",
            message_id="msg-1",
            correlation_id="corr-1",
            purpose="agent_visible_reasoning",
            provider="quasar",
            model="configured_chat_model",
            plan_version=3,
            max_calls=1,
        )
        second = ledger.claim(
            room_id="room-1",
            message_id="msg-1",
            correlation_id="corr-1",
            purpose="agent_visible_reasoning",
            provider="quasar",
            model="configured_chat_model",
            plan_version=3,
            max_calls=1,
        )

        self.assertTrue(first.allowed)
        self.assertEqual(first.evidence.dedupe_result, "executed")
        self.assertFalse(second.allowed)
        self.assertEqual(second.evidence.dedupe_result, "budget_exhausted")
        summary = ledger.summary(room_id="room-1", message_id="msg-1")
        self.assertEqual(summary["call_count"], 1)
        self.assertEqual(summary["calls"][0]["plan_version"], 3)

    def test_zero_call_summary_and_summary_claim_are_deterministic(self) -> None:
        ledger = ModelCallLedger()

        summary = ledger.summary(
            room_id="room-zero",
            message_id="msg-zero",
            correlation_id="corr-zero",
        )

        self.assertEqual(summary["call_count"], 0)
        self.assertEqual(summary["purposes"], [])
        self.assertTrue(ledger.claim_summary(room_id="room-zero", message_id="msg-zero"))
        self.assertFalse(ledger.claim_summary(room_id="room-zero", message_id="msg-zero"))


if __name__ == "__main__":
    unittest.main()
