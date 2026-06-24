from __future__ import annotations

import os
import sys
import json
import threading
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from plugins.AITool.services.lanchat_agent_orchestrator import LanChatAgentOrchestrator  # noqa: E402
from plugins.AITool.services.lanchat_summary_service import LANChatSummaryService  # noqa: E402
from plugins.AITool.services.lanchat_host_action_executor import LanChatHostActionExecutor  # noqa: E402
from plugins.AITool.services.lanchat_agent_worker import (  # noqa: E402
    LANChatAgentWorker,
    MAX_ACTIVE_ROOM_IDS,
    MAX_COORDINATOR_SEEN_MESSAGE_IDS,
)
from plugins.AITool.services.lanchat_scene_runtime import get_lanchat_scene_runtime  # noqa: E402
from plugins.AITool.services.generation_scheduler import GenerationScheduler  # noqa: E402
from plugins.AITool.services.interaction_coordinator import (  # noqa: E402
    ChatMessage,
    InteractionCoordinator,
    ReviewResult,
)
from plugins.AITool.services.seed_plan import SeedPlanStatus  # noqa: E402
from plugins.AITool.services.disclosure_policy import DisclosureEvent  # noqa: E402
from plugins.AITool.cai_extensions.agent.scene_composer import (  # noqa: E402
    _has_resolved_plan_context,
    _looks_generic_inventory,
)
from plugins.AITool.Quasar.ai_modules.three_d_generate.tools import model_tools  # noqa: E402
from plugins.AITool.Quasar.ai_media_resource import registry as media_registry  # noqa: E402


def _agent_factory():
    def _agent(persona, messages):
        assert messages
        return f"agent-reply persona={persona or 'none'} messages={len(messages)}"

    return _agent


def _trigger(text="@小B 添加一个篝火", agent_name="小B"):
    return {
        "trigger_id": "m1:a1",
        "message_id": "m1",
        "room_id": "r1",
        "sender_id": "user-a",
        "sender_name": "用户A",
        "agent_id": "agent-b",
        "agent_name": agent_name,
        "persona": "山贼",
        "text": text,
        "history": [
            {"message_id": "m0", "from": "用户A", "text": "我们做一个营地"},
            {"message_id": "m1", "from": "用户A", "text": text},
        ],
    }


class FakeEngine:
    def __init__(self, triggers, coordinator_messages=None, room_events=None, session_role="none"):
        self.triggers = list(triggers)
        self.coordinator_messages = list(coordinator_messages or [])
        self.room_events = list(room_events or [])
        self.session_role = session_role
        self.replies = []
        self.intents = []
        self.system_messages = []

    def network_pop_lanchat_room_event(self):
        return self.room_events.pop(0) if self.room_events else None

    def network_pop_lanchat_coordinator_sync_message(self):
        return self.coordinator_messages.pop(0) if self.coordinator_messages else None

    def network_pop_lanchat_agent_trigger(self):
        return self.triggers.pop(0) if self.triggers else None

    def network_session_role_name(self):
        return self.session_role

    def network_send_agent_reply(self, agent_id, agent_name, text):
        self.replies.append((agent_id, agent_name, text))
        return True

    def network_send_agent_reply_ex(
        self,
        agent_id,
        agent_name,
        text,
        message_kind="agent_reply",
        target_agent_id="",
        correlation_id="",
        metadata_json="",
    ):
        self.replies.append((
            agent_id,
            agent_name,
            text,
            message_kind,
            target_agent_id,
            correlation_id,
            metadata_json,
        ))
        return True

    def network_broadcast_intent(self, user_id, tooltip, preview_position, status):
        self.intents.append((user_id, tooltip, preview_position, status))
        return True

    def network_send_system_message(self, sender_id, sender_name, text):
        self.system_messages.append((sender_id, sender_name, text))
        return True

    def network_send_system_message_ex(
        self,
        sender_id,
        sender_name,
        text,
        message_kind="agent_reply",
        correlation_id="",
        metadata_json="",
    ):
        self.system_messages.append((
            sender_id,
            sender_name,
            text,
            message_kind,
            correlation_id,
            metadata_json,
        ))
        return True


class FakeGate:
    def __init__(self):
        self.calls = 0

    def run(self, fn, *args, **kwargs):
        self.calls += 1
        return fn(*args, **kwargs)


class FakeSceneActor:
    def __init__(self, name, position=None, rotation=None, scale=None, aabb=None):
        self.name = name
        self._position = list(position or [0.0, 0.0, 0.0])
        self._rotation = list(rotation or [0.0, 0.0, 0.0])
        self._scale = list(scale or [1.0, 1.0, 1.0])
        self._aabb = list(aabb) if aabb is not None else None
        self.color = None

    def get_position(self):
        return list(self._position)

    def set_position(self, value):
        self._position = list(value)

    def get_rotation(self):
        return list(self._rotation)

    def set_rotation(self, value):
        self._rotation = list(value)

    def get_scale(self):
        return list(self._scale)

    def set_scale(self, value):
        self._scale = list(value)

    def set_color(self, value):
        self.color = list(value)

    def get_aabb(self):
        if self._aabb is None:
            raise AttributeError("aabb unavailable")
        return list(self._aabb)


class TargetedHostFakeEngine(FakeEngine):
    def __init__(self, triggers):
        super().__init__(triggers)
        self.targeted_host_messages = []

    def network_send_system_message_to_host_ex(
        self,
        sender_id,
        sender_name,
        text,
        message_kind="agent_reply",
        correlation_id="",
        metadata_json="",
    ):
        self.targeted_host_messages.append((
            sender_id,
            sender_name,
            text,
            message_kind,
            correlation_id,
            metadata_json,
        ))
        return True


class FakeHostActionExecutor:
    def __init__(self):
        self.payloads = []

    def enqueue_and_process(self, payload):
        self.payloads.append(dict(payload))
        return None


class FakeScheduler:
    def __init__(self):
        self.submitted = []
        self.statuses = {}
        self.paused_sessions = []
        self.resumed_sessions = []

    def submit(self, payload):
        self.submitted.append(dict(payload))
        job_id = str(payload.get("job_id") or "job-worker-1")
        self.statuses[job_id] = {"job_id": job_id, "status": "queued", "result": {}}
        return {"job_id": job_id, "status": "queued", **payload}

    def status(self, job_id):
        return self.statuses.get(job_id, {"job_id": job_id, "status": "not_found"})

    def wait(self, job_id, timeout=5.0):
        return self.status(job_id)

    def pause_session(self, session_id):
        self.paused_sessions.append(str(session_id))
        return {"session_id": str(session_id), "status": "paused", "success": True}

    def resume_session(self, session_id):
        self.resumed_sessions.append(str(session_id))
        return {"session_id": str(session_id), "status": "running", "success": True}


def test_regular_role_agent_reply():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    result = orch.handle_trigger(_trigger())
    assert result.sender_id == "agent-b"
    assert result.sender_name == "小B"
    assert "agent-reply" in result.text
    assert result.discussion_state.pending_intents
    print("[OK] regular role agent reply goes through orchestrator")


def test_confirm_start_uses_active_coordinator_plan_instead_of_role_agent_gate():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="r1",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="最终简化方案：温暖神秘室外夜集，中央宽通道，两侧摊位，一侧休息区，灯火温暖，不要太恐怖",
    ))
    plan = coordinator.propose_seed_plan("r1")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    engine = FakeEngine([{
        **_trigger("@长者 确认开始", "长者"),
        "room_id": "r1",
        "sender_id": "host-a",
        "sender_name": "房主",
        "is_host": True,
        "sender_type": "host",
    }])

    def failing_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("role agent should not run when confirmed Coordinator plan exists")
        return _agent

    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=failing_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    assert worker.process_once() is True
    assert scheduler.submitted, "confirmed start must enqueue Coordinator SeedPlan"
    assert "最终简化方案" in scheduler.submitted[-1]["prompt"]
    assert engine.replies
    assert "SeedPlan" in engine.replies[-1][2]
    print("[OK] confirm start uses active Coordinator plan instead of RoleAgent planning gate")


def test_current_mentioned_agent_identity_overrides_history_mentions():
    captured = {}

    def agent_factory():
        def _agent(persona, messages):
            captured["messages"] = list(messages)
            return "我就是小D，可以继续处理。"

        return _agent

    trigger = _trigger("@小D 我明明是找你的呀", "小D")
    trigger["agent_id"] = "agent-d"
    trigger["history"] = [
        {"message_id": "m0", "from": "房主", "text": "@学者 你好"},
        {"message_id": "m1", "from": "学者", "text": "若需要我参与，请直接 @学者。"},
        {"message_id": "m2", "from": "房主", "text": "@小D 我明明是找你的呀"},
    ]

    orch = LanChatAgentOrchestrator(agent_factory=agent_factory)
    result = orch.handle_trigger(trigger)
    assert result.sender_id == "agent-d"
    assert result.sender_name == "小D"
    assert result.proposal is False
    assert any("本轮明确被 @ 的 AI 助手是：小D" in item for item in captured["messages"])
    assert any("请以该助手身份回应" in item for item in captured["messages"])
    print("[OK] current @agent identity is injected ahead of conflicting history")


def test_gm_proposal_for_conflict():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    result = orch.handle_trigger(_trigger("@GM 用户A要移动桌子，但是用户B不同意", "GM"))
    assert result.sender_id == "gm-system"
    assert result.sender_name == "GM"
    assert result.proposal is True
    assert "GM 提案" in result.text
    assert "房主可回复" in result.text
    assert result.action_payload["source_user_id"] == "user-a"
    assert result.action_payload["intent_text"]
    assert result.action_payload["execution"] == "host_single_writer"
    print("[OK] conflict or GM mention produces GM proposal")


def test_gm_generation_proposal_explains_plan_and_confirmation_effect():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@小女孩 我想要一个可爱的卧室，给我一个生成设计方案", "小女孩")
    trigger["sender_id"] = "host-a"
    trigger["sender_name"] = "房主"
    trigger["history"] = [
        {"message_id": "m0", "from": "用户B", "sender_id": "user-b", "text": "可以做可爱一点。"},
        {"message_id": "m1", "from": "房主", "sender_id": "host-a", "text": trigger["text"]},
    ]

    result = orch.handle_trigger(trigger)

    assert result.proposal is True
    assert "方案摘要" in result.text
    assert "确认后动作：开始生成 3D 场景" in result.text
    assert "确认该方案并开始生成" in result.text
    assert "可爱的卧室" in result.text
    assert result.action_payload["action_type"] == "start_generation"
    print("[OK] GM generation proposal explains plan and confirmation effect")


def test_gm_summary_does_not_create_proposal():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@GM 整理一下大家的想法", "GM")
    trigger["agent_id"] = "gm"
    result = orch.handle_trigger(trigger)
    assert result.sender_id == "gm-system"
    assert result.sender_name == "GM"
    assert result.proposal is False
    assert "GM 总结" in result.text
    assert "GM 提案" not in result.text
    print("[OK] @GM summary stays on GM control path without proposal")


def test_gm_summary_reads_agent_design_context_without_affecting_normal_summary():
    history = [
        {
            "message_id": "m0",
            "from": "房主",
            "sender_type": "user",
            "message_kind": "chat",
            "text": "围绕强盗藏宝室主题讨论一下",
        },
        {
            "message_id": "m1",
            "from": "长者",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": "方案建议：地下藏宝室，中央放金币堆和藏宝箱，两侧布置火把、武器架和酒桶。",
        },
        {
            "message_id": "m2",
            "from": "商人",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": "评价：这个方案适合包装成强盗首领的秘密金库，建议增加核心宝物和入口引导。",
        },
        {
            "message_id": "m3",
            "from": "GM",
            "sender_type": "user",
            "message_kind": "chat",
            "text": "@GM 总结一下当前方案",
        },
    ]

    normal_state = LANChatSummaryService().monitor(history)
    assert "长者" not in normal_state.summary
    assert "商人" not in normal_state.summary

    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@GM 总结一下当前方案", "GM")
    trigger["agent_id"] = "gm"
    trigger["history"] = history
    result = orch.handle_trigger(trigger)

    assert result.sender_id == "gm-system"
    assert result.proposal is False
    assert "GM 总结" in result.text
    assert "长者" in result.text
    assert "商人" in result.text
    assert "藏宝箱" in result.text
    assert "GM 提案" not in result.text
    print("[OK] @GM summary reads selected Agent design context without proposal")


def test_role_agent_not_hijacked_by_prior_agent_or_gm_messages():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@学者 给我介绍一下agent", "学者")
    trigger["agent_id"] = "agent-scholar"
    trigger["history"] = [
        {
            "message_id": "m0",
            "from": "山贼",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": "能看懂，直接生成也行。",
        },
        {
            "message_id": "m1",
            "from": "GM",
            "sender_type": "gm",
            "message_kind": "gm_proposal",
            "text": "【GM 提案 gm-1】待处理意图：内部摘要",
        },
        {
            "message_id": "m2",
            "from": "用户A",
            "sender_type": "user",
            "message_kind": "chat",
            "text": "@学者 给我介绍一下agent",
        },
    ]
    result = orch.handle_trigger(trigger)
    assert result.proposal is False
    assert result.sender_id == "agent-scholar"
    assert result.sender_name == "学者"
    print("[OK] prior agent/GM messages do not hijack a normal @agent reply")


def test_role_agent_theme_discussion_not_hijacked_by_gm():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@商人 围绕暗黑集市主题讨论一下吧，其他人都可以提出想法", "商人")
    trigger["agent_id"] = "merchant"
    trigger["sender_id"] = "host-a"
    trigger["sender_name"] = "房主"
    trigger["history"] = [
        {"message_id": "m0", "from": "房主", "sender_id": "host-a", "text": "@长者 介绍一下各位"},
        {"message_id": "m1", "from": "用户", "sender_id": "user-b", "text": "@房主 用商人"},
        {"message_id": "m2", "from": "房主", "sender_id": "host-a", "text": trigger["text"]},
    ]
    result = orch.handle_trigger(trigger)
    assert result.proposal is False
    assert result.sender_id == "merchant"
    assert result.sender_name == "商人"
    assert "GM 提案" not in result.text
    print("[OK] @Agent theme discussion stays on role agent path")


def test_role_agent_advice_request_not_hijacked_by_gm():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    result = orch.handle_trigger(_trigger("@商人 你有什么建议呢", "商人"))
    assert result.proposal is False
    assert result.sender_name == "商人"
    print("[OK] @Agent advice request does not create GM proposal")


def test_agent_plan_reference_resolves_before_gm_proposal():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@商人 就按照你的方案来执行吧", "商人")
    trigger["agent_id"] = "merchant"
    trigger["history"] = [
        {
            "message_id": "m0",
            "from": "房主",
            "sender_id": "host-a",
            "sender_type": "user",
            "message_kind": "chat",
            "text": "@商人 给我一个暗黑集市方案",
        },
        {
            "message_id": "m1",
            "from": "商人",
            "sender_id": "merchant",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": "暗黑集市方案：入口留出动线，中央主摊位，两侧货架，黑铁灯笼，贵重交易区靠内侧。",
        },
        {
            "message_id": "m2",
            "from": "房主",
            "sender_id": "host-a",
            "sender_type": "user",
            "message_kind": "chat",
            "text": trigger["text"],
        },
    ]
    result = orch.handle_trigger(trigger)
    assert result.proposal is True
    assert result.sender_name == "GM"
    assert result.action_payload["action_type"] == "start_generation"
    assert result.action_payload["source_agent_id"] == "merchant"
    assert "暗黑集市方案" in result.action_payload["resolved_intent_text"]
    assert "暗黑集市方案" in result.text
    assert "原始用户请求" in result.action_payload["resolved_intent_text"]
    assert "入口留出动线" in result.action_payload["resolved_intent_text"]
    print("[OK] @Agent plan reference is resolved before GM proposal")


def test_agent_plan_reference_resolves_real_runtime_phrase():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@商人 按照你说的这个方案进行场景建筑生成把", "商人")
    trigger["agent_id"] = "merchant"
    trigger["history"] = [
        {
            "message_id": "m1",
            "from": "商人",
            "sender_id": "merchant",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": (
                "暗黑集市布局方案：入口区设置黑木拱门，两侧放石灯、残碑；"
                "主通道铺暗色石板路，两侧摆摊位、货箱、展示架；"
                "深处设置主柜台、旧木桌、钱箱、账本、封印柜；灯光用暗红、幽紫和烛火。"
            ),
        },
        {
            "message_id": "m2",
            "from": "房主",
            "sender_id": "host-a",
            "sender_type": "user",
            "message_kind": "chat",
            "text": trigger["text"],
        },
    ]
    result = orch.handle_trigger(trigger)
    assert result.proposal is True
    assert result.action_payload["action_type"] == "start_generation"
    assert result.action_payload["source_agent_id"] == "merchant"
    resolved = result.action_payload["resolved_intent_text"]
    assert "原始用户请求" in resolved
    assert "黑木拱门" in resolved
    assert "暗色石板路" in resolved
    assert "主柜台" in resolved
    assert "现代主体建筑" not in resolved
    assert "黑木拱门" in result.text
    print("[OK] real runtime plan-reference phrase resolves to latest agent plan")


def test_agent_plan_reference_resolves_legacy_history_without_v2_fields():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@商人 按照你说的方案开始生成", "商人")
    trigger["agent_id"] = "merchant"
    trigger["history"] = [
        {
            "message_id": "m1",
            "from": "商人",
            "text": "暗黑集市方案：黑木拱门、石灯残碑、两侧摊位、深处主柜台、幽紫烛火。",
        },
        {
            "message_id": "m2",
            "from": "房主",
            "text": trigger["text"],
        },
    ]
    result = orch.handle_trigger(trigger)
    assert result.proposal is True
    assert result.action_payload["action_type"] == "start_generation"
    assert "黑木拱门" in result.action_payload["resolved_intent_text"]
    assert "主柜台" in result.action_payload["resolved_intent_text"]
    print("[OK] plan reference resolves from legacy history without v2 message fields")


def test_agent_plan_reference_does_not_cross_agents():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@长者 就按照你的方案来执行吧", "长者")
    trigger["agent_id"] = "elder"
    trigger["history"] = [
        {
            "message_id": "m1",
            "from": "商人",
            "sender_id": "merchant",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": "暗黑集市方案：中央主摊位，两侧货架，黑铁灯笼。",
        },
        {
            "message_id": "m2",
            "from": "房主",
            "sender_id": "host-a",
            "sender_type": "user",
            "message_kind": "chat",
            "text": trigger["text"],
        },
    ]
    result = orch.handle_trigger(trigger)
    assert result.proposal is False
    assert "没有找到长者刚才的可执行方案" in result.text
    assert result.action_payload is None
    print("[OK] plan reference only resolves against the currently mentioned agent")


def test_role_agent_opinion_about_named_agent_plan_receives_that_plan_context():
    captured = {}

    def agent_factory():
        def _agent(persona, messages):
            captured["persona"] = persona
            captured["messages"] = list(messages)
            joined = "\n".join(messages)
            assert "【被评价方案上下文】" in joined
            assert "小女孩" in joined
            assert "现代风客厅" in joined
            assert "沙发/座椅组" in joined
            assert "电视/媒体墙" in joined
            assert "她还没给出具体设计方案" not in joined
            return "我看到了小女孩的现代风客厅方案，会围绕沙发、茶几和电视墙评价。"
        return _agent

    orch = LanChatAgentOrchestrator(agent_factory=agent_factory)
    trigger = _trigger("@山贼 你对于小女孩的方案怎么看", "山贼")
    trigger["agent_id"] = "bandit"
    trigger["history"] = [
        {
            "message_id": "m0",
            "from": "房主",
            "sender_id": "host-a",
            "sender_type": "user",
            "message_kind": "chat",
            "text": "@小女孩 你是谁",
        },
        {
            "message_id": "m1",
            "from": "小女孩",
            "sender_id": "girl",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": "我是小女孩，你的场景设计助手。可以帮你做房间/展厅/商业空间等场景方案。",
        },
        {
            "message_id": "m2",
            "from": "房主",
            "sender_id": "host-a",
            "sender_type": "user",
            "message_kind": "chat",
            "text": "@小女孩 帮我设计一个现代风客厅",
        },
        {
            "message_id": "m3",
            "from": "小女孩",
            "sender_id": "girl",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": (
                "小女孩先帮你整理一个温柔、好落地的版本。\n"
                "我理解你的目标是：现代风客厅。\n\n"
                "方案内容：\n"
                "1. 风格定位：现代简约：中性色/低饱和配色，线条干净，材质对比清晰\n"
                "2. 空间布局：以交流区为中心，视觉焦点和收纳区分列两侧，主通道保持连续\n"
                "3. 核心物件：沙发/座椅组、茶几、电视/媒体墙、边柜/收纳柜、地毯\n"
                "4. 氛围装饰：装饰画、绿植、几何灯具、低饱和色彩点缀、重点照明"
            ),
        },
        {
            "message_id": "m4",
            "from": "房主",
            "sender_id": "host-a",
            "sender_type": "user",
            "message_kind": "chat",
            "text": trigger["text"],
        },
    ]
    result = orch.handle_trigger(trigger)
    assert result.proposal is False
    assert result.sender_name == "山贼"
    assert "现代风客厅方案" in result.text
    assert captured["messages"]
    print("[OK] role agent opinion about named agent plan receives referenced plan context")


def test_resolved_plan_generic_inventory_guard():
    text = (
        "原始用户请求：@商人 按照这个方案进行场景建筑生成把\n"
        "用户确认执行 @商人 最近方案。请严格围绕下列方案生成开放场景：黑木拱门、暗色石板路、摊位、主柜台。"
    )
    generic_items = [
        {"name": "现代主体建筑"},
        {"name": "入口门厅"},
        {"name": "铺装广场"},
        {"name": "指示牌"},
    ]
    specific_items = [
        {"name": "黑木拱门"},
        {"name": "暗色石板路"},
        {"name": "集市摊位"},
        {"name": "主柜台"},
    ]
    assert _has_resolved_plan_context(text) is True
    assert _looks_generic_inventory(generic_items) is True
    assert _looks_generic_inventory(specific_items) is False
    print("[OK] resolved-plan generic inventory guard detects fallback pollution")


def test_agent_plan_reference_after_stale_marker_is_rejected():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@商人 换方案，不要刚才那个。就按照你的方案来执行吧", "商人")
    trigger["agent_id"] = "merchant"
    trigger["history"] = [
        {
            "message_id": "m1",
            "from": "商人",
            "sender_id": "merchant",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": "暗黑集市方案：中央主摊位，两侧货架，黑铁灯笼。",
        },
        {
            "message_id": "m2",
            "from": "房主",
            "sender_id": "host-a",
            "sender_type": "user",
            "message_kind": "chat",
            "text": trigger["text"],
        },
    ]
    result = orch.handle_trigger(trigger)
    assert result.proposal is False
    assert "没有找到商人刚才的可执行方案" in result.text
    print("[OK] stale plan marker prevents executing an old plan reference")


def test_gm_pause_and_discussion_controls_do_not_reject_pending_proposal():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    runtime.set_mode("DISCUSSING")
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    proposal = orch.handle_trigger(_trigger("@GM 删除桌子", "GM"))
    assert proposal.proposal is True
    proposal_id = proposal.action_payload["proposal_id"]

    paused = orch.handle_trigger(_trigger("@GM 暂停", "GM"))
    assert paused.proposal is False
    assert "暂停状态" in paused.text
    assert paused.action_payload is None
    assert runtime.mode() == "PAUSED"

    discussing = orch.handle_trigger(_trigger("@GM 先讨论，不要生成", "GM"))
    assert "讨论模式" in discussing.text
    assert discussing.action_payload is None
    assert runtime.mode() == "DISCUSSING"

    resumed = orch.handle_trigger(_trigger("@GM 继续", "GM"))
    assert "恢复" in resumed.text
    assert runtime.mode() == "EXECUTING"

    confirmed = orch.handle_trigger(_trigger(f"@GM 确认 {proposal_id}", "GM"))
    assert confirmed.action_payload["status"] == "confirmed"
    runtime.end_compose()
    print("[OK] GM control commands do not accidentally reject pending proposals")


def test_single_user_major_action_stays_on_role_agent_path():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    result = orch.handle_trigger(_trigger("删除桌子", "小B"))
    assert result.proposal is False
    assert result.sender_id == "agent-b"
    assert "agent-reply" in result.text
    print("[OK] single-user major action is not swallowed by GM proposal")


def test_host_confirmation_consumes_pending_proposal():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    proposal = orch.handle_trigger(_trigger("@GM 删除桌子", "GM"))
    assert proposal.proposal is True
    proposal_id = proposal.action_payload["proposal_id"]
    confirmed = orch.handle_trigger(_trigger(f"@GM 确认 {proposal_id}", "GM"))
    assert confirmed.proposal is False
    assert "已确认" in confirmed.text
    assert confirmed.action_payload["status"] == "confirmed"
    assert confirmed.action_payload["source_user_id"] == "user-a"
    print("[OK] host confirmation consumes pending GM proposal")


def test_host_confirmation_rejects_wrong_proposal_id():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    proposal = orch.handle_trigger(_trigger("@GM 删除桌子", "GM"))
    assert proposal.proposal is True
    current_id = proposal.action_payload["proposal_id"]

    mismatch = orch.handle_trigger(_trigger("@GM 确认 gm-000000", "GM"))
    assert mismatch.proposal is False
    assert "编号不匹配" in mismatch.text
    assert mismatch.action_payload is None

    confirmed = orch.handle_trigger(_trigger(f"@GM 确认 {current_id}", "GM"))
    assert "已确认" in confirmed.text
    assert confirmed.action_payload["status"] == "confirmed"
    print("[OK] host confirmation validates proposal_id before consuming pending proposal")


def test_host_confirmation_replay_does_not_requeue_action():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    proposal = orch.handle_trigger(_trigger("@GM 删除桌子", "GM"))
    proposal_id = proposal.action_payload["proposal_id"]

    confirmed = orch.handle_trigger(_trigger(f"@GM 确认 {proposal_id}", "GM"))
    assert confirmed.action_payload["status"] == "confirmed"

    replay = orch.handle_trigger(_trigger(f"@GM 确认 {proposal_id}", "GM"))
    assert "已处理" in replay.text
    assert replay.action_payload is None
    print("[OK] repeated proposal confirmation does not requeue confirmed action")


def test_duplicate_trigger_reuses_existing_proposal_id():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    trigger = _trigger("@GM 用户A和用户B对桌子位置有冲突", "GM")
    first = orch.handle_trigger(trigger)
    second = orch.handle_trigger(dict(trigger))
    assert first.proposal is True and second.proposal is True
    assert first.action_payload["proposal_id"] == second.action_payload["proposal_id"]
    print("[OK] duplicate trigger reuses existing proposal instead of creating a second one")


def test_rejected_proposal_cannot_be_confirmed_later():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    proposal = orch.handle_trigger(_trigger("@GM 删除桌子", "GM"))
    proposal_id = proposal.action_payload["proposal_id"]
    rejected = orch.handle_trigger(_trigger(f"@GM 拒绝 {proposal_id}", "GM"))
    assert "已取消" in rejected.text
    assert rejected.action_payload["status"] == "rejected"
    replay = orch.handle_trigger(_trigger(f"@GM 确认 {proposal_id}", "GM"))
    assert "已处理" in replay.text
    assert replay.action_payload is None
    print("[OK] rejected proposal cannot be confirmed later")


def test_host_confirmation_rejects_explicit_non_host_role():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    proposal = orch.handle_trigger(_trigger("@GM 删除桌子", "GM"))
    proposal_id = proposal.action_payload["proposal_id"]
    guest_trigger = _trigger(f"@GM 确认 {proposal_id}", "GM")
    guest_trigger["sender_role"] = "guest"

    rejected = orch.handle_trigger(guest_trigger)
    assert "只有房主" in rejected.text
    assert rejected.action_payload is None

    host_trigger = _trigger(f"@GM 确认 {proposal_id}", "GM")
    host_trigger["sender_role"] = "host"
    confirmed = orch.handle_trigger(host_trigger)
    assert confirmed.action_payload["status"] == "confirmed"
    assert confirmed.action_payload["confirmation_mode"] == "verified_host"
    print("[OK] explicit non-host confirmation is rejected when role metadata exists")


def test_structured_confirmation_uses_correlation_id_and_metadata():
    orch = LanChatAgentOrchestrator(agent_factory=_agent_factory)
    proposal = orch.handle_trigger(_trigger("@GM 用户A和用户B对桌子位置有冲突", "GM"))
    assert proposal.proposal is True
    proposal_id = proposal.action_payload["proposal_id"]

    structured = _trigger("", "GM")
    structured["message_kind"] = "confirmation"
    structured["correlation_id"] = proposal_id
    structured["metadata_json"] = '{"decision":"confirm"}'

    confirmed = orch.handle_trigger(structured)
    assert confirmed.action_payload["status"] == "confirmed"
    assert confirmed.action_payload["confirmation_mode"] == "structured_confirmation"
    assert confirmed.action_payload["proposal_id"] == proposal_id
    print("[OK] structured confirmation consumes proposal by correlation_id")


def test_role_agent_api_error_is_sanitized():
    def broken_agent_factory():
        def _agent(persona, messages):
            raise RuntimeError("Error code: 401 - {'message': 'Invalid Token (request id: abc)'}")

        return _agent

    orch = LanChatAgentOrchestrator(agent_factory=broken_agent_factory)
    result = orch.handle_trigger(_trigger("@商人 你有什么建议", "商人"))
    assert result.proposal is False
    assert "当前模型服务不可用" in result.text
    assert "Invalid Token" not in result.text
    assert "request id" not in result.text
    print("[OK] role agent provider errors are sanitized before chat reply")


def test_worker_uses_orchestrator_and_sends_reply():
    engine = FakeEngine([_trigger()])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        async_agent_execution=False,
    )
    assert worker.process_once() is True
    assert len(engine.replies) == 1
    assert engine.replies[0][0] == "agent-b"
    assert "agent-reply" in engine.replies[0][2]
    assert engine.replies[0][3] == "agent_reply"
    print("[OK] worker polls C++ trigger and replies through C++")


def test_worker_client_role_does_not_execute_direct_agent_trigger():
    engine = FakeEngine([_trigger()], session_role="client")
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        async_agent_execution=False,
    )

    assert worker.process_once() is False
    assert engine.replies == []
    print("[OK] client role does not execute direct native agent trigger")


def test_worker_streams_sanitized_progress_reply_before_final():
    def progress_agent_factory():
        def _agent(persona, messages):
            from plugins.AITool.services.agent_progress_context import get_current_progress_sink

            sink = get_current_progress_sink()
            assert callable(sink)
            sink("生成进度  50% [█████░░░░░] 进行中：摆放物件。开始把主要物件放进场景。")
            return "final scene reply"

        return _agent

    engine = FakeEngine([_trigger("生成一个广场场景")])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=progress_agent_factory,
        async_agent_execution=False,
    )
    assert worker.process_once() is True
    assert len(engine.replies) == 2
    assert "生成进度" in engine.replies[0][2]
    assert engine.replies[0][3] == "progress"
    assert "final scene reply" in engine.replies[-1][2]
    assert engine.replies[-1][3] == "agent_reply"
    print("[OK] worker streams progress reply before final compose summary")


def test_worker_async_agent_execution_returns_before_slow_agent_reply():
    def slow_agent_factory():
        def _agent(persona, messages):
            time.sleep(0.05)
            return "slow final reply"

        return _agent

    engine = FakeEngine([_trigger("@小B 生成一个广场")])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=slow_agent_factory,
        async_agent_execution=True,
    )
    started = time.time()
    assert worker.process_once() is True
    assert time.time() - started < 0.04
    deadline = time.time() + 1.0
    while time.time() < deadline and (
        not engine.replies or "slow final reply" not in engine.replies[-1][2]
    ):
        time.sleep(0.01)
    assert engine.replies
    assert "slow final reply" in engine.replies[-1][2]
    print("[OK] async worker returns before slow agent reply and sends later")


def test_worker_async_sends_fast_ack_before_agent_lock_finishes():
    def slow_agent_factory():
        def _agent(persona, messages):
            time.sleep(0.08)
            return "slow compose done"

        return _agent

    engine = FakeEngine([_trigger("@小B 生成一个卧室", "小B")])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=slow_agent_factory,
        async_agent_execution=True,
    )
    assert worker.process_once() is True
    deadline = time.time() + 0.04
    while time.time() < deadline and not engine.replies:
        time.sleep(0.005)
    assert engine.replies
    assert engine.replies[0][3] == "progress"
    assert "已收到" in engine.replies[0][2]
    assert "正在生成" not in engine.replies[0][2]
    assert "下一阶段" not in engine.replies[0][2]
    assert "吸收" not in engine.replies[0][2]
    deadline = time.time() + 1.0
    while time.time() < deadline and "slow compose done" not in engine.replies[-1][2]:
        time.sleep(0.01)
    assert "slow compose done" in engine.replies[-1][2]
    print("[OK] async worker sends fast ack before long compose finishes")


def test_worker_records_busy_scene_message_without_agent_lock():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    runtime.start_compose("长者", "森林奇幻集市")
    try:
        engine = FakeEngine([_trigger("@小女孩 后面再加两个摊位和灯串", "小女孩")])
        worker = LANChatAgentWorker(
            corona_engine=engine,
            agent_factory=_agent_factory,
            async_agent_execution=False,
        )
        assert worker.process_once() is True
        assert len(engine.replies) == 1
        assert engine.replies[0][1] == "小女孩"
        assert "已记录" in engine.replies[0][2]
        notes = runtime.consume_notes()
        assert notes and notes[0].kind == "generation_delta"
        assert "灯串" in notes[0].text
    finally:
        runtime.end_compose()
        runtime.consume_notes()
    print("[OK] busy scene messages are recorded and replied without entering agent lock")


def test_worker_records_busy_layout_and_edit_notes():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    runtime.start_compose("长者", "森林奇幻集市")
    try:
        engine = FakeEngine([
            _trigger("@学者 后续不要挡中央活动区", "学者"),
            _trigger("@小女孩 放大摊位", "小女孩"),
        ])
        worker = LANChatAgentWorker(
            corona_engine=engine,
            agent_factory=_agent_factory,
            async_agent_execution=False,
        )
        assert worker.process_once() is True
        assert worker.process_once() is True
        assert len(engine.replies) == 2
        notes = runtime.consume_notes()
        assert [note.kind for note in notes] == ["layout_constraint", "edit_existing"]
        assert "中央活动区" in notes[0].text
        assert "放大" in notes[1].text
    finally:
        runtime.end_compose()
        runtime.consume_notes()
    print("[OK] busy path records layout constraints and edit requests for next-batch handling")


def test_planning_confirmation_gate_roundtrip():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    action, reply = runtime.handle_planning_gate("长者", "我有一个计划，建立一个森林奇幻集市")
    assert action == "reply"
    assert "确认开始" in str(reply)
    assert "建议先做" in str(reply)
    action, reply = runtime.handle_planning_gate("长者", "补充：要有灯串和木牌")
    assert action == "reply"
    assert "我已更新方案" in str(reply)
    action, compose_text = runtime.handle_planning_gate("长者", "确认开始，先生成前三个")
    assert action == "compose"
    assert "森林奇幻集市" in str(compose_text)
    assert "灯串" in str(compose_text) or "木牌" in str(compose_text)
    print("[OK] planning confirmation gate returns proposal then compose text")


def test_planning_confirmation_gate_returns_concrete_design_brief():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()

    try:
        action, reply = runtime.handle_planning_gate("小女孩", "@小女孩 帮我设计一个可爱的卧室")

        assert action == "reply"
        text = str(reply)
        assert "方案内容" in text
        assert "风格定位" in text
        assert "空间布局" in text
        assert "核心物件" in text
        assert "确认开始" in text
        assert "补充要求：" in text
        assert "我会先更新方案，不会立刻生成" in text
        assert "例如：补充要求：床边加一个小书架，整体更粉一点" in text
        assert "可爱的卧室" in text
        assert "..." not in text
        assert "主体建筑或摊位" not in text
        assert "环境主体" not in text
    finally:
        runtime.clear_pending_planning("小女孩")
    print("[OK] planning gate returns concrete bedroom design brief")


def test_worker_routes_plain_chat_supplement_to_pending_planning_gate():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    action, reply = runtime.handle_planning_gate("小女孩", "@小女孩 帮我设计一个可爱的卧室")
    assert action == "reply"
    assert "方案内容" in str(reply)

    coordinator = InteractionCoordinator()
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "plain-plan-supplement-1",
        "room_id": "r-plan-supplement",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "补充要求，减少方案中的细碎物体",
    }])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    try:
        assert worker.process_once() is True
    finally:
        runtime.clear_pending_planning("小女孩")
        runtime.end_compose("小女孩")

    assert engine.replies
    reply_text = str(engine.replies[-1][2])
    assert engine.replies[-1][1] == "小女孩"
    assert "我已更新方案" in reply_text
    assert "减少方案中的细碎物体" in reply_text
    print("[OK] worker routes no-mention supplement to pending planning gate")


def test_worker_client_sync_does_not_reply_from_plain_pending_planning_gate():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    action, reply = runtime.handle_planning_gate("小女孩", "@小女孩 帮我设计一个可爱的卧室")
    assert action == "reply"
    assert "方案内容" in str(reply)

    coordinator = InteractionCoordinator()
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "client-plain-plan-supplement-1",
        "room_id": "r-client-plan-supplement",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "补充要求，减少方案中的细碎物体",
    }], session_role="client")
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    try:
        assert worker.process_once() is True
    finally:
        runtime.clear_pending_planning("小女孩")
        runtime.end_compose("小女孩")

    assert engine.replies == []
    print("[OK] client sync does not reply from plain pending planning gate")


def test_plain_chat_generation_start_confirms_and_executes_active_seed_plan():
    coordinator = InteractionCoordinator()
    room_id = "r-plain-generation-start"
    coordinator.create_or_update_seed_plan(ChatMessage(
        room_id=room_id,
        sender_id="host-a",
        sender_name="房主",
        text="帮我设计一个现代简约客厅",
        is_host=True,
    ))
    engine = FakeEngine([], coordinator_messages=[
        {
            "message_id": "plain-generation-start-1",
            "room_id": room_id,
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "开始生成",
        },
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    plan = coordinator.active_plan_for_room(room_id)
    assert plan is not None
    assert plan.status in {SeedPlanStatus.DRAFT, SeedPlanStatus.PROPOSED}

    assert worker.process_once() is True
    plan = coordinator.active_plan_for_room(room_id)
    assert plan is not None
    assert plan.status == SeedPlanStatus.EXECUTING
    visible_text = "\n".join(str(item[2]) for item in [*engine.replies, *engine.system_messages])
    assert "已进入生成队列" in visible_text
    assert "等待房主确认：开始生成" not in visible_text
    assert "开始生成；开始生成" not in str(plan.intent_summary)
    print("[OK] plain chat generation start confirms and executes active SeedPlan")


def test_worker_agent_trigger_generation_start_consumes_pending_runtime_plan():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    runtime.clear_pending_planning()
    action, reply = runtime.handle_planning_gate("长者", "帮我设计一个现代简约客厅")
    assert action == "reply"
    assert "现代简约客厅" in str(reply)

    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    engine = FakeEngine([{
        **_trigger("开始生成", "长者"),
        "room_id": "r-runtime-confirm",
        "sender_id": "host-a",
        "sender_name": "房主",
        "is_host": True,
        "sender_type": "host",
    }])

    def failing_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("role agent should not run when pending runtime plan is confirmed")
        return _agent

    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=failing_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    try:
        assert worker.process_once() is True
    finally:
        runtime.clear_pending_planning("长者")
        runtime.end_compose("长者")

    plan = coordinator.active_plan_for_room("r-runtime-confirm")
    assert plan is not None
    assert plan.status == SeedPlanStatus.EXECUTING
    assert scheduler.submitted
    assert "现代简约客厅" in scheduler.submitted[-1]["prompt"]
    visible_text = "\n".join(str(item[2]) for item in [*engine.replies, *engine.system_messages])
    assert "已进入生成队列" in visible_text
    assert "尚未开始生成" not in visible_text
    assert "请先由房主确认" not in visible_text
    assert not runtime.pending_planning_snapshot()
    print("[OK] agent trigger generation start consumes pending runtime plan")


def test_worker_metadata_generate_consumes_pending_runtime_plan():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    runtime.clear_pending_planning()
    action, reply = runtime.handle_planning_gate("长者", "帮我设计一个现代简约客厅")
    assert action == "reply"
    assert "现代简约客厅" in str(reply)

    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "metadata-generate-runtime-plan-1",
        "room_id": "r-metadata-runtime-confirm",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "开始生成",
        "metadata": {
            "draft_action": "generate",
            "target_scope": "plan",
            "target_agent_name": "长者",
            "target_agent_id": "elder",
        },
    }])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    try:
        assert worker.process_once() is True
    finally:
        runtime.clear_pending_planning("长者")
        runtime.end_compose("长者")

    plan = coordinator.active_plan_for_room("r-metadata-runtime-confirm")
    assert plan is not None
    assert plan.status == SeedPlanStatus.EXECUTING
    assert scheduler.submitted
    assert "现代简约客厅" in scheduler.submitted[-1]["prompt"]
    visible_text = "\n".join(str(item[2]) for item in [*engine.replies, *engine.system_messages])
    assert "已进入生成队列" in visible_text
    assert "尚未开始生成" not in visible_text
    assert "请先由房主确认" not in visible_text
    print("[OK] metadata generate consumes pending runtime plan")


def test_worker_agent_trigger_plan_elaboration_uses_pending_runtime_plan_without_generation():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    runtime.clear_pending_planning()
    action, reply = runtime.handle_planning_gate("小女孩", "帮我设计一个现代风客厅")
    assert action == "reply"
    assert "现代风客厅" in str(reply)

    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    engine = FakeEngine([{
        **_trigger("继续输出风格方案、布局、物品清单", "小女孩"),
        "room_id": "r-runtime-plan-elaboration",
        "sender_id": "host-a",
        "sender_name": "房主",
        "is_host": True,
        "sender_type": "host",
    }])

    def failing_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("role agent should not run for pending plan elaboration")
        return _agent

    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=failing_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    try:
        assert worker.process_once() is True
        assert not scheduler.submitted
        active_plan = coordinator.active_plan_for_room("r-runtime-plan-elaboration")
        assert active_plan is None or active_plan.status != SeedPlanStatus.EXECUTING
        assert engine.replies
        reply_text = str(engine.replies[-1][2])
        assert "方案展开" in reply_text
        assert "风格方案" in reply_text
        assert "布局" in reply_text
        assert "物品清单" in reply_text
        assert "现代风客厅" in reply_text
        assert "尚未开始生成" not in reply_text
        assert "请先由房主确认" not in reply_text
        assert runtime.pending_planning_snapshot()
        print("[OK] agent trigger plan elaboration uses pending runtime plan without generation")
    finally:
        runtime.clear_pending_planning("小女孩")
        runtime.end_compose("小女孩")


def test_worker_disambiguates_plain_chat_supplement_when_multiple_plans_pending():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    action, reply = runtime.handle_planning_gate("小女孩", "@小女孩 帮我设计一个可爱的卧室")
    assert action == "reply"
    assert "方案内容" in str(reply)
    action, reply = runtime.handle_planning_gate("商人", "@商人 帮我设计一个暗黑集市")
    assert action == "reply"
    assert "方案内容" in str(reply)

    coordinator = InteractionCoordinator()
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "plain-plan-supplement-ambiguous-1",
        "room_id": "r-plan-supplement",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "补充要求，减少方案中的细碎物体",
    }])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    try:
        assert worker.process_once() is True
    finally:
        runtime.clear_pending_planning("小女孩")
        runtime.clear_pending_planning("商人")
        runtime.end_compose()

    assert engine.replies
    reply_text = str(engine.replies[-1][2])
    assert "请先 @ 指定要更新哪个方案" in reply_text
    assert "小女孩" in reply_text
    assert "商人" in reply_text
    assert coordinator.active_plan_for_room("r-plan-supplement") is None
    print("[OK] ambiguous no-mention supplement asks user to target one pending plan")


def test_runtime_routes_metadata_targeted_pending_plan_without_mention():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    action, reply = runtime.handle_planning_gate("小女孩", "@小女孩 帮我设计一个可爱的卧室")
    assert action == "reply"
    action, reply = runtime.handle_planning_gate("商人", "@商人 帮我设计一个暗黑集市")
    assert action == "reply"

    try:
        action, payload, agent_name = runtime.handle_targeted_planning_message(
            "小女孩",
            "减少方案中的细碎物体",
            draft_action="supplement",
        )
    finally:
        runtime.clear_pending_planning("小女孩")
        runtime.clear_pending_planning("商人")
        runtime.end_compose()

    assert action == "reply"
    assert agent_name == "小女孩"
    assert "我已更新方案" in str(payload)
    assert "减少方案中的细碎物体" in str(payload)
    assert "暗黑集市" not in str(payload)
    print("[OK] runtime routes metadata-targeted supplement to matching pending plan")


def test_runtime_metadata_generate_confirms_pending_plan_without_magic_text():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    action, reply = runtime.handle_planning_gate("小女孩", "@小女孩 帮我设计一个可爱的卧室")
    assert action == "reply"

    try:
        action, payload, agent_name = runtime.handle_targeted_planning_message(
            "小女孩",
            "就按这个执行",
            draft_action="generate",
        )
    finally:
        runtime.clear_pending_planning("小女孩")
        runtime.end_compose("小女孩")

    assert action == "compose"
    assert agent_name == "小女孩"
    assert "用户确认开始生成" in str(payload)
    assert "可爱的卧室" in str(payload)
    print("[OK] runtime metadata generate starts from pending plan without typed confirmation")


def test_worker_metadata_chat_targets_agent_without_at_or_coordinator_sync():
    coordinator = InteractionCoordinator()
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "metadata-chat-agent-1",
        "room_id": "r-metadata-chat",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "你是谁",
        "metadata": {
            "workspace_mode": "solo_single_agent",
            "draft_action": "chat",
            "target_scope": "agent",
            "target_agent_id": "agent-girl",
            "target_agent_name": "小女孩",
        },
    }])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    assert worker.process_once() is True
    assert engine.replies
    assert engine.replies[-1][0] == "agent-girl"
    assert engine.replies[-1][1] == "小女孩"
    assert coordinator.active_plan_for_room("r-metadata-chat") is None
    print("[OK] worker metadata chat targets agent without @ and skips Coordinator")


def test_worker_client_sync_does_not_execute_targeted_agent_chat():
    coordinator = InteractionCoordinator()
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "client-sync-agent-chat-1",
        "room_id": "r-client-sync-agent-chat",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "你是谁",
        "metadata": {
            "workspace_mode": "solo_single_agent",
            "draft_action": "chat",
            "target_scope": "agent",
            "target_agent_id": "agent-girl",
            "target_agent_name": "小女孩",
        },
    }], session_role="client")
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    assert worker.process_once() is True
    assert engine.replies == []
    assert coordinator.active_plan_for_room("r-client-sync-agent-chat") is None
    print("[OK] client coordinator sync does not execute targeted agent chat locally")


def test_worker_metadata_group_chat_triggers_each_agent_without_at():
    coordinator = InteractionCoordinator()
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "metadata-chat-group-1",
        "room_id": "r-metadata-group-chat",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "大家怎么看这个卧室？",
        "metadata": {
            "workspace_mode": "solo_multi_agent",
            "draft_action": "chat",
            "target_scope": "group",
            "target_agent_ids": ["agent-girl", "agent-merchant"],
            "target_agent_names": ["小女孩", "商人"],
        },
    }])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    assert worker.process_once() is True
    assert [item[1] for item in engine.replies[-2:]] == ["小女孩", "商人"]
    assert coordinator.active_plan_for_room("r-metadata-group-chat") is None
    print("[OK] worker metadata group chat triggers each target agent without @")


def test_worker_metadata_scene_discussion_feeds_seed_plan_before_confirmation():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    metadata = {
        "workspace_mode": "solo_single_agent",
        "draft_action": "chat",
        "target_scope": "agent",
        "target_agent_id": "agent-elder",
        "target_agent_name": "长者",
    }
    engine = FakeEngine([], coordinator_messages=[
        {
            "message_id": "dark-market-discussion-1",
            "room_id": "r-dark-market-authority",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "围绕暗黑集市主题讨论一下",
            "metadata": metadata,
        },
        {
            "message_id": "dark-market-discussion-2",
            "room_id": "r-dark-market-authority",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "给一个简化方案，我没那么多钱",
            "metadata": metadata,
        },
        {
            "message_id": "dark-market-discussion-3",
            "room_id": "r-dark-market-authority",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "还是采用暗黑集市方案吧",
            "metadata": metadata,
        },
        {
            "message_id": "dark-market-confirm-1",
            "room_id": "r-dark-market-authority",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "确认生成",
        },
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    assert worker.process_once() is True

    plan = coordinator.active_plan_for_room("r-dark-market-authority")
    assert plan is not None
    assert plan.status == SeedPlanStatus.EXECUTING
    assert "暗黑集市" in plan.design_brief
    assert scheduler.submitted
    assert "暗黑集市" in scheduler.submitted[-1]["prompt"]
    assert "确认生成" != plan.design_brief
    print("[OK] metadata scene discussion feeds authoritative SeedPlan before confirmation")


def test_worker_metadata_plan_targets_agent_and_returns_plan_reply_without_at():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    runtime.clear_pending_planning("长者")

    coordinator = InteractionCoordinator()
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "metadata-plan-agent-1",
        "room_id": "r-metadata-plan",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "帮我设计一个现代客厅，方案尽可能详细，且方案合理",
        "metadata": {
            "workspace_mode": "solo_multi_agent",
            "draft_action": "plan",
            "target_scope": "agent",
            "target_agent_id": "agent-elder",
            "target_agent_name": "长者",
        },
    }])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    try:
        assert worker.process_once() is True
    finally:
        runtime.clear_pending_planning("长者")
        runtime.end_compose()

    assert engine.replies
    assert engine.replies[-1][0] == "agent-elder"
    assert engine.replies[-1][1] == "长者"
    reply_text = str(engine.replies[-1][2])
    assert "方案内容" in reply_text
    assert "现代客厅" in reply_text
    assert "确认开始" in reply_text
    assert coordinator.active_plan_for_room("r-metadata-plan") is None
    print("[OK] worker metadata plan targets agent and returns plan reply without @")


def test_worker_explicit_at_agent_overrides_stale_metadata_owner():
    coordinator = InteractionCoordinator()
    engine = FakeEngine([])
    engine.network_lanchat_agents_snapshot = lambda: [
        {"agent_id": "agent-bandit", "name": "山贼"},
        {"agent_id": "agent-merchant", "name": "商人"},
    ]
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    handled = worker.sync_chat_message_to_coordinator({
        "message_id": "explicit-merchant-over-stale-bandit",
        "room_id": "r-agent-owner",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "@商人 在山贼方案基础上，给出一个藏宝室设计",
        "metadata": {
            "draft_action": "generate",
            "target_scope": "agent",
            "target_agent_id": "agent-bandit",
            "target_agent_name": "山贼",
        },
    })

    plan = coordinator.active_plan_for_room("r-agent-owner")
    assert handled is True
    assert plan is not None
    assert plan.review_policy["owner_agent_id"] == "agent-merchant"
    assert plan.review_policy["owner_agent_name"] == "商人"
    assert "山贼" in plan.review_policy["source_context_agents"]
    print("[OK] explicit @ agent overrides stale metadata owner before Coordinator ingest")


def test_worker_metadata_supplement_selects_plan_when_multiple_pending():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    action, reply = runtime.handle_planning_gate("小女孩", "@小女孩 帮我设计一个可爱的卧室")
    assert action == "reply"
    action, reply = runtime.handle_planning_gate("商人", "@商人 帮我设计一个暗黑集市")
    assert action == "reply"

    coordinator = InteractionCoordinator()
    engine = FakeEngine([], coordinator_messages=[{
        "message_id": "metadata-plan-supplement-1",
        "room_id": "r-plan-supplement",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "减少方案中的细碎物体",
        "metadata": {
            "draft_action": "supplement",
            "target_scope": "plan",
            "target_agent_name": "小女孩",
        },
    }])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    try:
        assert worker.process_once() is True
    finally:
        runtime.clear_pending_planning("小女孩")
        runtime.clear_pending_planning("商人")
        runtime.end_compose()

    assert engine.replies
    reply_text = str(engine.replies[-1][2])
    assert engine.replies[-1][1] == "小女孩"
    assert "我已更新方案" in reply_text
    assert "请先 @ 指定" not in reply_text
    print("[OK] worker metadata supplement selects target plan when multiple plans are pending")


def test_planning_gate_records_pre_generation_style_supplement():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.consume_notes()
    action, reply = runtime.handle_planning_gate("商人", "我想做一个有点神秘感的室外集市，不要太恐怖，适合几个人逛。")
    assert action == "reply"
    assert "确认开始" in str(reply)

    supplement = "我希望它更温暖一点，有灯光和休息区，不要全是暗黑风。"
    action, reply = runtime.handle_planning_gate("商人", supplement)
    assert action == "reply"
    assert "我已更新方案" in str(reply)
    assert "没有可编辑" not in str(reply)

    action, compose_text = runtime.handle_planning_gate("商人", "确认开始")
    assert action == "compose"
    assert "补充要求" in str(compose_text)
    assert "更温暖" in str(compose_text)
    assert "不要全是暗黑风" in str(compose_text)
    print("[OK] planning gate records pre-generation style supplement instead of edit fallback")


def test_worker_async_agent_calls_are_serialized_per_worker():
    active = 0
    max_active = 0
    lock = threading.Lock()

    def slow_agent_factory():
        def _agent(persona, messages):
            nonlocal active, max_active
            with lock:
                active += 1
                max_active = max(max_active, active)
            time.sleep(0.03)
            with lock:
                active -= 1
            return "serialized async reply"

        return _agent

    engine = FakeEngine([
        _trigger("@小B 第一条", "小B"),
        _trigger("@小B 第二条", "小B"),
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=slow_agent_factory,
        async_agent_execution=True,
    )
    assert worker.process_once() is True
    assert worker.process_once() is True
    deadline = time.time() + 1.0
    while time.time() < deadline and len(engine.replies) < 2:
        time.sleep(0.01)
    assert len(engine.replies) == 2
    assert max_active == 1
    print("[OK] async worker serializes agent/orchestrator calls per worker")


def test_worker_broadcasts_confirmed_gm_action():
    executor = FakeHostActionExecutor()
    engine = FakeEngine([
        _trigger("@GM 删除桌子", "GM"),
        _trigger("确认", "GM"),
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        host_action_executor=executor,
        async_agent_execution=False,
    )
    assert worker.process_once() is True
    assert worker.process_once() is True
    assert engine.system_messages and engine.system_messages[0][3] == "gm_proposal"
    assert len(engine.replies) == 1
    assert engine.replies[0][3] == "agent_reply"
    assert engine.intents, "confirmed GM action should be visible to C++ intent channel"
    assert engine.intents[-1][0] == "user-a"
    statuses = [row[3] for row in engine.intents]
    assert "confirmed_gm_action" in statuses
    assert executor.payloads, "confirmed GM action should enter host single-writer queue"
    assert executor.payloads[-1]["source_user_id"] == "user-a"
    print("[OK] worker broadcasts confirmed GM action payload and queues host execution")


def test_worker_acknowledges_final_adjustment_confirmation_without_host_execution():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_intervention({
        "room_id": "room-a",
        "plan_id": plan.plan_id,
        "actor_id": "actor-stall",
        "source_user_id": "u1",
        "intent_type": "remove",
        "content": "删除入口摊位",
        "priority": 2,
        "apply_policy": "final_adjustment",
    })
    coordinator.ingest_intervention({
        "room_id": "room-a",
        "plan_id": plan.plan_id,
        "actor_id": "actor-stall",
        "source_user_id": "u2",
        "intent_type": "modify",
        "content": "保留入口摊位并调暗",
        "priority": 2,
        "apply_policy": "final_adjustment",
    })
    proposal_id = coordinator.final_adjustment_plan(plan.plan_id)["conflicts"][0]["proposal_id"]
    event_start = len(coordinator.events)
    engine = FakeEngine([
        {
            **_trigger(f"@GM 确认 {proposal_id}", "GM"),
            "message_kind": "confirmation",
            "correlation_id": proposal_id,
            "metadata": {
                "decision": "confirm",
                "proposal_id": proposal_id,
            },
        },
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    assert worker.process_once() is True

    assert not executor.payloads
    assert not engine.intents
    assert engine.replies and f"已确认最终调整提案 {proposal_id}" in engine.replies[-1][2]
    new_events = coordinator.events[event_start:]
    assert any(item.event_type == "final_adjustment_conflict_confirmed" for item in new_events)
    assert any(
        json.loads(item[5]).get("disclosure", {}).get("metadata", {}).get("intervention", {}).get("proposal_id") == proposal_id
        for item in engine.system_messages
        if len(item) > 5 and item[3] == "action_status"
    )
    print("[OK] worker acknowledges final adjustment confirmation without host execution")


def test_worker_routes_agent_status_query_to_coordinator_without_model_agent():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-status",
        sender_id="host-a",
        sender_name="房主",
        text="做一个温暖的夜晚幻想集市",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("room-status")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    before_events = len(coordinator.events)

    def forbidden_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("status query should not call role model agent")
        return _agent

    engine = FakeEngine([
        {
            **_trigger("@商人 现在情况是什么样，生成计划是什么", "商人"),
            "room_id": "room-status",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "merchant",
            "agent_name": "商人",
        },
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=forbidden_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    assert worker.process_once() is True

    assert not executor.payloads
    assert not engine.intents
    assert engine.replies
    assert "当前方案" in engine.replies[-1][2]
    assert not any(item[3] == "gm_proposal" for item in engine.system_messages if len(item) > 3)
    assert any(item.event_type == "status_query" for item in coordinator.events[before_events:])
    print("[OK] worker routes @Agent status query to Coordinator without model agent/proposal")


def test_worker_does_not_crash_when_coordinator_blocks_start_generation():
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-blocked-start",
        sender_id="host-a",
        sender_name="房主",
        text="我想做一个集市，但是用户A和用户B对入口位置有冲突",
        is_host=True,
    ))
    coordinator.propose_seed_plan("room-blocked-start")
    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    payload = worker._prepare_confirmed_action_payload({  # noqa: SLF001 - covers crash guard for confirmed model payloads
        "action_type": "start_generation",
        "status": "confirmed",
        "intent_text": "按刚才方案开始生成",
    }, {
        **_trigger("@长者 确认开始", "长者"),
        "room_id": "room-blocked-start",
        "sender_id": "host-a",
        "sender_name": "房主",
        "is_host": True,
    })

    assert payload["action_type"] == "discussion_only"
    assert payload["coordinator_blocked"] is True
    assert payload["execution"] == "coordinator_confirmation_blocked"
    assert "冲突" in payload["reason"]
    print("[OK] blocked Coordinator confirmation no longer crashes Worker start payload preparation")


def test_worker_applies_host_vlm_generation_option_from_metadata():
    old_targets = os.environ.get("PROGRESSIVE_VLM_MAX_TARGETS")
    try:
        os.environ["PROGRESSIVE_VLM_MAX_TARGETS"] = "0"
        worker = LANChatAgentWorker(
            corona_engine=FakeEngine([]),
            agent_factory=_agent_factory,
            async_agent_execution=False,
        )

        worker._apply_generation_options_from_message({  # noqa: SLF001 - direct coverage for metadata bridge
            "sender_type": "host",
            "metadata_json": json.dumps({
                "generation_options": {
                    "vlm_enabled": True,
                    "vlm_max_targets": 1,
                },
            }, ensure_ascii=False),
        })
        assert os.environ.get("PROGRESSIVE_VLM_MAX_TARGETS") == "1"

        worker._apply_generation_options_from_message({  # noqa: SLF001
            "sender_type": "user",
            "metadata_json": json.dumps({
                "generation_options": {
                    "vlm_enabled": False,
                    "vlm_max_targets": 0,
                },
            }, ensure_ascii=False),
        })
        assert os.environ.get("PROGRESSIVE_VLM_MAX_TARGETS") == "1"

        worker._apply_generation_options_from_message({  # noqa: SLF001
            "sender_type": "host",
            "metadata_json": json.dumps({
                "generation_options": {
                    "vlm_enabled": False,
                    "vlm_max_targets": 0,
                },
            }, ensure_ascii=False),
        })
        assert os.environ.get("PROGRESSIVE_VLM_MAX_TARGETS") == "0"
    finally:
        if old_targets is None:
            os.environ.pop("PROGRESSIVE_VLM_MAX_TARGETS", None)
        else:
            os.environ["PROGRESSIVE_VLM_MAX_TARGETS"] = old_targets
    print("[OK] worker applies host-only VLM generation option from LANChat metadata")


def test_worker_routes_completed_layout_adjustment_to_coordinator_without_model_agent():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-adjust",
        sender_id="host-a",
        sender_name="房主",
        text="做一个温暖的夜晚幻想集市",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("room-adjust")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED
    before_events = len(coordinator.events)

    def forbidden_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("completed layout adjustment should not call role model agent")
        return _agent

    engine = FakeEngine([
        {
            **_trigger("@长者 感觉布局不是很合理呀，调整一下", "长者"),
            "room_id": "room-adjust",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "elder",
            "agent_name": "长者",
        },
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=forbidden_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    assert worker.process_once() is True

    pending = coordinator.pending_interventions(plan.plan_id)
    assert not executor.payloads
    assert engine.replies
    assert "布局调整建议" in engine.replies[-1][2]
    assert pending[-1].apply_policy == "final_adjustment"
    assert any(item.event_type == "layout_reflow_proposal_created" for item in coordinator.events[before_events:])
    print("[OK] worker routes completed layout adjustment to layout proposal without model agent")


def test_worker_confirms_completed_layout_reflow_and_moves_ordinary_actors():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-layout-confirm",
        sender_id="host-a",
        sender_name="房主",
        text="做一个藏宝室",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("room-layout-confirm")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED
    room_box = FakeSceneActor("__room_box", position=[0.0, 1.5, 0.0], scale=[6.5, 3.0, 6.5])
    chest = FakeSceneActor("藏宝箱", position=[0.0, 0.0, 0.0])
    table = FakeSceneActor("木桌", position=[0.1, 0.0, 0.0])
    statue = FakeSceneActor("天使雕像", position=[0.2, 0.0, 0.0])

    def forbidden_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("layout reflow confirmation should not call role model agent")
        return _agent

    engine = FakeEngine([
        {
            **_trigger("@商人 调整一下布局，我看模型位置冲突", "商人"),
            "room_id": "room-layout-confirm",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "merchant",
            "agent_name": "商人",
        },
        {
            **_trigger("确认调整", "商人"),
            "room_id": "room-layout-confirm",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "merchant",
            "agent_name": "商人",
        },
    ])
    engine.get_scene_actors = lambda: [room_box, chest, table, statue]
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=forbidden_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    assert worker.process_once() is True
    assert worker.process_once() is True

    assert not executor.payloads
    assert room_box.get_position() == [0.0, 1.5, 0.0]
    assert chest.get_position() != [0.0, 0.0, 0.0]
    assert table.get_position() != [0.1, 0.0, 0.0]
    assert statue.get_position() != [0.2, 0.0, 0.0]
    assert "布局调整完成" in engine.replies[-1][2]
    print("[OK] worker confirms completed layout reflow and moves ordinary actors")


def test_worker_layout_reflow_grounds_floating_actor_and_allows_repeat_adjustment():
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-layout-ground",
        sender_id="host-a",
        sender_name="房主",
        text="做一个藏宝室",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("room-layout-ground")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED
    room_box = FakeSceneActor("__room_box", position=[0.0, 1.5, 0.0], scale=[6.5, 3.0, 6.5])
    floating = FakeSceneActor(
        "藏宝箱",
        position=[0.0, 2.0, 0.0],
        aabb=[-0.4, 1.5, -0.4, 0.4, 2.5, 0.4],
    )
    table = FakeSceneActor("木桌", position=[0.2, 0.0, 0.0])
    wall_torch = FakeSceneActor(
        "火把",
        position=[0.0, 1.8, 0.0],
        aabb=[-0.1, 1.2, -0.1, 0.1, 2.0, 0.1],
    )

    def forbidden_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("layout grounding should not call role model agent")
        return _agent

    engine = FakeEngine([
        {
            **_trigger("@商人 浮空了，调整一下布局", "商人"),
            "room_id": "room-layout-ground",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "merchant",
            "agent_name": "商人",
        },
        {
            **_trigger("确认调整", "商人"),
            "room_id": "room-layout-ground",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "merchant",
            "agent_name": "商人",
        },
        {
            **_trigger("@商人 还是没贴地，再调整一下布局", "商人"),
            "room_id": "room-layout-ground",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "merchant",
            "agent_name": "商人",
        },
    ])
    engine.get_scene_actors = lambda: [room_box, floating, table, wall_torch]
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=forbidden_agent_factory,
        host_action_executor=FakeHostActionExecutor(),
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    assert worker.process_once() is True
    assert worker.process_once() is True
    assert floating.get_position()[1] == 0.5
    assert wall_torch.get_position()[1] == 1.8
    assert room_box.get_position() == [0.0, 1.5, 0.0]
    assert "布局调整完成" in engine.replies[-1][2]
    assert "已贴地修正地面物体" in engine.replies[-1][2]
    assert worker.process_once() is True
    assert "布局调整建议" in engine.replies[-1][2]
    print("[OK] worker layout reflow grounds floating actor and allows repeat adjustment")


def test_worker_executes_completed_boundary_adjustment_without_model_agent():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-boundary-adjust",
        sender_id="host-a",
        sender_name="房主",
        text="做一个温暖的夜晚幻想集市",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("room-boundary-adjust")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED
    boundary = FakeSceneActor("__terrain_boundary", scale=[2.0, 1.2, 2.0])

    def forbidden_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("completed boundary adjustment should use deterministic low-risk path")
        return _agent

    engine = FakeEngine([
        {
            **_trigger("@商人 你理解错了，我说的是 _terrain_boundary，换成更幻想集市风的低矮木栏/藤蔓围栏", "商人"),
            "room_id": "room-boundary-adjust",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "merchant",
            "agent_name": "商人",
        },
    ])
    engine.get_scene_actors = lambda: [boundary]
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=forbidden_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    assert worker.process_once() is True

    pending = coordinator.pending_interventions(plan.plan_id)
    assert not executor.payloads
    assert boundary.get_scale()[1] == 0.55
    assert boundary.color == [0.34, 0.45, 0.18]
    assert "已执行低风险最终调整" in engine.replies[-1][2]
    assert pending[-1].target_hint == "__terrain_boundary"
    assert pending[-1].apply_policy == "final_adjustment"
    print("[OK] completed terrain boundary adjustment executes deterministic low-risk path")


def test_worker_applies_completed_vlm_review_advice_from_metadata_chat():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-vlm-apply",
        sender_id="host-a",
        sender_name="房主",
        text="做一个奶油风卧室",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("room-vlm-apply")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED
    coordinator._active_generation_session_by_plan[plan.plan_id] = "session-vlm-apply"
    coordinator.ingest_review_result(ReviewResult(
        room_id="room-vlm-apply",
        plan_id=plan.plan_id,
        session_id="session-vlm-apply",
        batch_id="batch-final",
        review_type="vlm",
        passed=False,
        findings=["地毯偏大，建议缩小一点"],
        finding_details=[{
            "actor_id": "地毯",
            "fix_suggestion": "地毯偏大，缩小一点",
            "confidence": 0.42,
        }],
        severity="warn",
    ))
    rug = FakeSceneActor("地毯", scale=[1.0, 1.0, 1.0])

    def forbidden_agent_factory():
        def _agent(persona, messages):
            raise AssertionError("applying completed VLM advice should not call role model agent")
        return _agent

    metadata = {
        "draft_action": "chat",
        "target_scope": "agent",
        "target_agent_id": "girl",
        "target_agent_name": "小女孩",
    }
    engine = FakeEngine(
        [],
        coordinator_messages=[{
            "message_id": "m-vlm-apply",
            "room_id": "room-vlm-apply",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "agent_id": "girl",
            "agent_name": "小女孩",
            "target_agent_id": "girl",
            "target_agent_name": "小女孩",
            "text": "帮我按审查结果摆放物体",
            "metadata_json": json.dumps(metadata, ensure_ascii=False),
        }],
    )
    engine.get_scene_actors = lambda: [rug]
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=forbidden_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    assert worker.process_once() is True

    assert not executor.payloads
    assert rug.get_scale()[0] < 1.0
    assert engine.replies
    assert "已执行低风险最终调整" in engine.replies[-1][2]
    assert "地毯" in engine.replies[-1][2]
    print("[OK] completed metadata chat can apply recent VLM review advice")


def test_worker_acknowledges_conflict_resolution_rejection_without_host_execution():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        sender_name="用户A",
        text="我想要红墙集市",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        sender_name="用户B",
        text="这里有冲突：我想要蓝墙集市",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    plan.conflicts.append("用户A要红墙，用户B要蓝墙")
    proposal = coordinator.propose_conflict_resolution(
        plan.plan_id,
        proposed_by="gm",
        recommendation="建议折中为红蓝灯光分区。",
    )
    proposal_id = proposal.proposal_id
    event_start = len(coordinator.events)
    engine = FakeEngine([
        {
            **_trigger(f"@GM 拒绝 {proposal_id}", "GM"),
            "message_kind": "confirmation",
            "correlation_id": proposal_id,
            "metadata": {
                "decision": "reject",
                "proposal_id": proposal_id,
            },
        },
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    assert worker.process_once() is True

    blocked_plan_confirm = coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    assert not executor.payloads
    assert not engine.intents
    assert engine.replies and f"已拒绝冲突决议候选 {proposal_id}" in engine.replies[-1][2]
    assert proposal.status == "rejected"
    assert blocked_plan_confirm.ok is False
    assert blocked_plan_confirm.payload["requires_conflict_resolution"] is True
    new_events = coordinator.events[event_start:]
    assert any(item.event_type == "conflict_resolution_rejected" for item in new_events)
    print("[OK] worker acknowledges conflict resolution rejection without host execution")


def test_worker_rejects_coordinator_confirmation_without_sender_identity():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        sender_name="用户A",
        text="我想要红墙集市",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        sender_name="用户B",
        text="这里有冲突：我想要蓝墙集市",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    plan.conflicts.append("用户A要红墙，用户B要蓝墙")
    proposal = coordinator.propose_conflict_resolution(
        plan.plan_id,
        proposed_by="gm",
        recommendation="建议折中为红蓝灯光分区。",
    )
    proposal_id = proposal.proposal_id
    trigger = {
        **_trigger(f"@GM 确认 {proposal_id}", "GM"),
        "message_kind": "confirmation",
        "correlation_id": proposal_id,
        "metadata": {
            "decision": "confirm",
            "proposal_id": proposal_id,
        },
    }
    trigger.pop("sender_id", None)
    trigger.pop("source_user_id", None)
    event_start = len(coordinator.events)
    engine = FakeEngine([trigger])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    assert worker.process_once() is True

    assert not executor.payloads
    assert not engine.intents
    assert engine.replies and "缺少房主确认身份" in engine.replies[-1][2]
    assert proposal.status == "proposed"
    assert proposal.confirmed_by == ""
    assert not any(item.event_type == "conflict_resolution_confirmed" for item in coordinator.events[event_start:])
    assert plan.review_policy.get("conflict_resolutions") in (None, [])
    print("[OK] worker rejects coordinator confirmation without sender identity")


def test_worker_rejects_coordinator_confirmation_from_non_host_role():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        sender_name="用户A",
        text="我想要红墙集市",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        sender_name="用户B",
        text="这里有冲突：我想要蓝墙集市",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    plan.conflicts.append("用户A要红墙，用户B要蓝墙")
    proposal = coordinator.propose_conflict_resolution(
        plan.plan_id,
        proposed_by="gm",
        recommendation="建议折中为红蓝灯光分区。",
    )
    proposal_id = proposal.proposal_id
    event_start = len(coordinator.events)
    engine = FakeEngine([
        {
            **_trigger(f"@GM 确认 {proposal_id}", "GM"),
            "sender_id": "u2",
            "sender_name": "用户B",
            "sender_role": "participant",
            "message_kind": "confirmation",
            "correlation_id": proposal_id,
            "metadata": {
                "decision": "confirm",
                "proposal_id": proposal_id,
            },
        },
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    assert worker.process_once() is True

    assert not executor.payloads
    assert not engine.intents
    assert engine.replies and "只有房主可以处理" in engine.replies[-1][2]
    assert proposal.status == "proposed"
    assert proposal.confirmed_by == ""
    assert not any(item.event_type == "conflict_resolution_confirmed" for item in coordinator.events[event_start:])
    assert plan.review_policy.get("conflict_resolutions") in (None, [])
    print("[OK] worker rejects coordinator confirmation from non-host role")


def test_worker_rejects_coordinator_confirmation_from_metadata_non_host_role():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        sender_name="用户A",
        text="我想要红墙集市",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        sender_name="用户B",
        text="这里有冲突：我想要蓝墙集市",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    plan.conflicts.append("用户A要红墙，用户B要蓝墙")
    proposal = coordinator.propose_conflict_resolution(
        plan.plan_id,
        proposed_by="gm",
        recommendation="建议折中为红蓝灯光分区。",
    )
    proposal_id = proposal.proposal_id
    event_start = len(coordinator.events)
    engine = FakeEngine([
        {
            **_trigger(f"@GM 确认 {proposal_id}", "GM"),
            "message_kind": "confirmation",
            "correlation_id": proposal_id,
            "metadata": {
                "decision": "confirm",
                "proposal_id": proposal_id,
                "sender_role": "participant",
                "is_host": False,
            },
        },
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    assert worker.process_once() is True

    assert not executor.payloads
    assert not engine.intents
    assert engine.replies and "只有房主可以处理" in engine.replies[-1][2]
    assert proposal.status == "proposed"
    assert proposal.confirmed_by == ""
    assert not any(item.event_type == "conflict_resolution_confirmed" for item in coordinator.events[event_start:])
    assert plan.review_policy.get("conflict_resolutions") in (None, [])
    print("[OK] worker rejects coordinator confirmation from metadata non-host role")


def test_worker_wraps_confirmed_generation_as_seed_plan_payload():
    executor = FakeHostActionExecutor()
    coordinator = InteractionCoordinator()
    engine = FakeEngine([
        _trigger("@GM 开始生成暗黑集市场景", "GM"),
        _trigger("确认", "GM"),
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        host_action_executor=executor,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    assert worker.process_once() is True
    assert worker.process_once() is True

    assert executor.payloads, "confirmed generation should enter host queue"
    payload = executor.payloads[-1]
    assert payload["action_type"] == "start_generation"
    assert payload["execution"] == "coordinator_structured"
    assert payload["plan_id"]
    assert payload["seed_plan"]["plan_id"] == payload["plan_id"]
    assert payload["seed_plan"]["status"] == "confirmed"
    assert coordinator.get_plan(payload["plan_id"]) is not None
    print("[OK] worker wraps confirmed generation as structured SeedPlan payload")


def test_worker_default_host_executor_uses_coordinator_handler_for_seed_plan():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    engine = FakeEngine([])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=None,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="形成方案：室外暗黑集市，准备分批生成",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    confirmed = coordinator.confirm_seed_plan(plan.plan_id, "host-a")

    worker._execute_confirmed_action(confirmed.payload)  # noqa: SLF001 - direct unit coverage for default executor wiring

    assert scheduler.submitted
    assert scheduler.submitted[-1]["plan_id"] == plan.plan_id
    assert engine.system_messages
    assert any("SeedPlan" in item[2] for item in engine.system_messages)
    disclosure_messages = [
        item for item in engine.system_messages
        if len(item) >= 6 and item[3] == "action_status"
    ]
    assert disclosure_messages
    metadata = json.loads(disclosure_messages[-1][5])
    disclosure = metadata["disclosure"]
    assert disclosure["audience"] == "participant"
    assert disclosure["stage"] == "排队中"
    assert "等待资源" in disclosure["public_message"]
    assert "生成中 0%" not in disclosure["public_message"]
    assert "prompt" not in json.dumps(disclosure, ensure_ascii=False)
    print("[OK] default worker host executor uses Coordinator structured handler")


def test_worker_host_confirmation_disclosure_broadcast_is_sanitized_without_targeted_api():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator._disclosure_events.append(  # noqa: SLF001 - direct worker emission boundary coverage
        DisclosureEvent(
            event_id="disc-host-confirm-1",
            room_id="room-a",
            audience="host",
            stage="冲突仲裁",
            progress=20,
            public_message="GM 建议采用折中方案，等待房主确认。",
            available_actions=["confirm_conflict_resolution", "request_clarification"],
            requires_confirmation=True,
            metadata={
                "apply_policy": "host_confirmation",
                "proposal_id": "conflict-proposal-1",
                "requires_conflict_resolution": True,
            },
        )
    )
    engine = FakeEngine([])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=None,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    worker._emit_new_disclosure_events(coordinator, 0)  # noqa: SLF001 - direct unit coverage

    assert engine.system_messages
    message = engine.system_messages[-1]
    assert message[3] == "action_status"
    assert message[2] == "有一项需要房主确认的事项。"
    assert "GM 建议采用折中方案" not in message[2]
    metadata = json.loads(message[5])
    disclosure = metadata["disclosure"]
    assert disclosure["audience"] == "participant"
    assert disclosure["requires_confirmation"] is False
    assert disclosure["public_message"] == "有一项需要房主确认的事项。"
    assert disclosure["metadata"]["proposal_id"] == "conflict-proposal-1"
    assert "GM 建议采用折中方案" not in json.dumps(disclosure, ensure_ascii=False)
    assert "hidden_debug_ref" not in disclosure
    assert "prompt" not in json.dumps(disclosure, ensure_ascii=False)
    host_disclosure = metadata["host_disclosure"]
    assert host_disclosure["audience"] == "host"
    assert host_disclosure["requires_confirmation"] is True
    assert host_disclosure["requires_conflict_resolution"] is True
    assert host_disclosure["public_message"] == "GM 建议采用折中方案，等待房主确认。"
    assert host_disclosure["proposal_id"] == "conflict-proposal-1"
    assert host_disclosure["available_actions"] == ["confirm_conflict_resolution", "request_clarification"]
    assert host_disclosure["metadata"]["proposal_id"] == "conflict-proposal-1"
    assert "prompt" not in json.dumps(host_disclosure, ensure_ascii=False)
    assert "hidden_debug_ref" not in json.dumps(host_disclosure, ensure_ascii=False)
    print("[OK] worker host confirmation disclosure broadcast is sanitized without targeted API")


def test_worker_disclosure_emit_survives_coordinator_history_prune():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    for index in range(2051):
        coordinator._record_disclosures(  # noqa: SLF001 - direct watcher cursor coverage
            room_id="room-a",
            stage="batch",
            progress=index % 100,
            plan={"plan_id": "plan-a", "room_id": "room-a"},
            intervention={"intent_type": "batch_boundary", "status_message": f"batch-{index}"},
        )
    engine = FakeEngine([])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=None,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    emitted = worker._emit_new_disclosure_events(coordinator, 0)  # noqa: SLF001 - direct unit coverage

    assert emitted >= 2051
    assert engine.system_messages
    emitted_disclosures = [
        json.loads(item[5])["disclosure"]
        for item in engine.system_messages
        if len(item) >= 6 and item[3] == "action_status"
    ]
    assert any(
        item.get("metadata", {}).get("intervention", {}).get("status_message") == "batch-2050"
        for item in emitted_disclosures
    )
    print("[OK] worker disclosure emission survives Coordinator history pruning")


def test_worker_host_confirmation_disclosure_uses_targeted_api_when_available():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator._disclosure_events.append(  # noqa: SLF001 - direct worker emission boundary coverage
        DisclosureEvent(
            event_id="disc-host-confirm-2",
            room_id="room-a",
            audience="host",
            stage="冲突仲裁",
            progress=20,
            public_message="GM 建议采用折中方案，等待房主确认。",
            available_actions=["confirm_conflict_resolution", "request_clarification"],
            requires_confirmation=True,
            metadata={
                "apply_policy": "host_confirmation",
                "proposal_id": "conflict-proposal-2",
                "requires_conflict_resolution": True,
            },
        )
    )
    engine = TargetedHostFakeEngine([])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=None,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    worker._emit_new_disclosure_events(coordinator, 0)  # noqa: SLF001 - direct unit coverage

    assert not engine.system_messages
    assert engine.targeted_host_messages
    message = engine.targeted_host_messages[-1]
    assert message[3] == "action_status"
    assert message[2] == "有一项需要房主确认的事项。"
    metadata = json.loads(message[5])
    disclosure = metadata["disclosure"]
    assert disclosure["audience"] == "host"
    assert disclosure["requires_confirmation"] is True
    assert disclosure["public_message"] == "GM 建议采用折中方案，等待房主确认。"
    assert disclosure["metadata"]["proposal_id"] == "conflict-proposal-2"
    print("[OK] worker host confirmation disclosure uses targeted API when available")


def test_worker_default_coordinator_submits_confirmed_generation_to_scheduler():
    scheduler = FakeScheduler()
    engine = FakeEngine([
        _trigger("@GM 开始生成暗黑集市场景", "GM"),
        _trigger("确认", "GM"),
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )
    assert worker.process_once() is True
    assert worker.process_once() is True

    assert scheduler.submitted
    submitted = scheduler.submitted[-1]
    assert submitted["job_type"] == "scene_generation"
    assert submitted["plan_id"]
    assert submitted["seed_plan"]["status"] == "executing"
    assert any("SeedPlan" in str(item[2]) for item in engine.system_messages)
    print("[OK] worker default Coordinator submits confirmed generation to scheduler")


def test_worker_exposes_generation_scheduler_snapshot_without_payload_leak():
    scheduler = GenerationScheduler(queue_limit=2)
    worker_without_scheduler = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=None,
        async_agent_execution=False,
    )
    try:
        unavailable = worker_without_scheduler.generation_scheduler_snapshot()
        submitted = scheduler.submit({
            "plan_id": "seed-observe",
            "session_id": "room-a",
            "prompt": "private prompt should not appear in worker diagnostics",
        })
        other = scheduler.submit({
            "plan_id": "seed-other",
            "session_id": "room-b",
            "batch_id": "other-batch",
            "prompt": "other room private prompt",
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
        other_final = scheduler.wait(other["job_id"], timeout=2.0)
        worker = LANChatAgentWorker(
            corona_engine=FakeEngine([]),
            agent_factory=None,
            generation_scheduler=scheduler,
            async_agent_execution=False,
        )
        snapshot = worker.generation_scheduler_snapshot()
        session_snapshot = worker.generation_scheduler_session_snapshot("room-a")
    finally:
        worker_without_scheduler.stop()
        scheduler.shutdown()

    assert unavailable["available"] is False
    assert final["status"] == "done"
    assert other_final["status"] == "done"
    assert snapshot["available"] is True
    assert snapshot["total_jobs"] >= 1
    assert session_snapshot["available"] is True
    assert session_snapshot["total_jobs"] == 1
    assert "session_id" not in session_snapshot
    assert "job_id" not in str(session_snapshot)
    assert "seed-observe" not in str(session_snapshot)
    assert "other-batch" not in str(session_snapshot)
    assert "private prompt" not in str(snapshot)
    assert "private prompt" not in str(session_snapshot)
    assert "other room private prompt" not in str(session_snapshot)
    print("[OK] worker exposes generation scheduler snapshot without payload leak")


def test_worker_cancels_generation_session_without_touching_other_rooms():
    started = threading.Event()
    release = threading.Event()

    def submit(job):
        if job.session_id == "room-b":
            started.set()
            assert release.wait(timeout=1.0)

    scheduler = GenerationScheduler(stage_handlers={"submit": submit}, stage_order=("submit",))
    worker_without_scheduler = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=None,
        async_agent_execution=False,
    )
    try:
        unavailable = worker_without_scheduler.cancel_generation_session("room-a")
        worker = LANChatAgentWorker(
            corona_engine=FakeEngine([]),
            agent_factory=None,
            generation_scheduler=scheduler,
            async_agent_execution=False,
        )
        other = scheduler.submit({"plan_id": "seed-other", "session_id": "room-b"})
        assert started.wait(timeout=1.0)
        queued = scheduler.submit({"plan_id": "seed-room-a", "session_id": "room-a"})
        cancelled = worker.cancel_generation_session("room-a")
        final_cancelled = scheduler.wait(queued["job_id"], timeout=1.0)
        other_mid = scheduler.status(other["job_id"])
        release.set()
        other_final = scheduler.wait(other["job_id"], timeout=2.0)
    finally:
        release.set()
        worker_without_scheduler.stop()
        scheduler.shutdown()

    assert unavailable["available"] is False
    assert cancelled["available"] is True
    assert cancelled["success"] is True
    assert cancelled["job_ids"] == [queued["job_id"]]
    assert final_cancelled["status"] == "cancelled"
    assert other_mid["status"] == "submitting"
    assert other_final["status"] == "done"
    print("[OK] worker cancels generation session without touching other rooms")


def test_worker_room_closed_event_cancels_known_generation_session():
    scheduler = GenerationScheduler(auto_start=True)
    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=None,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )
    try:
        scheduler.pause_session("room-a")
        submitted = scheduler.submit({"plan_id": "seed-room-close", "session_id": "room-a"})
        time.sleep(0.05)
        worker.sync_chat_message_to_coordinator({
            "message_id": "room-a-msg-1",
            "room_id": "room-a",
            "sender_id": "user-a",
            "sender_name": "Alice",
            "sender_type": "user",
            "message_kind": "chat",
            "text": "先生成一个室外集市",
        }, emit_disclosure=False)
        handled = worker.handle_lanchat_room_event({"event": "room_closed"})
        final = scheduler.wait(submitted["job_id"], timeout=1.0)
        snapshot = scheduler.snapshot()
    finally:
        scheduler.shutdown()

    assert handled["handled"] is True
    assert handled["cancelled"][0]["available"] is True
    assert handled["cancelled"][0]["job_ids"] == [submitted["job_id"]]
    assert final["status"] == "abandoned"
    assert "room-a" not in snapshot["paused_sessions"]
    print("[OK] worker room_closed event cancels known generation session")


def test_worker_active_room_registry_is_bounded_for_close_fallback():
    scheduler = GenerationScheduler(auto_start=True)
    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=None,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )
    try:
        for index in range(MAX_ACTIVE_ROOM_IDS + 5):
            worker._remember_room_id(f"room-{index}")
        assert len(worker._active_room_ids) == MAX_ACTIVE_ROOM_IDS
        assert "room-0" not in worker._active_room_ids
        assert f"room-{MAX_ACTIVE_ROOM_IDS + 4}" in worker._active_room_ids

        latest_room = f"room-{MAX_ACTIVE_ROOM_IDS + 4}"
        scheduler.pause_session(latest_room)
        submitted = scheduler.submit({
            "plan_id": "seed-latest-room-close",
            "session_id": latest_room,
        })
        time.sleep(0.05)
        handled = worker.handle_lanchat_room_event({"event": "room_closed"})
        final = scheduler.wait(submitted["job_id"], timeout=1.0)
    finally:
        scheduler.shutdown()

    assert handled["handled"] is True
    assert any(
        result.get("available") is True and submitted["job_id"] in result.get("job_ids", [])
        for result in handled["cancelled"]
    )
    assert final["status"] == "abandoned"
    assert not worker._active_room_ids
    assert not worker._active_room_order
    print("[OK] worker active room registry is bounded for close fallback")


def test_worker_polls_native_room_closed_event_for_generation_cancel():
    scheduler = GenerationScheduler(auto_start=True)
    engine = FakeEngine(
        [],
        room_events=[{"event": "room_closed", "room_id": "room-a"}],
    )
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=None,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )
    try:
        scheduler.pause_session("room-a")
        submitted = scheduler.submit({"plan_id": "seed-native-room-close", "session_id": "room-a"})
        time.sleep(0.05)
        processed = worker.process_once()
        final = scheduler.wait(submitted["job_id"], timeout=1.0)
    finally:
        scheduler.shutdown()

    assert processed is True
    assert final["status"] == "abandoned"
    assert not engine.room_events
    print("[OK] worker polls native room_closed event and cancels generation session")


def test_worker_ignores_non_closing_lanchat_events_for_generation_cancel():
    scheduler = GenerationScheduler(auto_start=True)
    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=None,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )
    try:
        scheduler.pause_session("room-a")
        submitted = scheduler.submit({"plan_id": "seed-member-update", "session_id": "room-a"})
        time.sleep(0.05)
        handled = worker.handle_lanchat_room_event({"event": "member_update", "room_id": "room-a"})
        status = scheduler.status(submitted["job_id"])
    finally:
        scheduler.shutdown()

    assert handled["handled"] is False
    assert status["status"] == "paused"
    print("[OK] worker ignores non-closing LANChat events for generation cancel")


def test_worker_emits_safe_generation_scheduler_disclosure():
    scheduler = GenerationScheduler(queue_limit=2)
    engine = FakeEngine([])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=None,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )
    try:
        scheduler.pause_session("room-a")
        scheduler.pause_session("room-b")
        scheduler.submit({
            "plan_id": "seed-resource",
            "room_id": "room-a",
            "session_id": "exec-seed-resource-1",
            "batch_id": "batch-resource",
            "prompt": "private scheduler disclosure prompt",
        })
        scheduler.submit({
            "plan_id": "seed-other-resource",
            "room_id": "room-b",
            "session_id": "exec-seed-other-resource-1",
            "batch_id": "batch-other-resource",
            "prompt": "other room private scheduler prompt",
        })
        time.sleep(0.05)
        worker.handle_lanchat_room_event({"event": "member_update", "room_id": "room-a"})
        worker._emit_generation_scheduler_disclosure()  # noqa: SLF001 - direct safe-disclosure boundary coverage
    finally:
        worker.stop()
        scheduler.shutdown()

    assert engine.system_messages
    message = engine.system_messages[-1]
    assert message[3] == "action_status"
    metadata = json.loads(message[5])
    disclosure = metadata["disclosure"]
    disclosure_text = json.dumps(disclosure, ensure_ascii=False)
    assert disclosure["stage"] == "资源调度"
    assert disclosure["room_id"] == "room-a"
    assert disclosure["audience"] == "participant"
    assert disclosure["metadata"]["queued_count"] == 1
    assert disclosure["metadata"]["paused_session_count"] == 1
    assert disclosure["metadata"]["total_jobs"] == 1
    assert disclosure["metadata"]["diagnosis"]["state"] == "paused"
    assert "paused_sessions" in disclosure["metadata"]["diagnosis"]["reasons"]
    assert "resume_or_cancel_paused_sessions" in disclosure["metadata"]["diagnosis"]["recommended_actions"]
    assert "pause_session" in disclosure["metadata"]["recent_event_types"]
    assert "exec-seed-resource-1" not in disclosure_text
    assert "exec-seed-other-resource-1" not in disclosure_text
    assert "batch-other-resource" not in disclosure_text
    assert "private scheduler disclosure prompt" not in disclosure_text
    assert "other room private scheduler prompt" not in disclosure_text
    assert "job_id" not in disclosure_text
    print("[OK] worker emits safe generation scheduler disclosure")


def test_worker_routes_gm_pace_control_through_coordinator():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="形成方案：室外暗黑集市，分批生成",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    def _should_not_run_agent():
        raise AssertionError("GM pace control should be handled by Coordinator before model agent")

    trigger = {
        **_trigger("@GM 暂停一下，先等用户补充", "GM"),
        "room_id": "room-a",
        "agent_id": "gm",
        "agent_name": "GM",
        "sender_id": "host-a",
        "sender_name": "房主",
    }
    engine = FakeEngine([trigger])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_should_not_run_agent,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    assert worker.process_once() is True

    assert scheduler.paused_sessions == ["room-a"]
    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.PAUSED
    assert engine.replies
    assert "已暂停后续生成" in engine.replies[-1][2]
    disclosure_messages = [
        item for item in engine.system_messages
        if len(item) >= 6
        and item[3] == "action_status"
        and json.loads(item[5]).get("disclosure", {}).get("metadata", {}).get("intervention", {}).get("intent_type") == "pace_control"
    ]
    assert disclosure_messages
    disclosure = json.loads(disclosure_messages[-1][5])["disclosure"]
    assert disclosure["stage"] == "已暂停"
    assert disclosure["metadata"]["intervention"]["intent_type"] == "pace_control"
    assert "prompt" not in json.dumps(disclosure, ensure_ascii=False)
    print("[OK] worker routes GM pace control through Coordinator without model agent")


def test_worker_rejects_gm_pace_control_from_non_host_role():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="形成方案：室外暗黑集市，分批生成",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    def _should_not_run_agent():
        raise AssertionError("unauthorized GM pace control should be rejected before model agent")

    trigger = {
        **_trigger("@GM 暂停一下", "GM"),
        "room_id": "room-a",
        "agent_id": "gm",
        "agent_name": "GM",
        "sender_id": "u2",
        "sender_name": "用户B",
        "metadata": {
            "sender_role": "participant",
            "is_host": False,
        },
    }
    engine = FakeEngine([trigger])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_should_not_run_agent,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    assert worker.process_once() is True

    assert scheduler.paused_sessions == []
    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.EXECUTING
    assert engine.replies and "只有房主可以控制生成节奏" in engine.replies[-1][2]
    assert not any(
        item.event_type == "gm_pace_control"
        for item in coordinator.events
    )
    print("[OK] worker rejects GM pace control from non-host role")


def test_worker_routes_gm_clarification_through_coordinator():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="user-a",
        sender_name="用户A",
        text="我们想做一个场景，但室内室外还不清楚。",
    ))
    plan = coordinator.active_plan_for_room("room-a")
    assert plan is not None

    def _should_not_run_agent():
        raise AssertionError("GM clarification should be handled by Coordinator before model agent")

    trigger = {
        **_trigger("@GM 澄清一下：请确认室内、室外还是混合场景", "GM"),
        "room_id": "room-a",
        "agent_id": "gm",
        "agent_name": "GM",
        "sender_id": "host-a",
        "sender_name": "房主",
    }
    engine = FakeEngine([trigger])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_should_not_run_agent,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    assert worker.process_once() is True

    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.CLARIFYING
    assert coordinator.get_plan(plan.plan_id).review_policy["clarification_requests"][0]["status"] == "pending"
    assert engine.replies
    assert "GM 已请求补充澄清" in engine.replies[-1][2]
    disclosure_messages = [
        item for item in engine.system_messages
        if len(item) >= 6
        and item[3] == "action_status"
        and json.loads(item[5]).get("disclosure", {}).get("metadata", {}).get("intervention", {}).get("intent_type") == "clarification"
    ]
    assert disclosure_messages
    disclosure = json.loads(disclosure_messages[-1][5])["disclosure"]
    assert disclosure["stage"] == "方案整理中"
    assert "室内、室外还是混合" in disclosure["public_message"]
    assert "prompt" not in json.dumps(disclosure, ensure_ascii=False)
    print("[OK] worker routes GM clarification through Coordinator without model agent")


def test_worker_rejects_gm_clarification_from_non_host_role():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="user-a",
        sender_name="用户A",
        text="我们想做一个场景，但室内室外还不清楚。",
    ))
    plan = coordinator.active_plan_for_room("room-a")
    assert plan is not None

    def _should_not_run_agent():
        raise AssertionError("unauthorized GM clarification should be rejected before model agent")

    trigger = {
        **_trigger("@GM 澄清一下：请确认室内、室外还是混合场景", "GM"),
        "room_id": "room-a",
        "agent_id": "gm",
        "agent_name": "GM",
        "sender_id": "u2",
        "sender_name": "用户B",
        "sender_role": "participant",
    }
    engine = FakeEngine([trigger])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_should_not_run_agent,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    assert worker.process_once() is True

    assert coordinator.get_plan(plan.plan_id).status != SeedPlanStatus.CLARIFYING
    assert coordinator.get_plan(plan.plan_id).review_policy.get("clarification_requests") in (None, [])
    assert engine.replies and "只有房主可以发起强制澄清" in engine.replies[-1][2]
    assert not any(
        item.event_type == "clarification_requested"
        for item in coordinator.events
    )
    print("[OK] worker rejects GM clarification from non-host role")


def test_worker_syncs_plain_chat_history_to_coordinator_without_generation():
    from plugins.AITool.cai_extensions.agent import agent_adapter

    old_classify = agent_adapter.classify_intent
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    trigger = _trigger("@小B 你觉得这些想法怎么整理？", "小B")
    trigger["history"] = [
        {
            "message_id": "plain-1",
            "room_id": "r1",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "我们想做一个室外暗黑集市，摊位不要太密。",
        },
        {
            "message_id": "plain-2",
            "room_id": "r1",
            "sender_id": "user-b",
            "sender_name": "用户B",
            "sender_type": "user",
            "message_kind": "chat",
            "text": "入口处要留出路，后面可以加一座雕像。",
        },
        {
            "message_id": "agent-old",
            "room_id": "r1",
            "sender_id": "agent-b",
            "sender_name": "小B",
            "sender_type": "agent",
            "message_kind": "agent_reply",
            "text": "我来整理。",
        },
        {
            "message_id": trigger["message_id"],
            "room_id": "r1",
            "sender_id": "user-a",
            "sender_name": "用户A",
            "sender_type": "user",
            "message_kind": "chat",
            "text": trigger["text"],
        },
    ]
    engine = FakeEngine([trigger])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    agent_adapter.classify_intent = lambda text: "compose"
    try:
        assert worker.process_once() is True
    finally:
        agent_adapter.classify_intent = old_classify

    plan = coordinator.active_plan_for_room("r1")
    assert plan is not None
    summary = coordinator.memory_summary(room_id="r1", plan_id=plan.plan_id)
    assert "暗黑集市" in summary["summary_text"]
    assert "雕像" in summary["summary_text"]
    assert "@小B 你觉得" not in summary["summary_text"]
    assert not scheduler.submitted
    assert engine.replies
    assert not any(item[3] == "action_status" for item in engine.system_messages)
    print("[OK] worker syncs plain chat history into Coordinator without triggering generation")


def test_worker_syncs_only_scene_write_chat_to_coordinator_draft():
    from plugins.AITool.cai_extensions.agent import agent_adapter

    old_classify = agent_adapter.classify_intent
    coordinator = InteractionCoordinator()
    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    def classify(text: str) -> str:
        return "compose" if "设计" in str(text or "") else "chat"

    agent_adapter.classify_intent = classify
    try:
        assert worker.sync_chat_message_to_coordinator({
            "message_id": "scene-filter-chat-1",
            "room_id": "r-scene-filter",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "@小女孩 我想看你在大草原上跳舞",
        }) is False
        assert worker.sync_chat_message_to_coordinator({
            "message_id": "scene-filter-chat-2",
            "room_id": "r-scene-filter",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "@‘",
        }) is False
        assert coordinator.active_plan_for_room("r-scene-filter") is None
        assert worker.sync_chat_message_to_coordinator({
            "message_id": "scene-filter-compose-1",
            "room_id": "r-scene-filter",
            "sender_id": "host-a",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "text": "帮我设计一个明亮可爱的卧室",
        }) is True
    finally:
        agent_adapter.classify_intent = old_classify

    plan = coordinator.active_plan_for_room("r-scene-filter")
    assert plan is not None
    assert len(plan.participants) == 1
    assert plan.participants[0].text == "帮我设计一个明亮可爱的卧室"
    print("[OK] worker syncs only scene-write chat into Coordinator draft")


def test_worker_syncs_direct_plain_chat_as_runtime_intervention():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="r-direct",
        sender_id="host-a",
        sender_name="房主",
        text="我们要做一个室外暗黑集市，入口留出主路。",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("r-direct")
    confirm = coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    assert confirm.ok
    coordinator.execute_confirmed_plan(plan.plan_id)

    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    message = {
        "message_id": "direct-plain-1",
        "room_id": "r-direct",
        "sender_id": "user-b",
        "sender_name": "用户B",
        "sender_type": "user",
        "message_kind": "chat",
        "text": "第一批后帮我补一座天使雕塑，放在入口附近。",
    }
    assert worker.sync_chat_message_to_coordinator(message) is True
    assert worker.sync_chat_message_to_coordinator(message) is False
    assert worker.sync_chat_message_to_coordinator({
        "message_id": "agent-ignore",
        "room_id": "r-direct",
        "sender_id": "agent-b",
        "sender_type": "agent",
        "message_kind": "agent_reply",
        "text": "我来整理。",
    }) is False

    pending = coordinator.pending_interventions(plan.plan_id)
    assert len(pending) == 1
    assert pending[0].source_user_id == "user-b"
    assert pending[0].intent_type == "add"
    assert pending[0].apply_policy in {"next_batch", "final_adjustment"}
    assert "天使雕塑" in pending[0].target_hint
    summary = coordinator.memory_summary(room_id="r-direct", plan_id=plan.plan_id)
    assert "天使雕塑" in summary["summary_text"]
    disclosure_messages = [
        item for item in worker._corona_engine.system_messages  # noqa: SLF001 - direct bridge disclosure coverage
        if len(item) >= 6 and item[3] == "action_status"
    ]
    assert disclosure_messages
    metadata = json.loads(disclosure_messages[-1][5])
    disclosure = metadata["disclosure"]
    assert disclosure["audience"] == "participant"
    assert disclosure["stage"] == "可介入窗口"
    assert "天使雕塑" in disclosure["public_message"]
    assert "天使雕塑" in disclosure["metadata"]["intervention"]["target_hint"]
    assert "prompt" not in json.dumps(disclosure, ensure_ascii=False)
    print("[OK] worker syncs direct plain chat into runtime intervention without agent trigger")


def test_worker_syncs_plain_chat_without_message_id_once():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="r-direct-no-id",
        sender_id="host-a",
        sender_name="房主",
        text="我们要做一个室外暗黑集市，入口留出主路。",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("r-direct-no-id")
    assert coordinator.confirm_seed_plan(plan.plan_id, "host-a").ok
    coordinator.execute_confirmed_plan(plan.plan_id)

    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )
    message = {
        "room_id": "r-direct-no-id",
        "sender_id": "user-b",
        "sender_name": "用户B",
        "sender_type": "user",
        "message_kind": "chat",
        "text": "第一批后帮我补一座天使雕塑，放在入口附近。",
    }

    assert worker.sync_chat_message_to_coordinator(message) is True
    disclosure_count_after_first_sync = len(worker._corona_engine.system_messages)  # noqa: SLF001
    assert worker.sync_chat_message_to_coordinator(dict(message)) is False
    assert len(worker._corona_engine.system_messages) == disclosure_count_after_first_sync  # noqa: SLF001

    pending = coordinator.pending_interventions(plan.plan_id)
    assert len(pending) == 1
    assert pending[0].source_user_id == "user-b"
    assert pending[0].intent_type == "add"
    assert "天使雕塑" in pending[0].target_hint
    disclosure_messages = [
        item for item in worker._corona_engine.system_messages  # noqa: SLF001 - direct bridge disclosure coverage
        if len(item) >= 6 and item[3] == "action_status"
    ]
    assert disclosure_messages
    intervention_disclosures = [
        json.loads(item[5])["disclosure"]
        for item in disclosure_messages
        if "intervention" in json.loads(item[5])["disclosure"].get("metadata", {})
    ]
    assert intervention_disclosures
    assert "天使雕塑" in intervention_disclosures[-1]["metadata"]["intervention"]["target_hint"]
    print("[OK] worker syncs no-message-id plain chat once with fallback dedupe")


def test_worker_plain_chat_dedupe_cache_is_bounded():
    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=_agent_factory,
        interaction_coordinator=InteractionCoordinator(scheduler=FakeScheduler()),
        generation_scheduler=FakeScheduler(),
        async_agent_execution=False,
    )

    for index in range(MAX_COORDINATOR_SEEN_MESSAGE_IDS + 3):
        worker._remember_coordinator_seen_message_id(f"msg-{index}")  # noqa: SLF001

    assert len(worker._coordinator_seen_message_ids) == MAX_COORDINATOR_SEEN_MESSAGE_IDS  # noqa: SLF001
    assert len(worker._coordinator_seen_message_order) == MAX_COORDINATOR_SEEN_MESSAGE_IDS  # noqa: SLF001
    assert "msg-0" not in worker._coordinator_seen_message_ids  # noqa: SLF001
    assert f"msg-{MAX_COORDINATOR_SEEN_MESSAGE_IDS + 2}" in worker._coordinator_seen_message_ids  # noqa: SLF001
    worker._remember_coordinator_seen_message_id(f"msg-{MAX_COORDINATOR_SEEN_MESSAGE_IDS + 2}")  # noqa: SLF001
    assert len(worker._coordinator_seen_message_ids) == MAX_COORDINATOR_SEEN_MESSAGE_IDS  # noqa: SLF001
    print("[OK] worker plain chat Coordinator dedupe cache is bounded")


def test_worker_targeted_agent_message_replies_once_across_sync_and_trigger():
    metadata = {
        "draft_action": "chat",
        "target_scope": "agent",
        "target_agent_id": "agent-b",
        "target_agent_name": "小B",
    }
    message = {
        "message_id": "targeted-agent-1",
        "room_id": "r-targeted",
        "sender_id": "user-a",
        "sender_name": "用户A",
        "sender_type": "user",
        "message_kind": "chat",
        "target_agent_id": "agent-b",
        "correlation_id": "corr-targeted-1",
        "text": "@小B 你好",
        "metadata_json": json.dumps(metadata, ensure_ascii=False),
    }
    trigger = {
        **message,
        "trigger_id": "targeted-agent-1:agent-b",
        "agent_id": "agent-b",
        "agent_name": "小B",
        "persona": "山贼",
        "history": [message],
    }
    engine = FakeEngine(
        [dict(trigger), dict(trigger)],
        coordinator_messages=[dict(message)],
    )
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=InteractionCoordinator(scheduler=FakeScheduler()),
        generation_scheduler=FakeScheduler(),
        async_agent_execution=False,
    )

    assert worker.process_once() is True
    assert len(engine.replies) == 1
    assert engine.replies[0][0] == "agent-b"
    assert worker.process_once() is False
    assert len(engine.replies) == 1
    print("[OK] targeted agent chat replies once across Coordinator sync and native trigger")


def test_worker_syncs_plain_chat_metadata_actor_target_to_coordinator():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="r-actor-meta",
        sender_id="host-a",
        sender_name="房主",
        text="我们先做室外集市入口，后续允许用户调整已生成物体。",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("r-actor-meta")
    assert coordinator.confirm_seed_plan(plan.plan_id, "host-a").ok
    coordinator.execute_confirmed_plan(plan.plan_id)

    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    message = {
        "message_id": "direct-actor-meta-1",
        "room_id": "r-actor-meta",
        "sender_id": "user-b",
        "sender_name": "用户B",
        "sender_type": "user",
        "message_kind": "chat",
        "text": "把这个摊位往右挪一点，别挡住入口。",
        "metadata_json": json.dumps({
            "actor_id": "stall-actor-7",
            "actor_version": 4,
            "target_hint": "入口右侧摊位",
            "prompt": "hidden prompt must not be forwarded",
            "provider": "hidden provider",
        }, ensure_ascii=False),
    }

    assert worker.sync_chat_message_to_coordinator(message) is True
    pending = coordinator.pending_interventions(plan.plan_id)
    assert len(pending) == 1
    assert pending[0].actor_id == "stall-actor-7"
    assert pending[0].actor_version == 4
    assert pending[0].target_hint == "入口右侧摊位"
    assert pending[0].source_user_id == "user-b"
    assert "hidden" not in json.dumps(pending[0].as_dict(), ensure_ascii=False)
    print("[OK] worker preserves safe actor metadata for direct Coordinator interventions")


def test_worker_polls_native_plain_chat_queue_into_coordinator():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="r-native",
        sender_id="host-a",
        sender_name="房主",
        text="我们要做一个混合室内外展区，先生成入口和主路。",
        is_host=True,
    ))
    plan = coordinator.propose_seed_plan("r-native")
    assert coordinator.confirm_seed_plan(plan.plan_id, "host-a").ok
    coordinator.execute_confirmed_plan(plan.plan_id)
    submitted_before = len(scheduler.submitted)

    native_message = {
        "message_id": "native-plain-1",
        "room_id": "r-native",
        "sender_id": "user-c",
        "sender_name": "用户C",
        "sender_type": "user",
        "message_kind": "chat",
        "text": "第一批之后把入口右侧的摊位删掉，换成矮墙。",
    }
    engine = FakeEngine([], coordinator_messages=[native_message])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )

    assert worker.process_once() is True
    assert not engine.replies
    assert len(scheduler.submitted) == submitted_before
    pending = coordinator.pending_interventions(plan.plan_id)
    assert len(pending) == 1
    assert pending[0].source_user_id == "user-c"
    assert pending[0].intent_type in {"delete", "modify"}
    assert "入口右侧的摊位" in pending[0].content
    assert engine.system_messages
    print("[OK] worker polls native plain chat queue into Coordinator without agent trigger")


def test_worker_polls_native_host_chat_as_host_message():
    from plugins.AITool.cai_extensions.agent import agent_adapter

    old_classify = agent_adapter.classify_intent
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    native_message = {
        "message_id": "native-host-1",
        "room_id": "r-native-host",
        "sender_id": "host-a",
        "sender_name": "房主",
        "sender_type": "host",
        "message_kind": "chat",
        "text": "房主确认：先做室外暗黑集市入口，暂时不要生成室内部分。",
    }
    engine = FakeEngine([], coordinator_messages=[native_message])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    agent_adapter.classify_intent = lambda text: "compose"
    try:
        assert worker.process_once() is True
    finally:
        agent_adapter.classify_intent = old_classify
    plan = coordinator.active_plan_for_room("r-native-host")
    assert plan is not None
    assert plan.host_id == "host-a"
    assert "暗黑集市" in plan.intent_summary
    assert not engine.replies
    print("[OK] worker polls native host chat as host message into Coordinator")


def test_worker_does_not_starve_agent_trigger_behind_native_chat_queue():
    from plugins.AITool.cai_extensions.agent import agent_adapter

    old_classify = agent_adapter.classify_intent
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    native_messages = [
        {
            "message_id": f"native-burst-{index}",
            "room_id": "r-burst",
            "sender_id": f"user-{index}",
            "sender_name": f"用户{index}",
            "sender_type": "user",
            "message_kind": "chat",
            "text": f"第 {index} 条补充意见，先记录下来。",
        }
        for index in range(6)
    ]
    trigger = _trigger("@小B 请综合刚才的讨论给一句建议")
    trigger["room_id"] = "r-burst"
    engine = FakeEngine([trigger], coordinator_messages=native_messages)
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        interaction_coordinator=coordinator,
        async_agent_execution=False,
    )

    agent_adapter.classify_intent = lambda text: "compose"
    try:
        assert worker.process_once() is True
    finally:
        agent_adapter.classify_intent = old_classify
    assert engine.replies
    assert len(engine.coordinator_messages) == 2
    plan = coordinator.active_plan_for_room("r-burst")
    assert plan is not None
    assert "补充意见" in coordinator.memory_summary(
        room_id="r-burst",
        plan_id=plan.plan_id,
    )["summary_text"]
    print("[OK] worker does not starve agent trigger behind native chat queue")


def test_worker_installs_deferred_download_scheduler_hook():
    model_tools.set_deferred_download_scheduler(None)
    media_registry.set_media_task_scheduler(None)
    scheduler = FakeScheduler()
    worker = LANChatAgentWorker(
        corona_engine=FakeEngine([]),
        agent_factory=_agent_factory,
        generation_scheduler=scheduler,
        async_agent_execution=False,
    )
    try:
        assert model_tools.get_deferred_download_scheduler() is scheduler
        assert media_registry.get_media_task_scheduler() is scheduler
    finally:
        worker.stop()
    assert model_tools.get_deferred_download_scheduler() is None
    assert media_registry.get_media_task_scheduler() is None
    print("[OK] worker installs and clears generation scheduler hooks")


def test_deferred_provider_download_does_not_bypass_rejecting_scheduler():
    class RejectingScheduler:
        def submit(self, payload):
            raise AssertionError("control test should not call submit")

    rejected = model_tools._deferred_download_control(
        scheduler=RejectingScheduler(),
        scheduled={"job_id": "", "status": "waiting_user", "error": "generation queue is full"},
    )
    legacy = model_tools._deferred_download_control(scheduler=None, scheduled=None)
    scheduled = model_tools._deferred_download_control(
        scheduler=RejectingScheduler(),
        scheduled={"job_id": "gen-1", "status": "queued"},
    )

    assert rejected["mode"] == "rejected"
    assert rejected["start_legacy_thread"] is False
    assert rejected["status"] == "waiting_user"
    assert "queue is full" in rejected["error"]
    assert legacy["mode"] == "legacy_thread"
    assert legacy["start_legacy_thread"] is True
    assert scheduled["mode"] == "scheduled"
    assert scheduled["start_legacy_thread"] is False
    assert scheduled["job_id"] == "gen-1"
    print("[OK] deferred provider download respects scheduler backpressure instead of starting legacy thread")


def test_media_registry_submit_uses_generation_scheduler_hook():
    media_registry.reset_media_registry()
    media_registry.set_media_task_scheduler(None)

    class ImmediateScheduler:
        def __init__(self):
            self.submitted = []
            self.statuses = {}

        def submit(self, payload):
            self.submitted.append(dict(payload))
            job_id = str(payload["job_id"])
            handler = payload["_runtime_context"]["stage_handlers"]["submit"]
            result = handler(type("Job", (), {"payload": payload})())
            self.statuses[job_id] = {
                "job_id": job_id,
                "status": "done",
                "result": result,
            }
            return self.statuses[job_id]

        def wait(self, job_id, timeout=5.0):
            return self.statuses[job_id]

        def status(self, job_id):
            return self.statuses[job_id]

    scheduler = ImmediateScheduler()
    media_registry.set_media_task_scheduler(scheduler)
    try:
        registry = media_registry.get_media_registry()
        file_id = registry.submit(
            lambda: "https://example.invalid/generated.png",
            resource_type="image",
            session_id="room-a",
            content_text="dark market reference",
        )

        assert scheduler.submitted
        submitted = scheduler.submitted[-1]
        assert submitted["job_type"] == "media_resource_task"
        assert submitted["resource_type"] == "image"
        assert submitted["_runtime_context"]["stage_order"] == ("submit",)
        assert registry.resolve(file_id) == "https://example.invalid/generated.png"
        assert registry.get_status(file_id).value == "done"
    finally:
        media_registry.set_media_task_scheduler(None)
        media_registry.reset_media_registry()
    print("[OK] media registry submits async media task through GenerationScheduler hook")


def test_media_registry_does_not_bypass_rejecting_scheduler():
    media_registry.reset_media_registry()
    media_registry.set_media_task_scheduler(None)

    class RejectingScheduler:
        def __init__(self):
            self.submitted = []

        def submit(self, payload):
            self.submitted.append(dict(payload))
            return {
                "job_id": "",
                "status": "waiting_user",
                "error": "generation queue is full",
            }

    calls = {"task": 0}
    scheduler = RejectingScheduler()
    media_registry.set_media_task_scheduler(scheduler)
    try:
        registry = media_registry.get_media_registry()
        file_id = registry.submit(
            lambda: calls.__setitem__("task", calls["task"] + 1) or "https://example.invalid/generated.png",
            resource_type="image",
            session_id="room-a",
            content_text="overflow image",
        )

        assert scheduler.submitted
        assert calls["task"] == 0, "scheduler 背压拒绝后不能 fallback 到 TaskExecutor 执行任务"
        assert registry.get_status(file_id).value == "error"
        record = registry.get_by_file_id(file_id)
        assert record is not None
        assert "rejected" in record.error or "queue" in record.error
        try:
            registry.resolve(file_id, timeout=0.01)
        except RuntimeError as exc:
            assert "任务" in str(exc) and ("queue" in str(exc) or "rejected" in str(exc))
        else:
            raise AssertionError("rejected media task must not resolve successfully")
    finally:
        media_registry.set_media_task_scheduler(None)
        media_registry.reset_media_registry()
    print("[OK] media registry respects scheduler backpressure instead of falling back to TaskExecutor")


def test_worker_configured_composer_scheduler_runs_confirmed_seed_plan_context():
    compose_calls = []

    class FakeComposer:
        def compose(self, text, **kwargs):
            compose_calls.append((text, kwargs))
            progress_sink = kwargs.get("progress_sink")
            assert callable(progress_sink)
            progress_sink("生成进度 32% [███░░░░░░░] 准备所需模型。正在获取主体和物件资源，界面可能需要等待一会儿。")
            coordinator = kwargs["interaction_coordinator"]
            coordinator.bind_scene_session_progress(
                FakeProgressSession(),
                room_id=kwargs["room_id"],
                plan_id=kwargs["plan_id"],
                session_id=kwargs["session_id"],
            )
            return {"imported": ["market-stall"], "progressive": True}

    class FakeProgressSession:
        def set_progress_event_sink(self, sink):
            sink({
                "phase": "OBJECTS#1",
                "status": "done",
                "percent": 100,
                "batch_id": "r1_OBJECTS_b1",
                "user_message": "第一批已完成，可继续介入。",
            })

    engine = FakeEngine([
        _trigger("@GM 开始生成暗黑集市场景", "GM"),
        _trigger("确认", "GM"),
    ])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        composer_factory=lambda: FakeComposer(),
        async_agent_execution=False,
    )
    try:
        assert worker.process_once() is True
        assert worker.process_once() is True
        deadline = time.time() + 2.0
        while not compose_calls and time.time() < deadline:
            time.sleep(0.01)
        disclosure_deadline = time.time() + 2.0
        while time.time() < disclosure_deadline:
            if any(
                len(item) >= 6
                and item[3] == "action_status"
                and "第一批已完成" in item[2]
                for item in engine.system_messages
            ):
                break
            time.sleep(0.01)
    finally:
        worker.stop()

    assert compose_calls
    prompt, kwargs = compose_calls[-1]
    assert "暗黑集市" in prompt
    assert kwargs["room_id"]
    assert kwargs["plan_id"]
    assert kwargs["session_id"].startswith(f"exec-{kwargs['plan_id']}-")
    assert kwargs["session_id"] != kwargs["room_id"]
    coordinator = kwargs["interaction_coordinator"]
    event_types = [item.event_type for item in coordinator.events]
    assert "batch_intervention_window_open" in event_types
    assert "generation_completed" in event_types
    assert coordinator.active_plan_for_room(kwargs["room_id"]).status.value == "completed"
    assert "第一批已完成" in coordinator.memory_summary(room_id=kwargs["room_id"], plan_id=kwargs["plan_id"])["summary_text"]
    progress_disclosures = [
        item for item in engine.system_messages
        if len(item) >= 6 and item[3] == "action_status" and "第一批已完成" in item[2]
    ]
    assert progress_disclosures
    resource_progress_disclosures = [
        item for item in engine.system_messages
        if len(item) >= 6 and item[3] == "action_status" and "准备所需模型" in item[2]
    ]
    assert resource_progress_disclosures
    resource_metadata = json.loads(resource_progress_disclosures[-1][5])
    resource_disclosure = resource_metadata["disclosure"]
    assert resource_disclosure["stage"] == "资源准备"
    assert resource_disclosure["progress"] == 32
    assert resource_disclosure["audience"] == "participant"
    assert "prompt" not in json.dumps(resource_disclosure, ensure_ascii=False)
    assert "session_id" not in json.dumps(resource_disclosure, ensure_ascii=False)
    metadata = json.loads(progress_disclosures[-1][5])
    disclosure = metadata["disclosure"]
    assert disclosure["stage"] == "可介入窗口"
    assert disclosure["audience"] == "participant"
    assert disclosure["metadata"]["intervention"]["status_message"] == "第一批已完成，可继续介入。"
    assert "prompt" not in json.dumps(disclosure, ensure_ascii=False)
    print("[OK] configured worker scheduler runs composer with SeedPlan/Coordinator context")


def test_worker_does_not_execute_discussion_only_confirmed_payload():
    executor = FakeHostActionExecutor()
    engine = FakeEngine([])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        agent_factory=_agent_factory,
        host_action_executor=executor,
        async_agent_execution=False,
    )
    worker._broadcast_confirmed_action({  # noqa: SLF001 - direct unit coverage for dispatch gate
        "proposal_id": "gm-discuss",
        "status": "confirmed",
        "action_type": "discussion_only",
        "source_user_id": "user-a",
        "intent_text": "围绕暗黑集市主题讨论",
    })
    assert not executor.payloads
    assert not engine.intents
    print("[OK] discussion-only confirmed payload does not enter host executor")


def test_host_action_executor_runs_under_gate_and_reports_result():
    engine = FakeEngine([])
    gate = FakeGate()

    def agent_factory():
        def _agent(persona, messages):
            assert "host single-writer" in persona
            assert any("source_user_id=user-a" in item for item in messages)
            assert messages[-1] == "用户确认意图：添加篝火"
            assert "请在 host 机器上" not in messages[-1]
            return "scene delta applied"

        return _agent

    executor = LanChatHostActionExecutor(
        corona_engine=engine,
        agent_factory=agent_factory,
        engine_gate=gate,
    )
    result = executor.enqueue_and_process({
        "proposal_id": "gm-test",
        "status": "confirmed",
        "source_user_id": "user-a",
        "intent_text": "添加篝火",
    })
    assert gate.calls == 1
    assert result is not None and result.ok is True
    assert result.event_type == "SceneDelta"
    assert result.payload["source_user_id"] == "user-a"
    assert "peer actor sync" not in result.message
    assert [item[3] for item in engine.intents] == [
        "queued_host_action",
        "executing_host_action",
        "host_action_executed",
    ]
    assert engine.system_messages
    assert "scene delta applied" in engine.system_messages[-1][2]
    assert "host_single_writer" not in engine.system_messages[-1][2]
    assert "EngineWriteGate" not in engine.system_messages[-1][2]
    assert "peer actor sync" not in engine.system_messages[-1][2]
    assert engine.system_messages[-1][3] == "action_status"
    print("[OK] host action executor runs under EngineWriteGate and reports SceneDelta")


def test_host_action_executor_prefers_resolved_intent_text():
    captured = {}

    def agent_factory():
        def _agent(persona, messages):
            captured["messages"] = list(messages)
            assert "黑木拱门" in messages[-1]
            assert "暗色石板路" in messages[-1]
            assert "主柜台" in messages[-1]
            assert messages[-1].count("用户确认意图：") == 1
            return "scene delta applied"

        return _agent

    executor = LanChatHostActionExecutor(
        corona_engine=FakeEngine([]),
        agent_factory=agent_factory,
        engine_gate=FakeGate(),
    )
    result = executor.enqueue_and_process({
        "proposal_id": "gm-plan",
        "status": "confirmed",
        "source_user_id": "host-a",
        "source_agent_name": "商人",
        "resolved_from_plan_id": "plan-merchant-1",
        "intent_text": "@商人 按照这个方案进行场景建筑生成把",
        "resolved_intent_text": (
            "原始用户请求：@商人 按照这个方案进行场景建筑生成把\n"
            "用户确认执行 @商人 最近方案。请严格围绕下列方案生成开放场景：\n"
            "入口黑木拱门、石灯残碑、暗色石板路、两侧摊位、深处主柜台、暗红幽紫灯光。"
        ),
    })
    assert result is not None and result.ok is True
    assert captured["messages"][-1].startswith("用户确认意图：原始用户请求")
    print("[OK] host action executor prefers resolved plan intent over vague source text")


def test_host_action_executor_does_not_report_executed_for_empty_or_failed_result():
    engine = FakeEngine([])

    def empty_agent_factory():
        def _agent(persona, messages):
            return ""

        return _agent

    empty_executor = LanChatHostActionExecutor(
        corona_engine=engine,
        agent_factory=empty_agent_factory,
        engine_gate=None,
    )
    empty_result = empty_executor.enqueue_and_process({
        "proposal_id": "gm-empty",
        "status": "confirmed",
        "source_user_id": "user-a",
        "intent_text": "空执行",
    })
    assert empty_result is not None and empty_result.ok is False
    assert empty_result.event_type == "CommandRejected"

    def failed_agent_factory():
        def _agent(persona, messages):
            return "无法安全执行该操作"

        return _agent

    failed_executor = LanChatHostActionExecutor(
        corona_engine=engine,
        agent_factory=failed_agent_factory,
        engine_gate=None,
    )
    failed_result = failed_executor.enqueue_and_process({
        "proposal_id": "gm-failed",
        "status": "confirmed",
        "source_user_id": "user-a",
        "intent_text": "失败执行",
    })
    assert failed_result is not None and failed_result.ok is False
    assert failed_result.event_type == "CommandRejected"
    assert [item[3] for item in engine.intents].count("host_action_failed") == 2
    print("[OK] host action executor does not report complete execution for empty/failure replies")


def test_host_action_executor_reports_accepted_no_delta_when_no_executor_agent():
    engine = FakeEngine([])
    executor = LanChatHostActionExecutor(
        corona_engine=engine,
        agent_factory=None,
        engine_gate=None,
    )
    result = executor.enqueue_and_process({
        "proposal_id": "gm-no-agent",
        "status": "confirmed",
        "source_user_id": "user-a",
        "intent_text": "无执行器",
    })
    assert result is not None and result.ok is True
    assert result.event_type == "AcceptedNoDelta"
    assert "未修改场景" in result.message
    assert "typed actor delta" not in result.message
    assert [item[3] for item in engine.intents][-1] == "accepted_no_delta"
    print("[OK] host action executor separates accepted_no_delta from complete actor execution")


def test_host_action_executor_sanitizes_agent_api_error():
    engine = FakeEngine([])

    def broken_agent_factory():
        def _agent(persona, messages):
            raise RuntimeError("Error code: 401 - {'message': 'Invalid Token (request id: abc)'}")

        return _agent

    executor = LanChatHostActionExecutor(
        corona_engine=engine,
        agent_factory=broken_agent_factory,
        engine_gate=None,
    )
    result = executor.enqueue_and_process({
        "proposal_id": "gm-token",
        "status": "confirmed",
        "source_user_id": "user-a",
        "intent_text": "添加篝火",
    })
    assert result is not None and result.ok is False
    assert "Invalid Token" not in result.message
    assert "request id" not in result.message
    assert "当前模型服务不可用" in engine.system_messages[-1][2]
    assert "Invalid Token" not in engine.system_messages[-1][2]
    print("[OK] host executor provider errors are sanitized before chat status")


def test_host_action_executor_sanitizes_payload_text_in_status_and_result():
    engine = FakeEngine([])

    def agent_factory():
        def _agent(persona, messages):
            return "scene delta applied"

        return _agent

    executor = LanChatHostActionExecutor(
        corona_engine=engine,
        agent_factory=agent_factory,
        engine_gate=None,
    )
    result = executor.enqueue_and_process({
        "proposal_id": "gm-dirty",
        "status": "confirmed",
        "source_user_id": "user-a",
        "intent_text": (
            "添加篝火 prompt=PRIVATE_HOST_PROMPT_SHOULD_NOT_LEAK "
            "provider=host-provider-secret runtime_context token=host-token-secret "
            "scheduler_updates session_id=host-session-secret hidden_debug_ref=host-debug-secret "
            "api_key=host-api-key-secret vlm_raw=host-vlm-raw-secret"
        ),
    })
    exposed_text = repr(result.payload) + repr(engine.intents) + repr(engine.system_messages)

    assert result is not None and result.ok is True
    assert "添加篝火" in result.payload["intent_text"]
    assert "PRIVATE_HOST_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "host-provider-secret" not in exposed_text
    assert "host-token-secret" not in exposed_text
    assert "host-session-secret" not in exposed_text
    assert "host-debug-secret" not in exposed_text
    assert "host-api-key-secret" not in exposed_text
    assert "host-vlm-raw-secret" not in exposed_text
    print("[OK] host action executor sanitizes payload text in status and result")


def test_worker_sanitizes_pending_gm_proposal_payload_metadata():
    dirty_payload = {
        "proposal_id": "gm-dirty",
        "status": "pending_host_confirmation",
        "requires_host_confirm": True,
        "source_user_id": "user-a",
        "intent_text": (
            "添加篝火 prompt=PRIVATE_WORKER_PROMPT_SHOULD_NOT_LEAK "
            "provider=worker-provider-secret runtime_context token=worker-token-secret "
            "scheduler_updates session_id=worker-session-secret hidden_debug_ref=worker-debug-secret"
        ),
        "seed_plan": {
            "title": "营地方案",
            "raw_prompt": "PRIVATE_SEED_PROMPT_SHOULD_NOT_LEAK",
            "provider": "seed-provider-secret",
        },
        "finding_details": {
            "actor_id": "campfire-1",
            "fix_suggestion": "缩小一点",
            "vlm_raw": "PRIVATE_VLM_RAW_SHOULD_NOT_LEAK",
            "job_id": "worker-job-secret",
        },
    }

    engine = FakeEngine([])
    worker = LANChatAgentWorker(
        corona_engine=engine,
        async_agent_execution=False,
    )

    assert worker._send_final_reply(  # noqa: SLF001
        "gm-system",
        "GM",
        dirty_payload["intent_text"],
        _trigger(),
        dirty_payload,
    ) is True
    assert engine.system_messages
    message = engine.system_messages[-1]
    assert message[3] == "gm_proposal"
    metadata = json.loads(message[5])
    exposed_text = repr(engine.system_messages) + repr(engine.replies) + repr(engine.intents)

    assert metadata["proposal_id"] == "gm-dirty"
    assert metadata["requires_host_confirm"] is True
    assert metadata["intent_text"] == "添加篝火"
    assert metadata["seed_plan"]["title"] == "营地方案"
    assert metadata["finding_details"]["actor_id"] == "campfire-1"
    assert metadata["finding_details"]["fix_suggestion"] == "缩小一点"
    assert "raw_prompt" not in metadata["seed_plan"]
    assert "provider" not in metadata["seed_plan"]
    assert "vlm_raw" not in metadata["finding_details"]
    assert "job_id" not in metadata["finding_details"]
    assert "PRIVATE_WORKER_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "worker-provider-secret" not in exposed_text
    assert "worker-token-secret" not in exposed_text
    assert "worker-session-secret" not in exposed_text
    assert "worker-debug-secret" not in exposed_text
    assert "PRIVATE_SEED_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "PRIVATE_VLM_RAW_SHOULD_NOT_LEAK" not in exposed_text
    print("[OK] worker sanitizes pending GM proposal payload metadata")


def test_host_action_executor_serializes_parallel_confirmed_actions():
    engine = FakeEngine([])
    active = 0
    max_active = 0
    lock = threading.Lock()

    def agent_factory():
        def _agent(persona, messages):
            nonlocal active, max_active
            with lock:
                active += 1
                max_active = max(max_active, active)
            time.sleep(0.03)
            with lock:
                active -= 1
            return "parallel action done"

        return _agent

    executor = LanChatHostActionExecutor(
        corona_engine=engine,
        agent_factory=agent_factory,
        engine_gate=None,
    )
    payloads = [
        {
            "proposal_id": "gm-a",
            "status": "confirmed",
            "source_user_id": "user-a",
            "intent_text": "action A",
        },
        {
            "proposal_id": "gm-b",
            "status": "confirmed",
            "source_user_id": "user-b",
            "intent_text": "action B",
        },
    ]
    threads = [
        threading.Thread(target=executor.enqueue_and_process, args=(payload,))
        for payload in payloads
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=1.0)

    assert max_active == 1
    statuses = [item[3] for item in engine.intents]
    assert statuses.count("executing_host_action") == 2
    assert statuses.count("host_action_executed") == 2
    print("[OK] host action executor serializes parallel confirmed actions")


if __name__ == "__main__":
    test_regular_role_agent_reply()
    test_confirm_start_uses_active_coordinator_plan_instead_of_role_agent_gate()
    test_current_mentioned_agent_identity_overrides_history_mentions()
    test_gm_proposal_for_conflict()
    test_gm_generation_proposal_explains_plan_and_confirmation_effect()
    test_gm_summary_does_not_create_proposal()
    test_gm_summary_reads_agent_design_context_without_affecting_normal_summary()
    test_role_agent_not_hijacked_by_prior_agent_or_gm_messages()
    test_role_agent_theme_discussion_not_hijacked_by_gm()
    test_role_agent_advice_request_not_hijacked_by_gm()
    test_agent_plan_reference_resolves_before_gm_proposal()
    test_agent_plan_reference_resolves_real_runtime_phrase()
    test_agent_plan_reference_resolves_legacy_history_without_v2_fields()
    test_agent_plan_reference_does_not_cross_agents()
    test_role_agent_opinion_about_named_agent_plan_receives_that_plan_context()
    test_resolved_plan_generic_inventory_guard()
    test_agent_plan_reference_after_stale_marker_is_rejected()
    test_gm_pause_and_discussion_controls_do_not_reject_pending_proposal()
    test_single_user_major_action_stays_on_role_agent_path()
    test_host_confirmation_consumes_pending_proposal()
    test_host_confirmation_rejects_wrong_proposal_id()
    test_host_confirmation_replay_does_not_requeue_action()
    test_duplicate_trigger_reuses_existing_proposal_id()
    test_rejected_proposal_cannot_be_confirmed_later()
    test_host_confirmation_rejects_explicit_non_host_role()
    test_structured_confirmation_uses_correlation_id_and_metadata()
    test_role_agent_api_error_is_sanitized()
    test_worker_uses_orchestrator_and_sends_reply()
    test_worker_streams_sanitized_progress_reply_before_final()
    test_worker_async_agent_execution_returns_before_slow_agent_reply()
    test_worker_async_sends_fast_ack_before_agent_lock_finishes()
    test_worker_records_busy_scene_message_without_agent_lock()
    test_worker_records_busy_layout_and_edit_notes()
    test_planning_confirmation_gate_roundtrip()
    test_planning_confirmation_gate_returns_concrete_design_brief()
    test_worker_routes_plain_chat_supplement_to_pending_planning_gate()
    test_plain_chat_generation_start_confirms_and_executes_active_seed_plan()
    test_worker_agent_trigger_generation_start_consumes_pending_runtime_plan()
    test_worker_metadata_generate_consumes_pending_runtime_plan()
    test_worker_agent_trigger_plan_elaboration_uses_pending_runtime_plan_without_generation()
    test_worker_disambiguates_plain_chat_supplement_when_multiple_plans_pending()
    test_runtime_routes_metadata_targeted_pending_plan_without_mention()
    test_runtime_metadata_generate_confirms_pending_plan_without_magic_text()
    test_worker_metadata_chat_targets_agent_without_at_or_coordinator_sync()
    test_worker_metadata_group_chat_triggers_each_agent_without_at()
    test_worker_metadata_scene_discussion_feeds_seed_plan_before_confirmation()
    test_worker_metadata_plan_targets_agent_and_returns_plan_reply_without_at()
    test_worker_explicit_at_agent_overrides_stale_metadata_owner()
    test_worker_metadata_supplement_selects_plan_when_multiple_pending()
    test_planning_gate_records_pre_generation_style_supplement()
    test_worker_async_agent_calls_are_serialized_per_worker()
    test_worker_broadcasts_confirmed_gm_action()
    test_worker_acknowledges_final_adjustment_confirmation_without_host_execution()
    test_worker_routes_agent_status_query_to_coordinator_without_model_agent()
    test_worker_does_not_crash_when_coordinator_blocks_start_generation()
    test_worker_applies_host_vlm_generation_option_from_metadata()
    test_worker_routes_completed_layout_adjustment_to_coordinator_without_model_agent()
    test_worker_confirms_completed_layout_reflow_and_moves_ordinary_actors()
    test_worker_layout_reflow_grounds_floating_actor_and_allows_repeat_adjustment()
    test_worker_executes_completed_boundary_adjustment_without_model_agent()
    test_worker_applies_completed_vlm_review_advice_from_metadata_chat()
    test_worker_acknowledges_conflict_resolution_rejection_without_host_execution()
    test_worker_rejects_coordinator_confirmation_without_sender_identity()
    test_worker_rejects_coordinator_confirmation_from_non_host_role()
    test_worker_rejects_coordinator_confirmation_from_metadata_non_host_role()
    test_worker_wraps_confirmed_generation_as_seed_plan_payload()
    test_worker_default_host_executor_uses_coordinator_handler_for_seed_plan()
    test_worker_host_confirmation_disclosure_broadcast_is_sanitized_without_targeted_api()
    test_worker_disclosure_emit_survives_coordinator_history_prune()
    test_worker_host_confirmation_disclosure_uses_targeted_api_when_available()
    test_worker_default_coordinator_submits_confirmed_generation_to_scheduler()
    test_worker_exposes_generation_scheduler_snapshot_without_payload_leak()
    test_worker_cancels_generation_session_without_touching_other_rooms()
    test_worker_room_closed_event_cancels_known_generation_session()
    test_worker_active_room_registry_is_bounded_for_close_fallback()
    test_worker_polls_native_room_closed_event_for_generation_cancel()
    test_worker_ignores_non_closing_lanchat_events_for_generation_cancel()
    test_worker_emits_safe_generation_scheduler_disclosure()
    test_worker_routes_gm_pace_control_through_coordinator()
    test_worker_rejects_gm_pace_control_from_non_host_role()
    test_worker_routes_gm_clarification_through_coordinator()
    test_worker_rejects_gm_clarification_from_non_host_role()
    test_worker_syncs_plain_chat_history_to_coordinator_without_generation()
    test_worker_syncs_only_scene_write_chat_to_coordinator_draft()
    test_worker_syncs_direct_plain_chat_as_runtime_intervention()
    test_worker_syncs_plain_chat_without_message_id_once()
    test_worker_plain_chat_dedupe_cache_is_bounded()
    test_worker_targeted_agent_message_replies_once_across_sync_and_trigger()
    test_worker_syncs_plain_chat_metadata_actor_target_to_coordinator()
    test_worker_polls_native_plain_chat_queue_into_coordinator()
    test_worker_polls_native_host_chat_as_host_message()
    test_worker_does_not_starve_agent_trigger_behind_native_chat_queue()
    test_worker_installs_deferred_download_scheduler_hook()
    test_deferred_provider_download_does_not_bypass_rejecting_scheduler()
    test_media_registry_submit_uses_generation_scheduler_hook()
    test_media_registry_does_not_bypass_rejecting_scheduler()
    test_worker_configured_composer_scheduler_runs_confirmed_seed_plan_context()
    test_worker_does_not_execute_discussion_only_confirmed_payload()
    test_host_action_executor_runs_under_gate_and_reports_result()
    test_host_action_executor_prefers_resolved_intent_text()
    test_host_action_executor_does_not_report_executed_for_empty_or_failed_result()
    test_host_action_executor_reports_accepted_no_delta_when_no_executor_agent()
    test_host_action_executor_sanitizes_agent_api_error()
    test_host_action_executor_sanitizes_payload_text_in_status_and_result()
    test_worker_sanitizes_pending_gm_proposal_payload_metadata()
    test_host_action_executor_serializes_parallel_confirmed_actions()
    print("\n=== LANChat agent orchestrator ALL PASS ===")
