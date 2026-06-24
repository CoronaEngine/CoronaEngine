"""离线自验：progressive 混合环境几何约束接线。

覆盖目标：
- indoor/outdoor/shell zone 推断不依赖具体"蒙古包"关键词。
- 资产分流会写入 zone_id，避免后续 AABB zone 检查误伤室外物体。
- connector 能派生 door clearance AABB，用于防挡门/防穿模内回路。
"""
import os
import sys
import types
from dataclasses import dataclass, field
from pathlib import Path
from types import SimpleNamespace
from typing import List

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from cai_extensions.agent.scene_composer_progressive import (  # noqa: E402
    _apply_pending_notes_to_batch,
    _build_micro_batch_phase_plan,
    _collect_door_clearance_aabbs,
    _collect_zone_aabbs,
    _distribute_assets_to_phases,
    _emit_aabb_review_results,
    _emit_vlm_review_results,
    _filter_aabbs_by_zone,
    _generate_post_shell_framework,
    _infer_primary_zone_ids,
    _merge_final_and_vlm_review_text,
    _prioritize_high_risk_vlm_targets,
    _prioritize_vlm_targets,
    _run_vlm_advisory_review,
    _resolve_pending_resource_requests_for_batch,
    _vlm_checkpoint_user_message,
    _vlm_checkpoint_reports_user_text,
    _vlm_max_targets,
    build_batch_resource_plan,
    VlmCheckpointPolicy,
)
from cai_extensions.agent.scene_composer import SceneComposer, apply_scene_semantic_terrain_profile  # noqa: E402
from cai_extensions.agent.scene_session import SceneSession  # noqa: E402
from cai_extensions.data_model.zone_tree import Connector, Volume, Zone, ZoneAspect, ZoneTree  # noqa: E402
from services.terrain_component_resolver import TerrainComponentResolver  # noqa: E402


def _test_temp_root() -> str:
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "..", ".tmp", "test-temp"))
    os.makedirs(root, exist_ok=True)
    return root


def _named_test_dir(name: str) -> str:
    path = os.path.join(_test_temp_root(), name)
    os.makedirs(path, exist_ok=True)
    return path


@dataclass
class FakeInstance:
    instance_id: str
    zone_id: str
    layout_status: str = "active"


class FakeLayout:
    def __init__(self, instances: List[FakeInstance]):
        self._instances = instances

    def list_active(self):
        return [i for i in self._instances if i.layout_status == "active"]


class FakeComposer:
    def __init__(self):
        outdoor = Zone(
            zone_id="grassland",
            name="open field",
            role="outdoor",
            enclosure="terrain",
            volume=Volume(center=[0.0, 0.0, 0.0], size=[20.0, 20.0, 0.0]),
            aspects=[ZoneAspect(capability="boundary", params={"kind": "fence", "radius": 7.0})],
        )
        indoor = Zone(
            zone_id="main_building",
            name="main shell",
            role="indoor",
            enclosure="shell",
            volume=Volume(center=[0.0, 1.5, 0.0], size=[6.0, 6.0, 3.0]),
            primary_shell_asset_id="main shell",
            aspects=[ZoneAspect(capability="foundation_surface", params={"material": "stone", "shape": "quad", "padding": 0.8})],
        )
        indoor.connectors.append(Connector(
            connector_id="door_main",
            type="door",
            position=[0.0, 0.0, 3.0],
            size=[1.2, 2.0],
            target_zone_id="main_building",
        ))
        outdoor.sub_zones.append(indoor)
        self.zone_tree = ZoneTree(root=outdoor)
        self.floors = []
        self.foundations = []
        self.fences = []
        self.anchors = []
        self._terrain_extent = {"extent": 20.0, "width": 20.0, "depth": 20.0}
        self._shell_aabb = {
            "main shell": {"half_x": 3.0, "half_z": 3.0, "height": 4.0},
        }

    def _generate_interior_floor(self, zone):
        self.floors.append(zone.zone_id)

    def _generate_foundation_surface(self, zone):
        self.foundations.append(zone.zone_id)
        self._foundation_extent = {"width": 7.6, "depth": 7.6}

    def _generate_fence(self, params, anchor=None):
        self.fences.append(dict(params or {}))
        self.anchors.append(dict(anchor or {}))


class FakeCoordinator:
    def __init__(self):
        self.reviews = []

    def ingest_review_result(self, payload):
        self.reviews.append(dict(payload))
        return []


class FakeEngineGate:
    def __init__(self):
        self.screenshots = []

    def screenshot(self, fn, *args, **kwargs):
        self.screenshots.append((fn, args, kwargs))
        return fn(*args, **kwargs)


def test_zone_and_asset_routing():
    composer = FakeComposer()
    indoor, outdoor, shell = _infer_primary_zone_ids(composer)
    assert indoor == "main_building"
    assert outdoor == "grassland"
    assert shell == "main_building"

    phase_map = _distribute_assets_to_phases(
        [
            {"name": "wooden table", "model_path": "/m/table.glb"},
            {"name": "campfire", "model_path": "/m/fire.glb"},
            {"name": "fence", "model_path": "/m/fence.glb"},
            {"name": "lamp", "model_path": "/m/lamp.glb"},
            {"name": "angel statue", "model_path": "/m/statue.glb"},
            {"name": "fountain", "model_path": "/m/fountain.glb"},
        ],
        [],
        composer,
    )
    interior = phase_map["INTERIOR"][0]
    outdoor_object = phase_map["OBJECTS"][0]
    boundary = phase_map["BOUNDARY"][0]
    decoration = phase_map["DECORATION"][0]

    assert interior["zone_id"] == "main_building"
    assert interior["anchor_ref"] == "main_building"
    assert outdoor_object["zone_id"] == "grassland"
    assert boundary["zone_id"] == "grassland"
    assert decoration["zone_id"] == "main_building"
    outdoor_names = {a["name"]: a["zone_id"] for a in phase_map["OBJECTS"]}
    assert outdoor_names["angel statue"] == "grassland"
    assert outdoor_names["fountain"] == "grassland"
    positions = {a["name"]: tuple(a.get("pos", [])) for phase in phase_map.values() for a in phase}
    scales = {a["name"]: tuple(a.get("scale", [])) for phase in phase_map.values() for a in phase}
    roles = {a["name"]: a.get("layout_role") for phase in phase_map.values() for a in phase}
    assert positions["wooden table"] != positions["fountain"]
    assert positions["angel statue"] != (0.0, 0.0, 0.0)
    assert positions["fountain"] != (0.0, 0.0, 0.0)
    assert roles["fountain"] == "landmark"
    assert roles["angel statue"] == "landmark"
    assert scales["fountain"][0] >= 1.15
    assert scales["angel statue"][0] >= 1.25
    print("[OK] mixed environment asset routing writes zone_id / anchor_ref")


def test_zone_and_door_aabb_helpers():
    composer = FakeComposer()
    zone_aabbs = _collect_zone_aabbs(composer.zone_tree)
    door_aabbs = _collect_door_clearance_aabbs(composer.zone_tree)

    assert zone_aabbs["grassland"] == [-10.0, -0.1, -10.0, 10.0, 0.1, 10.0]
    assert zone_aabbs["main_building"] == [-3.0, 0.0, -3.0, 3.0, 3.0, 3.0]
    assert "door_main" in door_aabbs
    assert door_aabbs["door_main"][2] < 3.0 < door_aabbs["door_main"][5]
    print("[OK] ZoneTree derives zone AABB and door clearance AABB")


def test_indoor_room_slot_planner_uses_asset_semantics():
    composer = FakeComposer()
    phase_map = _distribute_assets_to_phases(
        [
            {"name": "儿童床", "model_path": "/m/bed.glb"},
            {"name": "书桌", "model_path": "/m/desk.glb"},
            {"name": "椅子", "model_path": "/m/chair.glb"},
            {"name": "衣柜", "model_path": "/m/wardrobe.glb"},
            {"name": "书架", "model_path": "/m/bookshelf.glb"},
            {"name": "地毯", "model_path": "/m/rug.glb"},
            {"name": "台灯", "model_path": "/m/lamp.glb"},
            {"name": "玩具柜", "model_path": "/m/toy.glb"},
        ],
        [],
        composer,
    )
    rows = {a["name"]: a for phase in phase_map.values() for a in phase}
    assert rows["地毯"]["layout_role"] == "surface"
    assert rows["儿童床"]["pos"][2] < 0.0
    assert rows["书桌"]["pos"][0] < 0.0
    assert rows["椅子"]["pos"] != rows["书桌"]["pos"]
    assert rows["台灯"]["pos"] != [0.0, 0.0, 0.0]
    unique_positions = {tuple(row["pos"]) for row in rows.values()}
    assert len(unique_positions) >= 6
    print("[OK] indoor room slot planner separates large furniture, surface, and dependents")


def test_filter_aabbs_by_zone():
    layout = FakeLayout([
        FakeInstance("table", "main_building"),
        FakeInstance("campfire", "grassland"),
    ])
    aabbs = {
        "table": [-0.5, 0.0, -0.5, 0.5, 1.0, 0.5],
        "campfire": [4.0, 0.0, 4.0, 5.0, 1.0, 5.0],
    }
    indoor = _filter_aabbs_by_zone(layout, aabbs, "main_building")
    outdoor = _filter_aabbs_by_zone(layout, aabbs, "grassland")
    assert list(indoor) == ["table"]
    assert list(outdoor) == ["campfire"]
    print("[OK] AABB zone checks are scoped by LayoutInstance.zone_id")


def test_micro_batch_plan_splits_content_phases():
    phase_assets = {
        "GROUND": [],
        "SHELL": [],
        "INTERIOR": [{"name": f"家具{i}", "layout_role": "furniture"} for i in range(7)],
        "BOUNDARY": [{"name": "围栏"}],
        "OBJECTS": [{"name": f"室外物{i}", "layout_role": "support"} for i in range(5)],
        "DECORATION": [{"name": f"装饰{i}", "layout_role": "decoration"} for i in range(4)],
    }
    sequence, metadata, batches = _build_micro_batch_phase_plan(phase_assets)
    assert sequence == [
        "INTERIOR#1", "INTERIOR#2", "INTERIOR#3",
        "BOUNDARY",
        "OBJECTS#1", "OBJECTS#2",
        "DECORATION#1", "DECORATION#2",
    ]
    assert metadata["INTERIOR#1"]["batch_index"] == 1
    assert metadata["INTERIOR#3"]["batch_total"] == 3
    assert len(batches["OBJECTS#1"]) == 3
    assert len(batches["DECORATION#2"]) == 1
    print("[OK] content phases split into real micro-batches")


def _legacy_test_pending_notes_apply_to_next_batch_context():
    session = SimpleNamespace(pending_tasks=[])
    assets = [
        {"name": "摊位", "layout_role": "foreground_object"},
        {"name": "玩具柜", "layout_role": "decoration"},
    ]
    notes = [
        SimpleNamespace(kind="generation_delta", text="后面再加灯串和发光蘑菇", source_agent="小女孩"),
        SimpleNamespace(kind="layout_constraint", text="后续不要挡中央活动区", source_agent="学者"),
        SimpleNamespace(kind="edit_existing", text="放大摊位", source_agent="小女孩"),
        SimpleNamespace(kind="generation_delta", text="不要再生成玩具柜", source_agent="长者"),
    ]
    out = _apply_pending_notes_to_batch(assets, notes, session)
    assert [item["name"] for item in out] == ["摊位"]
    assert session.pending_tasks
    statuses = {item["kind"]: item["status"] for item in session.pending_tasks}
    assert statuses["layout_constraint"] == "applied_to_batch_context"
    assert statuses["edit_existing"] == "queued_edit_or_waiting_for_actor"
    assert out[0]["runtime_generation_context"] == ["后面再加灯串和发光蘑菇"]
    assert out[0]["runtime_layout_constraints"] == ["后续不要挡中央活动区"]
    print("[OK] pending scene notes enter next-batch context and can remove forbidden future assets")


def test_pending_notes_apply_to_next_batch_context():
    session = SimpleNamespace(pending_tasks=[])
    assets = [
        {"name": "market stall", "layout_role": "foreground_object", "pos": [0.0, 0.0, 0.0]},
        {"name": "toy cabinet", "layout_role": "decoration", "pos": [1.0, 0.0, 0.0]},
    ]
    notes = [
        SimpleNamespace(kind="generation_delta", text="add lantern strings and glowing mushrooms later", source_agent="girl"),
        SimpleNamespace(kind="layout_constraint", text="keep central activity area clear", source_agent="scholar"),
        SimpleNamespace(kind="edit_existing", text="scale up stall", source_agent="girl"),
        SimpleNamespace(kind="generation_delta", text="do not generate toy cabinet", source_agent="elder"),
    ]
    out = _apply_pending_notes_to_batch(assets, notes, session)
    assert [item["name"] for item in out] == ["market stall"]
    statuses = [item["status"] for item in session.pending_tasks]
    assert "applied_to_next_batch_layout" in statuses
    assert "queued_edit_or_waiting_for_actor" in statuses
    assert "deferred_missing_asset" in statuses
    assert "applied_removed_from_remaining" in statuses
    assert out[0]["pos"] != [0.0, 0.0, 0.0]
    assert out[0]["runtime_generation_context"] == ["add lantern strings and glowing mushrooms later"]
    assert out[0]["runtime_layout_constraints"] == ["keep central activity area clear"]
    print("[OK] pending scene notes can mutate next batch positions and remove forbidden assets")


def test_pending_generation_delta_inserts_future_asset():
    session = SimpleNamespace(pending_tasks=[])
    current = [
        {"name": "market stall", "layout_role": "foreground_object"},
    ]
    micro_batches = {
        "OBJECTS#1": current,
        "DECORATION#1": [
            {"name": "lantern strings", "layout_role": "decoration"},
            {"name": "glowing mushrooms", "layout_role": "decoration"},
        ],
    }
    notes = [
        SimpleNamespace(kind="generation_delta", text="add lantern strings and glowing mushrooms later", source_agent="girl"),
    ]
    out = _apply_pending_notes_to_batch(
        current,
        notes,
        session,
        current_phase="OBJECTS#1",
        micro_phase_assets=micro_batches,
        phase_sequence=["OBJECTS#1", "DECORATION#1"],
        max_batch_size=3,
    )
    names = [item["name"] for item in out]
    assert names == ["market stall", "lantern strings", "glowing mushrooms"]
    assert micro_batches["DECORATION#1"] == []
    assert session.pending_tasks[-1]["status"] == "inserted_into_remaining_batch"
    assert session.pending_tasks[-1]["affected_assets"] == ["lantern strings", "glowing mushrooms"]
    assert out[1]["source"] == "USER_PENDING_DELTA"
    print("[OK] positive generation_delta pulls matching future assets into current batch")


def test_pending_generation_delta_can_remove_future_asset():
    session = SimpleNamespace(pending_tasks=[])
    current = [{"name": "market stall", "layout_role": "foreground_object"}]
    micro_batches = {
        "OBJECTS#1": current,
        "OBJECTS#2": [
            {"name": "cargo box", "layout_role": "support"},
            {"name": "sign board", "layout_role": "support"},
        ],
    }
    notes = [
        SimpleNamespace(kind="generation_delta", text="do not generate cargo box", source_agent="elder"),
    ]
    out = _apply_pending_notes_to_batch(
        current,
        notes,
        session,
        current_phase="OBJECTS#1",
        micro_phase_assets=micro_batches,
        phase_sequence=["OBJECTS#1", "OBJECTS#2"],
    )
    assert [item["name"] for item in out] == ["market stall"]
    assert [item["name"] for item in micro_batches["OBJECTS#2"]] == ["sign board"]
    assert session.pending_tasks[-1]["status"] == "applied_removed_from_remaining"
    assert "cargo box" in session.pending_tasks[-1]["affected_assets"]
    print("[OK] negative generation_delta removes matching future assets")


def test_progress_message_is_user_facing_with_batch_context():
    msg = SceneSession.format_progress_message({
        "phase": "INTERIOR#1",
        "status": "done",
        "percent": 50,
        "asset_count": 2,
        "imported_count": 2,
        "cumulative_imported": 3,
        "total_assets": 6,
        "batch_asset_names": ["床", "书桌"],
        "next_batch_asset_names": ["台灯", "书架"],
        "absorbed_notes": [{"text": "后续家具都靠墙"}],
        "deferred_notes": [{"text": "后面再加发光蘑菇"}],
        "resource_backlog": [{
            "kind": "resource_backlog",
            "status": "queued_for_later_batch",
            "remaining_count": 2,
            "queued_items": ["小狗", "喷泉"],
        }],
        "resource_plans": [{
            "kind": "batch_resource_plan",
            "status": "completed",
            "resolved_assets": ["天使雕像"],
            "batch_resource_plan": {
                "requested_items": [{"item_name": "天使雕像"}],
            },
        }],
    })
    assert "本批已放入 2/2 个物件：床、书桌" in msg
    assert "导入 2/2" in msg
    assert "资源准备：新增请求 1 个" in msg
    assert "图片 0/1" in msg
    assert "模型 1/1" in msg
    assert "下一批资源队列还有 2 个" in msg
    assert "下一批准备：台灯、书架" in msg
    assert "已吸收你的要求：后续家具都靠墙" in msg
    assert "已记录待补：后面再加发光蘑菇" in msg
    forbidden = ("INTERIOR#1", "batch_id", "EngineWriteGate", "SceneDelta", "runtime_generation_context")
    assert not any(item in msg for item in forbidden), msg
    print("[OK] progress message exposes user-facing batch context without internals")


def test_progress_message_reports_resource_provider_unavailable():
    msg = SceneSession.format_progress_message({
        "phase": "OBJECTS#2",
        "status": "done",
        "percent": 75,
        "asset_count": 0,
        "imported_count": 0,
        "resource_plans": [{
            "kind": "batch_resource_plan",
            "status": "model_provider_unavailable",
            "batch_resource_plan": {
                "requested_items": [{"item_name": "小狗"}],
            },
            "provider": "PRIVATE_PROVIDER_SHOULD_NOT_LEAK",
            "prompt": "PRIVATE_PROMPT_SHOULD_NOT_LEAK",
        }],
    })
    assert "资源准备：新增请求 1 个" in msg
    assert "图片 0/1" in msg
    assert "模型 0/1" in msg
    assert "失败/待重试 1" in msg
    assert "PRIVATE_PROVIDER_SHOULD_NOT_LEAK" not in msg
    assert "PRIVATE_PROMPT_SHOULD_NOT_LEAK" not in msg
    print("[OK] progress message reports resource failures without leaking internals")


def test_progressive_post_shell_framework_generates_floor_and_boundary():
    composer = FakeComposer()
    _generate_post_shell_framework(composer)
    assert composer.floors == ["main_building"]
    assert composer.foundations == ["main_building"]
    assert composer.fences and composer.fences[0]["kind"] == "fence"
    assert composer.anchors and composer.anchors[0]["anchor_type"] == "shell"
    print("[OK] progressive post-shell framework keeps interior floor and boundary chain")


def test_f5_demo_mode_disables_vlm_by_default():
    old_demo = os.environ.get("CORONA_F5_DEMO_MODE")
    old_targets = os.environ.get("PROGRESSIVE_VLM_MAX_TARGETS")
    try:
        os.environ["CORONA_F5_DEMO_MODE"] = "1"
        os.environ.pop("PROGRESSIVE_VLM_MAX_TARGETS", None)
        assert _vlm_max_targets() == 0
        os.environ["PROGRESSIVE_VLM_MAX_TARGETS"] = "1"
        assert _vlm_max_targets() == 1
    finally:
        if old_demo is None:
            os.environ.pop("CORONA_F5_DEMO_MODE", None)
        else:
            os.environ["CORONA_F5_DEMO_MODE"] = old_demo
        if old_targets is None:
            os.environ.pop("PROGRESSIVE_VLM_MAX_TARGETS", None)
        else:
            os.environ["PROGRESSIVE_VLM_MAX_TARGETS"] = old_targets
    print("[OK] F5 demo mode disables VLM by default but explicit target count wins")


def test_f5_demo_mode_reports_vlm_disabled_in_final_text():
    old_demo = os.environ.get("CORONA_F5_DEMO_MODE")
    old_targets = os.environ.get("PROGRESSIVE_VLM_MAX_TARGETS")
    try:
        os.environ["CORONA_F5_DEMO_MODE"] = "1"
        os.environ.pop("PROGRESSIVE_VLM_MAX_TARGETS", None)
        report = _run_vlm_advisory_review(["入口拱门"], FakeEngineGate(), composer=SimpleNamespace())
    finally:
        if old_demo is None:
            os.environ.pop("CORONA_F5_DEMO_MODE", None)
        else:
            os.environ["CORONA_F5_DEMO_MODE"] = old_demo
        if old_targets is None:
            os.environ.pop("PROGRESSIVE_VLM_MAX_TARGETS", None)
        else:
            os.environ["PROGRESSIVE_VLM_MAX_TARGETS"] = old_targets

    assert report is not None
    assert report.status == "disabled"
    assert "F5" in report.reason
    text = _merge_final_and_vlm_review_text("最终检查完成。", report.to_user_text())
    assert "VLM/外观检查" in text
    assert "未执行" in text
    assert "AABB" in text
    print("[OK] F5 demo mode reports VLM disabled in final report text")


def test_aabb_review_issues_flow_to_coordinator_review_result():
    coordinator = FakeCoordinator()
    _emit_aabb_review_results(
        [{
            "kind": "aabb_repair",
            "text": "actor-statue 仍有重叠或摆放冲突",
            "status": "needs_confirm",
            "actor_id": "actor-statue",
            "reason": "overlap",
        }],
        batch_id="r1_OBJECTS_b1",
        interaction_coordinator=coordinator,
        room_id="room-a",
        plan_id="seed-a",
        session_id="sess-a",
    )

    assert coordinator.reviews
    review = coordinator.reviews[-1]
    assert review["review_type"] == "geometry"
    assert review["passed"] is False
    assert review["batch_id"] == "r1_OBJECTS_b1"
    assert review["actor_id"] == "actor-statue"
    assert review["finding_details"][0]["actor_id"] == "actor-statue"
    assert review["finding_details"][0]["target_hint"] == "actor-statue"
    assert review["finding_details"][0]["action"] == "repair_geometry"
    assert review["finding_details"][0]["issue_type"] == "overlap"
    assert "重叠" in review["findings"][0]
    print("[OK] AABB unresolved issues flow to Coordinator ReviewResult")


def test_vlm_actionable_advice_flows_to_coordinator_review_result():
    coordinator = FakeCoordinator()
    advice = SimpleNamespace(
        actor_id="actor-lamp",
        overall="FAIL",
        issues=["朝向不符合用户意图"],
        fix_suggestion="移动到不穿模位置并旋转 90 度",
        position_correction=[2.5, 0.0, 3.5],
        rotation_correction=[0.0, 90.0, 0.0],
        scale_correction=[1.0, 1.0, 1.0],
    )
    report = SimpleNamespace(
        advices=[advice],
        skipped=[],
        timed_out=[],
        actionable=lambda: [advice],
    )

    _emit_vlm_review_results(
        report,
        interaction_coordinator=coordinator,
        room_id="room-a",
        plan_id="seed-a",
        session_id="sess-a",
    )

    assert coordinator.reviews
    review = coordinator.reviews[-1]
    assert review["review_type"] == "vlm"
    assert review["passed"] is False
    assert review["actor_id"] == "actor-lamp"
    assert review["severity"] == "fail"
    assert "朝向" in review["findings"][0]
    assert review["metadata"]["position_correction"] == [2.5, 0.0, 3.5]
    assert review["metadata"]["rotation_correction"] == [0.0, 90.0, 0.0]
    assert review["finding_details"][0]["actor_id"] == "actor-lamp"
    assert review["finding_details"][0]["target_hint"] == "actor-lamp"
    assert review["finding_details"][0]["action"] == "apply_vlm_advice"
    assert review["finding_details"][0]["position_correction"] == [2.5, 0.0, 3.5]
    assert review["finding_details"][0]["rotation_correction"] == [0.0, 90.0, 0.0]
    print("[OK] VLM actionable advice flows to Coordinator ReviewResult")


def test_vlm_low_confidence_advisory_flows_to_coordinator_review_result():
    coordinator = FakeCoordinator()
    advice = SimpleNamespace(
        actor_id="地毯",
        overall="WARN",
        issues=["地毯偏大"],
        fix_suggestion="地毯偏大，缩小一点",
        position_correction=[],
        rotation_correction=[0.0, 0.0, 0.0],
        scale_correction=[1.0, 1.0, 1.0],
        confidence=0.42,
    )
    report = SimpleNamespace(
        advices=[advice],
        skipped=[],
        timed_out=[],
        checkpoint_type="final_consistency_review",
        reviewed_targets=[{"actor_id": "地毯", "checkpoint_type": "final_consistency_review"}],
        advisory_items=[{
            "actor_id": "地毯",
            "checkpoint_type": "final_consistency_review",
            "overall": "WARN",
            "issues": ["地毯偏大"],
            "fix_suggestion": "地毯偏大，缩小一点",
            "confidence": 0.42,
            "proposal": False,
        }],
        proposal_items=[],
        actionable=lambda: [],
    )

    _emit_vlm_review_results(
        report,
        interaction_coordinator=coordinator,
        room_id="room-low",
        plan_id="seed-low",
        session_id="sess-low",
        batch_id="batch-low",
    )

    assert coordinator.reviews
    review = coordinator.reviews[-1]
    assert review["review_type"] == "vlm"
    assert review["passed"] is False
    assert review["actor_id"] == "地毯"
    assert review["severity"] == "warn"
    assert review["finding_details"][0]["actor_id"] == "地毯"
    assert review["finding_details"][0]["action"] == "apply_vlm_advisory"
    assert review["finding_details"][0]["fix_suggestion"] == "地毯偏大，缩小一点"
    assert review["metadata"]["advisory_items"][0]["confidence"] == 0.42
    print("[OK] low confidence VLM advisory flows to Coordinator ReviewResult")


def test_vlm_review_uses_composer_hooks_under_engine_gate():
    calls = []

    class HookedComposer:
        def vlm_target_provider(self, imported, max_targets=4):
            calls.append(("target_provider", list(imported), max_targets))
            return [
                {"actor_id": "actor-a", "model_name": "statue", "model_type": "decor"},
                {"actor_id": "actor-b", "model_name": "lamp", "model_type": "lighting"},
            ]

        def vlm_capture_fn(self, output_dir, model_name):
            calls.append(("capture", output_dir, model_name))
            return f"{output_dir}/shots"

        def vlm_review_fn(self, screenshot_dir, model_name, model_type):
            calls.append(("review", screenshot_dir, model_name, model_type))
            return {
                "overall": "WARN",
                "position_correction": [0.1, 0.0, 0.0],
                "rotation_correction": [0.0, 0.2, 0.0],
                "scale_correction": [1.0, 0.9, 1.0],
                "issues": ["朝向略偏"],
                "fix_suggestion": "轻微旋转并缩小",
                "confidence": 0.9,
            }

    old_targets = os.environ.get("PROGRESSIVE_VLM_MAX_TARGETS")
    try:
        os.environ["PROGRESSIVE_VLM_MAX_TARGETS"] = "2"
        gate = FakeEngineGate()
        report = _run_vlm_advisory_review(["legacy-a", "legacy-b"], gate, composer=HookedComposer())
    finally:
        if old_targets is None:
            os.environ.pop("PROGRESSIVE_VLM_MAX_TARGETS", None)
        else:
            os.environ["PROGRESSIVE_VLM_MAX_TARGETS"] = old_targets

    assert report is not None
    assert len(report.advices) == 2
    assert len(gate.screenshots) == 2
    assert any(call[0] == "target_provider" for call in calls)
    capture_calls = [call for call in calls if call[0] == "capture" and call[2] == "statue"]
    assert capture_calls
    capture_dir = capture_calls[0][1]
    assert os.path.isabs(capture_dir)
    assert os.path.isdir(capture_dir)
    assert not capture_dir.replace("\\", "/").startswith("_vlm_review/")
    assert ("review", f"{capture_dir}/shots", "statue", "decor") in calls
    assert report.actionable()[0].actor_id == "actor-a"
    print("[OK] VLM review uses composer target/capture/review hooks under EngineWriteGate")


def test_vlm_target_priority_prefers_scene_anchors_and_high_risk_additions():
    targets = _prioritize_vlm_targets(
        ["展示桌", "长椅", "灯笼", "入口拱门", "入一个天使雕像", "小狗", "__terrain_boundary"],
        4,
    )
    assert targets == ["__terrain_boundary", "入口拱门", "入一个天使雕像", "小狗"]

    one_target = _prioritize_vlm_targets(["展示桌", "长椅", "入一个天使雕像"], 1)
    assert one_target == ["入一个天使雕像"]
    print("[OK] VLM target priority prefers scene anchors and high-risk additions")


def test_vlm_checkpoint_policy_selects_structure_high_risk_and_final_targets():
    policy = VlmCheckpointPolicy()

    checkpoint_type, targets = policy.select(
        phase="INTERIOR",
        imported_this_batch=["展示桌", "入口拱门", "__terrain_boundary"],
        imported_so_far=["展示桌", "入口拱门", "__terrain_boundary"],
        max_targets=2,
    )
    assert checkpoint_type == "structure_review"
    assert targets == ["__terrain_boundary", "入口拱门"]

    checkpoint_type, targets = policy.select(
        phase="OBJECTS",
        imported_this_batch=["地毯", "展示桌"],
        imported_so_far=["展示桌", "入口拱门", "__terrain_boundary", "地毯"],
        max_targets=2,
    )
    assert checkpoint_type == ""
    assert targets == []

    checkpoint_type, targets = policy.select(
        phase="OBJECTS",
        imported_this_batch=["普通椅子", "天使雕像", "小狗"],
        imported_so_far=["展示桌", "入口拱门", "__terrain_boundary", "普通椅子", "天使雕像", "小狗"],
        max_targets=2,
    )
    assert checkpoint_type == "high_risk_object_review"
    assert targets == ["天使雕像", "小狗"]

    checkpoint_type, targets = policy.select(
        phase="FINAL",
        imported_this_batch=[],
        imported_so_far=["展示桌", "入口拱门", "__terrain_boundary", "天使雕像", "小狗"],
        max_targets=3,
        final=True,
    )
    assert checkpoint_type == "final_consistency_review"
    assert targets == ["__terrain_boundary", "入口拱门", "天使雕像"]
    print("[OK] VLM checkpoint policy selects structure, high-risk, and final targets")


def test_vlm_high_risk_priority_skips_plain_small_items():
    targets = _prioritize_high_risk_vlm_targets(
        ["普通椅子", "展示桌", "小狗", "大型灯光主体", "入口拱门"],
        2,
    )
    assert targets == ["入口拱门", "小狗"]
    assert _prioritize_high_risk_vlm_targets(["普通椅子", "展示桌"], 2) == []
    print("[OK] VLM high-risk checkpoint skips plain small items")


def test_vlm_review_result_carries_checkpoint_and_batch_context():
    coordinator = FakeCoordinator()
    advice = SimpleNamespace(
        actor_id="入口拱门",
        overall="FAIL",
        issues=["入口方向不清晰"],
        fix_suggestion="调整到主街前方并朝向入口",
        position_correction=[0.0, 0.0, -2.0],
        rotation_correction=[0.0, 180.0, 0.0],
        scale_correction=[1.0, 1.0, 1.0],
        confidence=0.92,
    )
    report = SimpleNamespace(
        advices=[advice],
        skipped=[],
        timed_out=[],
        checkpoint_type="structure_review",
        reviewed_targets=[{"actor_id": "入口拱门", "checkpoint_type": "structure_review"}],
        advisory_items=[],
        proposal_items=[{"actor_id": "入口拱门", "checkpoint_type": "structure_review", "proposal": True}],
        actionable=lambda: [advice],
    )

    _emit_vlm_review_results(
        report,
        interaction_coordinator=coordinator,
        room_id="room-vlm",
        plan_id="seed-vlm",
        session_id="sess-vlm",
        batch_id="r1_INTERIOR_b1",
    )

    review = coordinator.reviews[-1]
    assert review["batch_id"] == "r1_INTERIOR_b1"
    assert review["metadata"]["checkpoint_type"] == "structure_review"
    assert review["metadata"]["reviewed_targets"][0]["actor_id"] == "入口拱门"
    assert review["metadata"]["proposal_items"][0]["proposal"] is True
    assert review["finding_details"][0]["checkpoint_type"] == "structure_review"
    print("[OK] VLM review result carries checkpoint and batch context")


def test_vlm_checkpoint_reports_summarize_all_stages_without_internal_leakage():
    reports = [
        SimpleNamespace(
            checkpoint_type="structure_review",
            status="completed",
            reviewed_targets=[{"actor_id": "__terrain_boundary"}, {"actor_id": "入口拱门"}],
            proposal_items=[{"actor_id": "入口拱门", "proposal": True}],
            advisory_items=[],
            skipped=[],
            timed_out=[],
        ),
        SimpleNamespace(
            checkpoint_type="high_risk_object_review",
            status="completed",
            reviewed_targets=[{"actor_id": "天使雕像"}, {"actor_id": "小狗"}],
            proposal_items=[],
            advisory_items=[{
                "actor_id": "小狗",
                "issues": ["尺寸偏大"],
                "fix_suggestion": "缩小一点并靠边摆放",
                "confidence": 0.42,
                "proposal": False,
            }],
            skipped=[],
            timed_out=[],
        ),
        SimpleNamespace(
            checkpoint_type="final_consistency_review",
            status="unavailable",
            reason="provider=PRIVATE job_id=PRIVATE 审查服务不可用",
            reviewed_targets=[],
            proposal_items=[],
            advisory_items=[],
            skipped=[],
            timed_out=[],
        ),
    ]

    text = _vlm_checkpoint_reports_user_text(reports)
    assert "第一批结构审查" in text
    assert "中间批高风险审查" in text
    assert "最终一致性审查" in text
    assert "待确认调整建议" in text
    assert "不自动执行" in text
    assert "小狗：缩小一点并靠边摆放" in text
    assert "PRIVATE" not in text
    assert "provider" not in text
    assert "job_id" not in text
    print("[OK] VLM checkpoint reports summarize all stages without internal leakage")


def test_vlm_checkpoint_progress_message_includes_advisory_details():
    report = SimpleNamespace(
        status="completed",
        proposal_items=[],
        advisory_items=[
            {
                "actor_id": "床头柜",
                "issues": ["可能离床过远"],
                "fix_suggestion": "靠近床头一侧",
                "confidence": 0.43,
            },
            {
                "actor_id": "台灯",
                "issues": ["朝向不明确"],
                "fix_suggestion": "",
                "confidence": 0.39,
            },
        ],
    )

    text = _vlm_checkpoint_user_message(
        "final_consistency_review",
        ["床头柜", "台灯"],
        "done",
        report=report,
    )

    assert "发现 2 条低风险/低置信提示" in text
    assert "床头柜：靠近床头一侧" in text
    assert "台灯：朝向不明确" in text
    print("[OK] VLM checkpoint progress message includes advisory details")


def test_final_report_text_includes_vlm_status_without_duplicate():
    merged = _merge_final_and_vlm_review_text(
        "风格收口：warm、fantasy。",
        "VLM 审查未发现明显语义问题。",
    )
    assert "风格收口" in merged
    assert "VLM/外观检查：VLM 审查未发现明显语义问题。" in merged

    duplicate = _merge_final_and_vlm_review_text(merged, "VLM 审查未发现明显语义问题。")
    assert duplicate == merged

    vlm_only = _merge_final_and_vlm_review_text("", "VLM 外审未完成：截图失败/跳过 1 个，超时 0 个；本轮以 AABB 几何检查为准。")
    assert vlm_only.startswith("VLM/外观检查")
    print("[OK] final report text includes VLM status without duplicate")


def test_scene_composer_injects_shared_scoped_memory_only():
    class FakeMemoryCoordinator:
        def __init__(self):
            self.calls = []

        def memory_summary(self, **kwargs):
            self.calls.append(dict(kwargs))
            return {
                "summary_text": "已确认风格：暗黑集市；最近介入：入口留路，雕像靠后。",
                "entries": [
                    {"visibility": "shared", "text": "入口留路"},
                    {"visibility": "private", "text": "agent-private-secret"},
                ],
            }

    coordinator = FakeMemoryCoordinator()
    composer = SceneComposer(max_items=3)
    enhanced, context = composer._compose_generation_text(
        "生成一个市场",
        interaction_coordinator=coordinator,
        room_id="room-a",
        plan_id="seed-a",
        session_id="room-a",
    )

    assert coordinator.calls
    assert coordinator.calls[-1]["visibility"] == "shared"
    assert "跨批次已确认上下文" in enhanced
    assert "入口留路" in enhanced
    assert "agent-private-secret" not in enhanced
    assert context["entry_count"] == 2

    unchanged, empty_context = composer._compose_generation_text("生成一个市场")
    assert unchanged == "生成一个市场"
    assert empty_context == {}
    print("[OK] SceneComposer injects shared scoped memory without private leakage")


def test_scene_composer_can_focus_scoped_memory_on_target_actor():
    class FakeMemoryCoordinator:
        def __init__(self):
            self.calls = []

        def memory_summary(self, **kwargs):
            self.calls.append(dict(kwargs))
            actor_id = kwargs.get("actor_id")
            if actor_id == "actor-statue":
                return {
                    "summary_text": "雕像需要缩小并后移。",
                    "entries": [{"actor_id": "actor-statue", "text": "雕像需要缩小并后移。"}],
                }
            return {
                "summary_text": "灯具需要旋转。",
                "entries": [{"actor_id": "actor-lamp", "text": "灯具需要旋转。"}],
            }

    coordinator = FakeMemoryCoordinator()
    composer = SceneComposer(max_items=3)
    enhanced, context = composer._compose_generation_text(
        "调整下一批物体",
        interaction_coordinator=coordinator,
        room_id="room-a",
        plan_id="seed-a",
        session_id="room-a",
        actor_id="actor-statue",
    )

    assert coordinator.calls[-1]["actor_id"] == "actor-statue"
    assert context["actor_id"] == "actor-statue"
    assert "雕像需要缩小并后移" in enhanced
    assert "灯具需要旋转" not in enhanced
    print("[OK] SceneComposer can focus scoped memory on target actor")


def test_pending_generation_delta_creates_resource_request_for_missing_asset():
    session = SimpleNamespace(pending_tasks=[])
    note = SimpleNamespace(kind="generation_delta", text="新增：再加一个天使雕像", source_agent="host")

    remaining = _apply_pending_notes_to_batch(
        [{"name": "摊位", "pos": [0, 0, 0]}],
        [note],
        session,
        current_phase="OBJECTS#1",
        micro_phase_assets={"OBJECTS#1": [], "OBJECTS#2": [{"name": "灯笼"}]},
        phase_sequence=["OBJECTS#1", "OBJECTS#2"],
        max_batch_size=3,
    )

    assert [item["name"] for item in remaining] == ["摊位"]
    assert session.pending_tasks[-1]["status"] == "resource_request_created"
    assert session.pending_resource_requests[-1]["item_name"] == "天使雕像"
    assert session.pending_resource_requests[-1]["status"] == "planned"
    print("[OK] missing generation delta creates next-batch resource request")


def test_batch_resource_plan_carries_contract_version_and_interventions():
    plan = build_batch_resource_plan(
        plan_id="seed-a",
        batch_id="batch-2",
        phase="OBJECTS#2",
        contract_version=4,
        requested_items=[{"item_name": "天使雕像", "quantity": 1}],
        absorbed_interventions=[{"text": "新增天使雕像"}],
    )

    assert plan.as_dict()["contract_version"] == 4
    assert plan.as_dict()["requested_items"][0]["item_name"] == "天使雕像"
    assert plan.as_dict()["absorbed_interventions"][0]["text"] == "新增天使雕像"
    assert plan.status == "planned"
    print("[OK] BatchResourcePlan preserves batch/contract/intervention context")


def test_pending_resource_request_resolves_models_into_current_batch():
    class FakeComposerWithRetrieval:
        def __init__(self):
            self.calls = []

        def _run_model_retrieval(self, items):
            self.calls.append([dict(item) for item in items])
            return [
                {
                    "name": item["name"],
                    "model_path": f"C:/tmp/{item['name']}.glb",
                    "source": "fake_generation",
                }
                for item in items
            ]

    session = SimpleNamespace(
        pending_tasks=[],
        pending_resource_requests=[{
            "request_id": "resource-1",
            "kind": "add_object",
            "item_name": "天使雕像",
            "quantity": 1,
            "image_prompt": "fantasy angel statue",
            "original_text": "新增：再加一个天使雕像",
            "status": "planned",
        }],
    )

    assets = _resolve_pending_resource_requests_for_batch(
        FakeComposerWithRetrieval(),
        [{"name": "摊位", "model_path": "C:/tmp/stall.glb"}],
        session,
        plan_id="seed-a",
        phase="OBJECTS#2",
        contract_version=5,
    )

    assert [item["name"] for item in assets] == ["摊位", "天使雕像"]
    assert assets[-1]["model_path"].endswith("天使雕像.glb")
    assert session.pending_resource_requests == []
    assert session.pending_tasks[-1]["status"] == "completed"
    assert session.pending_tasks[-1]["batch_resource_plan"]["contract_version"] == 5
    print("[OK] pending resource request resolves generated model into current batch")


def test_pending_resource_request_defers_while_ready_assets_can_still_import():
    class FakeComposerWithRetrieval:
        def __init__(self):
            self.calls = []

        def _run_model_retrieval(self, items):
            self.calls.append([dict(item) for item in items])
            return [
                {"name": item["name"], "model_path": f"C:/tmp/{item['name']}.glb"}
                for item in items
            ]

    composer = FakeComposerWithRetrieval()
    request = {
        "request_id": "resource-1",
        "kind": "add_object",
        "item_name": "布娃娃",
        "quantity": 1,
        "image_prompt": "small rag doll",
        "original_text": "新增一个布娃娃",
        "status": "planned",
    }
    session = SimpleNamespace(
        pending_tasks=[],
        pending_resource_requests=[dict(request)],
    )

    assets = _resolve_pending_resource_requests_for_batch(
        composer,
        [{"name": "床", "model_path": "C:/tmp/bed.glb"}],
        session,
        plan_id="seed-a",
        phase="INTERIOR",
        contract_version=7,
        defer_when_assets_ready=True,
    )

    assert [item["name"] for item in assets] == ["床"]
    assert composer.calls == []
    assert session.pending_resource_requests == [request]
    backlog = [task for task in session.pending_tasks if task.get("kind") == "resource_backlog"][-1]
    assert backlog["status"] == "queued_for_later_batch"
    assert backlog["queued_items"] == ["布娃娃"]
    print("[OK] ready assets import before pending generated resource requests")


def test_pending_resource_request_runs_image_stage_before_model_retrieval():
    class FakeComposerWithImageAndRetrieval:
        def __init__(self):
            self.image_calls = []
            self.model_calls = []

        def _run_batch_image_generation(self, items):
            self.image_calls.append([dict(item) for item in items])
            return {
                "status": "completed",
                "image_urls": {"天使雕像": "fileid://angel-image"},
            }

        def _run_model_retrieval(self, items):
            self.model_calls.append([dict(item) for item in items])
            return [
                {
                    "name": item["name"],
                    "model_path": "C:/tmp/angel.glb",
                    "source": "fake_image_to_3d",
                }
                for item in items
            ]

    composer = FakeComposerWithImageAndRetrieval()
    session = SimpleNamespace(
        pending_tasks=[],
        pending_resource_requests=[{
            "request_id": "resource-1",
            "kind": "add_object",
            "item_name": "天使雕像",
            "quantity": 1,
            "image_prompt": "fantasy angel statue",
            "original_text": "新增：再加一个天使雕像",
            "status": "planned",
        }],
    )

    assets = _resolve_pending_resource_requests_for_batch(
        composer,
        [],
        session,
        plan_id="seed-a",
        phase="OBJECTS#2",
        contract_version=6,
    )

    assert [item["name"] for item in assets] == ["天使雕像"]
    assert composer.image_calls[0][0]["image_prompt"] == "fantasy angel statue"
    assert composer.model_calls[0][0]["image_url"] == "fileid://angel-image"
    task = session.pending_tasks[-1]
    assert task["image_status"] == "completed"
    assert task["image_generated"] == ["天使雕像"]
    assert task["batch_resource_plan"]["image_status"] == "completed"
    assert task["batch_resource_plan"]["model_status"] == "completed"
    print("[OK] pending resource request runs explicit image stage before model retrieval")


def test_scene_composer_passes_generated_images_to_model_retrieval_workflow():
    captured = {}
    function_id = 71001

    class FakeGraph:
        def invoke(self, state):
            captured["state"] = state
            model_path = captured["model_path"]
            return {
                "global_assets": {
                    "model_retrieval": {
                        "model_results": [{
                            "item_name": "天使雕像",
                            "model_path": model_path,
                            "source": "generation",
                        }],
                    },
                },
            }

    module_name = "cai_extensions.flows.model_retrieval_workflow"
    helpers_name = "cai_extensions.flows.model_retrieval_workflow.helpers"
    old_module = sys.modules.get(module_name)
    old_helpers = sys.modules.get(helpers_name)
    fake_module = types.ModuleType(module_name)
    fake_module.__path__ = []  # mark as package for helper submodule imports
    fake_module.MODEL_RETRIEVAL_FUNCTION_ID = function_id
    fake_module.WORKFLOWS = {function_id: FakeGraph()}
    fake_helpers = types.ModuleType(helpers_name)
    fake_helpers.resolve_model_file = lambda path: path
    try:
        sys.modules[module_name] = fake_module
        sys.modules[helpers_name] = fake_helpers
        tmp = _named_test_dir("scene_composer_generated_image")
        model_path = os.path.join(tmp, "angel.glb")
        with open(model_path, "wb") as f:
            f.write(b"glb")
        captured["model_path"] = model_path

        composer = SceneComposer(scene_name="image_stage_test", max_items=1)
        progress_messages = []
        composer._model_retrieval_progress_sink = progress_messages.append
        resolved = composer._run_model_retrieval([{
            "name": "天使雕像",
            "keywords": "fantasy angel statue",
            "image_url": "fileid://angel-image",
        }])
    finally:
        if old_module is None:
            sys.modules.pop(module_name, None)
        else:
            sys.modules[module_name] = old_module
        if old_helpers is None:
            sys.modules.pop(helpers_name, None)
        else:
            sys.modules[helpers_name] = old_helpers

    generated_images = (
        captured["state"]["global_assets"]["multi_scene"]["generated_images"]
    )
    assert generated_images == {"天使雕像": "fileid://angel-image"}
    sink = captured["state"]["metadata"].get("progress_sink")
    assert callable(sink)
    sink("资源准备-模型：测试进度")
    assert progress_messages == ["资源准备-模型：测试进度"]
    assert resolved[0]["model_path"] == captured["model_path"]
    print("[OK] SceneComposer passes explicit batch images and progress sink into model retrieval workflow")


def test_pending_resource_request_backlog_is_visible_when_batch_limit_is_hit():
    class FakeComposerWithRetrieval:
        def _run_batch_image_generation(self, items):
            return {
                "status": "completed",
                "image_urls": {item["item_name"]: f"fileid://{item['item_name']}" for item in items},
            }

        def _run_model_retrieval(self, items):
            return [
                {"name": item["name"], "model_path": f"C:/tmp/{item['name']}.glb"}
                for item in items
            ]

    old = os.environ.get("CORONA_PROGRESSIVE_RESOURCE_REQUESTS_PER_BATCH")
    os.environ["CORONA_PROGRESSIVE_RESOURCE_REQUESTS_PER_BATCH"] = "1"
    try:
        session = SimpleNamespace(
            pending_tasks=[],
            pending_resource_requests=[
                {"request_id": "r1", "item_name": "天使雕像", "image_prompt": "angel", "status": "planned"},
                {"request_id": "r2", "item_name": "小狗", "image_prompt": "dog", "status": "planned"},
                {"request_id": "r3", "item_name": "喷泉", "image_prompt": "fountain", "status": "planned"},
            ],
        )

        assets = _resolve_pending_resource_requests_for_batch(
            FakeComposerWithRetrieval(),
            [],
            session,
            plan_id="seed-a",
            phase="OBJECTS#1",
            contract_version=2,
        )
    finally:
        if old is None:
            os.environ.pop("CORONA_PROGRESSIVE_RESOURCE_REQUESTS_PER_BATCH", None)
        else:
            os.environ["CORONA_PROGRESSIVE_RESOURCE_REQUESTS_PER_BATCH"] = old

    assert [item["name"] for item in assets] == ["天使雕像"]
    assert [item["item_name"] for item in session.pending_resource_requests] == ["小狗", "喷泉"]
    backlog = [task for task in session.pending_tasks if task.get("kind") == "resource_backlog"][-1]
    assert backlog["remaining_count"] == 2
    assert backlog["queued_items"] == ["小狗", "喷泉"]
    print("[OK] pending resource backlog is visible when per-batch limit is hit")


def test_pending_resource_queue_is_bounded_and_reports_overflow():
    old = os.environ.get("CORONA_PROGRESSIVE_PENDING_RESOURCE_LIMIT")
    os.environ["CORONA_PROGRESSIVE_PENDING_RESOURCE_LIMIT"] = "2"
    try:
        session = SimpleNamespace(pending_tasks=[], pending_resource_requests=[])
        for name in ("旧灯", "旧椅子", "新雕像"):
            _apply_pending_notes_to_batch(
                [],
                [SimpleNamespace(kind="generation_delta", text=f"新增：{name}", source_agent="host")],
                session,
                current_phase="OBJECTS#1",
                micro_phase_assets={"OBJECTS#1": []},
                phase_sequence=["OBJECTS#1"],
                max_batch_size=1,
            )
    finally:
        if old is None:
            os.environ.pop("CORONA_PROGRESSIVE_PENDING_RESOURCE_LIMIT", None)
        else:
            os.environ["CORONA_PROGRESSIVE_PENDING_RESOURCE_LIMIT"] = old

    assert [item["item_name"] for item in session.pending_resource_requests] == ["旧椅子", "新雕像"]
    overflow = [task for task in session.pending_tasks if task.get("status") == "overflow_trimmed"][-1]
    assert overflow["overflow_count"] == 1
    assert overflow["dropped_items"] == ["旧灯"]
    print("[OK] pending resource queue is bounded and reports overflow")


def test_pending_resource_request_reports_provider_unavailable_without_fake_path():
    session = SimpleNamespace(
        pending_tasks=[],
        pending_resource_requests=[{
            "request_id": "resource-1",
            "kind": "add_object",
            "item_name": "小狗",
            "quantity": 1,
            "image_prompt": "small dog",
            "original_text": "新增：再增加一只小狗",
            "status": "planned",
        }],
    )

    assets = _resolve_pending_resource_requests_for_batch(
        SimpleNamespace(),
        [{"name": "摊位", "model_path": "C:/tmp/stall.glb"}],
        session,
        plan_id="seed-a",
        phase="OBJECTS#2",
        contract_version=5,
    )

    assert [item["name"] for item in assets] == ["摊位"]
    assert session.pending_tasks[-1]["status"] == "model_provider_unavailable"
    assert session.pending_resource_requests[-1]["status"] == "provider_unavailable"
    assert session.pending_resource_requests[-1]["item_name"] == "小狗"
    print("[OK] missing model provider is explicit and does not invent model path")


def test_fantasy_market_terrain_profile_uses_low_decorative_boundary():
    profile = TerrainComponentResolver().derive("夜晚幻想集市，有入口、摊位、灯光、小休息区", scene_type="outdoor")
    assert profile.scene_key == "fantasy_night_market"
    assert profile.boundary_spec["type"] == "low_decorative_boundary"
    assert profile.boundary_spec["height"] < 0.8
    assert "grassland yurt fence" in profile.boundary_spec["avoid"]

    zone = Zone(
        zone_id="market",
        name="market",
        role="outdoor",
        enclosure="terrain",
        volume=Volume(center=[0.0, 0.0, 0.0], size=[18.0, 18.0, 0.0]),
    )
    apply_scene_semantic_terrain_profile(zone, "夜晚幻想集市，有入口、摊位、灯光、小休息区", "outdoor")
    boundary = [item for item in zone.aspects if item.capability == "boundary"][0]
    assert boundary.params["style"] == "vine_wood_lantern"
    assert boundary.params["height"] < 0.8
    print("[OK] fantasy market derives low decorative terrain boundary")


def test_warm_mysterious_market_overrides_generic_stone_wall_boundary():
    text = "有点神秘感的室外集市，不要太恐怖，更温暖一点，有灯光和休息区"
    profile = TerrainComponentResolver().derive(text, scene_type="outdoor")
    assert profile.scene_key == "fantasy_night_market"

    zone = Zone(
        zone_id="market",
        name="market",
        role="outdoor",
        enclosure="terrain",
        volume=Volume(center=[0.0, 0.0, 0.0], size=[18.0, 18.0, 0.0]),
        aspects=[
            ZoneAspect(capability="boundary", params={"kind": "wall", "material": "stone", "height": 0.8}),
        ],
    )
    apply_scene_semantic_terrain_profile(zone, text, "outdoor")
    boundary = [item for item in zone.aspects if item.capability == "boundary"][0]

    assert boundary.params["kind"] == "fence"
    assert boundary.params["material"] == "wood"
    assert boundary.params["style"] == "vine_wood_lantern"
    assert boundary.params["coverage"] == "partial"
    assert boundary.params["height"] < 0.8
    print("[OK] warm mysterious market overrides generic stone wall boundary")


def _install_fake_corona_scene(initial_actor_names=None):
    class ExistingActor:
        def __init__(self, name):
            self.name = name

    class FakeGeometry:
        def get_aabb(self):
            return [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5]

    class FakeActor(ExistingActor):
        def __init__(self, name, route="", actor_type="", parent_scene=None):
            super().__init__(name)
            self.route = route
            self.actor_type = actor_type
            self.parent_scene = parent_scene
            self.position = None
            self.scale = None
            self._geometry = FakeGeometry()
            self._mechanics = SimpleNamespace(set_physics_enabled=lambda _enabled: None)

        def set_position(self, value, _world=True):
            self.position = list(value)

        def set_scale(self, value, _world=True):
            self.scale = list(value)

    class FakeScene:
        def __init__(self):
            self.actors = [ExistingActor(name) for name in (initial_actor_names or [])]

        def get_actors(self):
            return list(self.actors)

        def add_actor(self, actor):
            self.actors.append(actor)

    scene = FakeScene()
    fake_scene_manager = SimpleNamespace(get=lambda _route="": scene, list_all=lambda: ["fake.scene"])

    fake_corona = types.ModuleType("CoronaCore")
    fake_core = types.ModuleType("CoronaCore.core")
    fake_managers = types.ModuleType("CoronaCore.core.managers")
    fake_entities = types.ModuleType("CoronaCore.core.entities")
    fake_actor_module = types.ModuleType("CoronaCore.core.entities.actor")
    fake_managers.scene_manager = fake_scene_manager
    fake_actor_module.Actor = FakeActor

    module_names = [
        "CoronaCore",
        "CoronaCore.core",
        "CoronaCore.core.managers",
        "CoronaCore.core.entities",
        "CoronaCore.core.entities.actor",
    ]
    old_modules = {name: sys.modules.get(name) for name in module_names}
    sys.modules["CoronaCore"] = fake_corona
    sys.modules["CoronaCore.core"] = fake_core
    sys.modules["CoronaCore.core.managers"] = fake_managers
    sys.modules["CoronaCore.core.entities"] = fake_entities
    sys.modules["CoronaCore.core.entities.actor"] = fake_actor_module

    def restore():
        for name, old in old_modules.items():
            if old is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = old

    return scene, restore


def _actor_by_name(scene, name):
    return next((actor for actor in scene.get_actors() if actor.name == name), None)


def test_scene_framework_indoor_fallback_generates_room_box_only():
    scene, restore = _install_fake_corona_scene()
    try:
        composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="indoor_matrix")
        asset_dir = Path(_named_test_dir("framework_indoor"))
        composer._generated_asset_dir = lambda: (asset_dir, "tmp/framework_indoor")

        composer._generate_scene_framework("一个可爱的室内卧室，有床、台灯和小书桌")
    finally:
        restore()

    room_box = _actor_by_name(scene, "__room_box")
    assert room_box is not None
    assert _actor_by_name(scene, "__room_terrain") is None
    assert room_box.scale == [5.0, 3.0, 3.0]
    assert room_box.position == [0.0, 1.5, 0.0]
    print("[OK] indoor fallback creates room_box only")


def test_scene_framework_outdoor_fallback_generates_terrain_only():
    scene, restore = _install_fake_corona_scene()
    try:
        composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="outdoor_matrix")
        asset_dir = Path(_named_test_dir("framework_outdoor"))
        composer._generated_asset_dir = lambda: (asset_dir, "tmp/framework_outdoor")

        composer._generate_scene_framework("一个温暖神秘的室外夜晚幻想集市，有入口、摊位和灯光")
    finally:
        restore()

    terrain = _actor_by_name(scene, "__room_terrain")
    assert terrain is not None
    assert _actor_by_name(scene, "__room_box") is None
    assert terrain.scale and terrain.scale[0] >= 18.0
    assert terrain.position and len(terrain.position) == 3
    print("[OK] outdoor fallback creates terrain only")


def test_single_indoor_box_decompose_fallback_keeps_room_box():
    scene, restore = _install_fake_corona_scene()
    try:
        composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="treasure_room")
        asset_dir = Path(_named_test_dir("framework_treasure_room"))
        composer._generated_asset_dir = lambda: (asset_dir, "tmp/framework_treasure_room")
        composer._llm_decompose = lambda _text: [{  # type: ignore[method-assign]
            "id": "treasure",
            "name": "山贼藏宝室",
            "role": "indoor",
            "enclosure": "box",
            "size": [6.0, 5.0, 3.0],
            "style_context": {"material_palette": ["stone", "wood"]},
        }]

        composer.zone_tree = composer.decompose_zone_tree("山贼据点里的藏宝室，有藏宝箱、木桶和武器架")
        composer._generate_scene_framework("山贼据点里的藏宝室，有藏宝箱、木桶和武器架")
    finally:
        restore()

    room_box = _actor_by_name(scene, "__room_box")
    assert room_box is not None
    assert _actor_by_name(scene, "__room_terrain") is None
    assert room_box.scale == [6.0, 3.0, 5.0]
    assert room_box.position == [0.0, 1.5, 0.0]
    print("[OK] single indoor box decompose fallback keeps room_box")


def test_scene_framework_mixed_zone_tree_generates_terrain_and_room_box():
    scene, restore = _install_fake_corona_scene()
    try:
        terrain = Zone(
            zone_id="market",
            name="night market",
            role="outdoor",
            enclosure="terrain",
            volume=Volume(center=[0.0, 0.0, 0.0], size=[18.0, 18.0, 0.0]),
        )
        room = Zone(
            zone_id="rest_area",
            name="rest area",
            role="indoor",
            enclosure="box",
            volume=Volume(center=[0.0, 1.5, 0.0], size=[5.0, 5.0, 3.0]),
            connectors=[
                Connector(
                    connector_id="door_rest_area",
                    type="door",
                    position=[0.0, 0.0, 2.5],
                    size=[1.2, 2.2],
                    target_zone_id="rest_area",
                )
            ],
        )
        terrain.sub_zones.append(room)
        composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="mixed_matrix")
        composer.zone_tree = ZoneTree(root=terrain)
        asset_dir = Path(_named_test_dir("framework_mixed"))
        composer._generated_asset_dir = lambda: (asset_dir, "tmp/framework_mixed")

        composer._generate_scene_framework("室外夜晚幻想集市里有一个可进入的小休息区")
    finally:
        restore()

    terrain_actor = _actor_by_name(scene, "__room_terrain")
    room_box = _actor_by_name(scene, "__room_box")
    assert terrain_actor is not None
    assert room_box is not None
    assert terrain_actor.scale and terrain_actor.scale[0] >= 18.0
    assert room_box.scale == [5.0, 3.0, 5.0]
    assert room_box.position == [0.0, 1.5, 0.0]
    print("[OK] mixed zone_tree creates terrain and room_box")


def test_mixed_outdoor_structure_fallback_creates_terrain_and_room_box_when_llm_fails():
    composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="mixed_yurt")
    composer._llm_decompose = lambda _text: []

    tree = composer.decompose_zone_tree("草原上有一个蒙古包，里面有毯子、桌子和火炉")

    assert tree is not None
    zones = tree.list_all_zones()
    assert [zone.enclosure for zone in zones] == ["terrain", "box"]
    assert zones[0].role == "outdoor"
    assert zones[1].role == "indoor"
    assert zones[1].metadata.get("parent") == "outdoor_ground"
    assert any(connector.type == "door" for connector in zones[1].connectors)
    print("[OK] mixed outdoor structure fallback creates terrain + room box when LLM fails")


def test_mixed_outdoor_structure_repairs_terrain_only_llm_decompose():
    composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="mixed_terrain_only")
    composer._llm_decompose = lambda _text: [
        {
            "id": "grassland",
            "name": "草原地形",
            "role": "outdoor",
            "enclosure": "terrain",
            "size": [18.0, 18.0, 0.0],
        }
    ]

    tree = composer.decompose_zone_tree("草原上有一个蒙古包，里面有毯子、桌子和火炉")

    assert tree is not None
    zones = tree.list_all_zones()
    assert [zone.enclosure for zone in zones] == ["terrain", "box"]
    assert zones[1].metadata.get("parent") == "grassland"
    assert any(connector.type == "door" for connector in zones[1].connectors)
    print("[OK] mixed guardrail repairs terrain-only LLM zone into terrain + room_box")


def test_mixed_outdoor_structure_repairs_box_only_llm_decompose():
    composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="mixed_box_only")
    composer._llm_decompose = lambda _text: [
        {
            "id": "yurt_inside",
            "name": "蒙古包内部",
            "role": "indoor",
            "enclosure": "box",
            "size": [5.0, 5.0, 3.0],
        }
    ]

    tree = composer.decompose_zone_tree("户外营地有一个蒙古包，内部有地毯和桌子")

    assert tree is not None
    zones = tree.list_all_zones()
    assert [zone.enclosure for zone in zones] == ["terrain", "box"]
    assert zones[1].zone_id == "yurt_inside"
    assert zones[1].metadata.get("parent") == "outdoor_ground"
    assert any(connector.type == "door" for connector in zones[1].connectors)
    print("[OK] mixed guardrail repairs box-only LLM zone into terrain + room_box")


def test_mixed_outdoor_structure_keeps_valid_shell_zone():
    composer = SceneComposer(room_size=[5.0, 3.0, 3.0], scene_name="mixed_shell")
    composer._llm_decompose = lambda _text: [
        {
            "id": "camp_ground",
            "name": "营地地形",
            "role": "outdoor",
            "enclosure": "terrain",
            "size": [18.0, 18.0, 0.0],
        },
        {
            "id": "yurt_shell",
            "name": "蒙古包",
            "role": "indoor",
            "enclosure": "shell",
            "parent": "camp_ground",
            "shell_asset": "蒙古包",
            "size": [5.0, 5.0, 3.0],
        },
    ]

    tree = composer.decompose_zone_tree("户外营地有一个蒙古包，里面有毯子和桌子")

    assert tree is not None
    zones = tree.list_all_zones()
    assert [zone.enclosure for zone in zones] == ["terrain", "shell"]
    assert zones[1].metadata.get("parent") == "camp_ground"
    assert zones[1].primary_shell_asset_id == "蒙古包"
    print("[OK] mixed guardrail preserves valid terrain + shell zone")


def test_room_box_generation_is_not_blocked_by_existing_room_terrain():
    class ExistingActor:
        def __init__(self, name):
            self.name = name

    class FakeActor(ExistingActor):
        def __init__(self, name, route="", actor_type="", parent_scene=None):
            super().__init__(name)
            self.route = route
            self.actor_type = actor_type
            self.parent_scene = parent_scene
            self.position = None
            self.scale = None
            self._mechanics = SimpleNamespace(set_physics_enabled=lambda _enabled: None)

        def set_position(self, value, _world=True):
            self.position = list(value)

        def set_scale(self, value, _world=True):
            self.scale = list(value)

    class FakeScene:
        def __init__(self):
            self.actors = [ExistingActor("__room_terrain")]

        def get_actors(self):
            return list(self.actors)

        def add_actor(self, actor):
            self.actors.append(actor)

    scene = FakeScene()
    fake_scene_manager = SimpleNamespace(get=lambda _route="": scene, list_all=lambda: ["fake.scene"])

    fake_corona = types.ModuleType("CoronaCore")
    fake_core = types.ModuleType("CoronaCore.core")
    fake_managers = types.ModuleType("CoronaCore.core.managers")
    fake_entities = types.ModuleType("CoronaCore.core.entities")
    fake_actor_module = types.ModuleType("CoronaCore.core.entities.actor")
    fake_managers.scene_manager = fake_scene_manager
    fake_actor_module.Actor = FakeActor

    module_names = [
        "CoronaCore",
        "CoronaCore.core",
        "CoronaCore.core.managers",
        "CoronaCore.core.entities",
        "CoronaCore.core.entities.actor",
    ]
    old_modules = {name: sys.modules.get(name) for name in module_names}
    try:
        sys.modules["CoronaCore"] = fake_corona
        sys.modules["CoronaCore.core"] = fake_core
        sys.modules["CoronaCore.core.managers"] = fake_managers
        sys.modules["CoronaCore.core.entities"] = fake_entities
        sys.modules["CoronaCore.core.entities.actor"] = fake_actor_module

        composer = SceneComposer()
        composer.zone_tree = ZoneTree(root=Zone(
            zone_id="room",
            name="room",
            role="indoor",
            enclosure="box",
            volume=Volume(center=[0.0, 1.5, 0.0], size=[5.0, 5.0, 3.0]),
        ))
        asset_dir = Path(_named_test_dir("room_box_with_terrain"))
        composer._generated_asset_dir = lambda: (asset_dir, "tmp/room_box_with_terrain")

        composer._generate_room_box()
    finally:
        for name, old in old_modules.items():
            if old is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = old

    names = [actor.name for actor in scene.get_actors()]
    assert "__room_terrain" in names
    assert "__room_box" in names
    print("[OK] existing __room_terrain no longer blocks __room_box generation")


def test_indoor_detection_prefers_explicit_room_over_terrain_context():
    prompt = (
        "用户确认开始生成：奶油风卧室\n"
        "## 地形与边界约束\n"
        "terrain_spec={'type': 'neutral_ground', 'surface': 'neutral', 'walkable': True}"
    )

    assert SceneComposer._detect_scene_indoor(prompt) is True
    print("[OK] explicit indoor room wins over generic terrain context")


def test_indoor_room_budget_expands_for_many_items():
    composer = SceneComposer(room_size=[5.0, 3.0, 3.0])
    items = [{"name": f"家具{i}"} for i in range(8)]

    composer._apply_indoor_room_budget(items, "做一个可爱的卧室")

    assert 6.5 <= composer.room_size[0] <= 8.5
    assert 6.5 <= composer.room_size[1] <= 8.5
    assert composer.room_size[2] >= 3.0
    assert composer._last_room_budget_summary["changed"] is True
    print("[OK] indoor room budget expands moderately for dense furniture lists")


def test_indoor_room_budget_stays_compact_for_small_rooms():
    composer = SceneComposer(room_size=[5.0, 3.0, 3.0])

    composer._apply_indoor_room_budget([{"name": "床"}, {"name": "台灯"}, {"name": "地毯"}], "可爱卧室")

    assert 5.5 <= composer.room_size[0] <= 6.8
    assert 5.5 <= composer.room_size[1] <= 6.8
    assert composer.room_size[2] >= 3.0
    print("[OK] indoor room budget stays compact for small rooms")


def test_indoor_room_budget_expands_for_large_furniture():
    composer = SceneComposer(room_size=[5.0, 3.0, 3.0])

    composer._apply_indoor_room_budget([{"name": "床"}, {"name": "衣柜"}], "可爱卧室")

    assert 5.5 <= composer.room_size[0] <= 7.2
    assert 5.5 <= composer.room_size[1] <= 7.2
    assert composer.room_size[2] >= 3.2
    print("[OK] large indoor furniture expands room footprint")


def test_indoor_room_budget_respects_larger_zone_tree_box():
    composer = SceneComposer(room_size=[5.0, 3.0, 3.0])
    composer.zone_tree = ZoneTree(root=Zone(
        zone_id="room",
        name="room",
        role="indoor",
        enclosure="box",
        volume=Volume(center=[0.0, 1.6, 0.0], size=[9.0, 9.0, 3.2]),
    ))

    composer._apply_indoor_room_budget([{"name": "床"}], "卧室")

    assert composer.room_size == [9.0, 9.0, 3.2]
    assert composer.zone_tree.root.volume.size == [9.0, 9.0, 3.2]
    print("[OK] larger zone_tree boxes are not shrunk by room budget")


def test_room_bounds_clamp_uses_actor_extents_and_reports_oversized():
    pos, changed, issue, fit_scale = SceneComposer._clamp_position_to_room_bounds(
        [2.4, 0.2, 2.4],
        [0.5, 0.5, 0.5],
        [5.0, 5.0, 3.0],
    )

    assert changed is True
    assert issue == ""
    assert fit_scale == 1.0
    assert pos[0] <= 1.85
    assert pos[2] <= 1.85

    _pos, changed, issue, fit_scale = SceneComposer._clamp_position_to_room_bounds(
        [0.0, 0.0, 0.0],
        [5.0, 1.0, 5.0],
        [5.0, 5.0, 3.0],
    )
    assert changed is False
    assert issue == "object_too_large_for_room_bounds"
    assert fit_scale < 0.65
    print("[OK] room bounds clamp accounts for actor extents and reports oversized objects")


if __name__ == "__main__":
    test_zone_and_asset_routing()
    test_zone_and_door_aabb_helpers()
    test_indoor_room_slot_planner_uses_asset_semantics()
    test_filter_aabbs_by_zone()
    test_micro_batch_plan_splits_content_phases()
    test_pending_notes_apply_to_next_batch_context()
    test_pending_generation_delta_inserts_future_asset()
    test_pending_generation_delta_can_remove_future_asset()
    test_progress_message_is_user_facing_with_batch_context()
    test_progress_message_reports_resource_provider_unavailable()
    test_progressive_post_shell_framework_generates_floor_and_boundary()
    test_f5_demo_mode_disables_vlm_by_default()
    test_f5_demo_mode_reports_vlm_disabled_in_final_text()
    test_aabb_review_issues_flow_to_coordinator_review_result()
    test_vlm_actionable_advice_flows_to_coordinator_review_result()
    test_vlm_low_confidence_advisory_flows_to_coordinator_review_result()
    test_vlm_review_uses_composer_hooks_under_engine_gate()
    test_vlm_target_priority_prefers_scene_anchors_and_high_risk_additions()
    test_vlm_checkpoint_policy_selects_structure_high_risk_and_final_targets()
    test_vlm_high_risk_priority_skips_plain_small_items()
    test_vlm_review_result_carries_checkpoint_and_batch_context()
    test_vlm_checkpoint_reports_summarize_all_stages_without_internal_leakage()
    test_vlm_checkpoint_progress_message_includes_advisory_details()
    test_final_report_text_includes_vlm_status_without_duplicate()
    test_scene_composer_injects_shared_scoped_memory_only()
    test_scene_composer_can_focus_scoped_memory_on_target_actor()
    test_pending_generation_delta_creates_resource_request_for_missing_asset()
    test_batch_resource_plan_carries_contract_version_and_interventions()
    test_pending_resource_request_resolves_models_into_current_batch()
    test_pending_resource_request_defers_while_ready_assets_can_still_import()
    test_pending_resource_request_runs_image_stage_before_model_retrieval()
    test_scene_composer_passes_generated_images_to_model_retrieval_workflow()
    test_pending_resource_request_backlog_is_visible_when_batch_limit_is_hit()
    test_pending_resource_queue_is_bounded_and_reports_overflow()
    test_pending_resource_request_reports_provider_unavailable_without_fake_path()
    test_fantasy_market_terrain_profile_uses_low_decorative_boundary()
    test_warm_mysterious_market_overrides_generic_stone_wall_boundary()
    test_scene_framework_indoor_fallback_generates_room_box_only()
    test_scene_framework_outdoor_fallback_generates_terrain_only()
    test_single_indoor_box_decompose_fallback_keeps_room_box()
    test_scene_framework_mixed_zone_tree_generates_terrain_and_room_box()
    test_mixed_outdoor_structure_fallback_creates_terrain_and_room_box_when_llm_fails()
    test_mixed_outdoor_structure_repairs_terrain_only_llm_decompose()
    test_mixed_outdoor_structure_repairs_box_only_llm_decompose()
    test_mixed_outdoor_structure_keeps_valid_shell_zone()
    test_room_box_generation_is_not_blocked_by_existing_room_terrain()
    test_indoor_detection_prefers_explicit_room_over_terrain_context()
    test_indoor_room_budget_expands_for_many_items()
    test_indoor_room_budget_stays_compact_for_small_rooms()
    test_indoor_room_budget_expands_for_large_furniture()
    test_indoor_room_budget_respects_larger_zone_tree_box()
    test_room_bounds_clamp_uses_actor_extents_and_reports_oversized()
    print("\n=== progressive mixed geometry ALL PASS ===")
