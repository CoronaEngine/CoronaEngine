from __future__ import annotations

import os
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
EDITOR_ROOT = os.path.join(REPO_ROOT, "editor")
AI_TOOL_ROOT = os.path.join(EDITOR_ROOT, "plugins", "AITool")
for path in (EDITOR_ROOT, AI_TOOL_ROOT):
    if path not in sys.path:
        sys.path.insert(0, path)

from plugins.AITool.services.intent_understanding import IntentUnderstandingService  # noqa: E402
from plugins.AITool.services.lanchat_scene_runtime import LanChatSceneRuntime  # noqa: E402


def test_status_query_overrides_bad_llm_generation_start() -> None:
    service = IntentUnderstandingService(lambda _text: {
        "intent": "generation_start",
        "confidence": 0.99,
        "reason": "bad llm",
    })
    decision = service.classify("@GM 现在生成到哪里了")
    assert decision.intent == "status_query"


def test_generation_start_requires_explicit_confirmation_language() -> None:
    service = IntentUnderstandingService(lambda _text: {
        "intent": "generation_start",
        "confidence": 0.96,
        "reason": "over eager llm",
    })
    decision = service.classify("@商人 帮我写一个可爱卧室的生成方案")
    assert decision.intent == "plan_drafting"


def test_active_generation_add_is_pending_intervention() -> None:
    service = IntentUnderstandingService()
    decision = service.classify("后面再加入一个天使雕像", allow_llm=False, generation_active=True)
    assert decision.intent == "intervention_add"
    assert service.scene_note_kind("后面再加入一个天使雕像") == "generation_delta"


def test_floating_layout_request_routes_to_final_adjustment() -> None:
    service = IntentUnderstandingService()
    decision = service.classify("这个宝箱浮空了，没贴地，调整一下布局", allow_llm=False)
    assert decision.intent == "final_adjustment_request"
    assert decision.as_dict()["route"] == "edit_existing"
    assert decision.as_dict()["state_hint"] == "completed_scene"


def test_structured_route_exposes_agent_self_and_scene_actions() -> None:
    service = IntentUnderstandingService()
    identity = service.classify("@小女孩 你是谁", allow_llm=False)
    performance = service.classify("@小女孩 我想看你在大草原上跳舞", allow_llm=False)
    plan = service.classify("@小女孩 帮我设计一个可爱的卧室", allow_llm=False)
    supplement = service.classify("补充要求，减少方案中的细碎物体", allow_llm=False)
    start = service.classify("确认开始", allow_llm=False)
    edit = service.classify("把床放大一点", allow_llm=False)
    gm = service.classify("@GM 整理一下大家的想法", allow_llm=False)

    assert identity.as_dict()["route"] == "agent_self"
    assert performance.as_dict()["route"] == "agent_self"
    assert plan.as_dict()["route"] == "plan_drafting"
    assert supplement.as_dict()["route"] == "plan_revision"
    assert start.as_dict()["route"] == "generation_start"
    assert edit.as_dict()["route"] == "edit_existing"
    assert edit.as_dict()["state_hint"] == "completed_scene"
    assert gm.as_dict()["route"] == "gm_control"


def test_design_opinion_question_stays_discussion_even_with_plan_words() -> None:
    service = IntentUnderstandingService()
    decision = service.classify("你对于长者的设计方案怎么看", allow_llm=False)
    assert decision.intent == "discussion"


def test_planning_disclosure_splits_models_from_scene_substrate() -> None:
    runtime = LanChatSceneRuntime()
    action, reply = runtime.handle_planning_gate(
        "小女孩",
        "我想做一个草原天空森林里的可爱卧室，有床和台灯",
    )
    assert action == "reply"
    assert reply is not None
    assert "提炼结果" in reply
    assert "准备生成模型" in reply
    assert "床" in reply
    assert "台灯" in reply
    assert "环境/地形" in reply
    assert "草原" in reply
    assert "天空" in reply
    assert "森林" in reply


def test_pending_plan_ignores_opinion_question_and_ui_placeholder() -> None:
    runtime = LanChatSceneRuntime()
    action, reply = runtime.handle_planning_gate("长者", "帮我设计一个现代风客厅")
    assert action == "reply"
    assert reply is not None

    action, payload, agent_name = runtime.handle_pending_planning_message("你对于长者的设计方案怎么看")
    assert (action, payload, agent_name) == ("pass", None, None)

    action, payload, agent_name = runtime.handle_pending_planning_message("补充要求：写明要改的风格、物件、布局或限制")
    assert (action, payload, agent_name) == ("pass", None, None)

    snapshot = runtime.pending_planning_snapshot()
    assert len(snapshot) == 1
    assert snapshot[0]["scene_goal"] == "现代风客厅"
    assert snapshot[0]["constraints"] == []


def test_plan_supplement_is_explicit_and_does_not_swallow_edits() -> None:
    service = IntentUnderstandingService()
    assert service.is_plan_supplement("补充要求：新增一个柜式空调") is True
    assert service.is_plan_supplement("我希望整体更明亮") is True
    assert service.is_plan_supplement("新增一个柜式空调") is False
    assert service.is_plan_supplement("调整沙发位置") is False
    assert service.is_plan_supplement("删除电视墙") is False

    runtime = LanChatSceneRuntime()
    action, reply = runtime.handle_planning_gate("长者", "帮我设计一个现代风客厅")
    assert action == "reply"
    assert reply is not None

    action, payload, agent_name = runtime.handle_pending_planning_message("调整沙发位置")
    assert (action, payload, agent_name) == ("pass", None, None)
    snapshot = runtime.pending_planning_snapshot()
    assert snapshot and snapshot[0]["constraints"] == []

    action, payload, agent_name = runtime.handle_pending_planning_message("补充要求：新增一个柜式空调")
    assert action == "reply"
    assert agent_name == "长者"
    assert payload is not None
    assert "柜式空调" in payload


def test_pending_plan_elaboration_expands_without_generation_or_supplement() -> None:
    service = IntentUnderstandingService()
    assert service.is_plan_elaboration_request("继续输出风格方案、布局、物品清单") is True
    assert service.is_plan_supplement("继续输出风格方案、布局、物品清单") is False
    assert service.is_plan_elaboration_request("新增一个柜式空调") is False
    assert service.is_plan_elaboration_request("确认开始") is False

    runtime = LanChatSceneRuntime()
    action, reply = runtime.handle_planning_gate("小女孩", "帮我设计一个现代风客厅")
    assert action == "reply"
    assert reply is not None

    action, payload, agent_name = runtime.handle_pending_planning_message("继续输出风格方案、布局、物品清单")
    assert action == "reply"
    assert agent_name == "小女孩"
    assert payload is not None
    assert "方案展开" in payload
    assert "风格方案" in payload
    assert "布局" in payload
    assert "物品清单" in payload
    assert "现代风客厅" in payload
    assert "生成类请求" not in payload
    assert "请先由房主确认" not in payload
    snapshot = runtime.pending_planning_snapshot()
    assert len(snapshot) == 1
    assert snapshot[0]["scene_goal"] == "现代风客厅"


def test_busy_generation_does_not_capture_plain_chat() -> None:
    runtime = LanChatSceneRuntime()
    runtime.start_compose("长者", "现代风客厅")
    try:
        assert runtime.record_busy_message(
            agent_name="小女孩",
            text="你是谁",
            source_user_id="host-a",
        ) is None
        add_reply = runtime.record_busy_message(
            agent_name="小女孩",
            text="新增一个柜式空调",
            source_user_id="host-a",
        )
        assert add_reply is not None
        assert "后续补充" in add_reply
    finally:
        runtime.end_compose()


def test_scene_plans_use_general_profile_instead_of_topic_specific_fallbacks() -> None:
    runtime = LanChatSceneRuntime()
    cases = [
        ("长者", "帮我设计一个科技展厅", ("展台", "展墙", "导视")),
        ("小女孩", "帮我规划一个温暖的商业咖啡店", ("吧台", "座位", "收银")),
        ("商人", "设计一个户外亲子活动区", ("入口", "活动", "休息")),
    ]
    banned = ("主体空间", "主要功能物件", "支撑物件", "主题装饰", "小型道具")
    for agent_name, prompt, expected_terms in cases:
        action, reply = runtime.handle_planning_gate(agent_name, prompt)
        assert action == "reply"
        assert reply is not None
        assert not any(term in reply for term in banned), reply
        assert sum(1 for term in expected_terms if term in reply) >= 2, reply
        assert "风格定位" in reply
        assert "空间布局" in reply


def test_planning_reply_keeps_light_role_voice() -> None:
    runtime = LanChatSceneRuntime()
    action, elder_reply = runtime.handle_planning_gate("长者", "帮我设计一个现代简约客厅")
    assert action == "reply"
    assert elder_reply is not None
    assert "长者" in elder_reply
    assert "稳妥" in elder_reply

    action, girl_reply = runtime.handle_planning_gate("小女孩", "帮我设计一个温暖的商业咖啡店")
    assert action == "reply"
    assert girl_reply is not None
    assert "小女孩" in girl_reply
    assert "温柔" in girl_reply or "可爱" in girl_reply


def test_treasure_room_plan_uses_concrete_profile_and_no_abstract_models() -> None:
    runtime = LanChatSceneRuntime()
    action, reply = runtime.handle_planning_gate("山贼", "帮我设计一个强盗藏宝室")
    assert action == "reply"
    assert reply is not None
    assert "藏宝箱" in reply
    assert "金币堆" in reply
    assert "木桌" in reply
    snapshot = runtime.pending_planning_snapshot()
    assert snapshot
    assert "藏宝箱" in snapshot[0]["proposed_items"]
    assert "主活动区" not in snapshot[0]["proposed_items"]
    assert "视觉焦点" not in snapshot[0]["proposed_items"]


def test_pending_plan_elaboration_reuses_existing_treasure_room_goal() -> None:
    runtime = LanChatSceneRuntime()
    action, reply = runtime.handle_planning_gate("山贼", "帮我设计一个强盗藏宝室")
    assert action == "reply"
    assert reply is not None

    action, payload = runtime.handle_planning_gate("山贼", "整理一下方案")
    assert action == "reply"
    assert payload is not None
    assert "方案展开：强盗藏宝室" in payload
    assert "藏宝箱" in payload
    assert "整理一下方案" not in payload

    action, compose_text = runtime.handle_planning_gate("山贼", "确认生成")
    assert action == "compose"
    assert compose_text is not None
    assert "用户确认开始生成：强盗藏宝室" in compose_text
    assert "整理一下方案" not in compose_text


def test_targeted_plan_can_clone_source_agent_context() -> None:
    runtime = LanChatSceneRuntime()
    action, reply = runtime.handle_planning_gate("山贼", "帮我设计一个强盗藏宝室")
    assert action == "reply"
    assert reply is not None

    action, payload, agent_name = runtime.handle_targeted_planning_message(
        "商人",
        "在山贼方案基础上进行调整",
        draft_action="plan",
        source_context_agent="山贼",
    )
    assert action == "reply"
    assert agent_name == "商人"
    assert payload is not None
    assert "我已更新方案" in payload
    assert "强盗藏宝室" in payload
    assert "参考来源：山贼" in payload
    snapshot = runtime.pending_planning_snapshot()
    merchant = [item for item in snapshot if item["target_agent"] == "商人"]
    assert merchant
    assert merchant[0]["scene_goal"] == "强盗藏宝室"
    assert "藏宝箱" in merchant[0]["proposed_items"]
    assert "山贼" in merchant[0]["source_context_agents"]


if __name__ == "__main__":
    test_status_query_overrides_bad_llm_generation_start()
    test_generation_start_requires_explicit_confirmation_language()
    test_active_generation_add_is_pending_intervention()
    test_floating_layout_request_routes_to_final_adjustment()
    test_structured_route_exposes_agent_self_and_scene_actions()
    test_design_opinion_question_stays_discussion_even_with_plan_words()
    test_planning_disclosure_splits_models_from_scene_substrate()
    test_pending_plan_ignores_opinion_question_and_ui_placeholder()
    test_plan_supplement_is_explicit_and_does_not_swallow_edits()
    test_pending_plan_elaboration_expands_without_generation_or_supplement()
    test_busy_generation_does_not_capture_plain_chat()
    test_scene_plans_use_general_profile_instead_of_topic_specific_fallbacks()
    test_planning_reply_keeps_light_role_voice()
    test_treasure_room_plan_uses_concrete_profile_and_no_abstract_models()
    test_pending_plan_elaboration_reuses_existing_treasure_room_goal()
    test_targeted_plan_can_clone_source_agent_context()
    print("[OK] Intent understanding service tests passed")
