from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from plugins.AITool.services.seed_plan import ParticipantIntent, SeedPlan, SeedPlanStatus  # noqa: E402


def test_seed_plan_freezes_after_confirmation():
    plan = SeedPlan(room_id="room-a")
    plan.add_intent(ParticipantIntent(user_id="u1", user_name="用户A", text="做一个暗黑集市"))
    plan.propose()
    plan.confirm("host-a")

    assert plan.status == SeedPlanStatus.CONFIRMED
    assert plan.confirmed_by == "host-a"
    assert "暗黑集市" in plan.intent_summary

    try:
        plan.add_intent(ParticipantIntent(user_id="u2", text="再加一个天使雕塑"))
    except ValueError as exc:
        assert "frozen" in str(exc)
    else:
        raise AssertionError("confirmed SeedPlan must be frozen")

    print("[OK] SeedPlan freezes after host confirmation")


def test_seed_plan_requires_confirmation_before_execution():
    plan = SeedPlan(room_id="room-a")
    try:
        plan.mark_executing()
    except ValueError as exc:
        assert "confirmed" in str(exc)
    else:
        raise AssertionError("draft SeedPlan must not execute")

    plan.confirm("host-a")
    plan.mark_executing()
    assert plan.status == SeedPlanStatus.EXECUTING
    print("[OK] SeedPlan execution requires confirmation")


def test_seed_plan_rejects_empty_host_confirmation():
    plan = SeedPlan(room_id="room-a")
    plan.propose()

    try:
        plan.confirm("  ")
    except ValueError as exc:
        assert "non-empty host_id" in str(exc)
    else:
        raise AssertionError("SeedPlan confirmation must require a non-empty host_id")

    assert plan.status == SeedPlanStatus.PROPOSED
    assert plan.confirmed_by == ""
    print("[OK] SeedPlan rejects empty host confirmation")


def test_seed_plan_can_pause_confirmed_or_executing_generation():
    plan = SeedPlan(room_id="room-a")
    plan.confirm("host-a")
    plan.pause_execution()
    assert plan.status == SeedPlanStatus.PAUSED

    executing = SeedPlan(room_id="room-b")
    executing.confirm("host-b")
    executing.mark_executing()
    executing.pause_execution()
    assert executing.status == SeedPlanStatus.PAUSED
    print("[OK] SeedPlan can pause confirmed or executing generation")


def test_seed_plan_round_trips_to_dict():
    plan = SeedPlan(room_id="room-a", scene_type="outdoor")
    plan.add_intent(ParticipantIntent(user_id="u1", text="室外暗黑集市，注意比例和接地"))
    plan.style_constraints.append("暗黑集市")
    plan.placement_constraints.append("注意比例和接地")

    restored = SeedPlan.from_dict(plan.as_dict())

    assert restored.plan_id == plan.plan_id
    assert restored.scene_type == "outdoor"
    assert restored.participants[0].text == "室外暗黑集市，注意比例和接地"
    assert restored.style_constraints == ["暗黑集市"]
    print("[OK] SeedPlan serializes for structured action payloads")


def test_seed_plan_design_brief_round_trips_to_dict():
    plan = SeedPlan(room_id="room-a", scene_type="outdoor")
    plan.set_design_brief(
        "暗黑小巷鬼市：窄巷、三处摊位、红灯笼门牌。",
        ["药剂摊", "符咒摊", "骨器摊"],
        source="lanchat_discussion",
    )

    restored = SeedPlan.from_dict(plan.as_dict())

    assert restored.design_brief == "暗黑小巷鬼市：窄巷、三处摊位、红灯笼门牌。"
    assert restored.design_items == ["药剂摊", "符咒摊", "骨器摊"]
    assert restored.design_source == "lanchat_discussion"
    print("[OK] SeedPlan design brief serializes as authoritative context")


def test_seed_plan_review_policy_output_is_sanitized():
    plan = SeedPlan(room_id="room-a")
    plan.review_policy = {
        "conflict_resolutions": [{
            "proposal_id": "cr-1",
            "recommendation": "保留入口灯，删除重复摊位",
            "status": "confirmed",
            "prompt": "PRIVATE_REVIEW_POLICY_PROMPT_SHOULD_NOT_LEAK",
            "provider": "review-policy-provider-secret",
            "job_id": "review-policy-job-secret",
            "runtime_context": {"token": "review-policy-token-secret"},
            "scheduler_updates": [{"session_id": "review-policy-session-secret"}],
            "hidden_debug_ref": "review-policy-debug-secret",
        }],
        "clarification_requests": [{
            "question": "入口区域优先通行还是装饰？",
            "target_user_id": "u1",
            "raw_prompt": "PRIVATE_CLARIFICATION_PROMPT_SHOULD_NOT_LEAK",
        }],
    }

    payload = plan.as_dict()
    exposed_text = repr(payload)

    assert payload["review_policy"]["conflict_resolutions"][0]["proposal_id"] == "cr-1"
    assert payload["review_policy"]["conflict_resolutions"][0]["recommendation"] == "保留入口灯，删除重复摊位"
    assert payload["review_policy"]["clarification_requests"][0]["question"] == "入口区域优先通行还是装饰？"
    assert "PRIVATE_REVIEW_POLICY_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    assert "review-policy-provider-secret" not in exposed_text
    assert "review-policy-job-secret" not in exposed_text
    assert "review-policy-token-secret" not in exposed_text
    assert "review-policy-session-secret" not in exposed_text
    assert "review-policy-debug-secret" not in exposed_text
    assert "PRIVATE_CLARIFICATION_PROMPT_SHOULD_NOT_LEAK" not in exposed_text
    print("[OK] SeedPlan review_policy output is sanitized")


if __name__ == "__main__":
    test_seed_plan_freezes_after_confirmation()
    test_seed_plan_requires_confirmation_before_execution()
    test_seed_plan_rejects_empty_host_confirmation()
    test_seed_plan_can_pause_confirmed_or_executing_generation()
    test_seed_plan_round_trips_to_dict()
    test_seed_plan_design_brief_round_trips_to_dict()
    test_seed_plan_review_policy_output_is_sanitized()
