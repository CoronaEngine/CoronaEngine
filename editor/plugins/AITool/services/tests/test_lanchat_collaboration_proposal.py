from __future__ import annotations

import json
import unittest
from copy import deepcopy
from unittest.mock import patch

from editor.plugins.AITool.services.agent_runtime import AgentRuntime, AgentRuntimeFlags
from editor.plugins.AITool.services.agent_collaboration.production_reasoners import (
    CollaborationReasoningError,
)
from editor.plugins.AITool.services.lanchat_agent_worker import LANChatAgentWorker
from editor.plugins.AITool.services.lanchat_scene_runtime import get_lanchat_scene_runtime


def _collaboration_completion(_trigger, *, purpose, **_kwargs):
    if purpose == "planning_artifact_reasoning":
        return json.dumps({
            "game_design_brief": {
                "project_goal": "迪士尼风格卧室",
                "player_experience": ["探索温暖卧室", "完成钥匙开门目标"],
                "core_rules": ["先取得钥匙再开门"],
                "acceptance_criteria": ["可完成单人闭环"],
            },
            "level_plan": {
                "level_goal": "找到钥匙并抵达终点",
                "zones": ["出生区", "钥匙区", "门区", "终点区"],
                "progression": ["出生", "拾取钥匙", "开门", "到达终点"],
                "acceptance_criteria": ["路径清晰"],
            },
        }, ensure_ascii=False)
    if purpose == "program_artifact_reasoning":
        return json.dumps({
            "gameplay_logic_plan": {
                "states": ["key_available", "key_collected", "door_unlocked", "complete"],
                "entity_roles": {
                    "player_spawn": {"slot_id": "player", "required_capabilities": ["player"]},
                    "collectible_key": {"slot_id": "key", "required_capabilities": ["collectible"]},
                    "locked_door": {"slot_id": "door", "required_capabilities": ["lockable"]},
                    "goal_zone": {"slot_id": "goal", "required_capabilities": ["trigger_zone"]},
                },
                "primitives": [
                    {"primitive_id": "collect", "kind": "on_collect", "subject_slot": "key", "target_slot": "player", "parameters": {}},
                    {"primitive_id": "unlock", "kind": "unlock", "subject_slot": "key", "target_slot": "door", "parameters": {"required_state": "key_collected"}},
                    {"primitive_id": "enter", "kind": "on_enter", "subject_slot": "goal", "target_slot": "player", "parameters": {}},
                    {"primitive_id": "complete", "kind": "complete_objective", "subject_slot": "goal", "target_slot": "player", "parameters": {"objective_id": "reach_goal"}},
                ],
                "win_conditions": ["complete"],
                "lose_conditions": ["无失败条件"],
                "triggers": ["collect", "enter"],
                "rules": ["door requires key"],
            },
        }, ensure_ascii=False)
    if purpose == "art_artifact_reasoning":
        return json.dumps({
            "art_direction": {
                "style_keywords": ["迪士尼动画卧室", "低多边形"],
                "palette": ["暖粉", "木色", "天蓝"],
                "lighting": ["柔和晨光"],
                "avoid_keywords": ["恐怖", "杂乱"],
            },
            "scene_composition_plan": {
                "scene_type": "bedroom",
                "environment_requirements": ["room_box", "room_floor"],
                "layout_rules": ["保持出生点到终点路径清晰"],
                "global_visual_prompt": "迪士尼风格卧室道具，低多边形，白底单体",
                "role_visual_overrides": {
                    "collectible_key": "迪士尼风格魔法钥匙，白底单体",
                    "locked_door": "迪士尼风格卧室门，白底单体",
                },
            },
        }, ensure_ascii=False)
    if purpose == "collaboration_proposal_narration":
        return "方案将卧室设计为温暖的迪士尼动画空间，玩家拾取魔法钥匙后开启房门并抵达终点。当前仅为待确认方案，尚未生成图片、模型或写入场景。"
    raise AssertionError(purpose)


class _Engine:
    def __init__(self) -> None:
        self.messages: list[dict] = []

    def network_send_system_message_ex(
        self,
        sender_id,
        sender_name,
        text,
        message_kind,
        event_id,
        metadata_json,
    ):
        self.messages.append({
            "sender_id": sender_id,
            "sender_name": sender_name,
            "text": text,
            "message_kind": message_kind,
            "event_id": event_id,
            "metadata": json.loads(metadata_json),
        })
        return True

    def network_send_agent_reply_ex(
        self,
        sender_id,
        sender_name,
        text,
        message_kind,
        target_agent_id,
        correlation_id,
        metadata_json,
    ):
        self.messages.append({
            "sender_id": sender_id,
            "sender_name": sender_name,
            "text": text,
            "message_kind": message_kind,
            "correlation_id": correlation_id,
            "metadata": json.loads(metadata_json),
        })
        return True


class LANChatCollaborationProposalTests(unittest.TestCase):
    def setUp(self) -> None:
        get_lanchat_scene_runtime().clear_pending_planning()

    def tearDown(self) -> None:
        get_lanchat_scene_runtime().clear_pending_planning()

    def _worker(self):
        engine = _Engine()
        worker = LANChatAgentWorker(
            corona_engine=engine,
            agent_runtime=AgentRuntime(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        return worker, engine

    def test_discussion_stays_on_real_chat_path_without_proposal(self) -> None:
        worker, engine = self._worker()
        trigger = {
            "room_id": "room-discussion-only",
            "message_id": "message-discussion-only",
            "text": "@小女孩 围绕迪士尼乐园主题讨论一下",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        }

        handled = worker._handle_collaboration_proposal(trigger)

        self.assertFalse(handled)
        self.assertEqual(engine.messages, [])

    def test_tool_free_greeting_does_not_disclose_stale_runtime_identity(self) -> None:
        worker, engine = self._worker()
        runtime_before = deepcopy(worker._agent_runtime.state.rooms)
        operation_count_before = len(worker._agent_runtime.operation_log.entries())
        trigger = {
            "room_id": "room-greeting",
            "message_id": "message-greeting",
            "text": "@小女孩 你好",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
            "proposal_id": "proposal-stale",
            "proposal_version": 3,
            "proposal_hash": "sha256:" + "a" * 64,
            "_dispatch_owner": "agent_trigger",
        }

        with patch.object(worker, "_complete_tool_free_chat", return_value="你好，我在。想继续聊卧室主题还是先换个话题？"):
            handled = worker._handle_tool_free_discussion(trigger)

        self.assertTrue(handled)
        metadata = engine.messages[-1]["metadata"]
        self.assertEqual(metadata["reply_contract"], "discussion_reply")
        self.assertNotIn("proposal_id", metadata)
        self.assertNotIn("runtime_plan_id", metadata)
        self.assertEqual(worker._agent_runtime.state.rooms, runtime_before)
        self.assertEqual(len(worker._agent_runtime.operation_log.entries()), operation_count_before)

    def test_design_request_creates_versioned_three_role_proposal(self) -> None:
        worker, engine = self._worker()
        trigger = {
            "room_id": "room-proposal",
            "message_id": "message-proposal-1",
            "text": "@小女孩 按照迪士尼风格来设计一个卧室",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
            "_dispatch_owner": "agent_trigger",
        }

        with patch.object(worker, "_complete_tool_free_chat", side_effect=_collaboration_completion):
            handled = worker._handle_collaboration_proposal(trigger)

        self.assertTrue(handled)
        proposal_messages = [row for row in engine.messages if row["message_kind"] == "gm_proposal"]
        self.assertEqual(len(proposal_messages), 1)
        metadata = proposal_messages[0]["metadata"]
        self.assertEqual(metadata["proposal_version"], 1)
        self.assertTrue(metadata["proposal_hash"].startswith("sha256:"))
        self.assertEqual(len(metadata["artifact_refs"]), 5)
        self.assertEqual(metadata["reply_to"], "message-proposal-1")
        self.assertEqual(metadata["origin_message_id"], "message-proposal-1")
        self.assertEqual(metadata["reply_contract"], "planning_proposal")
        self.assertEqual(metadata["resolved_intent"], "plan_drafting")
        role_events = [
            (
                row["metadata"]["progress_event"]["detail"].get("owner_role"),
                row["metadata"]["progress_event"].get("status"),
            )
            for row in engine.messages
            if row["message_kind"] == "action_status"
            and row["metadata"]["progress_event"]["detail"].get("owner_role")
        ]
        self.assertEqual(role_events, [
            ("planning", "in_progress"),
            ("planning", "completed"),
            ("program", "in_progress"),
            ("program", "completed"),
            ("art", "in_progress"),
            ("art", "completed"),
            ("gm", "in_progress"),
            ("gm", "completed"),
        ])
        progress_ids = {
            row["event_id"]
            for row in engine.messages
            if row["message_kind"] == "action_status"
        }
        self.assertEqual(len(progress_ids), 1)
        context = worker._conversation_turn_contexts.get("room-proposal")
        self.assertEqual(context.phase, "proposal_ready")
        self.assertEqual(context.proposal_hash, metadata["proposal_hash"])

    def test_program_contract_failure_reports_stage_and_stops_downstream(self) -> None:
        worker, engine = self._worker()
        calls: list[str] = []
        runtime_before = deepcopy(worker._agent_runtime.state.rooms)
        operation_count_before = len(worker._agent_runtime.operation_log.entries())

        def failing_completion(trigger, *, purpose, **kwargs):
            calls.append(purpose)
            if purpose != "program_artifact_reasoning":
                return _collaboration_completion(trigger, purpose=purpose, **kwargs)
            payload = json.loads(_collaboration_completion(trigger, purpose=purpose, **kwargs))
            payload["gameplay_logic_plan"]["primitives"][0]["target_slot"] = "missing-slot"
            return json.dumps(payload, ensure_ascii=False)

        trigger = {
            "room_id": "room-program-blocked",
            "message_id": "message-program-blocked",
            "text": "@小女孩 按照迪士尼风格来设计一个卧室",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        }

        with patch.object(worker, "_complete_tool_free_chat", side_effect=failing_completion):
            handled = worker._handle_collaboration_proposal(trigger)

        self.assertTrue(handled)
        self.assertEqual(calls, ["planning_artifact_reasoning", "program_artifact_reasoning"])
        progress = [
            row["metadata"]["progress_event"]
            for row in engine.messages
            if row["message_kind"] == "action_status"
        ]
        self.assertEqual(
            [(row["detail"]["owner_role"], row["status"]) for row in progress],
            [
                ("planning", "in_progress"),
                ("planning", "completed"),
                ("program", "in_progress"),
                ("program", "blocked"),
            ],
        )
        self.assertIn("unknown_slot", engine.messages[-1]["text"])
        self.assertEqual(engine.messages[-1]["metadata"]["reply_contract"], "collaboration_blocked")
        self.assertEqual(engine.messages[-1]["metadata"]["resolved_intent"], "plan_drafting")
        self.assertEqual(
            worker._agent_runtime.state.rooms,
            runtime_before,
        )
        self.assertEqual(len(worker._agent_runtime.operation_log.entries()), operation_count_before)
        project_id = worker._stable_collaboration_id("project", "", seed="room-program-blocked")
        report = worker._get_collaboration_coordinator().last_attempt(project_id)
        self.assertEqual(report.overall_status, "blocked")
        self.assertIsNone(worker._get_collaboration_coordinator().current(project_id))

        status_trigger = {
            "room_id": "room-program-blocked",
            "message_kind": "chat",
            "text": "@GM \u73b0\u5728\u662f\u4ec0\u4e48\u60c5\u51b5",
        }
        # Keep this source fixture ASCII-safe on Windows while exercising the
        # same explicit status-query route.
        status_trigger["text"] = "@GM status"
        status_reply = worker._handle_coordinator_status_query(status_trigger)
        self.assertIn("程序：阻断（unknown_slot）", status_reply)
        self.assertNotIn("Runtime 状态", status_reply)
        self.assertLessEqual(len(status_reply.splitlines()), 6)
        self.assertTrue(status_trigger["_control_plane_only"])
        self.assertEqual(status_trigger["resolved_intent"], "status_query")

    def test_red_gate_confirmation_preserves_pending_proposal(self) -> None:
        worker, engine = self._worker()
        proposal_trigger = {
            "room_id": "room-red-confirmation",
            "message_id": "message-red-proposal",
            "text": "@小女孩 按照迪士尼风格来设计一个卧室",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
            "_dispatch_owner": "agent_trigger",
        }
        with patch.object(worker, "_complete_tool_free_chat", side_effect=_collaboration_completion):
            self.assertTrue(worker._handle_collaboration_proposal(proposal_trigger))

        confirmation = {
            "room_id": "room-red-confirmation",
            "message_id": "message-red-confirmation",
            "text": "@GM 确认生成",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "_dispatch_owner": "native_queue",
        }

        self.assertTrue(worker._handle_gm_pending_planning_confirmation(confirmation))
        reply = engine.messages[-1]
        self.assertEqual(reply["sender_name"], "GM")
        self.assertEqual(reply["metadata"]["reply_contract"], "runtime_write_blocked")
        self.assertIn("Full R3 Gate", reply["text"])
        project_id = worker._stable_collaboration_id("project", "", seed="room-red-confirmation")
        proposal = worker._get_collaboration_coordinator().current(project_id)
        self.assertIsNotNone(proposal)
        self.assertEqual(proposal.status, "proposal_ready")
        self.assertEqual(engine.messages[-1]["metadata"]["origin_message_id"], "message-red-confirmation")

    def test_gm_confirmation_without_proposal_is_final_control_plane_reply(self) -> None:
        worker, engine = self._worker()
        runtime_before = deepcopy(worker._agent_runtime.state.rooms)
        operation_count_before = len(worker._agent_runtime.operation_log.entries())
        trigger = {
            "room_id": "room-no-proposal",
            "message_id": "message-no-proposal-confirmation",
            "text": "@GM 确认生成",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "_dispatch_owner": "agent_trigger",
        }

        self.assertTrue(worker._handle_gm_pending_planning_confirmation(trigger))

        reply = engine.messages[-1]
        self.assertEqual(reply["sender_name"], "GM")
        self.assertEqual(reply["metadata"]["reply_contract"], "collaboration_blocked")
        self.assertEqual(reply["metadata"]["resolved_intent"], "generation_start")
        self.assertEqual(reply["metadata"]["reply_to"], "message-no-proposal-confirmation")
        self.assertEqual(worker._agent_runtime.state.rooms, runtime_before)
        self.assertEqual(len(worker._agent_runtime.operation_log.entries()), operation_count_before)

    def test_gm_delegation_enters_collaboration_proposal(self) -> None:
        worker, engine = self._worker()
        trigger = {
            "room_id": "room-gm-delegation",
            "message_id": "message-gm-delegation",
            "text": "@GM 让@小女孩 给我一个方案",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
        }

        with patch.object(worker, "_complete_tool_free_chat", side_effect=_collaboration_completion):
            handled = worker._handle_collaboration_proposal(trigger)

        self.assertTrue(handled)
        self.assertEqual(engine.messages[-1]["message_kind"], "gm_proposal")
        self.assertEqual(engine.messages[-1]["metadata"]["reply_contract"], "planning_proposal")

    def test_identical_successful_request_reuses_proposal_without_model_calls(self) -> None:
        worker, engine = self._worker()
        calls: list[str] = []

        def completion(trigger, *, purpose, **kwargs):
            calls.append(purpose)
            return _collaboration_completion(trigger, purpose=purpose, **kwargs)

        base = {
            "room_id": "room-proposal-reuse",
            "text": "@小女孩 按照迪士尼风格来设计一个卧室",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        }
        with patch.object(worker, "_complete_tool_free_chat", side_effect=completion):
            self.assertTrue(worker._handle_collaboration_proposal({**base, "message_id": "message-reuse-1"}))
            first_call_count = len(calls)
            self.assertTrue(worker._handle_collaboration_proposal({**base, "message_id": "message-reuse-2"}))

        self.assertEqual(first_call_count, 4)
        self.assertEqual(len(calls), 4)
        proposals = [row for row in engine.messages if row["message_kind"] == "gm_proposal"]
        self.assertEqual(proposals[-1]["metadata"]["revision_status"], "unchanged")
        self.assertIn("不重复调用三职能模型", proposals[-1]["text"])

    def test_alternative_plan_request_becomes_a_versioned_revision(self) -> None:
        worker, engine = self._worker()
        base = {
            "room_id": "room-proposal-revision",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        }
        def revision_completion(trigger, *, purpose, **kwargs):
            payload = _collaboration_completion(trigger, purpose=purpose, **kwargs)
            if purpose != "art_artifact_reasoning" or trigger.get("message_id") != "message-revision-2":
                return payload
            revised = json.loads(payload)
            revised["art_direction"]["palette"].append("midnight blue")
            return json.dumps(revised, ensure_ascii=False)

        with patch.object(worker, "_complete_tool_free_chat", side_effect=revision_completion):
            self.assertTrue(worker._handle_collaboration_proposal({
                **base,
                "message_id": "message-revision-1",
                "text": "@小女孩 设计一个迪士尼风格卧室方案",
            }))
            self.assertTrue(worker._handle_collaboration_proposal({
                **base,
                "message_id": "message-revision-2",
                "text": "@小女孩 再给一个方案，改成夜晚氛围",
            }))

        proposals = [row for row in engine.messages if row["message_kind"] == "gm_proposal"]
        self.assertEqual(proposals[-1]["metadata"]["proposal_version"], 2)
        self.assertEqual(proposals[-1]["metadata"]["resolved_intent"], "plan_revision")

    def test_narration_failure_discards_candidate_proposal(self) -> None:
        worker, engine = self._worker()

        def completion(trigger, *, purpose, **kwargs):
            if purpose == "collaboration_proposal_narration":
                raise CollaborationReasoningError(
                    "narration blocked",
                    stage="narration",
                    error_code="narration_not_user_facing",
                )
            return _collaboration_completion(trigger, purpose=purpose, **kwargs)

        trigger = {
            "room_id": "room-narration-blocked",
            "message_id": "message-narration-blocked",
            "text": "@小女孩 设计一个迪士尼风格卧室方案",
            "sender_id": "host",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        }
        with patch.object(worker, "_complete_tool_free_chat", side_effect=completion):
            handled = worker._handle_collaboration_proposal(trigger)

        self.assertTrue(handled)
        project_id = worker._stable_collaboration_id("project", "", seed="room-narration-blocked")
        coordinator = worker._get_collaboration_coordinator()
        self.assertIsNone(coordinator.current(project_id))
        report = coordinator.last_attempt(project_id)
        self.assertEqual(report.overall_status, "blocked")
        self.assertEqual(report.stage("narration").error_code, "narration_not_user_facing")
        self.assertEqual(engine.messages[-1]["metadata"]["reply_contract"], "collaboration_blocked")

    def test_agent_reply_preserves_versioned_proposal_identity(self) -> None:
        worker, engine = self._worker()
        trigger = {
            "room_id": "room-versioned-reply",
            "message_id": "message-versioned-reply",
            "agent_id": "elder",
            "agent_name": "长者",
            "_dispatch_owner": "agent_trigger",
            "proposal_id": "proposal-1",
            "agent_plan_id": "proposal-1",
            "artifact_ref": "legacy-plan:proposal-1",
            "proposal_version": 2,
            "proposal_hash": "sha256:proposal-2",
            "artifact_refs": ["artifact:brief", "artifact:level"],
            "reply_contract": "generation_confirmation",
            "resolved_intent": "generation_start",
        }

        sent = worker._send_final_reply("elder", "长者", "已确认。", trigger)

        self.assertTrue(sent)
        metadata = engine.messages[-1]["metadata"]
        self.assertEqual(metadata["proposal_version"], 2)
        self.assertEqual(metadata["proposal_hash"], "sha256:proposal-2")
        self.assertEqual(metadata["artifact_refs"], ["artifact:brief", "artifact:level"])


if __name__ == "__main__":
    unittest.main()
