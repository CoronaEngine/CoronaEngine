from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from plugins.AITool.services.interaction_coordinator import (  # noqa: E402
    BatchEvent,
    ChatMessage,
    GenerationJobRef,
    InteractionCoordinator,
    InterventionRequest,
    MAX_COORDINATOR_DISCLOSURE_EVENTS,
    MAX_COORDINATOR_EVENTS,
    MAX_GENERATION_JOB_REFS_PER_PLAN,
    MAX_PENDING_INTERVENTIONS_PER_PLAN,
    MAX_RESOLVED_COORDINATOR_PROPOSALS,
    ReviewResult,
)
from plugins.AITool.services.lanchat_host_action_executor import LanChatHostActionExecutor  # noqa: E402
from plugins.AITool.services.lanchat_scene_runtime import get_lanchat_scene_runtime  # noqa: E402
from plugins.AITool.services.seed_plan import SeedPlanStatus  # noqa: E402


class FakeScheduler:
    def __init__(self):
        self.submitted = []
        self.paused_sessions = []
        self.resumed_sessions = []

    def submit(self, payload):
        self.submitted.append(dict(payload))
        return {"job_id": "job-1", "status": "queued", **payload}

    def pause_session(self, session_id):
        self.paused_sessions.append(str(session_id))
        return {"session_id": str(session_id), "status": "paused", "success": True}

    def resume_session(self, session_id):
        self.resumed_sessions.append(str(session_id))
        return {"session_id": str(session_id), "status": "running", "success": True}


class FakeUpdatableScheduler(FakeScheduler):
    def __init__(self):
        super().__init__()
        self.updates = []
        self.status_by_job = {}

    def submit(self, payload):
        submitted = super().submit(payload)
        self.status_by_job[submitted["job_id"]] = submitted["status"]
        return submitted

    def update_job(self, job_id, *, priority=None, payload_updates=None):
        update = {
            "job_id": job_id,
            "priority": priority,
            "payload_updates": dict(payload_updates or {}),
        }
        self.updates.append(update)
        status = self.status_by_job.get(job_id, "not_found")
        if status != "queued":
            return {"job_id": job_id, "status": status, "success": False, "error": "not queued"}
        return {
            "job_id": job_id,
            "status": "queued",
            "success": True,
            "priority": priority,
            "payload": dict(payload_updates or {}),
        }


class FakeGate:
    def run(self, fn, *args, **kwargs):
        return fn(*args, **kwargs)


class FakeProgressSession:
    def __init__(self):
        self.sink = None

    def set_progress_event_sink(self, sink):
        self.sink = sink

    def emit(self, event):
        assert self.sink is not None
        self.sink(dict(event))


def test_chat_updates_seed_plan_without_generation():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)

    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        sender_name="用户A",
        text="我们想做一个室外暗黑集市，风格要统一",
    ))
    plan = coordinator.active_plan_for_room("room-a")

    assert event.event_type == "seed_plan_updated"
    assert plan is not None
    assert plan.status == SeedPlanStatus.DRAFT
    assert plan.scene_type == "outdoor"
    assert scheduler.submitted == []
    print("[OK] ordinary chat only updates SeedPlan draft")


def test_confirmed_seed_plan_executes_through_scheduler():
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
    confirmed = coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)

    assert confirmed.ok is True
    assert confirmed.payload["action_type"] == "start_generation"
    assert confirmed.payload["seed_plan"]["plan_id"] == plan.plan_id
    assert ref.job_id == "job-1"
    assert scheduler.submitted[0]["plan_id"] == plan.plan_id
    assert scheduler.submitted[0]["session_id"] == ref.session_id
    assert ref.session_id.startswith(f"exec-{plan.plan_id}-")
    assert scheduler.submitted[0]["_runtime_context"]["interaction_coordinator"] is coordinator
    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.EXECUTING
    participant_disclosures = [
        event for event in coordinator.disclosure_events
        if event.audience == "participant"
    ]
    assert participant_disclosures[-1].stage == "排队中"
    assert "等待资源" in participant_disclosures[-1].public_message
    assert "生成中 0%" not in participant_disclosures[-1].public_message
    print("[OK] confirmed SeedPlan enters structured generation scheduler")


def test_confirmed_seed_plan_creates_scene_design_contract_with_negative_preferences():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="我想做一个有点神秘感的室外集市，不要太恐怖，适合几个人逛。",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="做一个夜晚幻想集市，有入口、摊位、灯光、小休息区，整体风格统一。",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    confirmed = coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    contract = coordinator.scene_design_contract(plan.plan_id)

    assert confirmed.ok is True
    assert "不要太恐怖" not in plan.conflicts
    assert "too horror" in contract["avoid_keywords"]
    assert "dark horror" in contract["avoid_keywords"]
    assert contract["boundary_spec"]["type"] == "low_decorative_boundary"
    assert contract["boundary_spec"]["style"] == "vine_wood_lantern"
    assert confirmed.payload["scene_design_contract"]["contract_id"] == contract["contract_id"]
    print("[OK] confirmed SeedPlan creates long-lived scene design contract")


def test_seed_plan_design_brief_survives_confirmation_phrases():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(
        room_id="room-dark-market",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="围绕暗黑集市主题讨论一下",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-dark-market",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="给一个简化方案，我没那么多钱",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-dark-market",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="还是采用暗黑集市方案吧",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-dark-market",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="确认生成",
    ))
    plan = coordinator.active_plan_for_room("room-dark-market")
    confirmed = coordinator.confirm_seed_plan(plan.plan_id, "host-a")

    assert confirmed.ok is True
    assert "暗黑集市" in plan.design_brief
    assert plan.design_brief != "确认生成"
    assert "暗黑集市" in confirmed.payload["intent_text"]
    print("[OK] SeedPlan design brief remains authoritative through confirmation phrases")


def test_seed_plan_confirmation_rejects_confirmation_only_context():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(
        room_id="room-empty-confirm",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="确认生成",
    ))
    plan = coordinator.active_plan_for_room("room-empty-confirm")
    assert plan is not None

    confirmed = coordinator.confirm_seed_plan(plan.plan_id, "host-a")

    assert confirmed.ok is False
    assert "具体方案" in confirmed.message
    print("[OK] SeedPlan refuses confirmation-only context")


def test_seed_plan_design_brief_ignores_opinion_and_elaboration_queries():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(
        room_id="room-opinion-query",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="做一个现代风客厅，有沙发、茶几和电视墙",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-opinion-query",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="你对于小女孩的方案怎么看",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-opinion-query",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="你觉得这个现代客厅设计怎么样",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-opinion-query",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="继续输出风格方案、布局、物品清单",
    ))
    plan = coordinator.active_plan_for_room("room-opinion-query")
    confirmed = coordinator.confirm_seed_plan(plan.plan_id, "host-a")

    assert confirmed.ok is True
    assert "现代风客厅" in plan.design_brief
    assert "怎么看" not in plan.design_brief
    assert "怎么样" not in plan.design_brief
    assert "继续输出" not in plan.design_brief
    print("[OK] SeedPlan design brief skips opinion/detail-only queries")


def test_status_query_does_not_create_intervention_or_generation_job():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外夜晚幻想集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)

    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="@GM 生成到哪里了，为什么不执行呀",
    ))

    assert event.event_type == "status_query"
    assert event.payload["intent_type"] == "status_query"
    assert event.payload["status"] == "executing"
    assert "等待资源调度" in event.message
    assert coordinator.pending_interventions(plan.plan_id) == []
    assert len(scheduler.submitted) == 1
    assert scheduler.submitted[0]["session_id"] == ref.session_id
    print("[OK] status query is read-only and does not create proposal/intervention")


def test_completed_generation_add_routes_to_post_generation_add():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="夜晚幻想集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED

    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="@GM 添加生成一个天使雕像",
    ))
    pending = coordinator.pending_interventions(plan.plan_id)

    assert event.event_type == "post_generation_add_routed"
    assert event.payload["intent_type"] == "post_generation_add"
    assert event.payload["apply_policy"] == "post_generation_add"
    assert pending[-1].intent_type == "post_generation_add"
    assert pending[-1].apply_policy == "post_generation_add"
    assert "天使雕像" in pending[-1].target_hint
    print("[OK] completed generation add request routes to post-generation append batch")


def test_completed_generation_modify_routes_to_final_adjustment():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="夜晚幻想集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED

    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="@长者 感觉布局不是很合理呀，调整一下",
    ))
    pending = coordinator.pending_interventions(plan.plan_id)

    assert event.event_type == "layout_reflow_proposal_created"
    assert event.payload["apply_policy"] == "final_adjustment"
    assert event.payload["layout_reflow_proposal"]["status"] == "proposed"
    assert event.payload["layout_reflow_proposal"]["proposal_id"].startswith("layout-")
    assert pending[-1].apply_policy == "final_adjustment"
    assert coordinator.active_plan_for_room("room-a").plan_id == plan.plan_id
    assert "布局" in pending[-1].content
    print("[OK] completed generation layout request creates final adjustment proposal")


def test_completed_layout_reflow_confirmation_confirms_latest_proposal():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="夜晚幻想集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED
    created = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="调整一下布局，我看模型位置冲突",
    ))

    confirmed = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="确认调整",
    ))

    assert created.event_type == "layout_reflow_proposal_created"
    assert confirmed.event_type == "layout_reflow_confirmed"
    assert confirmed.payload["status"] == "confirmed"
    assert confirmed.payload["proposal_id"] == created.payload["layout_reflow_proposal"]["proposal_id"]
    assert coordinator.active_plan_for_room("room-a").plan_id == plan.plan_id
    print("[OK] completed layout reflow confirmation confirms latest proposal")


def test_completed_generation_boundary_alias_routes_to_final_adjustment():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="夜晚幻想集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED

    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="@商人 这个栅栏有点奇怪，换成低矮藤蔓围栏",
    ))
    pending = coordinator.pending_interventions(plan.plan_id)

    assert event.event_type == "final_adjustment_routed"
    assert pending[-1].actor_id == "__terrain_boundary"
    assert pending[-1].target_hint == "__terrain_boundary"
    assert pending[-1].apply_policy == "final_adjustment"
    print("[OK] completed generation boundary alias routes to terrain final adjustment")


def test_generation_add_target_hint_strips_followup_verbs_and_mentions():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="温暖神秘夜晚集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="@长者 后面再加入一个天使雕像",
    ))
    pending = coordinator.pending_interventions(plan.plan_id)

    assert event.event_type == "intervention_routed"
    assert event.payload["target_hint"] == "天使雕像"
    assert pending[-1].target_hint == "天使雕像"
    assert "入一个" not in pending[-1].target_hint
    print("[OK] follow-up add target hint strips mention and verbs")


def test_execute_confirmed_plan_payload_uses_full_contract_prompt():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="做一个有点神秘感的室外集市，不要太恐怖，适合几个人逛"))
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="希望它更温暖一点，有灯光和休息区，不要全是暗黑风"))
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="最终简化方案：夜色神秘，灯火温暖，可逛可坐，中央宽通道，两侧摊位，一侧休息区"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")

    coordinator.execute_confirmed_plan(plan.plan_id)
    payload = scheduler.submitted[-1]
    prompt = payload["prompt"]

    assert "最终简化方案" in prompt
    assert "灯火温暖" in prompt
    assert "too horror" in prompt
    assert "boundary_spec" in prompt
    assert payload["intent_text"] == prompt
    print("[OK] confirmed generation payload carries full SeedPlan contract prompt")


def test_execute_confirmed_plan_sets_lanchat_runtime_executing():
    runtime = get_lanchat_scene_runtime()
    runtime.end_compose()
    runtime.set_mode("DISCUSSING")
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="room-runtime",
        sender_id="host-runtime",
        sender_name="房主",
        is_host=True,
        text="做一个二战前线小型交战场地",
    ))
    plan = coordinator.propose_seed_plan("room-runtime")
    coordinator.confirm_seed_plan(plan.plan_id, "host-runtime")

    try:
        coordinator.execute_confirmed_plan(plan.plan_id)
        snapshot = runtime.active_snapshot()
    finally:
        runtime.end_compose()

    assert scheduler.submitted
    assert runtime.mode() == "DISCUSSING"
    assert snapshot["mode"] == "EXECUTING"
    assert snapshot["active"] is True
    assert snapshot["active_goal"]
    print("[OK] executing confirmed SeedPlan flips LANChat scene runtime into executing mode")


def test_system_actor_aliases_are_canonicalized_for_terrain_boundary():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="夜晚幻想集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="@商人 你理解错了，我说的是_terrain_boundary，换成低矮木栏/藤蔓围栏。",
    ))

    assert event.event_type == "intervention_routed"
    assert event.payload["actor_id"] == "__terrain_boundary"
    assert event.payload["target_hint"] == "__terrain_boundary"
    print("[OK] terrain boundary aliases canonicalize to system actor id")


def test_seed_plan_confirmation_requires_non_empty_host_identity():
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

    result = coordinator.confirm_seed_plan(plan.plan_id, "  ")

    assert result.ok is False
    assert "房主确认身份" in result.message
    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.PROPOSED
    assert coordinator.get_plan(plan.plan_id).confirmed_by == ""
    assert scheduler.submitted == []
    print("[OK] SeedPlan confirmation rejects empty host identity")


def test_gm_conflict_resolution_requires_host_confirmation_before_plan_confirm():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        sender_name="用户A",
        text="我想要室外暗黑集市，风格要很暗",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        sender_name="用户B",
        text="这里有冲突：我不同意太暗，但是要保留集市主题",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    plan.conflicts.append("用户A希望极暗，用户B希望入口更亮")
    plan.conflicts.append("用户A希望密集摊位，用户B希望入口留白")

    blocked_confirm = coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    assert blocked_confirm.ok is False
    assert blocked_confirm.payload["requires_conflict_resolution"] is True
    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.PROPOSED
    proposal = coordinator.propose_conflict_resolution(
        plan.plan_id,
        proposed_by="gm",
        recommendation="折中：保留暗黑集市主题，入口区域略亮，核心区域保持暗色。",
    )
    before_confirm = coordinator.get_plan(plan.plan_id)
    confirmed_resolution = coordinator.confirm_conflict_resolution(proposal.proposal_id, "host-a")
    duplicate_resolution = coordinator.confirm_conflict_resolution(proposal.proposal_id, "host-a")
    assert before_confirm.status == SeedPlanStatus.PROPOSED
    confirmed_plan = coordinator.confirm_seed_plan(plan.plan_id, "host-a")

    assert proposal.status == "confirmed"
    assert confirmed_resolution.ok is True
    assert duplicate_resolution.ok is True
    assert confirmed_plan.ok is True
    resolutions = confirmed_plan.payload["seed_plan"]["review_policy"]["conflict_resolutions"]
    assert len(resolutions) == 1
    assert resolutions[0]["confirmed_by"] == "host-a"
    assert set(resolutions[0]["conflict_items"]) == set(plan.conflicts)
    assert "入口区域略亮" in resolutions[0]["recommendation"]
    assert any(item.event_type == "conflict_resolution_proposed" for item in coordinator.events)
    assert any(item.event_type == "conflict_resolution_confirmed" for item in coordinator.events)
    host_disclosures = [
        item for item in coordinator.disclosure_events
        if item.audience == "host" and item.metadata.get("intervention", {}).get("proposal_id") == proposal.proposal_id
    ]
    assert host_disclosures
    assert host_disclosures[-1].requires_confirmation is True
    assert "confirm_conflict_resolution" in host_disclosures[-1].available_actions
    print("[OK] GM conflict proposal requires host confirmation before SeedPlan confirmation")


def test_gm_conflict_resolution_requires_non_empty_host_identity():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
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
        recommendation="建议折中为暗红蓝混合灯光。",
    )

    empty_confirm = coordinator.confirm_conflict_resolution(proposal.proposal_id, "")
    empty_reject = coordinator.reject_conflict_resolution(proposal.proposal_id, "  ")

    assert empty_confirm.ok is False
    assert empty_reject.ok is False
    assert "房主确认身份" in empty_confirm.message
    assert "房主确认身份" in empty_reject.message
    assert proposal.status == "proposed"
    assert proposal.confirmed_by == ""
    assert plan.review_policy.get("conflict_resolutions") in (None, [])
    print("[OK] GM conflict proposal rejects empty host identity")


def test_gm_conflict_resolution_rejection_blocks_later_confirmation():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
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
        recommendation="建议折中为暗红蓝混合灯光。",
    )

    rejected = coordinator.reject_conflict_resolution(proposal.proposal_id, "host-a")
    confirm_after_reject = coordinator.confirm_conflict_resolution(proposal.proposal_id, "host-a")
    plan_after_reject = coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    resolutions = plan.review_policy.get("conflict_resolutions") or []

    assert rejected.ok is True
    assert proposal.status == "rejected"
    assert rejected.payload["proposal"]["status"] == "rejected"
    assert resolutions[-1]["proposal_id"] == proposal.proposal_id
    assert resolutions[-1]["status"] == "rejected"
    assert confirm_after_reject.ok is False
    assert "已拒绝" in confirm_after_reject.message
    assert plan_after_reject.ok is False
    assert plan_after_reject.payload["requires_conflict_resolution"] is True
    assert any(item.event_type == "conflict_resolution_rejected" for item in coordinator.events)
    print("[OK] GM conflict proposal rejection blocks later confirmation")


def test_generation_interventions_are_routed_by_intent():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    add_event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        sender_name="用户B",
        text="第一批后补一个天使雕塑",
    ))
    repair_event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        text="雕塑跟地面穿模了",
    ))
    wrong_event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="你执行错方案了，不是正儿八经的暗黑集市方案",
    ))

    routes = [item.apply_policy for item in coordinator.pending_interventions(plan.plan_id)]
    assert add_event.event_type == "intervention_routed"
    assert repair_event.payload["intent_type"] == "repair"
    assert wrong_event.payload["intent_type"] == "wrong_plan"
    assert routes == ["next_batch", "geometry_review", "pause_and_replan"]
    assert scheduler.paused_sessions == ["room-a"]
    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.PAUSED
    assert wrong_event.payload["scheduler_control"]["success"] is True
    print("[OK] generation follow-up messages route to batch intervention protocol")


def test_prefilled_disclosure_drafts_route_to_expected_interventions():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    add_event = coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="u1", text="新增：入口右侧摊位，挂一盏灯"))
    modify_event = coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="u2", text="调整：入口右侧摊位，往后挪一点"))
    issue_event = coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="u3", text="问题：入口右侧摊位，和地面穿模"))
    note_event = coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="u4", text="说明：入口右侧摊位，保持暗色"))
    pending = coordinator.pending_interventions(plan.plan_id)

    assert add_event.payload["intent_type"] == "add"
    assert add_event.payload["target_hint"] == "入口右侧摊位"
    assert modify_event.payload["intent_type"] == "modify"
    assert modify_event.payload["target_hint"] == "入口右侧摊位"
    assert issue_event.payload["intent_type"] == "repair"
    assert issue_event.payload["apply_policy"] == "geometry_review"
    assert issue_event.payload["target_hint"] == "入口右侧摊位"
    assert note_event.payload["intent_type"] == "modify"
    assert note_event.payload["apply_policy"] == "next_batch"
    assert pending[-1].target_hint == "入口右侧摊位"
    print("[OK] prefilled disclosure drafts route to expected Coordinator interventions")


def test_gm_pace_control_enters_coordinator_and_scheduler():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    paused = coordinator.control_pace("room-a", "pause", actor_id="gm", note="@GM 暂停一下")
    resumed = coordinator.control_pace("room-a", "resume", actor_id="gm", note="@GM 继续")
    summary = coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)
    participant_disclosures = [
        item for item in coordinator.disclosure_events
        if item.audience == "participant"
        and item.metadata.get("intervention", {}).get("intent_type") == "pace_control"
    ]

    assert paused.event_type == "gm_pace_control"
    assert paused.payload["previous_status"] == "executing"
    assert paused.payload["status"] == "paused"
    assert paused.payload["scheduler_control"]["success"] is True
    assert resumed.payload["previous_status"] == "paused"
    assert resumed.payload["status"] == "executing"
    assert scheduler.paused_sessions == ["room-a"]
    assert scheduler.resumed_sessions == ["room-a"]
    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.EXECUTING
    assert "gm_pace_control" in summary["summary_text"]
    assert participant_disclosures
    assert participant_disclosures[-2].metadata["intervention"]["apply_policy"] == "pause_after_batch"
    assert "暂停" in participant_disclosures[-2].metadata["intervention"]["status_message"]
    assert participant_disclosures[-1].metadata["intervention"]["apply_policy"] == "continue_generation"
    assert "恢复" in participant_disclosures[-1].metadata["intervention"]["status_message"]
    print("[OK] GM pace control enters Coordinator, scheduler, memory, and disclosure")


def test_clarification_blocks_confirmation_until_answered():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="user-a",
        sender_name="用户A",
        text="想做一个适合多人讨论的场景，但风格还没定",
    ))
    plan = coordinator.active_plan_for_room("room-a")
    assert plan is not None

    clarification = coordinator.request_clarification(
        "room-a",
        "请确认场景是室内、室外还是混合，并补充主要风格。",
        requested_by="gm",
    )
    blocked = coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="做室外暗黑集市，入口留白，后面分批生成。",
    ))
    confirmed = coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    summary = coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)
    participant_disclosures = [
        item for item in coordinator.disclosure_events
        if item.audience == "participant"
        and item.metadata.get("intervention", {}).get("intent_type") == "clarification"
    ]

    assert clarification.event_type == "clarification_requested"
    assert plan.review_policy["clarification_requests"][0]["status"] == "answered"
    assert blocked.ok is False
    assert blocked.payload["requires_clarification"] is True
    assert confirmed.ok is True
    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.CONFIRMED
    assert "clarification_requested" in summary["summary_text"]
    assert "clarification_answered" in summary["summary_text"]
    assert participant_disclosures
    assert "室内、室外还是混合" in participant_disclosures[-1].public_message
    print("[OK] clarification blocks SeedPlan confirmation until answered")


def test_paused_plan_can_spawn_gm_replan_version_for_host_confirmation():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="执行错方案了，不是这个方案，重来",
    ))

    replan = coordinator.propose_replan_from_paused(
        plan.plan_id,
        proposer_id="gm",
        note="GM 重提案：保留暗黑集市，但先生成入口与主雕塑。",
    )
    confirmed = coordinator.confirm_seed_plan(replan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(replan.plan_id)

    assert coordinator.get_plan(plan.plan_id).status == SeedPlanStatus.PAUSED
    assert replan.plan_id != plan.plan_id
    assert replan.version == plan.version + 1
    assert replan.status == SeedPlanStatus.EXECUTING
    assert coordinator.active_plan_for_room("room-a").plan_id == replan.plan_id
    assert "重来" in confirmed.payload["seed_plan"]["intent_summary"]
    assert "入口与主雕塑" in confirmed.payload["seed_plan"]["intent_summary"]
    assert ref.job_id == "job-1"
    assert scheduler.submitted[-1]["plan_id"] == replan.plan_id
    assert scheduler.paused_sessions == ["room-a"]
    assert scheduler.resumed_sessions == ["room-a"]
    assert scheduler.submitted[-1]["scheduler_resume"]["success"] is True
    assert scheduler.submitted[-1]["scheduler_resume"]["source_plan_id"] == plan.plan_id
    assert any(item.event_type == "seed_plan_replan_proposed" for item in coordinator.events)
    print("[OK] paused SeedPlan can spawn GM replan version before host confirmation")


def test_next_batch_intervention_updates_queued_generation_job():
    scheduler = FakeUpdatableScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-1",
        stage="batch_boundary",
        status="done",
        intervention_window_open=True,
        metadata={"batch_index": 1},
    ))

    host_event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="第一批后补一个主雕塑，优先放到入口中央",
    ))
    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        text="第一批后补一个天使雕塑，放到入口左侧",
    ))

    assert host_event.event_type == "intervention_routed"
    assert event.event_type == "intervention_routed"
    assert scheduler.updates
    update = scheduler.updates[-1]
    assert update["job_id"] == "job-1"
    assert update["priority"] == 2
    assert update["payload_updates"]["intervention_revision"] == 2
    assert update["payload_updates"]["latest_intervention"]["apply_policy"] == "next_batch"
    assert update["payload_updates"]["latest_intervention"]["target_hint"] == "天使雕塑"
    assert update["payload_updates"]["pending_interventions"][0]["priority"] == 2
    assert update["payload_updates"]["pending_interventions"][1]["priority"] == 1
    assert "天使雕塑" in update["payload_updates"]["latest_intervention"]["content"]
    assert event.payload["scheduler_updates"][0]["success"] is True
    assert event.payload["scheduler_update_summary"]["updated_count"] >= 1
    assert event.payload["scheduler_update_summary"]["deferred_to_pending"] is False
    participant_disclosures = [
        item for item in coordinator.disclosure_events
        if item.audience == "participant" and item.stage == "可介入窗口"
    ]
    assert participant_disclosures
    disclosure_intervention = participant_disclosures[-1].metadata["intervention"]
    assert disclosure_intervention["scheduler_update_summary"]["updated_count"] >= 1
    assert disclosure_intervention["scheduler_update_summary"]["deferred_to_pending"] is False
    assert "scheduler_updates" not in disclosure_intervention
    assert "job_id" not in str(disclosure_intervention)
    print("[OK] next-batch intervention updates queued scheduler job payload")


def test_next_batch_intervention_reports_deferred_when_generation_already_running():
    scheduler = FakeUpdatableScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)
    scheduler.status_by_job[ref.job_id] = "submitting"

    event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        text="第一批后补一个天使雕塑，放到入口左侧",
    ))

    summary = event.payload["scheduler_update_summary"]
    assert event.event_type == "intervention_routed"
    assert event.payload["scheduler_updates"][0]["success"] is False
    assert event.payload["scheduler_updates"][0]["status"] == "submitting"
    assert summary["attempted_count"] == 1
    assert summary["updated_count"] == 0
    assert summary["failed_count"] == 1
    assert summary["deferred_to_pending"] is True
    assert summary["reason"] == "no queued future generation job accepted the intervention update"
    assert coordinator.pending_interventions(plan.plan_id)[-1].content.startswith("第一批后补")
    participant_disclosures = [
        item for item in coordinator.disclosure_events
        if item.audience == "participant" and item.stage == "可介入窗口"
    ]
    assert participant_disclosures[-1].metadata["intervention"]["scheduler_update_summary"]["deferred_to_pending"] is True
    print("[OK] next-batch intervention reports deferred when generation already running")


def test_intervention_extracts_actor_target_for_final_adjustment_conflicts():
    scheduler = FakeUpdatableScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-2",
        stage="batch_boundary",
        status="done",
        intervention_window_open=True,
        metadata={"batch_index": 2},
    ))

    remove_event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        text="最后删除这个雕像，它挡住入口",
        metadata={"actor_id": "statue-entrance", "actor_version": 3},
    ))
    modify_event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u2",
        text="最后把 actor:statue-entrance 缩小一点，保留在入口左侧",
    ))
    final_plan = coordinator.final_adjustment_plan(plan.plan_id)

    pending = coordinator.pending_interventions(plan.plan_id)
    assert remove_event.payload["actor_id"] == "statue-entrance"
    assert remove_event.payload["actor_version"] == 3
    assert remove_event.payload["target_hint"] == "statue-entrance"
    assert modify_event.payload["actor_id"] == "statue-entrance"
    assert any(item.target_hint == "statue-entrance" for item in pending)
    assert final_plan["conflicts"]
    assert final_plan["conflicts"][0]["actor_id"] == "statue-entrance"
    assert any(item.event_type == "final_adjustment_conflict_requires_confirmation" for item in coordinator.events)
    print("[OK] intervention extracts actor target for final adjustment conflicts")


def test_direct_intervention_finding_details_are_sanitized():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    decision = coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-2",
        actor_id="actor-lamp",
        target_hint="入口灯",
        content="入口灯需要最终旋转",
        apply_policy="final_adjustment",
        priority=2,
        finding_details=[{
            "action": "apply_vlm_advice",
            "rotation_correction": [0.0, 45.0, 0.0],
            "fix_suggestion": "旋转 45 度",
            "prompt": "PRIVATE_INTERVENTION_PROMPT_SHOULD_NOT_LEAK",
            "provider": "intervention-provider-secret",
            "runtime_context": {"token": "intervention-token-secret"},
            "scheduler_updates": [{"session_id": "intervention-session-secret"}],
            "hidden_debug_ref": "intervention-debug-secret",
        }],
    ))
    adjustment = coordinator.final_adjustment_plan(plan.plan_id)
    exposed_text = (
        repr(decision.payload)
        + repr([item.as_dict() for item in coordinator.pending_interventions(plan.plan_id)])
        + repr(adjustment)
        + repr([item.as_dict() for item in coordinator.disclosure_events])
    )

    assert decision.accepted is True
    assert decision.payload["finding_details"][0]["rotation_correction"] == [0.0, 45.0, 0.0]
    assert adjustment["selected"][0]["finding_details"][0]["fix_suggestion"] == "旋转 45 度"
    assert "PRIVATE_INTERVENTION_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "intervention-provider-secret" not in exposed_text
    assert "intervention-token-secret" not in exposed_text
    assert "intervention-session-secret" not in exposed_text
    assert "intervention-debug-secret" not in exposed_text
    print("[OK] direct intervention finding details are sanitized")


def test_coordinator_records_scoped_memory_for_plan_lifecycle():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        sender_name="用户A",
        text="室外暗黑集市，风格统一",
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-b",
        sender_id="u2",
        sender_name="用户B",
        text="室内大厅，要明亮",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u3",
        text="第一批后补一个天使雕塑",
    ))

    room_a_summary = coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)
    room_b_summary = coordinator.memory_summary(room_id="room-b")

    assert "室外暗黑集市" in room_a_summary["summary_text"]
    assert "SeedPlan" in room_a_summary["summary_text"]
    assert "天使雕塑" in room_a_summary["summary_text"]
    assert "室内大厅" not in room_a_summary["summary_text"]
    assert "室内大厅" in room_b_summary["summary_text"]
    print("[OK] coordinator records scoped lifecycle memory without cross-room leakage")


def test_batch_event_opens_intervention_window_and_records_memory():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)

    event = coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id=plan.plan_id,
        session_id=ref.session_id,
        batch_id="batch-1",
        stage="batch_boundary",
        status="waiting_intervention",
        progress=35,
        message="第一批已完成，可补充下一批要求。",
        intervention_window_open=True,
        metadata={
            "batch_index": 1,
            "batch_total": 4,
            "prompt": "PRIVATE_DIRECT_BATCH_PROMPT_SHOULD_NOT_LEAK",
            "provider": "direct-batch-provider",
            "job_id": "direct-job-secret",
            "session_id": "direct-session-secret",
            "runtime_context": {"token": "direct-runtime-token"},
            "scheduler_updates": [{"prompt": "direct nested batch secret"}],
            "hidden_debug_ref": "direct-debug-secret",
        },
    ))
    summary = coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id, batch_id="batch-1")

    assert event.event_type == "batch_intervention_window_open"
    assert event.payload["metadata"]["batch_index"] == 1
    assert "第一批已完成" in summary["summary_text"]
    assert coordinator.disclosure_events[-1].stage == "可介入窗口"
    assert coordinator.disclosure_events[-1].metadata["intervention"]["apply_policy"] == "next_batch"
    exposed_text = (
        repr(event.payload)
        + repr(summary)
        + repr([item.as_dict() for item in coordinator.disclosure_events])
    )
    assert "PRIVATE_DIRECT_BATCH_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "direct-batch-provider" not in exposed_text
    assert "direct-job-secret" not in exposed_text
    assert "direct-session-secret" not in exposed_text
    assert "direct-runtime-token" not in exposed_text
    assert "direct nested batch secret" not in exposed_text
    assert "direct-debug-secret" not in exposed_text
    print("[OK] batch boundary events open intervention window through Coordinator")


def test_batch_event_rejects_stale_generation_session_without_disclosure():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)
    disclosure_count = len(coordinator.disclosure_events)

    stale = coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id=plan.plan_id,
        session_id=f"{ref.session_id}-old",
        batch_id="batch-stale",
        stage="batch_boundary",
        status="done",
        message="旧会话不应打开介入窗口",
        intervention_window_open=True,
    ))

    assert stale.event_type == "batch_event_rejected"
    assert stale.payload["reject_reason"] == "session_mismatch"
    assert stale.payload["expected_session_id"] == ref.session_id
    assert len(coordinator.disclosure_events) == disclosure_count
    assert "旧会话不应打开介入窗口" not in coordinator.memory_summary(
        room_id="room-a",
        plan_id=plan.plan_id,
    )["summary_text"]
    print("[OK] stale batch session is rejected without disclosure or memory writes")


def test_batch_event_rejects_unknown_or_foreign_plan_without_disclosure():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    disclosure_count = len(coordinator.disclosure_events)

    unknown = coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id="missing-plan",
        batch_id="batch-x",
        stage="batch_boundary",
        status="done",
        message="不应打开介入窗口",
        intervention_window_open=True,
    ))
    foreign = coordinator.ingest_batch_event(BatchEvent(
        room_id="room-b",
        plan_id=plan.plan_id,
        batch_id="batch-y",
        stage="batch_boundary",
        status="done",
        message="也不应打开介入窗口",
        intervention_window_open=True,
    ))

    assert unknown.event_type == "batch_event_rejected"
    assert unknown.payload["reject_reason"] == "unknown_plan"
    assert foreign.event_type == "batch_event_rejected"
    assert foreign.payload["reject_reason"] == "room_mismatch"
    assert len(coordinator.disclosure_events) == disclosure_count
    assert "不应打开介入窗口" not in coordinator.memory_summary(room_id="room-a", plan_id="missing-plan")["summary_text"]
    print("[OK] batch events reject unknown or foreign plan without disclosure")


def test_coordinator_binds_scene_session_progress_as_batch_events():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    session = FakeProgressSession()
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")

    coordinator.bind_scene_session_progress(
        session,
        room_id="room-a",
        plan_id=plan.plan_id,
        session_id="sess-1",
    )
    session.emit({
        "phase": "OBJECTS#1",
        "status": "done",
        "percent": 50,
        "batch_id": "r1_OBJECTS_b1",
        "user_message": "第一批已放入，你可以继续提出调整。",
        "asset_count": 2,
        "imported_count": 2,
        "batch_index": 1,
        "batch_total": 2,
        "prompt": "PRIVATE_BATCH_PROMPT_SHOULD_NOT_LEAK",
        "provider": "internal-batch-provider",
        "job_id": "job-secret-batch",
        "session_id": "exec-secret-batch",
        "runtime_context": {"token": "secret-batch-token"},
        "scheduler_updates": [{"prompt": "nested batch secret"}],
        "hidden_debug_ref": "debug-secret-batch",
    })

    assert coordinator.events[-1].event_type == "batch_intervention_window_open"
    assert coordinator.events[-1].payload["batch_id"] == "r1_OBJECTS_b1"
    assert coordinator.events[-1].payload["metadata"]["batch_index"] == 1
    assert coordinator.disclosure_events[-1].stage == "可介入窗口"
    assert "第一批已放入" in coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)["summary_text"]
    exposed_text = (
        repr(coordinator.events[-1].payload)
        + repr(coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id))
        + repr([event.as_dict() for event in coordinator.disclosure_events])
    )
    assert "PRIVATE_BATCH_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "internal-batch-provider" not in exposed_text
    assert "job-secret-batch" not in exposed_text
    assert "exec-secret-batch" not in exposed_text
    assert "secret-batch-token" not in exposed_text
    assert "nested batch secret" not in exposed_text
    assert "debug-secret-batch" not in exposed_text
    print("[OK] Coordinator binds SceneSession structured progress to BatchEvent")


def test_failed_review_routes_structured_intervention():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)

    events = coordinator.ingest_review_result(ReviewResult(
        room_id="room-a",
        plan_id=plan.plan_id,
        session_id=ref.session_id,
        batch_id="batch-1",
        actor_id="actor-statue",
        actor_version=3,
        review_type="geometry",
        passed=False,
        findings=["雕塑跟地面穿模", "比例偏大"],
        severity="fail",
    ))
    intervention = coordinator.pending_interventions(plan.plan_id)[-1]

    assert [item.event_type for item in events] == ["review_failed", "review_intervention_routed"]
    assert intervention.apply_policy == "geometry_review"
    assert intervention.actor_id == "actor-statue"
    assert intervention.actor_version == 3
    assert "穿模" in coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)["summary_text"]
    print("[OK] failed review results route to structured intervention protocol")


def test_review_result_dict_false_string_routes_failed_review():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)

    events = coordinator.ingest_review_result({
        "room_id": "room-a",
        "plan_id": plan.plan_id,
        "session_id": ref.session_id,
        "batch_id": "batch-1",
        "actor_id": "actor-lamp",
        "review_type": "vlm",
        "passed": "false",
        "findings": ["灯具和墙面穿插"],
        "severity": "fail",
    })
    intervention = coordinator.pending_interventions(plan.plan_id)[-1]

    assert [item.event_type for item in events] == ["review_failed", "review_intervention_routed"]
    assert intervention.apply_policy == "final_adjustment"
    assert intervention.actor_id == "actor-lamp"
    assert "灯具和墙面穿插" in coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)["summary_text"]
    print("[OK] ReviewResult dict false string routes to failed review")


def test_review_result_rejects_stale_generation_session_without_intervention():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)
    disclosure_count = len(coordinator.disclosure_events)

    stale = coordinator.ingest_review_result(ReviewResult(
        room_id="room-a",
        plan_id=plan.plan_id,
        session_id=f"{ref.session_id}-old",
        batch_id="batch-stale",
        review_type="vlm",
        passed=False,
        findings=["旧会话审查不应进入最终调整"],
        severity="fail",
    ))

    assert [item.event_type for item in stale] == ["review_result_rejected"]
    assert stale[0].payload["reject_reason"] == "session_mismatch"
    assert stale[0].payload["expected_session_id"] == ref.session_id
    assert coordinator.pending_interventions(plan.plan_id) == []
    assert len(coordinator.disclosure_events) == disclosure_count
    assert "旧会话审查" not in coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)["summary_text"]
    print("[OK] stale review session is rejected without intervention or disclosure")


def test_review_result_rejects_unknown_or_foreign_plan_without_intervention():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    disclosure_count = len(coordinator.disclosure_events)

    unknown = coordinator.ingest_review_result(ReviewResult(
        room_id="room-a",
        plan_id="missing-plan",
        batch_id="batch-x",
        review_type="vlm",
        passed=False,
        findings=["不应进入最终调整"],
        severity="fail",
    ))
    foreign = coordinator.ingest_review_result(ReviewResult(
        room_id="room-b",
        plan_id=plan.plan_id,
        batch_id="batch-y",
        review_type="geometry",
        passed=False,
        findings=["不应进入几何修复"],
        severity="fail",
    ))

    assert [item.event_type for item in unknown] == ["review_result_rejected"]
    assert unknown[0].payload["reject_reason"] == "unknown_plan"
    assert [item.event_type for item in foreign] == ["review_result_rejected"]
    assert foreign[0].payload["reject_reason"] == "room_mismatch"
    assert coordinator.pending_interventions("missing-plan") == []
    assert coordinator.pending_interventions(plan.plan_id) == []
    assert len(coordinator.disclosure_events) == disclosure_count
    assert "不应进入" not in coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)["summary_text"]
    print("[OK] review results reject unknown or foreign plan without intervention")


def test_memory_summary_can_focus_on_actor_history():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-1",
        actor_id="actor-statue",
        source_user_id="u1",
        intent_type="modify",
        content="中心雕塑太大，最后要缩小",
        priority=2,
        apply_policy="final_adjustment",
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-1",
        actor_id="actor-lamp",
        source_user_id="u2",
        intent_type="modify",
        content="入口灯要转向",
        priority=1,
        apply_policy="final_adjustment",
    ))

    summary = coordinator.memory_summary(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-statue",
    )

    assert summary["actor_id"] == "actor-statue"
    assert "中心雕塑太大" in summary["summary_text"]
    assert "入口灯" not in summary["summary_text"]
    print("[OK] Coordinator memory summary can focus on one actor's cross-batch history")


def test_review_result_finding_details_feed_final_adjustment_target_hint():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)

    events = coordinator.ingest_review_result(ReviewResult(
        room_id="room-a",
        plan_id=plan.plan_id,
        session_id=ref.session_id,
        batch_id="batch-3",
        review_type="vlm",
        passed=False,
        findings=["朝向不符合用户意图"],
        finding_details=[{
            "actor_id": "actor-lamp",
            "action": "apply_vlm_advice",
            "target_hint": "actor-lamp",
            "rotation_correction": [0.0, 90.0, 0.0],
            "fix_suggestion": "旋转 90 度",
            "prompt": "PRIVATE_VLM_PROMPT_SHOULD_NOT_LEAK",
            "provider": "internal-vlm-provider",
            "runtime_context": {"token": "secret-runtime-token"},
            "scheduler_updates": [{"session_id": "exec-secret-session"}],
            "finding_details": "raw nested VLM details",
            "hidden_debug_ref": "debug-secret",
        }],
        metadata={
            "prompt": "PRIVATE_REVIEW_PROMPT_SHOULD_NOT_LEAK",
            "provider": "internal-review-provider",
            "job_id": "job-secret-review",
            "safe_note": "review summary",
        },
        severity="fail",
    ))
    pending = coordinator.pending_interventions(plan.plan_id)
    adjustment = coordinator.final_adjustment_plan(plan.plan_id)
    exposed_text = repr(events) + repr([item.as_dict() for item in pending]) + repr(adjustment)

    assert events[-1].event_type == "review_intervention_routed"
    assert events[-1].payload["actor_id"] == "actor-lamp"
    assert events[-1].payload["target_hint"] == "actor-lamp"
    assert events[-1].payload["finding_details"][0]["action"] == "apply_vlm_advice"
    assert pending[-1].actor_id == "actor-lamp"
    assert pending[-1].target_hint == "actor-lamp"
    assert pending[-1].apply_policy == "final_adjustment"
    assert adjustment["selected"][0]["target_hint"] == "actor-lamp"
    assert adjustment["selected"][0]["finding_details"][0]["rotation_correction"] == [0.0, 90.0, 0.0]
    assert "PRIVATE_VLM_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "PRIVATE_REVIEW_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "internal-vlm-provider" not in exposed_text
    assert "internal-review-provider" not in exposed_text
    assert "secret-runtime-token" not in exposed_text
    assert "exec-secret-session" not in exposed_text
    assert "raw nested VLM details" not in exposed_text
    assert "debug-secret" not in exposed_text
    assert "job-secret-review" not in exposed_text
    print("[OK] review finding details feed targeted final adjustment planning")


def test_final_adjustment_plan_prefers_recent_strong_interventions():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)

    coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-1",
        stage="batch_boundary",
        status="done",
        intervention_window_open=True,
        metadata={"batch_index": 1},
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="u1",
        text="第一批旁边加一个很小的摊位",
    ))
    coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-3",
        stage="batch_boundary",
        status="done",
        intervention_window_open=True,
        metadata={"batch_index": 3},
    ))
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="最后收尾时把中心雕塑缩小并贴地，优先保证不穿模",
    ))
    coordinator.ingest_review_result(ReviewResult(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-3",
        actor_id="actor-statue",
        actor_version=7,
        review_type="vlm",
        passed=False,
        findings=["中心雕塑比例仍然偏大"],
        severity="fail",
    ))

    adjustment = coordinator.final_adjustment_plan(plan.plan_id, recent_batch_window=2)
    selected_text = "；".join(item["content"] for item in adjustment["selected"])
    deferred_text = "；".join(item["content"] for item in adjustment["deferred"])

    assert adjustment["latest_batch_index"] == 3
    assert "最后收尾" in selected_text
    assert "中心雕塑比例仍然偏大" in selected_text
    assert "很小的摊位" in deferred_text
    assert adjustment["selected"][0]["is_recent"] is True
    assert "final_adjustment_plan" in coordinator.memory_summary(room_id="room-a", plan_id=plan.plan_id)["entries"][-1]["entry_type"]
    print("[OK] final adjustment plan prioritizes late strong interventions and defers old weak requests")


def test_final_adjustment_conflict_requires_host_confirmation():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-4",
        stage="batch_boundary",
        status="done",
        intervention_window_open=True,
        metadata={"batch_index": 4},
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-4",
        actor_id="actor-stall",
        source_user_id="u1",
        intent_type="remove",
        content="用户A要求删除这个摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-4",
        actor_id="actor-stall",
        source_user_id="u2",
        intent_type="modify",
        content="用户B要求保留并调暗这个摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))

    event_count = len(coordinator.events)
    disclosure_count = len(coordinator.disclosure_events)
    adjustment = coordinator.final_adjustment_plan(plan.plan_id)

    assert adjustment["conflicts"]
    assert adjustment["conflicts"][0]["actor_id"] == "actor-stall"
    assert adjustment["conflicts"][0]["proposal_id"].startswith(f"fa-{plan.plan_id}-")
    new_events = coordinator.events[event_count:]
    assert any(item.event_type == "final_adjustment_conflict_requires_confirmation" for item in new_events)
    new_disclosures = coordinator.disclosure_events[disclosure_count:]
    host_disclosures = [
        item for item in new_disclosures
        if item.stage == "最终调整中" and item.audience == "host" and item.requires_confirmation
    ]
    assert host_disclosures
    assert host_disclosures[-1].metadata["intervention"]["proposal_id"] == adjustment["conflicts"][0]["proposal_id"]
    participant_messages = [item.public_message for item in new_disclosures if item.audience == "participant"]
    assert participant_messages
    print("[OK] final adjustment conflicts require GM/host confirmation")


def test_final_adjustment_conflict_confirmation_is_recorded():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-stall",
        source_user_id="u1",
        intent_type="remove",
        content="用户A要求删除这个摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-stall",
        source_user_id="u2",
        intent_type="modify",
        content="用户B要求保留并调暗这个摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    proposal_id = coordinator.final_adjustment_plan(plan.plan_id)["conflicts"][0]["proposal_id"]
    disclosure_start = len(coordinator.disclosure_events)
    result = coordinator.confirm_final_adjustment_conflict(proposal_id, "host-a", decision="confirm")
    updated = coordinator.final_adjustment_plan(plan.plan_id)

    assert result.ok is True
    assert result.payload["proposal"]["status"] == "confirmed"
    assert updated["conflicts"] == []
    assert updated["resolved_conflicts"][0]["status"] == "confirmed"
    assert updated["resolved_conflicts"][0]["confirmed_by"] == "host-a"
    assert any(item["actor_id"] == "actor-stall" for item in updated["selected"])
    new_disclosures = coordinator.disclosure_events[disclosure_start:]
    assert any(
        item.audience == "host"
        and item.metadata["intervention"]["proposal_id"] == proposal_id
        and item.metadata["intervention"]["apply_policy"] == "confirmed"
        for item in new_disclosures
    )
    print("[OK] final adjustment conflict confirmation is recorded in Coordinator")


def test_rejected_final_adjustment_conflict_defers_target_adjustments():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-stall",
        source_user_id="u1",
        intent_type="remove",
        content="用户A要求删除这个摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-stall",
        source_user_id="u2",
        intent_type="modify",
        content="用户B要求保留并调暗这个摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    proposal_id = coordinator.final_adjustment_plan(plan.plan_id)["conflicts"][0]["proposal_id"]

    result = coordinator.confirm_final_adjustment_conflict(proposal_id, "host-a", decision="reject")
    updated = coordinator.final_adjustment_plan(plan.plan_id)

    assert result.ok is True
    assert updated["conflicts"] == []
    assert updated["resolved_conflicts"][0]["status"] == "rejected"
    assert not any(item["actor_id"] == "actor-stall" for item in updated["selected"])
    assert {
        item.get("defer_reason")
        for item in updated["deferred"]
        if item.get("actor_id") == "actor-stall"
    } == {"final_adjustment_conflict_rejected_by_host"}
    print("[OK] rejected final adjustment conflict defers target adjustments instead of executing them")


def test_final_adjustment_conflict_requires_non_empty_host_identity():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-stall",
        source_user_id="u1",
        intent_type="remove",
        content="用户A要求删除这个摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-stall",
        source_user_id="u2",
        intent_type="modify",
        content="用户B要求保留并调暗这个摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    proposal_id = coordinator.final_adjustment_plan(plan.plan_id)["conflicts"][0]["proposal_id"]

    empty_confirm = coordinator.confirm_final_adjustment_conflict(proposal_id, "", decision="confirm")
    empty_reject = coordinator.confirm_final_adjustment_conflict(proposal_id, "  ", decision="reject")
    updated = coordinator.final_adjustment_plan(plan.plan_id)

    assert empty_confirm.ok is False
    assert empty_reject.ok is False
    assert "房主确认身份" in empty_confirm.message
    assert "房主确认身份" in empty_reject.message
    assert updated["conflicts"][0]["proposal_id"] == proposal_id
    assert updated["conflicts"][0]["status"] == "proposed"
    assert updated["conflicts"][0]["confirmed_by"] == ""
    assert updated["resolved_conflicts"] == []
    print("[OK] final adjustment conflict rejects empty host identity")


def test_final_adjustment_conflict_uses_target_hint_when_actor_id_missing():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_batch_event(BatchEvent(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-4",
        stage="batch_boundary",
        status="done",
        intervention_window_open=True,
        metadata={"batch_index": 4},
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-4",
        source_user_id="u1",
        target_hint="入口摊位",
        intent_type="remove",
        content="用户A要求删除入口摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        batch_id="batch-4",
        source_user_id="u2",
        target_hint="入口摊位",
        intent_type="modify",
        content="用户B要求保留并调暗入口摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))

    adjustment = coordinator.final_adjustment_plan(plan.plan_id)

    assert adjustment["conflicts"]
    assert adjustment["conflicts"][0]["actor_id"] == ""
    assert adjustment["conflicts"][0]["target_hint"] == "入口摊位"
    assert adjustment["conflicts"][0]["reason"] == "same_target_has_remove_and_keep_modify_requests"
    print("[OK] final adjustment conflicts can use target_hint without actor_id")


def test_final_adjustment_conflict_links_target_hint_to_actor_bound_request():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        source_user_id="u1",
        target_hint="入口摊位",
        intent_type="remove",
        content="用户A要求删除入口摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-stall-generated",
        source_user_id="u2",
        target_hint="入口摊位",
        intent_type="modify",
        content="用户B要求保留并调暗入口摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))

    adjustment = coordinator.final_adjustment_plan(plan.plan_id)

    assert len(adjustment["conflicts"]) == 1
    assert adjustment["conflicts"][0]["actor_id"] == "actor-stall-generated"
    assert adjustment["conflicts"][0]["target_hint"] == "入口摊位"
    assert adjustment["conflicts"][0]["target_key"] == "入口摊位"
    assert adjustment["conflicts"][0]["reason"] == "same_actor_has_remove_and_keep_modify_requests"
    proposal_id = adjustment["conflicts"][0]["proposal_id"]

    repeated = coordinator.final_adjustment_plan(plan.plan_id)

    assert repeated["conflicts"][0]["proposal_id"] == proposal_id
    assert repeated["conflicts"][0]["target_key"] == "入口摊位"
    print("[OK] final adjustment conflict links target_hint-only and actor-bound requests")


def test_rejected_target_hint_conflict_defers_actor_bound_adjustment():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="host-a", is_host=True, text="室外暗黑集市"))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        source_user_id="u1",
        target_hint="入口摊位",
        intent_type="remove",
        content="用户A要求删除入口摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        source_user_id="u2",
        target_hint="入口摊位",
        intent_type="modify",
        content="用户B要求保留并调暗入口摊位",
        priority=2,
        apply_policy="final_adjustment",
    ))
    proposal_id = coordinator.final_adjustment_plan(plan.plan_id)["conflicts"][0]["proposal_id"]
    result = coordinator.confirm_final_adjustment_conflict(proposal_id, "host-a", decision="reject")
    coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        actor_id="actor-stall-generated",
        source_user_id="u3",
        target_hint="入口摊位",
        intent_type="style_adjust",
        content="后续批次已生成入口摊位，但仍想调暗",
        priority=2,
        apply_policy="final_adjustment",
    ))

    updated = coordinator.final_adjustment_plan(plan.plan_id)

    assert result.ok is True
    assert updated["conflicts"] == []
    assert updated["resolved_conflicts"][0]["status"] == "rejected"
    assert not any(item.get("actor_id") == "actor-stall-generated" for item in updated["selected"])
    assert {
        item.get("defer_reason")
        for item in updated["deferred"]
        if item.get("actor_id") == "actor-stall-generated"
    } == {"final_adjustment_conflict_rejected_by_host"}
    print("[OK] rejected target_hint final adjustment conflict defers later actor-bound adjustments")


def test_host_executor_uses_structured_handler_for_seed_plan_action():
    calls = {"agent": 0, "handler": 0}

    def agent_factory():
        def _agent(persona, messages):
            calls["agent"] += 1
            return "agent should not run"

        return _agent

    def handler(payload):
        calls["handler"] += 1
        assert payload["plan_id"] == "seed-1"
        return "SeedPlan seed-1 已进入生成队列：job-1 (queued)"

    executor = LanChatHostActionExecutor(
        agent_factory=agent_factory,
        engine_gate=FakeGate(),
        structured_action_handler=handler,
    )
    result = executor.enqueue_and_process({
        "action_type": "start_generation",
        "plan_id": "seed-1",
        "status": "confirmed",
        "seed_plan": {"plan_id": "seed-1", "room_id": "room-a", "status": "confirmed"},
    })

    assert result is not None
    assert result.ok is True
    assert result.event_type == "SceneDelta"
    assert calls == {"agent": 0, "handler": 1}
    print("[OK] host executor avoids natural-language reclassification for SeedPlan actions")


def test_host_executor_uses_structured_handler_for_post_generation_add_action():
    calls = {"agent": 0, "handler": 0}

    def agent_factory():
        def _agent(persona, messages):
            calls["agent"] += 1
            return "agent should not run"

        return _agent

    def handler(payload):
        calls["handler"] += 1
        assert payload["action_type"] == "post_generation_add"
        assert payload["plan_id"] == "seed-1"
        assert "天使雕像" in payload["intent_text"]
        return "追加生成请求已进入生成队列：job-append (queued)"

    executor = LanChatHostActionExecutor(
        agent_factory=agent_factory,
        engine_gate=FakeGate(),
        structured_action_handler=handler,
    )
    result = executor.enqueue_and_process({
        "action_type": "post_generation_add",
        "plan_id": "seed-1",
        "room_id": "room-a",
        "status": "confirmed",
        "intent_text": "添加生成一个天使雕像",
    })

    assert result is not None
    assert result.ok is True
    assert result.event_type == "SceneDelta"
    assert calls == {"agent": 0, "handler": 1}
    print("[OK] host executor routes post-generation add through structured handler")


def test_coordinator_executes_post_generation_add_as_append_job():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="做一个温暖的夜晚幻想集市，不要太恐怖，有入口、摊位和灯光",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    plan.status = SeedPlanStatus.COMPLETED

    message = coordinator.execute_action_payload({
        "action_type": "post_generation_add",
        "plan_id": plan.plan_id,
        "room_id": "room-a",
        "source_user_id": "host-a",
        "status": "confirmed",
        "intent_text": "添加生成一个天使雕像",
    })

    assert "追加生成请求已进入生成队列" in message
    assert scheduler.submitted
    submitted = scheduler.submitted[-1]
    assert submitted["job_type"] == "scene_generation_append"
    assert submitted["action_type"] == "post_generation_add"
    assert submitted["append_mode"] is True
    assert submitted["max_items"] == 2
    assert submitted["prompt"] == "添加生成一个天使雕像"
    assert submitted["pending_interventions"][0]["apply_policy"] == "post_generation_add"
    assert "天使雕像" in submitted["pending_interventions"][0]["content"]
    assert submitted["scene_design_contract"]["plan_id"] == plan.plan_id
    assert coordinator.pending_interventions(plan.plan_id)[-1].apply_policy == "post_generation_add"
    print("[OK] Coordinator submits post-generation add as append generation job")


def test_coordinator_keeps_generation_add_in_next_batch_while_executing():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="房主",
        is_host=True,
        text="做一个夜晚幻想集市",
    ))
    plan = coordinator.propose_seed_plan("room-a")
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    coordinator.execute_confirmed_plan(plan.plan_id)
    existing_session = coordinator._active_generation_session_by_plan[plan.plan_id]  # noqa: SLF001
    submitted_count = len(scheduler.submitted)

    message = coordinator.execute_action_payload({
        "action_type": "post_generation_add",
        "plan_id": plan.plan_id,
        "room_id": "room-a",
        "source_user_id": "host-a",
        "status": "confirmed",
        "intent_text": "新增一只小狗",
    })

    assert message == "已记录追加请求，将在下一批前吸收。"
    assert len(scheduler.submitted) == submitted_count
    assert coordinator._active_generation_session_by_plan[plan.plan_id] == existing_session  # noqa: SLF001
    assert coordinator.pending_interventions(plan.plan_id)[-1].apply_policy == "next_batch"
    print("[OK] Coordinator does not open append job while main generation is still executing")


def test_coordinator_event_histories_are_bounded():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    for index in range(MAX_COORDINATOR_EVENTS + 3):
        coordinator._record("synthetic_event", f"event-{index}", {"index": index})  # noqa: SLF001
    for index in range(MAX_COORDINATOR_DISCLOSURE_EVENTS + 3):
        coordinator._record_disclosures(  # noqa: SLF001
            room_id="room-a",
            stage="batch",
            progress=index % 100,
            plan={"plan_id": "plan-a", "room_id": "room-a"},
            intervention={"intent_type": "batch_boundary", "status_message": f"batch-{index}"},
        )

    assert len(coordinator.events) == MAX_COORDINATOR_EVENTS
    assert coordinator.events[0].message == "event-3"
    assert coordinator.events[-1].message == f"event-{MAX_COORDINATOR_EVENTS + 2}"
    assert len(coordinator.disclosure_events) == MAX_COORDINATOR_DISCLOSURE_EVENTS
    assert coordinator.disclosure_events[-1].metadata["intervention"]["status_message"] == (
        f"batch-{MAX_COORDINATOR_DISCLOSURE_EVENTS + 2}"
    )
    late_events, cursor_advance = coordinator.disclosure_events_since(0)
    assert len(late_events) == MAX_COORDINATOR_DISCLOSURE_EVENTS
    assert cursor_advance == coordinator._disclosure_events_start_index + len(coordinator.disclosure_events)  # noqa: SLF001
    assert cursor_advance > len(late_events)
    assert late_events[-1].metadata["intervention"]["status_message"] == (
        f"batch-{MAX_COORDINATOR_DISCLOSURE_EVENTS + 2}"
    )
    print("[OK] Coordinator event and disclosure histories are bounded")


def test_coordinator_pending_interventions_are_bounded_without_dropping_critical_items():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    plan = coordinator.create_or_update_seed_plan(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        sender_name="Host",
        is_host=True,
        text="做一个可分批生成的室内外混合场景",
    ))
    protected = [
        InterventionRequest(
            room_id="room-a",
            plan_id=plan.plan_id,
            content="执行错方案，先暂停重新整理",
            priority=2,
            apply_policy="pause_and_replan",
            intervention_id="iv-protected-pause",
        ),
        InterventionRequest(
            room_id="room-a",
            plan_id=plan.plan_id,
            content="最终收尾时修正入口摊位穿模",
            actor_id="actor-stall",
            priority=1,
            apply_policy="final_adjustment",
            intervention_id="iv-protected-final",
        ),
        InterventionRequest(
            room_id="room-a",
            plan_id=plan.plan_id,
            content="几何审查：桥面和地形接缝错误",
            target_hint="bridge seam",
            finding_details=[{"target_hint": "bridge seam", "issue": "overlap"}],
            priority=1,
            apply_policy="geometry_review",
            intervention_id="iv-protected-geometry",
        ),
    ]
    for item in protected:
        coordinator.ingest_intervention(item)
    for index in range(MAX_PENDING_INTERVENTIONS_PER_PLAN + 12):
        coordinator.ingest_intervention(InterventionRequest(
            room_id="room-a",
            plan_id=plan.plan_id,
            content=f"低优先级旧补充 {index}",
            priority=0,
            apply_policy="next_batch",
            intervention_id=f"iv-low-{index}",
        ))

    pending = coordinator.pending_interventions(plan.plan_id)
    pending_ids = {item.intervention_id for item in pending}

    assert len(pending) == MAX_PENDING_INTERVENTIONS_PER_PLAN
    assert {"iv-protected-pause", "iv-protected-final", "iv-protected-geometry"} <= pending_ids
    assert "iv-low-0" not in pending_ids
    assert f"iv-low-{MAX_PENDING_INTERVENTIONS_PER_PLAN + 11}" in pending_ids
    assert coordinator.final_adjustment_plan(plan.plan_id)["selected"]
    print("[OK] Coordinator pending interventions are bounded without dropping critical items")


def test_coordinator_resolved_proposal_histories_are_bounded_without_dropping_pending():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="u1", text="我想要红墙集市"))
    coordinator.ingest_message(ChatMessage(room_id="room-a", sender_id="u2", text="这里有冲突：我想要蓝墙集市"))
    plan = coordinator.propose_seed_plan("room-a")
    plan.conflicts.append("用户A要红墙，用户B要蓝墙")

    pending_ids = []
    for index in range(3):
        proposal = coordinator.propose_conflict_resolution(
            plan.plan_id,
            proposed_by="gm",
            recommendation=f"保留待确认冲突方案 {index}",
        )
        pending_ids.append(proposal.proposal_id)
    resolved_ids = []
    for index in range(MAX_RESOLVED_COORDINATOR_PROPOSALS + 5):
        proposal = coordinator.propose_conflict_resolution(
            plan.plan_id,
            proposed_by="gm",
            recommendation=f"已解决冲突方案 {index}",
        )
        resolved_ids.append(proposal.proposal_id)
        coordinator.confirm_conflict_resolution(proposal.proposal_id, "host-a")

    stored = coordinator._pending_conflict_resolutions  # noqa: SLF001
    stored_resolved = [
        proposal for proposal in stored.values()
        if proposal.status in {"confirmed", "rejected"}
    ]

    assert len(stored_resolved) == MAX_RESOLVED_COORDINATOR_PROPOSALS
    assert resolved_ids[0] not in stored
    assert resolved_ids[-1] in stored
    assert all(proposal_id in stored for proposal_id in pending_ids)
    assert all(stored[proposal_id].status == "proposed" for proposal_id in pending_ids)
    print("[OK] Coordinator resolved proposal histories are bounded without dropping pending proposals")


def test_coordinator_resolved_final_adjustment_conflicts_are_bounded():
    coordinator = InteractionCoordinator(scheduler=FakeScheduler())
    for index in range(3):
        coordinator._pending_final_adjustment_conflicts[f"fa-pending-{index}"] = {  # noqa: SLF001
            "proposal_id": f"fa-pending-{index}",
            "plan_id": "plan-a",
            "room_id": "room-a",
            "status": "proposed",
            "updated_at": index,
        }
    for index in range(MAX_RESOLVED_COORDINATOR_PROPOSALS + 5):
        coordinator._pending_final_adjustment_conflicts[f"fa-resolved-{index}"] = {  # noqa: SLF001
            "proposal_id": f"fa-resolved-{index}",
            "plan_id": "plan-a",
            "room_id": "room-a",
            "status": "confirmed" if index % 2 == 0 else "rejected",
            "updated_at": index,
        }

    coordinator._prune_resolved_final_adjustment_conflicts()  # noqa: SLF001
    stored = coordinator._pending_final_adjustment_conflicts  # noqa: SLF001
    resolved = [
        item for item in stored.values()
        if item.get("status") in {"confirmed", "rejected"}
    ]

    assert len(resolved) == MAX_RESOLVED_COORDINATOR_PROPOSALS
    assert "fa-resolved-0" not in stored
    assert f"fa-resolved-{MAX_RESOLVED_COORDINATOR_PROPOSALS + 4}" in stored
    assert all(f"fa-pending-{index}" in stored for index in range(3))
    print("[OK] Coordinator resolved final-adjustment conflict history is bounded")


def test_coordinator_generation_job_refs_are_bounded_for_future_updates():
    scheduler = FakeUpdatableScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    plan = coordinator.create_or_update_seed_plan(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="做一个多批次室外集市",
    ))
    for index in range(MAX_GENERATION_JOB_REFS_PER_PLAN + 5):
        job_id = f"job-{index}"
        scheduler.status_by_job[job_id] = "queued"
        coordinator._remember_generation_job_ref(GenerationJobRef(  # noqa: SLF001
            job_id=job_id,
            plan_id=plan.plan_id,
            status="queued",
        ))

    decision = coordinator.ingest_intervention(InterventionRequest(
        room_id="room-a",
        plan_id=plan.plan_id,
        content="下一批把入口加宽一点",
        priority=1,
        apply_policy="next_batch",
    ))
    stored_refs = coordinator._generation_jobs_by_plan[plan.plan_id]  # noqa: SLF001
    updated_job_ids = [item["job_id"] for item in scheduler.updates]

    assert decision.accepted is True
    assert len(stored_refs) == MAX_GENERATION_JOB_REFS_PER_PLAN
    assert stored_refs[0].job_id == "job-5"
    assert stored_refs[-1].job_id == f"job-{MAX_GENERATION_JOB_REFS_PER_PLAN + 4}"
    assert "job-0" not in updated_job_ids
    assert updated_job_ids[0] == "job-5"
    assert updated_job_ids[-1] == f"job-{MAX_GENERATION_JOB_REFS_PER_PLAN + 4}"
    assert len(updated_job_ids) == MAX_GENERATION_JOB_REFS_PER_PLAN
    print("[OK] Coordinator generation job refs are bounded for future updates")


def test_generation_job_done_marks_seed_plan_completed_for_status_query():
    scheduler = FakeScheduler()
    coordinator = InteractionCoordinator(scheduler=scheduler)
    plan = coordinator.create_or_update_seed_plan(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="做一个温暖的夜晚幻想集市",
    ))
    plan.propose()
    coordinator.confirm_seed_plan(plan.plan_id, "host-a")
    ref = coordinator.execute_confirmed_plan(plan.plan_id)

    event = coordinator.ingest_generation_job_status({
        "job_id": ref.job_id,
        "room_id": "room-a",
        "plan_id": plan.plan_id,
        "session_id": ref.session_id,
        "status": "done",
    })
    status_event = coordinator.ingest_message(ChatMessage(
        room_id="room-a",
        sender_id="host-a",
        is_host=True,
        text="@GM 现在生成到哪里了",
    ))

    assert event.event_type == "generation_completed"
    assert coordinator.active_plan_for_room("room-a").status == SeedPlanStatus.COMPLETED
    assert "已完成" in status_event.message
    assert "正在生成" not in status_event.message
    print("[OK] generation job done marks SeedPlan completed for status query")


if __name__ == "__main__":
    test_chat_updates_seed_plan_without_generation()
    test_confirmed_seed_plan_executes_through_scheduler()
    test_confirmed_seed_plan_creates_scene_design_contract_with_negative_preferences()
    test_seed_plan_design_brief_survives_confirmation_phrases()
    test_seed_plan_confirmation_rejects_confirmation_only_context()
    test_seed_plan_design_brief_ignores_opinion_and_elaboration_queries()
    test_status_query_does_not_create_intervention_or_generation_job()
    test_completed_generation_add_routes_to_post_generation_add()
    test_completed_generation_modify_routes_to_final_adjustment()
    test_completed_layout_reflow_confirmation_confirms_latest_proposal()
    test_completed_generation_boundary_alias_routes_to_final_adjustment()
    test_generation_add_target_hint_strips_followup_verbs_and_mentions()
    test_execute_confirmed_plan_payload_uses_full_contract_prompt()
    test_execute_confirmed_plan_sets_lanchat_runtime_executing()
    test_system_actor_aliases_are_canonicalized_for_terrain_boundary()
    test_seed_plan_confirmation_requires_non_empty_host_identity()
    test_gm_conflict_resolution_requires_host_confirmation_before_plan_confirm()
    test_gm_conflict_resolution_requires_non_empty_host_identity()
    test_gm_conflict_resolution_rejection_blocks_later_confirmation()
    test_generation_interventions_are_routed_by_intent()
    test_prefilled_disclosure_drafts_route_to_expected_interventions()
    test_gm_pace_control_enters_coordinator_and_scheduler()
    test_clarification_blocks_confirmation_until_answered()
    test_paused_plan_can_spawn_gm_replan_version_for_host_confirmation()
    test_next_batch_intervention_updates_queued_generation_job()
    test_next_batch_intervention_reports_deferred_when_generation_already_running()
    test_intervention_extracts_actor_target_for_final_adjustment_conflicts()
    test_direct_intervention_finding_details_are_sanitized()
    test_coordinator_records_scoped_memory_for_plan_lifecycle()
    test_batch_event_opens_intervention_window_and_records_memory()
    test_batch_event_rejects_stale_generation_session_without_disclosure()
    test_batch_event_rejects_unknown_or_foreign_plan_without_disclosure()
    test_coordinator_binds_scene_session_progress_as_batch_events()
    test_failed_review_routes_structured_intervention()
    test_review_result_dict_false_string_routes_failed_review()
    test_review_result_rejects_stale_generation_session_without_intervention()
    test_review_result_rejects_unknown_or_foreign_plan_without_intervention()
    test_memory_summary_can_focus_on_actor_history()
    test_review_result_finding_details_feed_final_adjustment_target_hint()
    test_final_adjustment_plan_prefers_recent_strong_interventions()
    test_final_adjustment_conflict_requires_host_confirmation()
    test_final_adjustment_conflict_confirmation_is_recorded()
    test_rejected_final_adjustment_conflict_defers_target_adjustments()
    test_final_adjustment_conflict_requires_non_empty_host_identity()
    test_final_adjustment_conflict_uses_target_hint_when_actor_id_missing()
    test_final_adjustment_conflict_links_target_hint_to_actor_bound_request()
    test_rejected_target_hint_conflict_defers_actor_bound_adjustment()
    test_host_executor_uses_structured_handler_for_seed_plan_action()
    test_host_executor_uses_structured_handler_for_post_generation_add_action()
    test_coordinator_executes_post_generation_add_as_append_job()
    test_coordinator_keeps_generation_add_in_next_batch_while_executing()
    test_coordinator_event_histories_are_bounded()
    test_coordinator_pending_interventions_are_bounded_without_dropping_critical_items()
    test_coordinator_resolved_proposal_histories_are_bounded_without_dropping_pending()
    test_coordinator_resolved_final_adjustment_conflicts_are_bounded()
    test_coordinator_generation_job_refs_are_bounded_for_future_updates()
    test_generation_job_done_marks_seed_plan_completed_for_status_query()
