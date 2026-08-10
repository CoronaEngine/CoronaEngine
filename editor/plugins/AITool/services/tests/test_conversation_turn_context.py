from __future__ import annotations

import unittest

from editor.plugins.AITool.services.conversation_turn_context import ConversationTurnContextStore


class ConversationTurnContextStoreTests(unittest.TestCase):
    def test_short_plan_instruction_preserves_accumulated_goal(self) -> None:
        store = ConversationTurnContextStore(clock=lambda: 1.0)
        store.record_turn(
            room_id="room-1",
            message_id="msg-1",
            text="@小女孩 围绕迪士尼乐园主题讨论一下",
            target_agent_name="小女孩",
            intent="discussion",
        )
        store.record_turn(
            room_id="room-1",
            message_id="msg-2",
            text="@小女孩 按照迪士尼风格的卧室来设计呢",
            target_agent_name="小女孩",
            intent="plan_drafting",
        )
        context = store.record_turn(
            room_id="room-1",
            message_id="msg-3",
            text="@长者 请你给出一个方案",
            target_agent_name="长者",
            intent="plan_drafting",
        )

        self.assertIn("迪士尼风格的卧室", context.accumulated_goal)
        self.assertNotIn("迪士尼乐园主题讨论", context.accumulated_goal)
        self.assertEqual(
            store.effective_planning_text("room-1", "@长者 请你给出一个方案"),
            context.accumulated_goal,
        )

    def test_new_goal_replaces_old_goal_and_duplicate_message_is_idempotent(self) -> None:
        store = ConversationTurnContextStore(clock=lambda: 2.0)
        store.record_turn(room_id="room-1", message_id="msg-1", text="设计一个卧室")
        replaced = store.record_turn(room_id="room-1", message_id="msg-2", text="改成森林营地")
        replay = store.record_turn(room_id="room-1", message_id="msg-2", text="不应写入")

        self.assertEqual(replaced.accumulated_goal, "改成森林营地")
        self.assertEqual(replay, replaced)

    def test_plan_binding_requires_stable_references(self) -> None:
        store = ConversationTurnContextStore()

        with self.assertRaises(ValueError):
            store.bind_plan(
                room_id="room-1",
                target_agent_id="elder-id",
                target_agent_name="长者",
                agent_plan_id="",
                artifact_ref="",
            )

        bound = store.bind_plan(
            room_id="room-1",
            target_agent_id="elder-id",
            target_agent_name="长者",
            agent_plan_id="plan-elder",
            artifact_ref="legacy-plan:plan-elder",
        )
        self.assertEqual(bound.active_agent_plan_id, "plan-elder")
        self.assertEqual(bound.artifact_ref, "legacy-plan:plan-elder")
        self.assertEqual(bound.phase, "proposal_ready")
        self.assertEqual(bound.proposal_version, 1)

    def test_discussion_and_proposal_phases_are_explicit(self) -> None:
        store = ConversationTurnContextStore(clock=lambda: 3.0)

        discussed = store.record_turn(
            room_id="room-phase",
            message_id="msg-discuss",
            text="围绕主题聊聊",
            intent="discussion",
        )
        drafting = store.record_turn(
            room_id="room-phase",
            message_id="msg-plan",
            text="设计一个卧室",
            intent="plan_drafting",
        )
        ready = store.bind_plan(
            room_id="room-phase",
            target_agent_id="planning",
            target_agent_name="策划",
            agent_plan_id="plan-phase",
            artifact_ref="legacy-plan:plan-phase",
            proposal_version=2,
            proposal_hash="sha256:proposal",
            artifact_refs=("planning.game-design-brief@1",),
        )
        greeting = store.record_turn(
            room_id="room-phase",
            message_id="msg-greeting",
            text="你好",
            intent="discussion",
        )

        self.assertEqual(discussed.phase, "discussion")
        self.assertEqual(drafting.phase, "drafting")
        self.assertEqual(ready.phase, "proposal_ready")
        self.assertEqual(greeting.phase, "proposal_ready")
        self.assertEqual(greeting.proposal_hash, "sha256:proposal")


if __name__ == "__main__":
    unittest.main()
