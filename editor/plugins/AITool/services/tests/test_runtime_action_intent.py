from __future__ import annotations

import unittest

from editor.plugins.AITool.services.runtime_action_intent import (
    EntityNameValidator,
    MessageDispatchLedger,
    RuntimeActionIntentService,
)


class RuntimeActionIntentServiceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.service = RuntimeActionIntentService()

    def test_entity_lifecycle_question_is_read_only(self) -> None:
        intent = self.service.classify(
            "@GM 丘比特雕像已经加入了吗",
            message_id="msg-query",
            room_id="room-1",
            target_plan_id="plan-1",
        )

        self.assertEqual(intent.route, "runtime_read")
        self.assertEqual(intent.operation, "entity_status")
        self.assertEqual([item.canonical_name for item in intent.entities], ["丘比特雕像"])

    def test_entity_completion_question_is_read_only(self) -> None:
        intent = self.service.classify("丘比特雕像有没有生成完成")

        self.assertEqual(intent.route, "runtime_read")
        self.assertEqual(intent.operation, "entity_status")
        self.assertEqual([item.canonical_name for item in intent.entities], ["丘比特雕像"])

    def test_explicit_add_is_one_validated_entity(self) -> None:
        intent = self.service.classify("再加入一个丘比特雕像")

        self.assertEqual(intent.route, "runtime_write")
        self.assertEqual(intent.operation, "add")
        self.assertEqual([item.canonical_name for item in intent.entities], ["丘比特雕像"])

    def test_repeated_add_phrase_is_deduped_by_canonical_name(self) -> None:
        intent = self.service.classify("再加入一个丘比特雕像再加入一个丘比特雕像")

        self.assertEqual(intent.route, "runtime_write")
        self.assertEqual([item.canonical_name for item in intent.entities], ["丘比特雕像"])

    def test_suspected_typo_requires_clarification(self) -> None:
        intent = self.service.classify("再加入一个切比特雕像")

        self.assertEqual(intent.route, "runtime_read")
        self.assertTrue(intent.requires_confirmation)
        self.assertIn("丘比特雕像", intent.clarification)
        self.assertEqual(intent.entities, [])

    def test_low_confidence_llm_write_is_demoted(self) -> None:
        service = RuntimeActionIntentService(lambda _text: {
            "route": "runtime_write",
            "operation": "add",
            "modality": "request_command",
            "confidence": 0.4,
            "entities": [{"canonical_name": "小狗"}],
        })

        intent = service.classify("能不能考虑一只小狗", allow_llm=True)

        self.assertEqual(intent.route, "runtime_read")
        self.assertTrue(intent.clarification)

    def test_high_risk_llm_write_requires_confirmation(self) -> None:
        service = RuntimeActionIntentService(lambda _text: {
            "route": "runtime_write",
            "operation": "modify",
            "modality": "command",
            "confidence": 0.95,
            "entities": [{"canonical_name": "入口"}],
            "risk_level": "high",
        })

        intent = service.classify("覆盖入口布局", allow_llm=True)

        self.assertEqual(intent.route, "runtime_write")
        self.assertTrue(intent.requires_confirmation)

    def test_entity_name_validator_rejects_generic_user_instructions(self) -> None:
        canonical, reason = EntityNameValidator.validate("请你给出一个方案")

        self.assertEqual(canonical, "")
        self.assertIn("用户指令", reason)


class MessageDispatchLedgerTests(unittest.TestCase):
    def test_message_can_only_have_one_authoritative_owner(self) -> None:
        ledger = MessageDispatchLedger()

        self.assertTrue(ledger.claim("room-1", "msg-1", owner="native", route="runtime_read"))
        self.assertFalse(ledger.claim("room-1", "msg-1", owner="agent", route="runtime_read"))
        ledger.transition("room-1", "msg-1", "replied", reply="ready")
        self.assertEqual(ledger.entry("room-1", "msg-1")["reply"], "ready")

    def test_execution_claim_is_strict_even_for_the_same_owner(self) -> None:
        ledger = MessageDispatchLedger()

        self.assertTrue(ledger.claim_execution("room-1", "msg-1", owner="agent", route="agent_chat"))
        self.assertFalse(ledger.claim_execution("room-1", "msg-1", owner="agent", route="agent_chat"))
        self.assertFalse(ledger.claim_execution("room-1", "msg-1", owner="native", route="planning"))

    def test_reply_claim_freezes_explicit_target_and_allows_failed_send_retry(self) -> None:
        ledger = MessageDispatchLedger()
        self.assertTrue(ledger.claim_execution(
            "room-1",
            "msg-1",
            owner="agent_trigger",
            route="agent_chat",
            target_agent_id="elder-id",
            target_agent_name="长者",
        ))

        self.assertFalse(ledger.claim_reply(
            "room-1",
            "msg-1",
            owner="agent_trigger:girl-id",
            agent_id="girl-id",
            agent_name="小女孩",
        ))
        self.assertTrue(ledger.claim_reply(
            "room-1",
            "msg-1",
            owner="agent_trigger:elder-id",
            agent_id="elder-id",
            agent_name="长者",
        ))
        ledger.complete_reply(
            "room-1",
            "msg-1",
            owner="agent_trigger:elder-id",
            sent=False,
        )
        self.assertTrue(ledger.claim_reply(
            "room-1",
            "msg-1",
            owner="agent_trigger:elder-id",
            agent_id="elder-id",
            agent_name="长者",
        ))
        ledger.complete_reply(
            "room-1",
            "msg-1",
            owner="agent_trigger:elder-id",
            sent=True,
            reply="ready",
        )
        self.assertFalse(ledger.claim_reply(
            "room-1",
            "msg-1",
            owner="agent_trigger:elder-id",
            agent_id="elder-id",
            agent_name="长者",
        ))
        entry = ledger.entry("room-1", "msg-1")
        self.assertTrue(entry["final_reply_sent"])
        self.assertEqual(entry["reply"], "ready")


if __name__ == "__main__":
    unittest.main()
