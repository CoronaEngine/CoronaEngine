"""Function-level AgentRuntime tool adapters.

These adapters are deliberately small and side-effect free.  They are the first
step toward decomposing generation capabilities into ToolCall-sized units.
"""

from __future__ import annotations

from functools import partial
from pathlib import Path
import re
import time
from typing import Any, Callable, Mapping, Sequence
import uuid

from .core import (
    ActorFactValidator,
    AssetFactValidator,
    EnvironmentComponentValidator,
    GeometryReviewValidator,
    ResourcePlanValidator,
    RiskLevel,
    StatePatch,
    ToolCall,
    ToolCategory,
    ToolRegistry,
    ToolResult,
)
from .support_semantics import classify_support_type


_ABSTRACT_LAYOUT_TERMS = (
    "主活动区",
    "视觉焦点",
    "功能支撑点",
    "停留点",
    "重点照明",
    "材质/色彩点缀",
    "通行动线",
    "入口/边界",
)

_FALLBACK_SUBSTRATE_TERMS = (
    "天空", "天幕", "草地", "草原", "森林", "树林", "地形", "地面", "山坡",
    "道路", "河流", "小河", "溪流", "湖泊", "湖面", "水面", "sky", "grassland",
    "grass", "forest", "woods", "terrain", "ground", "hill", "road", "river",
    "stream", "lake", "water",
)
_FALLBACK_LAYOUT_TERMS = (
    "入口", "出口", "通道", "动线", "主路", "主街", "区域", "边界", "围合",
    "休息区", "layout", "entrance", "exit", "walkway", "zone", "area", "boundary",
)


class _FallbackSceneElementRoute:
    def __init__(self, name: str, target_pipeline: str) -> None:
        self.name = name
        self.target_pipeline = target_pipeline

    def as_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "category": "environment" if self.target_pipeline == "scene_substrate" else (
                "layout" if self.target_pipeline == "layout_structure" else "asset"
            ),
            "target_pipeline": self.target_pipeline,
            "confidence": 0.5,
            "reason": "runtime fallback classification",
            "generation_mode_hint": "",
        }


class _FallbackSceneElementClassifier:
    """Rule-only classifier used when the integration classifier is unavailable."""

    @staticmethod
    def route_model_items(
        _scene_goal: str,
        items: list[dict[str, Any]],
    ) -> tuple[list[dict[str, Any]], list[_FallbackSceneElementRoute]]:
        model_items: list[dict[str, Any]] = []
        routes: list[_FallbackSceneElementRoute] = []
        for item in items:
            name = str(item.get("name") or item.get("item_name") or "").strip()
            if not name:
                continue
            folded = name.lower()
            if any(term in name or term.lower() in folded for term in _FALLBACK_SUBSTRATE_TERMS):
                pipeline = "scene_substrate"
            elif any(term in name or term.lower() in folded for term in _FALLBACK_LAYOUT_TERMS):
                pipeline = "layout_structure"
            else:
                pipeline = "model"
                model_items.append(dict(item))
            routes.append(_FallbackSceneElementRoute(name, pipeline))
        return model_items, routes

    @staticmethod
    def summarize_classification(routes: Sequence[_FallbackSceneElementRoute]) -> str:
        model_names = [route.name for route in routes if route.target_pipeline == "model"]
        substrate_names = [route.name for route in routes if route.target_pipeline == "scene_substrate"]
        layout_names = [route.name for route in routes if route.target_pipeline == "layout_structure"]
        parts: list[str] = []
        if model_names:
            parts.append("准备生成模型：" + "、".join(model_names))
        if substrate_names:
            parts.append("环境/地形：" + "、".join(substrate_names) + " 将作为场景基底处理，不单独生成模型")
        if layout_names:
            parts.append("布局结构：" + "、".join(layout_names) + " 会进入摆放/结构规划")
        return "；".join(parts)


_FALLBACK_SCENE_ELEMENT_CLASSIFIER = _FallbackSceneElementClassifier()

_TREASURE_ROOM_ITEMS = ("木门", "藏宝箱", "金币堆", "木桌", "木椅", "木箱", "酒桶", "武器架", "火把")
_BEDROOM_ITEMS = ("床", "书桌", "衣柜", "台灯", "地毯", "玩偶", "书架")
_MARKET_ITEMS = ("入口拱门", "摊位", "导视牌", "长椅", "灯笼", "展示架")
_COMMON_ADD_OBJECTS = (
    "天使雕像",
    "雕像",
    "小狗",
    "狗",
    "向日葵",
    "花盆",
    "沙袋",
    "木箱",
    "火把",
    "灯笼",
)

_TREASURE_ROOM_TEXT_MARKERS = (
    "藏宝室",
    "宝库",
    "密室",
    "藏宝",
    "宝箱",
    "金币",
    "treasure",
    "vault",
    "钘忓疂",
    "瀹濈",
    "疂绠",
    "寮虹洍",
    "鐩楄棌",
)
_BEDROOM_TEXT_MARKERS = ("卧室", "bedroom", "鍗у", "搴", "台灯", "床")
_MARKET_TEXT_MARKERS = ("集市", "夜市", "市场", "market")
_ADD_OBJECT_ALIASES = (
    (("天使雕像", "天使", "雕像", "澶╀娇", "闆曞儚", "ぉ浣", "洉鍍"), "天使雕像"),
    (("小狗", "狗", "灏忕嫍", "嫍"), "小狗"),
    (("向日葵", "葵", "鍚戞棩钁"), "向日葵"),
    (("帐篷", "帐", "甯愮", "帐�"), "帐篷"),
    (("藏宝箱", "宝箱", "钘忓疂绠"), "藏宝箱"),
    (("金币堆", "金币", "閲戝竵"), "金币堆"),
)


ResourceProvider = Callable[[dict[str, Any]], dict[str, Any]]


def register_agent_runtime_planning_tools(
    registry: ToolRegistry,
    *,
    scene_element_classifier: Any = None,
    image_resource_provider: ResourceProvider | None = None,
    model_resource_provider: ResourceProvider | None = None,
    environment_component_provider: ResourceProvider | None = None,
    environment_import_provider: ResourceProvider | None = None,
    actor_import_provider: ResourceProvider | None = None,
    review_provider: ResourceProvider | None = None,
    vlm_review_provider: ResourceProvider | None = None,
    require_engine_environment_import: bool = False,
    require_engine_actor_import: bool = False,
    strict_image_to_model_pipeline: bool = False,
) -> None:
    """Register no-side-effect planning/classification tools."""

    classifier = scene_element_classifier or _FALLBACK_SCENE_ELEMENT_CLASSIFIER

    if not registry.has("runtime.plan.extract"):
        registry.register(
            "runtime.plan.extract",
            _extract_scene_plan_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "text"),
            produces_state=("plan_extractions",),
            description="Extract concrete scene items and layout hints from user text.",
        )
    if not registry.has("scene.extract_objects"):
        registry.register(
            "scene.extract_objects",
            partial(_scene_extract_objects_tool, scene_element_classifier=classifier),
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "text"),
            produces_state=("plan_extractions",),
            description="Phase 3 Agent-native object extraction tool; side-effect-free and planner-owned.",
        )
    if not registry.has("scene.classify_type"):
        registry.register(
            "scene.classify_type",
            _scene_classify_type_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "text"),
            produces_state=("custom_scene_facts",),
            description="Phase 3 Agent-native scene type classification tool; side-effect-free and planner-owned.",
        )
    if not registry.has("scene.extract_constraints"):
        registry.register(
            "scene.extract_constraints",
            _scene_extract_constraints_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "text"),
            produces_state=("custom_scene_facts",),
            description="Phase 3 Agent-native scene constraint extraction tool; side-effect-free and planner-owned.",
        )
    if not registry.has("room.estimate_bounds"):
        registry.register(
            "room.estimate_bounds",
            partial(_room_estimate_bounds_tool, scene_element_classifier=classifier),
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "text"),
            produces_state=("custom_scene_facts",),
            description="Phase 3 Agent-native room or terrain bounds estimate tool; side-effect-free and planner-owned.",
        )
    if not registry.has("zone.decompose"):
        registry.register(
            "zone.decompose",
            _zone_decompose_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "text"),
            produces_state=("custom_scene_facts",),
            description="Phase 3 Agent-native zone decomposition tool; side-effect-free and planner-owned.",
        )
    if not registry.has("asset.route_item"):
        registry.register(
            "asset.route_item",
            partial(_asset_route_item_tool, scene_element_classifier=classifier),
            category=ToolCategory.ASSET,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "items"),
            consumes_state={
                "items": {
                    "state_key": "plan_extractions",
                    "scope": "plan",
                }
            },
            produces_state=("asset_request_plans",),
            description="Phase 3 Agent-native per-item asset routing tool; side-effect-free and planner-owned.",
        )
    if not registry.has("placement.prepare_items"):
        registry.register(
            "placement.prepare_items",
            partial(_placement_prepare_items_tool, scene_element_classifier=classifier),
            category=ToolCategory.GEOMETRY,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "items"),
            consumes_state={
                "items": {
                    "state_key": "plan_extractions",
                    "scope": "plan",
                }
            },
            produces_state=("placement_proposals",),
            description="Phase 3 Agent-native placement input preparation tool; side-effect-free and planner-owned.",
        )
    if not registry.has("batch.prioritize_items"):
        registry.register(
            "batch.prioritize_items",
            _batch_prioritize_items_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "items"),
            produces_state=("custom_batch_facts",),
            description="Phase 4 Agent-native batch item prioritization tool; side-effect-free and batch-plan-owned.",
        )
    if not registry.has("batch.merge_intervention"):
        registry.register(
            "batch.merge_intervention",
            _batch_merge_intervention_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "interventions"),
            produces_state=("custom_batch_facts",),
            description="Phase 4 Agent-native pending intervention merge tool; side-effect-free and batch-plan-owned.",
        )
    if not registry.has("batch.create"):
        registry.register(
            "batch.create",
            _batch_create_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "items"),
            produces_state=("custom_batch_facts",),
            description="Phase 4 Agent-native batch creation tool; emits batch plan drafts before RuntimeState persistence.",
        )
    if not registry.has("runtime.queue.select_next_graph"):
        registry.register(
            "runtime.queue.select_next_graph",
            _select_next_tool_graph_from_queue_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id",),
            consumes_state={
                "queue": {
                    "state_key": "tool_graph_queue",
                    "scope": "room",
                },
            },
            produces_state=("custom_queue_facts",),
            description="Phase 5 Runtime queue selection tool; emits an auditable next ToolCallGraph decision.",
        )
    if not registry.has("runtime.queue.plan_enqueue_items"):
        registry.register(
            "runtime.queue.plan_enqueue_items",
            _plan_tool_graph_queue_items_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "graph_refs"),
            consumes_state={
                "queue": {
                    "state_key": "tool_graph_queue",
                    "scope": "room",
                },
            },
            produces_state=("custom_queue_facts",),
            description="Phase 5 Runtime queue enqueue planning tool; emits safe queue item drafts before persistence.",
        )
    if not registry.has("runtime.queue.mark_graph_status"):
        registry.register(
            "runtime.queue.mark_graph_status",
            _mark_tool_graph_queue_status_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "target_graph_ref", "status"),
            consumes_state={
                "queue": {
                    "state_key": "tool_graph_queue",
                    "scope": "room",
                },
            },
            produces_state=("tool_graph_queue",),
            description="Phase 5 Runtime queue status tool; persists auditable ToolCallGraph queue state transitions.",
        )
    if not registry.has("scene.extract_environment"):
        registry.register(
            "scene.extract_environment",
            partial(_scene_extract_environment_tool, scene_element_classifier=classifier),
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "text"),
            produces_state=("element_routes", "classification_summaries", "substrate_plans"),
            description="Phase 3 Agent-native environment/substrate extraction tool; side-effect-free and planner-owned.",
        )
    if not registry.has("runtime.elements.classify"):
        registry.register(
            "runtime.elements.classify",
            partial(_classify_scene_elements_tool, scene_element_classifier=classifier),
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "text", "items"),
            consumes_state={
                "items": {
                    "state_key": "plan_extractions",
                    "scope": "plan",
                }
            },
            produces_state=("element_routes", "model_item_lists", "classification_summaries"),
            description="Route model objects versus terrain/substrate/layout elements.",
        )
    if not registry.has("runtime.asset.plan"):
        registry.register(
            "runtime.asset.plan",
            _plan_asset_requests_tool,
            category=ToolCategory.ASSET,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "model_items"),
            consumes_state={
                "model_items": {
                    "state_key": "model_item_lists",
                    "scope": "batch",
                }
            },
            produces_state=("asset_request_plans",),
            description="Create side-effect-free asset request records for model items.",
        )
    if not registry.has("runtime.substrate.plan"):
        registry.register(
            "runtime.substrate.plan",
            _plan_substrate_requests_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "routes"),
            consumes_state={
                "routes": {
                    "state_key": "element_routes",
                    "scope": "batch",
                }
            },
            produces_state=("substrate_plans",),
            description="Create side-effect-free terrain/environment/substrate plan records from classified routes.",
        )
    if not registry.has("runtime.substrate.resolve"):
        registry.register(
            "runtime.substrate.resolve",
            _resolve_substrate_requests_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "substrate_plan"),
            consumes_state={
                "substrate_plan": {
                    "state_key": "substrate_plans",
                    "scope": "batch",
                }
            },
            produces_state=("substrate_resolutions",),
            description="Resolve substrate plans into engine-neutral component requests without writing the scene.",
        )
    if not registry.has("runtime.environment.create_components"):
        registry.register(
            "runtime.environment.create_components",
            _make_environment_components_tool(environment_component_provider),
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "substrate_resolutions"),
            consumes_state={
                "substrate_resolutions": {
                    "state_key": "substrate_resolutions",
                    "scope": "batch",
                },
                "room_bounds": {
                    "state_key": "custom_scene_facts",
                    "scope": "key",
                    "source_arg": "bounds_fact_id",
                },
            },
            produces_state=("environment_components", "assets"),
            requires_user_visible_failure=True,
            description="Create Runtime environment component facts from resolved substrate requests without engine writes.",
        )
    if not registry.has("runtime.environment.import_components"):
        registry.register(
            "runtime.environment.import_components",
            _make_environment_import_components_tool(
                environment_import_provider,
                require_engine_environment_import=bool(require_engine_environment_import),
            ),
            category=ToolCategory.IMPORT,
            default_risk_level=RiskLevel.LOW,
            requires_write=True,
            required_args=("room_id", "batch_id", "environment_components"),
            consumes_state={
                "environment_components": {
                    "state_key": "environment_components",
                    "scope": "batch",
                }
            },
            produces_state=("environment_components", "custom_import_facts"),
            requires_user_visible_failure=True,
            description="Import Runtime environment components through the dedicated engine bridge.",
        )
    if not registry.has("runtime.asset.image.prepare"):
        registry.register(
            "runtime.asset.image.prepare",
            _make_image_resource_tool(
                image_resource_provider,
                strict_image_to_model=bool(strict_image_to_model_pipeline),
            ),
            category=ToolCategory.ASSET,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "asset_requests"),
            consumes_state={
                "asset_requests": {
                    "state_key": "asset_request_plans",
                    "scope": "batch",
                },
                "model_items": {
                    "state_key": "model_item_lists",
                    "scope": "batch",
                }
            },
            produces_state=("image_resource_plans", "custom_resource_phase_facts"),
            description="Prepare per-batch image/reference resource facts through the configured provider or Runtime fallback.",
        )
    if not registry.has("runtime.asset.model.prepare"):
        registry.register(
            "runtime.asset.model.prepare",
            _make_model_resource_tool(
                model_resource_provider,
                strict_image_to_model=bool(strict_image_to_model_pipeline),
            ),
            category=ToolCategory.ASSET,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "asset_requests"),
            consumes_state={
                "asset_requests": {
                    "state_key": "asset_request_plans",
                    "scope": "batch",
                },
                "model_items": {
                    "state_key": "model_item_lists",
                    "scope": "batch",
                },
                "image_resources": {
                    "state_key": "image_resource_plans",
                    "scope": "batch",
                }
            },
            produces_state=("model_resource_plans", "assets", "custom_resource_phase_facts"),
            description="Prepare per-batch model resource facts through the configured provider or Runtime fallback.",
        )
    if not registry.has("runtime.placement.propose"):
        registry.register(
            "runtime.placement.propose",
            _propose_placement_tool,
            category=ToolCategory.GEOMETRY,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "model_items"),
            consumes_state={
                "model_items": {
                    "state_key": "model_item_lists",
                    "scope": "batch",
                },
                "observed_actors": {
                    "state_key": "observed_actors",
                    "scope": "room",
                },
                "environment_components": {
                    "state_key": "environment_components",
                    "scope": "batch",
                },
            },
            produces_state=("placement_proposals",),
            description="Create deterministic low-risk placement proposals without engine writes.",
        )
    if not registry.has("runtime.actor.plan_import_batch"):
        registry.register(
            "runtime.actor.plan_import_batch",
            _plan_actor_import_batch_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "model_items"),
            consumes_state={
                "model_items": {
                    "state_key": "model_item_lists",
                    "scope": "batch",
                },
                "image_resources": {
                    "state_key": "image_resource_plans",
                    "scope": "batch",
                },
                "model_resources": {
                    "state_key": "model_resource_plans",
                    "scope": "batch",
                },
                "placements": {
                    "state_key": "placement_proposals",
                    "scope": "batch",
                },
                "environment_components": {
                    "state_key": "environment_components",
                    "scope": "batch",
                },
            },
            produces_state=("custom_import_facts",),
            description="Create an auditable actor import plan before the engine-write import step.",
        )
    if not registry.has("runtime.actor.import_batch"):
        registry.register(
            "runtime.actor.import_batch",
            _make_actor_import_tool(
                actor_import_provider,
                require_engine_actor_import=bool(require_engine_actor_import),
            ),
            category=ToolCategory.IMPORT,
            default_risk_level=RiskLevel.LOW,
            requires_write=True,
            required_args=("room_id", "batch_id", "model_items"),
            consumes_state={
                "model_items": {
                    "state_key": "model_item_lists",
                    "scope": "batch",
                },
                "image_resources": {
                    "state_key": "image_resource_plans",
                    "scope": "batch",
                },
                "model_resources": {
                    "state_key": "model_resource_plans",
                    "scope": "batch",
                },
                "placements": {
                    "state_key": "placement_proposals",
                    "scope": "batch",
                },
                "environment_components": {
                    "state_key": "environment_components",
                    "scope": "batch",
                },
                "actor_import_plan": {
                    "state_key": "custom_import_facts",
                    "scope": "batch",
                },
            },
            produces_state=("actors", "custom_import_facts"),
            description="Import a whole Runtime batch through the configured actor import provider.",
        )
    if not registry.has("runtime.geometry.review"):
        registry.register(
            "runtime.geometry.review",
            _make_geometry_review_tool(review_provider),
            category=ToolCategory.GEOMETRY,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "batch_id", "placements"),
            consumes_state={
                "placements": {
                    "state_key": "placement_proposals",
                    "scope": "batch",
                },
                "environment_components": {
                    "state_key": "environment_components",
                    "scope": "batch",
                },
            },
            produces_state=("geometry_reviews",),
            description="Review proposed placements with optional advisory provider, then persist review facts.",
        )
    if not registry.has("runtime.geometry.compute_aabb"):
        registry.register(
            "runtime.geometry.compute_aabb",
            _compute_actor_aabb_facts_tool,
            category=ToolCategory.GEOMETRY,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "actors"),
            consumes_state={
                "actors": {
                    "state_key": "actors",
                    "scope": "room",
                },
            },
            produces_state=("custom_geometry_facts",),
            description="Compute safe Runtime actor AABB facts without engine writes.",
        )
    if not registry.has("runtime.geometry.check_overlap"):
        registry.register(
            "runtime.geometry.check_overlap",
            _check_actor_overlap_facts_tool,
            category=ToolCategory.GEOMETRY,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "actors"),
            consumes_state={
                "actors": {
                    "state_key": "actors",
                    "scope": "room",
                },
            },
            produces_state=("custom_geometry_facts",),
            description="Check safe Runtime actor AABB overlaps without engine writes.",
        )
    if not registry.has("runtime.geometry.snap_to_ground_selective"):
        registry.register(
            "runtime.geometry.snap_to_ground_selective",
            _snap_actor_grounding_facts_tool,
            category=ToolCategory.GEOMETRY,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "actors"),
            consumes_state={
                "actors": {
                    "state_key": "actors",
                    "scope": "room",
                },
            },
            produces_state=("custom_geometry_facts", "geometry_reviews"),
            description="Plan selective floor-supported actor ground snapping without engine writes.",
        )
    if not registry.has("runtime.review.vlm_checkpoint"):
        registry.register(
            "runtime.review.vlm_checkpoint",
            _make_vlm_checkpoint_tool(vlm_review_provider),
            category=ToolCategory.REVIEW,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "batch_id"),
            consumes_state={
                "actors": {
                    "state_key": "actors",
                    "scope": "room",
                },
                "placements": {
                    "state_key": "placement_proposals",
                    "scope": "batch",
                },
                "environment_components": {
                    "state_key": "environment_components",
                    "scope": "batch",
                },
            },
            produces_state=("custom_vlm_checkpoint_facts", "review_advisory_proposals"),
            suppress_dependency_skip_event=True,
            description="Run an optional VLM checkpoint and persist advisory proposals without changing the scene.",
        )
    if not registry.has("runtime.review.summarize_batch"):
        registry.register(
            "runtime.review.summarize_batch",
            _summarize_batch_review_tool,
            category=ToolCategory.REVIEW,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "batch_id"),
            consumes_state={
                "geometry_review": {
                    "state_key": "geometry_reviews",
                    "scope": "batch",
                },
                "ground_snap_reviews": {
                    "state_key": "geometry_reviews",
                    "scope": "room",
                },
                "vlm_checkpoints": {
                    "state_key": "custom_vlm_checkpoint_facts",
                    "scope": "room",
                },
                "actor_import_plan": {
                    "state_key": "custom_import_facts",
                    "scope": "batch",
                },
                "actors": {
                    "state_key": "actors",
                    "scope": "room",
                },
            },
            produces_state=("custom_review_summary_facts",),
            description="Create a safe per-batch review summary fact after import and advisory checkpoints.",
        )
    if not registry.has("runtime.review.generate_adjustment_proposal"):
        registry.register(
            "runtime.review.generate_adjustment_proposal",
            _generate_review_adjustment_proposal_tool,
            category=ToolCategory.REVIEW,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "batch_id"),
            consumes_state={
                "geometry_review": {
                    "state_key": "geometry_reviews",
                    "scope": "batch",
                },
                "ground_snap_reviews": {
                    "state_key": "geometry_reviews",
                    "scope": "room",
                },
                "batch_review_summary": {
                    "state_key": "custom_review_summary_facts",
                    "scope": "batch",
                },
                "review_advisories": {
                    "state_key": "review_advisory_proposals",
                    "scope": "room",
                },
            },
            produces_state=("layout_adjustment_proposals",),
            description="Convert safe review findings into a confirmable low-risk adjustment proposal without applying it.",
        )
    if not registry.has("runtime.layout.adjust_propose"):
        registry.register(
            "runtime.layout.adjust_propose",
            _propose_layout_adjustment_tool,
            category=ToolCategory.GEOMETRY,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id", "issues"),
            consumes_state={
                "issues": {
                    "state_key": "geometry_reviews",
                    "scope": "plan",
                }
            },
            produces_state=("layout_adjustment_proposals",),
            description="Convert low-risk geometry issues into layout adjustment deltas.",
        )
    if not registry.has("runtime.intervention.plan_next_batch"):
        registry.register(
            "runtime.intervention.plan_next_batch",
            _plan_next_intervention_batch_tool,
            category=ToolCategory.PLAN,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id", "plan_id"),
            consumes_state={
                "pending_interventions": {
                    "state_key": "pending_interventions",
                    "scope": "room",
                }
            },
            produces_state=("custom_intervention_route_facts",),
            description="Classify pending interventions into next-batch absorbable and non-absorbable route facts.",
        )


def register_agent_runtime_scene_read_tools(
    registry: ToolRegistry,
    scene_snapshot_provider: Callable[[Any], dict[str, Any]] | None = None,
) -> None:
    provider = scene_snapshot_provider or _empty_scene_snapshot_provider
    if not registry.has("runtime.scene.snapshot"):
        registry.register(
            "runtime.scene.snapshot",
            _make_scene_snapshot_tool(provider),
            category=ToolCategory.SYNC,
            default_risk_level=RiskLevel.LOW,
            required_args=("room_id",),
            produces_state=("engine_scene_snapshots", "observed_actors", "actors"),
            description="Read current engine scene actor snapshot into RuntimeState.",
        )


def extract_candidate_items(text: str) -> list[str]:
    """Small deterministic extraction slice used until Planner Agent is wired in."""

    clean = str(text or "")
    lowered = clean.lower()
    aliased_objects = [
        canonical
        for aliases, canonical in _ADD_OBJECT_ALIASES
        if any(alias and alias in clean for alias in aliases)
    ]
    if any(marker in clean or marker in lowered for marker in _TREASURE_ROOM_TEXT_MARKERS):
        return list(_TREASURE_ROOM_ITEMS)
    if any(marker in clean or marker in lowered for marker in _BEDROOM_TEXT_MARKERS):
        return list(_BEDROOM_ITEMS)
    if any(marker in clean or marker in lowered for marker in _MARKET_TEXT_MARKERS):
        return list(_MARKET_ITEMS)
    explicit_mentions = _explicit_item_candidates_from_text(clean)
    if explicit_mentions:
        # Keep the user's concrete noun phrase. A broad alias such as "statue"
        # must not expand a specific Cupid statue into a second, different item.
        exact_aliases = [canonical for canonical in aliased_objects if canonical in clean]
        merged_mentions = list(dict.fromkeys([*explicit_mentions, *exact_aliases]))
        if len(merged_mentions) > 1 or not any(_looks_like_mojibake_name(name) for name in merged_mentions):
            return merged_mentions
    if aliased_objects:
        return list(dict.fromkeys(aliased_objects))
    explicit_objects = [item for item in _COMMON_ADD_OBJECTS if item in clean]
    explicit_objects.extend(aliased_objects)
    if explicit_objects:
        return list(dict.fromkeys(explicit_objects))
    return ["主体物件", "辅助物件"]


def _explicit_item_candidates_from_text(text: str) -> list[str]:
    value = str(text or "").strip()
    if not value:
        return []
    value = re.sub(r"@\S+\s*", "", value)
    value = re.sub(
        r"(?i)\b(?:create|generate|make|build|with|and|plus|include|including)\b",
        ",",
        value,
    )
    value = re.sub(
        r"(?:生成|创建|做一个|做个|有|包含|包括|以及|和|还有|再加入|再添加|再新增|再加|加入|添加|新增)",
        "，",
        value,
    )
    chunks = re.split(r"[、，,;；。.\n]+", value)
    ignored_tokens = {
        "简单",
        "一个",
        "场景",
    }
    candidates: list[str] = []
    for chunk in chunks:
        item = re.sub(r"\s+", " ", str(chunk or "").strip())
        item = re.sub(
            r"^(?:请|后面再|后续再|再)?(?:加入|添加|新增|放入|生成添加)\s*(?:一个|一只|一座)?\s*",
            "",
            item,
        )
        item = item.strip(" -:：")
        if not item:
            continue
        lowered = item.lower()
        if lowered in ignored_tokens or item in ignored_tokens:
            continue
        if _is_scene_container_phrase(item):
            continue
        if len(item) > 32:
            continue
        if any(word in lowered for word in ("please", "scene", "style", "simple")):
            continue
        candidates.append(item)
    return list(dict.fromkeys(candidates))[:8]


def _is_scene_container_phrase(value: str) -> bool:
    text = str(value or "").strip()
    lowered = text.lower()
    if not text:
        return False
    if text.endswith(("场景", "空间", "营地", "房间", "集市", "市场")) and len(text) > 2:
        return True
    if lowered.startswith(("a ", "an ", "the ")):
        lowered = lowered.split(" ", 1)[1].strip()
    return lowered.endswith((" scene", " space", " camp", " room", " market"))


def _explicit_substrate_candidates_from_text(text: str) -> list[str]:
    substrate_terms = (
        "天空",
        "天幕",
        "草地",
        "草原",
        "森林",
        "树林",
        "地形",
        "地面",
        "山坡",
        "道路",
        "河流",
        "小河",
        "溪流",
        "湖泊",
        "湖面",
        "水面",
        "sky",
        "grassland",
        "grass",
        "forest",
        "woods",
        "terrain",
        "ground",
        "hill",
        "road",
        "river",
        "stream",
        "lake",
        "water",
    )
    out: list[str] = []
    clean = str(text or "")
    lowered_text = clean.lower()
    for term in substrate_terms:
        term_text = str(term)
        if term_text and (term_text in clean or term_text.lower() in lowered_text):
            out.append(term_text)
    for item in _explicit_item_candidates_from_text(text):
        normalized = str(item or "").strip()
        lowered = normalized.lower()
        if normalized in substrate_terms:
            out.append(normalized)
        elif lowered in substrate_terms:
            out.append(lowered)
    return list(dict.fromkeys(out))


def _validate_entity_names(
    items: list[str],
    *,
    source_text: str,
) -> tuple[list[str], list[dict[str, str]]]:
    from ..runtime_action_intent import EntityNameValidator

    valid: list[str] = []
    rejected: list[dict[str, str]] = []
    for item in items:
        canonical, reason = EntityNameValidator.validate(item, source_text=source_text)
        if not canonical:
            rejected.append({"raw_name": str(item or ""), "reason": reason or "invalid entity name"})
            continue
        if canonical not in valid:
            valid.append(canonical)
    return valid, rejected


def route_candidate_model_names(
    text: str,
    items: list[str] | None = None,
    *,
    scene_element_classifier: Any = None,
) -> list[str]:
    candidates, _ = _validate_entity_names(
        list(items or extract_candidate_items(text)),
        source_text=text,
    )
    rows = [{"name": item} for item in candidates]
    classifier = scene_element_classifier or _FALLBACK_SCENE_ELEMENT_CLASSIFIER
    route_model_items = classifier.route_model_items
    model_items, _ = route_model_items(text, rows)
    routed = [str(item.get("name") or "") for item in model_items if str(item.get("name") or "")]
    if any(_looks_like_mojibake_name(name) for name in routed):
        known_candidates = [
            str(item)
            for item in candidates
            if str(item) and str(item) not in {"森林", "天空", "草地", "地形", "环境"}
        ]
        if len(routed) == len(known_candidates):
            return known_candidates
        if len(routed) <= len(known_candidates):
            return known_candidates[:len(routed)]
    validated, _ = _validate_entity_names(routed, source_text=text)
    return validated


def _looks_like_mojibake_name(name: str) -> bool:
    text = str(name or "")
    if "\ufffd" in text or "�" in text:
        return True
    return any("\ue000" <= char <= "\uf8ff" for char in text)


def _extract_scene_plan_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    text = str(call.args.get("text") or "")
    candidates, rejected_items = _validate_entity_names(
        _filter_abstract_items(extract_candidate_items(text)),
        source_text=text,
    )
    layout_items = _derive_layout_items(text)
    extraction_id = str(call.args.get("plan_id") or call.tool_call_id)
    item_count = len([item for item in candidates if str(item or "")])
    component_count = len([item for item in layout_items if str(item or "")])
    return ToolResult(
        True,
        "scene plan extracted",
        state_patch=StatePatch(
            room_id=room_id,
            changes={
                "plan_extractions": {
                    extraction_id: {
                        "text": text,
                        "candidate_items": list(candidates),
                        "layout_items": list(layout_items),
                    }
                }
            },
        ),
        payload={
            "candidate_items": candidates,
            "layout_items": layout_items,
            "item_count": item_count,
            "component_count": component_count,
            "rejected_entity_items": rejected_items,
        },
        user_visible_message=f"方案提炼完成：识别出 {item_count} 个候选物体，{component_count} 个布局/环境要素。",
    )


def _scene_extract_objects_tool(
    call: ToolCall,
    *,
    scene_element_classifier: Any = None,
) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    text = str(call.args.get("text") or "")
    extraction_id = str(call.args.get("plan_id") or call.args.get("extraction_id") or call.tool_call_id)
    candidates, rejected_items = _validate_entity_names(
        _filter_abstract_items(extract_candidate_items(text)),
        source_text=text,
    )
    object_items = route_candidate_model_names(
        text,
        candidates,
        scene_element_classifier=scene_element_classifier,
    )
    layout_items = _derive_layout_items(text)
    return ToolResult(
        True,
        "scene objects extracted",
        state_patch=StatePatch(
            room_id=room_id,
            changes={
                "plan_extractions": {
                    extraction_id: {
                        "text": text,
                        "candidate_items": list(object_items),
                        "layout_items": list(layout_items),
                    }
                }
            },
        ),
        payload={
            "object_items": list(object_items),
            "layout_items": list(layout_items),
            "item_count": len(object_items),
            "rejected_entity_items": rejected_items,
        },
        user_visible_message=f"物体提炼完成：准备生成模型 {len(object_items)} 个。",
    )


def _infer_scene_type(text: str) -> dict[str, str]:
    clean = str(text or "")
    lowered = clean.lower()
    if any(term in clean for term in ("室内外", "内外混合", "混合场景", "室内与室外")) or any(
        term in lowered for term in ("indoor outdoor", "indoor-outdoor", "mixed scene", "hybrid scene")
    ):
        return {
            "scene_type": "mixed",
            "environment_type": "mixed_foundation",
            "reason": "mixed indoor/outdoor keywords",
        }
    if any(term in clean for term in ("藏宝室", "宝库", "密室", "卧室", "房间", "室内")) or any(
        term in lowered for term in ("treasure room", "vault", "chamber", "bedroom", "indoor")
    ):
        return {
            "scene_type": "indoor",
            "environment_type": "room_box",
            "reason": "indoor keywords",
        }
    if any(term in clean for term in ("森林", "草原", "营地", "天空", "山坡", "河流", "小河", "溪流", "湖泊", "水面", "室外", "集市", "夜市")) or any(
        term in lowered for term in ("forest", "grassland", "camp", "sky", "outdoor", "market", "river", "stream", "lake", "water")
    ):
        return {
            "scene_type": "outdoor",
            "environment_type": "terrain_substrate",
            "reason": "outdoor/substrate keywords",
        }
    return {
        "scene_type": "unspecified",
        "environment_type": "unknown",
        "reason": "no stable scene type signal",
    }


def _scene_classify_type_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    text = str(call.args.get("text") or "")
    fact_id = str(call.args.get("plan_id") or call.args.get("fact_id") or call.tool_call_id)
    inferred = _infer_scene_type(text)
    fact = {
        "fact_id": fact_id,
        "fact_type": "scene_type",
        "source_tool": "scene.classify_type",
        "text_preview": text[:160],
        **inferred,
    }
    return ToolResult(
        True,
        "scene type classified",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_scene_facts": {fact_id: fact}},
        ),
        payload=fact,
        user_visible_message=(
            f"空间类型判断完成：{inferred['scene_type']} / {inferred['environment_type']}。"
        ),
    )


def _has_any(text: str, lowered: str, terms: tuple[str, ...]) -> bool:
    for term in terms:
        if not term:
            continue
        if term.isascii():
            if term.lower() in lowered:
                return True
        elif term in text:
            return True
    return False


def _append_unique(target: list[str], value: str) -> None:
    value = str(value or "").strip()
    if value and value not in target:
        target.append(value)


def _extract_scene_constraints(text: str) -> dict[str, Any]:
    clean = str(text or "")
    lowered = clean.lower()
    mood: list[str] = []
    style_keywords: list[str] = []
    avoid_keywords: list[str] = []
    palette: list[str] = []
    lighting: list[str] = []
    scale_rules: list[str] = []
    placement_rules: list[str] = []

    if _has_any(clean, lowered, ("神秘", "mysterious", "mystery")):
        _append_unique(mood, "mysterious")
        _append_unique(style_keywords, "mystery")
    if _has_any(clean, lowered, ("温暖", "暖", "warm", "cozy")):
        _append_unique(mood, "warm")
        _append_unique(palette, "warm")
    if _has_any(clean, lowered, ("可爱", "cute", "soft")):
        _append_unique(mood, "cute")
        _append_unique(style_keywords, "soft")
    if _has_any(clean, lowered, ("强盗", "山贼", "bandit", "robber")):
        _append_unique(style_keywords, "bandit")
    if _has_any(clean, lowered, ("藏宝室", "宝库", "密室", "treasure", "vault", "chamber")):
        _append_unique(style_keywords, "treasure_room")

    if _has_any(clean, lowered, ("不要太恐怖", "别太恐怖", "不恐怖", "不要恐怖", "not too scary", "not horror")):
        _append_unique(avoid_keywords, "too_horror")
    if _has_any(clean, lowered, ("不要全是暗黑", "不要全暗", "not all dark", "avoid all dark")):
        _append_unique(avoid_keywords, "all_dark_style")
    if _has_any(clean, lowered, ("不要穿模", "不能穿模", "no clipping")):
        _append_unique(avoid_keywords, "clipping")

    if _has_any(clean, lowered, ("灯光", "灯笼", "火把", "烛", "light", "lantern", "torch", "candle")):
        _append_unique(lighting, "practical_lights")
    if _has_any(clean, lowered, ("暖光", "暖黄", "warm light", "golden")):
        _append_unique(lighting, "warm_light")
        _append_unique(palette, "warm_gold")
    if _has_any(clean, lowered, ("金色", "金币", "gold")):
        _append_unique(palette, "gold")

    if _has_any(clean, lowered, ("足够大", "大一点", "大型", "large", "big")):
        _append_unique(scale_rules, "large_object_allowed")
    if _has_any(clean, lowered, ("几个人", "可逛", "能逛", "walkable", "several people")):
        _append_unique(placement_rules, "keep_walkable_path")
    if _has_any(clean, lowered, ("入口", "通道", "主街", "entrance", "path", "main street")):
        _append_unique(placement_rules, "preserve_entrance_and_path")
    if _has_any(clean, lowered, ("休息区", "长椅", "坐", "rest area", "bench")):
        _append_unique(placement_rules, "reserve_rest_area")
    if _has_any(clean, lowered, ("风格统一", "统一风格", "consistent style", "style consistency")):
        _append_unique(placement_rules, "preserve_style_consistency")

    return {
        "mood": mood,
        "style_keywords": style_keywords,
        "avoid_keywords": avoid_keywords,
        "palette": palette,
        "lighting": lighting,
        "scale_rules": scale_rules,
        "placement_rules": placement_rules,
    }


def _scene_extract_constraints_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    text = str(call.args.get("text") or "")
    fact_id = str(call.args.get("plan_id") or call.args.get("fact_id") or call.tool_call_id)
    constraints = _extract_scene_constraints(text)
    fact = {
        "fact_id": fact_id,
        "fact_type": "scene_constraints",
        "source_tool": "scene.extract_constraints",
        "text_preview": text[:160],
        **constraints,
    }
    signal_count = sum(len(value) for value in constraints.values() if isinstance(value, list))
    return ToolResult(
        True,
        "scene constraints extracted",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_scene_facts": {fact_id: fact}},
        ),
        payload=fact,
        user_visible_message=f"方案约束提炼完成：识别出 {signal_count} 条风格、避雷或摆放约束。",
    )


def _estimate_bounds_from_text(
    text: str,
    items: list[str] | None = None,
    *,
    scene_element_classifier: Any = None,
) -> dict[str, Any]:
    clean = str(text or "")
    inferred = _infer_scene_type(clean)
    raw_items = items if items is not None else extract_candidate_items(clean)
    object_items = route_candidate_model_names(
        clean,
        _filter_abstract_items(raw_items),
        scene_element_classifier=scene_element_classifier,
    )
    lowered_items = " ".join(str(item or "").lower() for item in object_items)
    item_count = len(object_items)
    large_terms = (
        "床",
        "沙发",
        "衣柜",
        "柜",
        "书架",
        "长桌",
        "宝箱",
        "藏宝箱",
        "雕像",
        "bed",
        "sofa",
        "cabinet",
        "wardrobe",
        "shelf",
        "statue",
        "chest",
    )
    large_count = sum(1 for term in large_terms if term in lowered_items or term in clean)
    scene_type = inferred["scene_type"]
    environment_type = inferred["environment_type"]
    if environment_type == "room_box":
        width = 5.8
        depth = 5.8
        if item_count >= 5:
            width = depth = 6.8
        if item_count >= 8:
            width = depth = 7.6
        if large_count:
            width += min(1.0, 0.35 * large_count)
            depth += min(1.0, 0.35 * large_count)
        width = min(8.5, round(width, 2))
        depth = min(8.5, round(depth, 2))
        height = 3.2 if large_count else 3.0
        return {
            "scene_type": scene_type,
            "environment_type": environment_type,
            "bounds_type": "room_box",
            "width": width,
            "depth": depth,
            "height": height,
            "size": [width, depth, height],
            "open_sides": ["front"],
            "item_count": item_count,
            "large_item_count": large_count,
            "reason": "indoor object budget",
        }
    if environment_type == "terrain_substrate":
        width = 12.0
        depth = 12.0
        if item_count >= 5:
            width = depth = 14.0
        return {
            "scene_type": scene_type,
            "environment_type": environment_type,
            "bounds_type": "terrain_area",
            "width": width,
            "depth": depth,
            "height": 0.0,
            "size": [width, depth, 0.0],
            "open_sides": ["all"],
            "item_count": item_count,
            "large_item_count": large_count,
            "reason": "outdoor terrain budget",
        }
    return {
        "scene_type": scene_type,
        "environment_type": environment_type,
        "bounds_type": "unknown",
        "width": 6.0,
        "depth": 6.0,
        "height": 3.0,
        "size": [6.0, 6.0, 3.0],
        "open_sides": [],
        "item_count": item_count,
        "large_item_count": large_count,
        "reason": "fallback bounds",
    }


def _room_estimate_bounds_tool(
    call: ToolCall,
    *,
    scene_element_classifier: Any = None,
) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    text = str(call.args.get("text") or "")
    fact_id = str(call.args.get("plan_id") or call.args.get("fact_id") or call.tool_call_id)
    items_arg = call.args.get("items")
    items = [str(item) for item in items_arg] if isinstance(items_arg, list) else None
    bounds = _estimate_bounds_from_text(
        text,
        items,
        scene_element_classifier=scene_element_classifier,
    )
    fact = {
        "fact_id": fact_id,
        "fact_type": "room_bounds_estimate",
        "source_tool": "room.estimate_bounds",
        "text_preview": text[:160],
        **bounds,
    }
    return ToolResult(
        True,
        "room bounds estimated",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_scene_facts": {fact_id: fact}},
        ),
        payload=fact,
        user_visible_message=(
            f"空间范围估算完成：{bounds['bounds_type']} "
            f"{bounds['width']} x {bounds['depth']} x {bounds['height']}。"
        ),
    )


def _zone_rows_for_text(text: str, bounds: dict[str, Any]) -> list[dict[str, Any]]:
    clean = str(text or "")
    lowered = clean.lower()
    environment_type = str(bounds.get("environment_type") or "")
    if environment_type == "room_box":
        if _has_any(clean, lowered, ("藏宝室", "宝库", "密室", "treasure", "vault", "chamber")):
            return [
                {"zone_id": "entry", "role": "entrance", "priority": 1},
                {"zone_id": "treasure_focus", "role": "visual_focus", "priority": 1},
                {"zone_id": "side_storage", "role": "support_storage", "priority": 2},
                {"zone_id": "walkable_path", "role": "circulation", "priority": 1},
            ]
        return [
            {"zone_id": "entry", "role": "entrance", "priority": 1},
            {"zone_id": "main_activity", "role": "main_area", "priority": 1},
            {"zone_id": "side_support", "role": "support_area", "priority": 2},
            {"zone_id": "decoration", "role": "decoration_area", "priority": 3},
        ]
    if environment_type == "terrain_substrate":
        if _has_any(clean, lowered, ("集市", "夜市", "market")):
            return [
                {"zone_id": "market_entry", "role": "entrance", "priority": 1},
                {"zone_id": "main_street", "role": "circulation", "priority": 1},
                {"zone_id": "stalls", "role": "object_cluster", "priority": 1},
                {"zone_id": "rest_area", "role": "rest_area", "priority": 2},
            ]
        return [
            {"zone_id": "terrain_base", "role": "substrate", "priority": 1},
            {"zone_id": "camp_core", "role": "main_area", "priority": 1},
            {"zone_id": "sky_context", "role": "environment_backdrop", "priority": 3},
            {"zone_id": "path_hint", "role": "circulation", "priority": 2},
        ]
    return [
        {"zone_id": "main_area", "role": "main_area", "priority": 1},
        {"zone_id": "support_area", "role": "support_area", "priority": 2},
    ]


def _zone_decompose_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    text = str(call.args.get("text") or "")
    fact_id = str(call.args.get("plan_id") or call.args.get("fact_id") or call.tool_call_id)
    bounds_arg = call.args.get("bounds")
    bounds = dict(bounds_arg) if isinstance(bounds_arg, dict) else _estimate_bounds_from_text(text)
    zones = _zone_rows_for_text(text, bounds)
    fact = {
        "fact_id": fact_id,
        "fact_type": "zone_decomposition",
        "source_tool": "zone.decompose",
        "text_preview": text[:160],
        "bounds_type": str(bounds.get("bounds_type") or ""),
        "environment_type": str(bounds.get("environment_type") or ""),
        "zone_count": len(zones),
        "zones": zones,
    }
    return ToolResult(
        True,
        "zones decomposed",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_scene_facts": {fact_id: fact}},
        ),
        payload=fact,
        user_visible_message=f"区域拆分完成：识别出 {len(zones)} 个功能区域。",
    )


def _coerce_item_names(raw_items: Any) -> list[str]:
    names: list[str] = []
    if isinstance(raw_items, dict):
        if isinstance(raw_items.get("candidate_items"), list):
            raw_items = raw_items.get("candidate_items")
        elif isinstance(raw_items.get("items"), list):
            raw_items = raw_items.get("items")
        elif isinstance(raw_items.get("model_items"), list):
            raw_items = raw_items.get("model_items")
        else:
            raw_items = [raw_items]
    for item in raw_items or []:
        if isinstance(item, dict):
            name = str(item.get("name") or item.get("label") or "").strip()
        else:
            name = str(item or "").strip()
        if name and name not in names:
            names.append(name)
    return names


def _layout_items_from_raw_items(raw_items: Any) -> list[str]:
    if isinstance(raw_items, dict) and isinstance(raw_items.get("layout_items"), list):
        return [str(item) for item in raw_items.get("layout_items") or [] if str(item or "")]
    return []


def _asset_route_item_tool(
    call: ToolCall,
    *,
    scene_element_classifier: Any = None,
) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.args.get("batch_id") or call.tool_call_id)
    raw_items = call.args.get("items") or []
    text = str(call.args.get("text") or "")
    item_names = route_candidate_model_names(
        text,
        _coerce_item_names(raw_items),
        scene_element_classifier=scene_element_classifier,
    )
    asset_requests = {
        name: {
            "asset_request_id": f"asset-route-{index + 1:02d}",
            "name": name,
            "status": "planned",
            "preferred_source": _preferred_asset_source(name),
        }
        for index, name in enumerate(item_names)
    }
    return ToolResult(
        True,
        "asset items routed",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"asset_request_plans": {plan_id: asset_requests}},
        ),
        payload={"asset_requests": asset_requests, "item_count": len(asset_requests)},
        user_visible_message=f"资源路由完成：{len(asset_requests)} 个物体进入模型资源准备。",
    )


def _placement_prepare_items_tool(
    call: ToolCall,
    *,
    scene_element_classifier: Any = None,
) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.args.get("batch_id") or call.tool_call_id)
    text = str(call.args.get("text") or "")
    raw_items = call.args.get("items") or []
    layout_items = [str(item) for item in (call.args.get("layout_items") or []) if str(item or "")]
    if not layout_items:
        layout_items = _layout_items_from_raw_items(raw_items)
    if not layout_items:
        layout_items = _derive_layout_items(text)
    item_names = route_candidate_model_names(
        text,
        _coerce_item_names(raw_items),
        scene_element_classifier=scene_element_classifier,
    )
    proposals = build_placement_proposals(item_names, layout_items)
    return ToolResult(
        True,
        "placement items prepared",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"placement_proposals": {plan_id: proposals}},
        ),
        payload={"placements": proposals, "item_count": len(proposals)},
        user_visible_message=f"摆放输入准备完成：{len(proposals)} 个物体已形成低风险摆放草案。",
    )


def _batch_item_priority(name: str, index: int) -> int:
    lowered = str(name or "").lower()
    text = str(name or "")
    if any(token in text for token in ("入口", "门", "地形", "房间", "地面", "主路")):
        return 10 + index
    if any(token in text for token in ("床", "沙发", "桌", "柜", "藏宝箱", "金币堆", "摊位", "帐篷")):
        return 20 + index
    if any(token in text for token in ("雕像", "动物", "小狗", "狗", "大型装饰")):
        return 30 + index
    if any(token in text for token in ("椅", "箱", "桶", "武器架", "长椅", "导视牌")):
        return 40 + index
    if any(token in text for token in ("灯", "火把", "烛", "花", "玩偶", "装饰")) or any(
        token in lowered for token in ("light", "lamp", "decor")
    ):
        return 60 + index
    return 50 + index


def _batch_prioritize_items_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    raw_items = call.args.get("items") or []
    item_names = _coerce_item_names(raw_items)
    seen: set[str] = set()
    priority_rows: list[dict[str, Any]] = []
    for index, name in enumerate(item_names):
        if not name or name in seen:
            continue
        seen.add(name)
        priority_rows.append(
            {
                "name": name,
                "priority": _batch_item_priority(name, index),
                "source_index": index,
            }
        )
    ordered_items = [str(row.get("name") or "") for row in priority_rows if str(row.get("name") or "")]
    fact_id = f"{plan_id}:item_priorities"
    return ToolResult(
        True,
        "batch items prioritized",
        state_patch=StatePatch(
            room_id=room_id,
            changes={
                "custom_batch_facts": {
                    fact_id: {
                        "plan_id": plan_id,
                        "ordered_items": ordered_items,
                        "priorities": priority_rows,
                    }
                }
            },
        ),
        payload={"ordered_items": ordered_items, "item_count": len(ordered_items)},
        user_visible_message=f"批次优先级规划完成：{len(ordered_items)} 个物体已排序。",
    )


def _batch_merge_intervention_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    batch_id = str(call.args.get("batch_id") or "")
    base_items = _coerce_item_names(call.args.get("base_items") or call.args.get("items") or [])
    interventions_raw = call.args.get("interventions") or []
    interventions = [dict(item) for item in interventions_raw if isinstance(item, dict)]

    merged_items = list(base_items)
    seen = set(merged_items)
    absorbed_intervention_ids: list[str] = []
    merged_rows: list[dict[str, Any]] = []
    for index, intervention in enumerate(interventions):
        patch_id = str(intervention.get("patch_id") or "").strip()
        patch_type = str(intervention.get("patch_type") or "").strip()
        items = _coerce_item_names(intervention.get("items") or [])
        added_items: list[str] = []
        for item in items:
            if not item or item in seen:
                continue
            seen.add(item)
            merged_items.append(item)
            added_items.append(item)
        if items and patch_id:
            absorbed_intervention_ids.append(patch_id)
        merged_rows.append(
            {
                "intervention_ref": patch_id,
                "patch_type": patch_type,
                "item_count": len(items),
                "added_item_count": len(added_items),
                "source_index": index,
            }
        )

    fact_id = f"{plan_id}:merged_interventions" if not batch_id else f"{batch_id}:merged_interventions"
    fact = {
        "plan_id": plan_id,
        "batch_id": batch_id,
        "base_items": base_items,
        "merged_items": merged_items,
        "absorbed_intervention_ids": absorbed_intervention_ids,
        "interventions": merged_rows,
    }
    return ToolResult(
        True,
        "batch interventions merged",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_batch_facts": {fact_id: fact}},
        ),
        payload={
            "merged_items": merged_items,
            "absorbed_intervention_ids": absorbed_intervention_ids,
            "item_count": len(merged_items),
        },
        user_visible_message=f"介入合并完成：{len(absorbed_intervention_ids)} 条介入进入下一批。",
    )


def _batch_create_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    item_names = _coerce_item_names(call.args.get("items") or [])
    if not item_names:
        item_names = ["示例物体"]
    try:
        max_items_per_batch = int(call.args.get("max_items_per_batch") or 3)
    except Exception:
        max_items_per_batch = 3
    max_items_per_batch = max(1, max_items_per_batch)
    absorbed_intervention_ids = _dedupe_nonempty_text(call.args.get("absorbed_intervention_ids") or [])

    chunks = [
        item_names[index : index + max_items_per_batch]
        for index in range(0, len(item_names), max_items_per_batch)
    ] or [["示例物体"]]
    now = time.time()
    total = len(chunks)
    batches: list[dict[str, Any]] = []
    for index, chunk in enumerate(chunks, start=1):
        batch_id = f"batch-{uuid.uuid4().hex[:12]}"
        batches.append({
            "batch_id": batch_id,
            "plan_id": plan_id,
            "room_id": room_id,
            "requested_items": list(chunk),
            "batch_index": index,
            "total_batches": total,
            "absorbed_intervention_ids": list(absorbed_intervention_ids) if index == 1 else [],
            "status": "planned",
            "tool_graph_id": "",
            "created_at": now,
            "updated_at": now,
        })

    fact_id = f"{plan_id}:created_batches"
    fact = {
        "plan_id": plan_id,
        "room_id": room_id,
        "max_items_per_batch": max_items_per_batch,
        "item_count": len(item_names),
        "batch_count": len(batches),
        "batches": batches,
    }
    return ToolResult(
        True,
        "batch drafts created",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_batch_facts": {fact_id: fact}},
        ),
        payload={"batch_count": len(batches), "item_count": len(item_names)},
        user_visible_message=f"批次草案创建完成：{len(batches)} 个批次等待进入 Runtime 状态。",
    )


def _select_next_tool_graph_from_queue_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or "")
    queue = {
        str(key): dict(value)
        for key, value in dict(call.args.get("queue") or {}).items()
        if isinstance(value, dict)
    }
    queued_items = [
        dict(item)
        for item in queue.values()
        if str(item.get("status") or "") == "queued"
        and (not plan_id or str(item.get("plan_id") or "") == plan_id)
    ]
    queued_items.sort(
        key=lambda item: (
            _float(item.get("queued_at") or 0.0),
            str(item.get("graph_id") or ""),
        )
    )
    selected = queued_items[0] if queued_items else {}
    graph_id = str(selected.get("graph_id") or "")
    fact_key = f"{room_id}:{plan_id or '*'}:next_tool_graph"
    fact = {
        "room_id": room_id,
        "plan_id": plan_id,
        "selected_graph_ref": graph_id,
        "batch_id": str(selected.get("batch_id") or ""),
        "status": "selected" if graph_id else "empty",
        "queued_count": len(queued_items),
        "source": "runtime_queue_select_next_graph",
    }
    return ToolResult(
        True,
        "tool graph queue selection completed",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_queue_facts": {fact_key: fact}},
        ),
        payload=fact,
        user_visible_message=(
            f"Runtime 队列选择完成：下一项 {graph_id}。"
            if graph_id
            else "Runtime 队列当前没有可执行图。"
        ),
    )


def _plan_tool_graph_queue_items_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or "")
    raw_refs = call.args.get("graph_refs") or []
    queue = {
        str(key): dict(value)
        for key, value in dict(call.args.get("queue") or {}).items()
        if isinstance(value, dict)
    }
    now = time.time()
    drafts: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(raw_refs):
        if not isinstance(raw, dict):
            continue
        graph_id = str(raw.get("graph_id") or "").strip()
        if not graph_id:
            continue
        existing = dict(queue.get(graph_id) or {})
        queued_at = _float(existing.get("queued_at") or raw.get("queued_at") or now)
        draft = {
            "target_graph_ref": graph_id,
            "plan_id": str(raw.get("plan_id") or existing.get("plan_id") or plan_id),
            "batch_id": str(raw.get("batch_id") or existing.get("batch_id") or ""),
            "status": "queued",
            "queued_at": queued_at,
            "started_at": _float(existing.get("started_at") or 0.0),
            "completed_at": _float(existing.get("completed_at") or 0.0),
            "updated_at": _float(raw.get("updated_at") or now),
            "source": "runtime_queue_plan_enqueue_items",
        }
        drafts[graph_id] = draft
    fact_key = f"{room_id}:{plan_id or '*'}:enqueue_item_drafts"
    fact = {
        "room_id": room_id,
        "plan_id": plan_id,
        "draft_count": len(drafts),
        "graph_refs": list(drafts.keys()),
        "queue_item_drafts": drafts,
        "source": "runtime_queue_plan_enqueue_items",
    }
    return ToolResult(
        True,
        "tool graph queue enqueue items planned",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_queue_facts": {fact_key: fact}},
        ),
        payload=fact,
        user_visible_message=f"Runtime 入队草案完成：{len(drafts)} 个执行图等待持久化。",
    )


def _mark_tool_graph_queue_status_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    graph_id = str(call.args.get("target_graph_ref") or "")
    status = str(call.args.get("status") or "")
    if not room_id or not graph_id or not status:
        return ToolResult(
            False,
            "queue status update missing required fields",
            error_code="queue_status_missing_fields",
            user_visible_message="运行队列状态更新缺少必要信息，已停止该步骤。",
        )
    queue = {
        str(key): dict(value)
        for key, value in dict(call.args.get("queue") or {}).items()
        if isinstance(value, dict)
    }
    existing = dict(queue.get(graph_id) or {})
    now = time.time()
    existing.update(
        {
            "graph_id": graph_id,
            "plan_id": str(call.args.get("target_plan_id") or existing.get("plan_id") or ""),
            "batch_id": str(call.args.get("target_batch_id") or existing.get("batch_id") or ""),
            "status": status,
            "updated_at": now,
        }
    )
    if status == "running" and not existing.get("started_at"):
        existing["started_at"] = now
    if status not in {"queued", "running"}:
        existing["completed_at"] = now
    return ToolResult(
        True,
        "tool graph queue status persisted",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"tool_graph_queue": {graph_id: existing}},
        ),
        payload={
            "target_graph_ref": graph_id,
            "status": status,
            "plan_id": existing.get("plan_id") or "",
            "batch_id": existing.get("batch_id") or "",
        },
        user_visible_message=f"Runtime 队列状态已更新：{status}。",
    )


def _scene_extract_environment_tool(
    call: ToolCall,
    *,
    scene_element_classifier: Any = None,
) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    text = str(call.args.get("text") or "")
    batch_id = str(call.args.get("batch_id") or call.args.get("plan_id") or call.tool_call_id)
    raw_items = call.args.get("items")
    candidates = _filter_abstract_items(extract_candidate_items(text)) if raw_items is None else raw_items
    items = [{"name": str(item)} if not isinstance(item, dict) else dict(item) for item in (candidates or [])]
    classifier = scene_element_classifier or _FALLBACK_SCENE_ELEMENT_CLASSIFIER
    _, routed = classifier.route_model_items(text, items)
    route_rows = [route.as_dict() for route in routed]
    environment_items: list[dict[str, Any]] = []
    for route in route_rows:
        name = str(route.get("name") or "").strip()
        pipeline = str(route.get("target_pipeline") or "").strip()
        if not name or pipeline in {"model", "layout_structure"}:
            continue
        environment_items.append({
            "name": name,
            "target_pipeline": pipeline or "scene_substrate",
            "status": "planned",
            "preferred_handler": _preferred_substrate_handler(name, pipeline),
        })
    return ToolResult(
        True,
        "scene environment extracted",
        state_patch=StatePatch(
            room_id=room_id,
            changes={
                "element_routes": {batch_id: route_rows},
                "classification_summaries": {batch_id: classifier.summarize_classification(routed)},
                "substrate_plans": {batch_id: environment_items},
            },
        ),
        payload={
            "environment_items": environment_items,
            "environment_count": len(environment_items),
        },
        user_visible_message=f"环境/地形提炼完成：识别出 {len(environment_items)} 个环境或布局要素。",
    )


def _dedupe_nonempty_text(items: Any) -> list[str]:
    result: list[str] = []
    seen: set[str] = set()
    source = items if isinstance(items, list) else [items]
    for item in source:
        text = str(item or "").strip()
        if not text or text in seen:
            continue
        seen.add(text)
        result.append(text)
    return result


def _plan_next_intervention_batch_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "default")
    plan_id = str(call.args.get("plan_id") or "").strip()
    pending_raw = call.args.get("pending_interventions") or {}
    if isinstance(pending_raw, list):
        pending_values = [entry for entry in pending_raw if isinstance(entry, dict)]
    elif isinstance(pending_raw, dict):
        pending_values = [entry for entry in pending_raw.values() if isinstance(entry, dict)]
    else:
        pending_values = []

    absorbable_patch_types = {"intervention_add", "post_generation_add"}
    absorbable: list[dict[str, Any]] = []
    non_absorbable: list[dict[str, Any]] = []
    requested_items: list[str] = []
    for patch in pending_values:
        if str(patch.get("status") or "") != "pending":
            continue
        if plan_id and str(patch.get("plan_id") or "") != plan_id:
            continue
        patch_type = str(patch.get("patch_type") or "")
        items = _dedupe_nonempty_text(patch.get("items") or [])
        row = {
            "patch_type": patch_type,
            "item_count": len(items),
            "items": items,
            "source_user": str(patch.get("source_user") or ""),
        }
        if patch_type in absorbable_patch_types:
            absorbable.append(row)
            requested_items = _dedupe_nonempty_text([*requested_items, *items])
        else:
            non_absorbable.append(row)

    route_fact = {
        "plan_id": plan_id,
        "absorbable_patch_types": sorted(absorbable_patch_types),
        "absorbable_count": len(absorbable),
        "non_absorbable_count": len(non_absorbable),
        "requested_items": requested_items,
        "absorbable": absorbable,
        "non_absorbable": non_absorbable,
    }
    return ToolResult(
        True,
        "pending interventions routed",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"custom_intervention_route_facts": {plan_id: route_fact}},
            source_tool_call_id=call.tool_call_id,
            operations=[
                {
                    "op": "upsert",
                    "key": "custom_intervention_route_facts",
                    "plan_id": plan_id,
                }
            ],
        ),
        payload=route_fact,
    )


def _classify_scene_elements_tool(
    call: ToolCall,
    *,
    scene_element_classifier: Any = None,
) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    text = str(call.args.get("text") or "")
    raw_items = call.args.get("items") or []
    if isinstance(raw_items, dict):
        raw_items = raw_items.get("candidate_items") or raw_items.get("items") or []
    merged_raw_items = list(raw_items)
    for candidate in _explicit_substrate_candidates_from_text(text):
        if candidate not in merged_raw_items:
            merged_raw_items.append(candidate)
    items = [{"name": str(item)} if not isinstance(item, dict) else dict(item) for item in merged_raw_items]
    classifier = scene_element_classifier or _FALLBACK_SCENE_ELEMENT_CLASSIFIER
    route_model_items = classifier.route_model_items
    summarize_classification = classifier.summarize_classification

    model_items, routed = route_model_items(text, items)
    route_rows = [route.as_dict() for route in routed]
    model_names = [str(item.get("name") or "") for item in model_items if str(item.get("name") or "")]
    substrate_names = [
        str(route.get("name") or "")
        for route in route_rows
        if str(route.get("target_pipeline") or "") == "scene_substrate"
        and str(route.get("name") or "")
    ]
    layout_names = [
        str(route.get("name") or "")
        for route in route_rows
        if str(route.get("target_pipeline") or "") == "layout_only"
        and str(route.get("name") or "")
    ]
    classification_id = str(call.args.get("classification_id") or call.args.get("plan_id") or call.tool_call_id)
    return ToolResult(
        True,
        "scene elements classified",
        state_patch=StatePatch(
            room_id=room_id,
            changes={
                "element_routes": {classification_id: route_rows},
                "model_item_lists": {classification_id: model_names},
                "classification_summaries": {classification_id: summarize_classification(routed)},
            },
        ),
        payload={
            "model_items": model_names,
            "routes": route_rows,
            "item_count": len(model_names),
            "component_count": len(substrate_names) + len(layout_names),
        },
        user_visible_message=(
            f"元素分类完成：准备生成模型 {len(model_names)} 个，"
            f"环境/地形或布局要素 {len(substrate_names) + len(layout_names)} 个。"
        ),
    )


def _plan_asset_requests_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    model_items = [str(item) for item in (call.args.get("model_items") or []) if str(item or "")]
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    batch_id = str(call.args.get("batch_id") or plan_id or call.tool_call_id)
    asset_requests = {
        name: {
            "asset_request_id": f"asset-req-{index + 1:02d}",
            "name": name,
            "status": "planned",
            "preferred_source": _preferred_asset_source(name),
        }
        for index, name in enumerate(model_items)
    }
    return ToolResult(
        True,
        "asset requests planned",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"asset_request_plans": {batch_id: asset_requests}},
        ),
        payload={"plan_id": plan_id, "batch_id": batch_id, "asset_requests": asset_requests},
    )


def _plan_substrate_requests_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    batch_id = str(call.args.get("batch_id") or call.tool_call_id)
    routes = call.args.get("routes") or []
    substrate_items: list[dict[str, Any]] = []
    if isinstance(routes, list):
        for route in routes:
            if not isinstance(route, dict):
                continue
            name = str(route.get("name") or "").strip()
            pipeline = str(route.get("target_pipeline") or "").strip()
            if not name or pipeline in {"model", "layout_structure"}:
                continue
            substrate_items.append({
                "name": name,
                "target_pipeline": pipeline or "substrate",
                "status": "planned",
                "preferred_handler": _preferred_substrate_handler(name, pipeline),
            })
    return ToolResult(
        True,
        "substrate plan created",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"substrate_plans": {batch_id: substrate_items}},
        ),
        payload={"substrate_items": substrate_items},
    )


def _resolve_substrate_requests_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    batch_id = str(call.args.get("batch_id") or call.tool_call_id)
    substrate_plan = call.args.get("substrate_plan") or []
    resolutions: list[dict[str, Any]] = []
    if isinstance(substrate_plan, list):
        for item in substrate_plan:
            if not isinstance(item, dict):
                continue
            name = str(item.get("name") or "").strip()
            handler = str(item.get("preferred_handler") or "").strip()
            if not name:
                continue
            resolutions.append({
                "name": name,
                "component_type": _substrate_component_type(name, handler),
                "handler": handler or _preferred_substrate_handler(name),
                "status": "resolved",
                "requires_engine_write": False,
            })
    return ToolResult(
        True,
        "substrate requests resolved",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"substrate_resolutions": {batch_id: resolutions}},
        ),
        payload={"substrate_resolutions": resolutions},
    )


def _make_environment_components_tool(provider: ResourceProvider | None) -> Callable[[ToolCall], ToolResult]:
    effective_provider = provider or _default_environment_component_provider

    def _tool(call: ToolCall) -> ToolResult:
        room_id = str(call.args.get("room_id") or "")
        batch_id = str(call.args.get("batch_id") or call.tool_call_id)
        substrate_resolutions = [
            dict(item)
            for item in (call.args.get("substrate_resolutions") or [])
            if isinstance(item, dict)
        ]
        payload = {
            "room_id": room_id,
            "batch_id": batch_id,
            "plan_id": str(call.args.get("plan_id") or ""),
            "scene_name": str(call.args.get("scene_name") or ""),
            "design_brief": str(call.args.get("design_brief") or ""),
            "layout_items": [
                str(item)
                for item in (call.args.get("layout_items") or [])
                if str(item or "").strip()
            ],
            "requested_items": [
                str(item)
                for item in (call.args.get("requested_items") or [])
                if str(item or "").strip()
            ],
            "substrate_resolutions": substrate_resolutions,
            "room_bounds": (
                dict(call.args.get("room_bounds") or {})
                if isinstance(call.args.get("room_bounds"), Mapping)
                else {}
            ),
            "environment_type": str(call.args.get("environment_type") or ""),
            "required_environment_components": [
                str(item)
                for item in (call.args.get("required_environment_components") or [])
                if str(item or "").strip()
            ],
        }
        try:
            components = dict(effective_provider(payload) or {})
        except Exception as exc:  # noqa: BLE001
            return _provider_failure_tool_result(
                "environment_component",
                exc,
                user_visible_message="环境组件准备失败，系统会保留模型批次并等待后续处理。",
            )
        substrate_request_count = len(substrate_resolutions)
        if substrate_request_count and not components:
            return ToolResult(
                False,
                "environment component provider returned no components",
                retryable=True,
                error_code="environment_component_provider_empty",
                user_visible_message="环境组件准备没有返回可用结果，系统会保留模型批次并等待后续处理。",
            )
        # A configured semantic provider may only resolve explicit substrate
        # items. Required framework components remain Runtime-owned contract
        # facts and must not disappear merely because that provider is enabled.
        _add_default_framework_components(payload, components)
        requested_count = len(components)
        _normalize_environment_components_for_runtime(
            components,
            batch_id=batch_id,
            plan_id=str(call.args.get("plan_id") or ""),
            scene_name=str(call.args.get("scene_name") or ""),
        )
        try:
            EnvironmentComponentValidator.validate_component_batches({batch_id: components})
        except Exception as exc:  # noqa: BLE001
            return _provider_failure_tool_result(
                "environment_component",
                exc,
                user_visible_message="环境组件结果不符合系统协议，系统会保留模型批次并等待后续处理。",
            )
        assets = _assets_from_environment_components(
            components,
            batch_id=batch_id,
            plan_id=str(call.args.get("plan_id") or ""),
            scene_name=str(call.args.get("scene_name") or ""),
        )
        return ToolResult(
            True,
            "environment components created",
            state_patch=StatePatch(
                room_id=room_id,
                changes={
                    "environment_components": {batch_id: components},
                    "assets": assets,
                },
            ),
            payload={
                "environment_components": components,
                "requested_count": requested_count,
                "ready_count": len(components),
                "failed_count": max(0, requested_count - len(components)),
                "assets": assets,
            },
        )

    return _tool


def _make_environment_import_components_tool(
    provider: ResourceProvider | None,
    *,
    require_engine_environment_import: bool = False,
) -> Callable[[ToolCall], ToolResult]:
    def _failed_component_patch_result(
        *,
        call: ToolCall,
        room_id: str,
        batch_id: str,
        components: Mapping[str, Any],
        message: str,
        error_code: str,
        source: str,
        user_visible_message: str,
        import_results: list[dict[str, Any]] | None = None,
        provider_result: Mapping[str, Any] | None = None,
    ) -> ToolResult:
        failed_components = _failed_environment_import_components(
            batch_id=batch_id,
            components=components,
            source=source,
        )
        safe_import_results = list(import_results or [])
        engine_write_boundary = _environment_import_boundary_fact(
            provider_result or {},
            requested_count=len(components),
            imported_count=0,
            import_results=safe_import_results,
            imported_component_ids=[],
        ) if safe_import_results or provider_result else None
        changes: dict[str, Any] = {
            "environment_components": {batch_id: failed_components},
            "custom_import_facts": {
                f"{batch_id}:environment_import_result": _environment_import_result_fact(
                    {
                        "plan_id": str(call.args.get("plan_id") or ""),
                        "batch_id": batch_id,
                    },
                    requested_count=len(components),
                    imported_count=0,
                    failed_count=max(len(failed_components), len(safe_import_results)),
                    status="failed",
                    import_results=safe_import_results,
                    engine_write_boundary=engine_write_boundary,
                )
            },
        }
        payload = {
            "environment_components": failed_components,
            "environment_import_results": safe_import_results,
            "engine_write_result": dict((provider_result or {}).get("engine_write_result") or {}) if provider_result else {},
            "requested_count": len(components),
            "ready_count": 0,
            "failed_count": len(failed_components),
        }
        return ToolResult(
            False,
            message,
            retryable=True,
            error_code=error_code,
            user_visible_message=user_visible_message,
            state_patch=StatePatch(
                room_id=room_id,
                changes=changes,
                source_tool_call_id=call.tool_call_id,
            ),
            payload=payload,
        )

    def _tool(call: ToolCall) -> ToolResult:
        room_id = str(call.args.get("room_id") or "")
        batch_id = str(call.args.get("batch_id") or call.tool_call_id)
        components = {
            str(component_id): dict(component)
            for component_id, component in dict(call.args.get("environment_components") or {}).items()
            if str(component_id or "").strip() and isinstance(component, dict)
        }
        if require_engine_environment_import and provider is None and components:
            import_results = [
                {
                    "component_id": component_id,
                    "name": str(component.get("name") or component_id),
                    "component_type": str(component.get("component_type") or "environment"),
                    "status": "failed",
                    "failure_code": "engine_environment_import_unavailable",
                    "reason": "engine environment import provider unavailable",
                }
                for component_id, component in components.items()
            ]
            return _failed_component_patch_result(
                call=call,
                room_id=room_id,
                batch_id=batch_id,
                components=components,
                message="engine environment import provider unavailable",
                error_code="engine_environment_import_unavailable",
                source="engine_environment_import_required",
                user_visible_message="场景基础环境写入能力不可用，本批不会继续导入普通物体。",
                import_results=import_results,
            )
        if provider is None:
            imported_components: dict[str, dict[str, Any]] = {}
            for component_id, component_raw in components.items():
                component = dict(component_raw)
                component["component_id"] = str(component.get("component_id") or component_id)
                component["status"] = "runtime_state_only"
                component["source"] = "runtime_default_environment_import"
                component["requires_engine_write"] = False
                component.setdefault("sync_status", "runtime_state_only")
                component.setdefault("sync_lifecycle_status", component.get("sync_status") or "runtime_state_only")
                imported_components[str(component["component_id"])] = component
            EnvironmentComponentValidator.validate_component_batches({batch_id: imported_components})
            import_results = [
                {
                    "component_id": component_id,
                    "name": str(component.get("name") or component_id),
                    "component_type": str(component.get("component_type") or "environment"),
                    "status": "runtime_state_only",
                    "reason": "environment import provider unavailable; RuntimeState-only component retained",
                }
                for component_id, component in imported_components.items()
            ]
            provider_result = {
                "environment_components": imported_components,
                "environment_import_results": import_results,
                "source": "runtime_default_environment_import",
                "engine_write_result": {
                    "provider_source": "runtime_default_environment_import",
                    "requested_count": len(components),
                    "identity_result_count": len(imported_components),
                    "missing_identity_count": 0,
                    "status_counts": {"runtime_state_only": len(imported_components)} if imported_components else {},
                    "bridge_call_count": 0,
                    "bridge_success_count": 0,
                    "bridge_failed_count": 0,
                    "bridge_method_counts": {},
                    "bridge_error_code_counts": {},
                },
            }
            return ToolResult(
                True,
                "environment components retained as RuntimeState-only facts",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "environment_components": {batch_id: imported_components},
                        "custom_import_facts": {
                            f"{batch_id}:environment_import_result": _environment_import_result_fact(
                                {
                                    "plan_id": str(call.args.get("plan_id") or ""),
                                    "batch_id": batch_id,
                                },
                                requested_count=len(components),
                                imported_count=len(imported_components),
                                failed_count=0,
                                status="runtime_state_only",
                                import_results=import_results,
                                engine_write_boundary=_environment_import_boundary_fact(
                                    provider_result,
                                    requested_count=len(components),
                                    imported_count=len(imported_components),
                                    import_results=import_results,
                                    imported_component_ids=list(imported_components),
                                ),
                            ),
                        },
                    },
                    source_tool_call_id=call.tool_call_id,
                ),
                payload={
                    "environment_components": imported_components,
                    "environment_import_results": import_results,
                    "engine_write_result": dict(provider_result["engine_write_result"]),
                    "requested_count": len(components),
                    "ready_count": len(imported_components),
                    "failed_count": 0,
                },
                user_visible_message="Environment components are tracked in RuntimeState and still require F5 engine verification.",
            )
        payload = {
            "room_id": room_id,
            "batch_id": batch_id,
            "plan_id": str(call.args.get("plan_id") or ""),
            "scene_name": str(call.args.get("scene_name") or ""),
            "environment_components": components,
        }
        try:
            result = dict(provider(payload) or {})
        except Exception as exc:  # noqa: BLE001
            _ = exc
            return _failed_component_patch_result(
                call=call,
                room_id=room_id,
                batch_id=batch_id,
                components=components,
                message="environment import failed",
                error_code="environment_import_failed",
                source="runtime_environment_import_failed",
                user_visible_message="环境组件导入失败，系统不会伪装为已写入地形或边界。",
            )
        imported_components = dict(result.get("environment_components") or {})
        imported_components = {
            str(component_id): {
                **dict(component),
                "sync_lifecycle_status": str(
                    dict(component).get("sync_lifecycle_status")
                    or dict(component).get("last_sync_event")
                    or dict(component).get("sync_status")
                    or dict(component).get("status")
                    or "runtime_state"
                ),
            }
            for component_id, component in imported_components.items()
            if isinstance(component, Mapping)
        }
        import_results = []
        for raw_result in list(result.get("environment_import_results") or []):
            if not isinstance(raw_result, Mapping):
                continue
            row = dict(raw_result)
            component_id = str(row.get("component_id") or "").strip()
            component = imported_components.get(component_id, {})
            if "sync_status" not in row and isinstance(component, Mapping) and component.get("sync_status"):
                row["sync_status"] = component.get("sync_status")
            if "sync_lifecycle_status" not in row:
                row["sync_lifecycle_status"] = str(
                    row.get("last_sync_event")
                    or row.get("sync_status")
                    or (component.get("sync_lifecycle_status") if isinstance(component, Mapping) else "")
                    or (component.get("sync_status") if isinstance(component, Mapping) else "")
                    or row.get("status")
                    or "runtime_state"
                )
            import_results.append(row)
        requested_count = len(components)
        if requested_count and imported_components:
            reported_component_ids = {
                str(row.get("component_id") or "").strip()
                for row in import_results
                if str(row.get("component_id") or "").strip()
            }
            for component_id, component in components.items():
                safe_component_id = str(component_id or "").strip()
                if (
                    not safe_component_id
                    or safe_component_id in imported_components
                    or safe_component_id in reported_component_ids
                ):
                    continue
                import_results.append(
                    {
                        "component_id": safe_component_id,
                        "name": str(component.get("name") or safe_component_id),
                        "component_type": str(component.get("component_type") or "environment"),
                        "status": "failed",
                        "failure_code": "environment_import_missing_component",
                        "reason": "environment import provider did not return this requested component",
                        "sync_lifecycle_status": "failed",
                    }
                )
        if requested_count and not imported_components:
            return _failed_component_patch_result(
                call=call,
                room_id=room_id,
                batch_id=batch_id,
                components=components,
                message="environment import provider returned no components",
                error_code="environment_import_provider_empty",
                source="runtime_environment_import_empty",
                user_visible_message="环境组件导入没有返回可用结果，系统不会伪装为已写入地形或边界。",
                import_results=list(result.get("environment_import_results") or []),
                provider_result=result,
            )
        if requested_count and len(imported_components) < requested_count:
            missing_components = {
                component_id: component
                for component_id, component in components.items()
                if component_id not in imported_components
            }
            failed_components = _failed_environment_import_components(
                batch_id=batch_id,
                components=missing_components,
                source="runtime_environment_import_partial",
            )
            combined_components = {**imported_components, **failed_components}
            ready_count = sum(
                1
                for component in imported_components.values()
                if bool(component.get("bounds_ready"))
                and str(component.get("engine_lifecycle_status") or "") == "bounds_ready"
            )
            return ToolResult(
                False,
                "environment import returned only part of the required components",
                retryable=True,
                error_code="environment_import_partial",
                user_visible_message="场景基础环境仅部分写入，普通物体导入已停止，避免生成不完整场景。",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "environment_components": {batch_id: combined_components},
                        "custom_import_facts": {
                            f"{batch_id}:environment_import_result": _environment_import_result_fact(
                                payload,
                                requested_count=requested_count,
                                imported_count=len(imported_components),
                                failed_count=requested_count - len(imported_components),
                                status="partial",
                                ready_count=ready_count,
                                import_results=import_results,
                                engine_write_boundary=_environment_import_boundary_fact(
                                    result,
                                    requested_count=requested_count,
                                    imported_count=len(imported_components),
                                    import_results=import_results,
                                    imported_component_ids=list(imported_components),
                                ),
                            ),
                        },
                    },
                    source_tool_call_id=call.tool_call_id,
                ),
                payload={
                    "environment_components": combined_components,
                    "environment_import_results": import_results,
                    "engine_write_result": dict(result.get("engine_write_result") or {}),
                    "requested_count": requested_count,
                    "ready_count": ready_count,
                    "failed_count": requested_count - len(imported_components),
                },
            )
        try:
            EnvironmentComponentValidator.validate_component_batches({batch_id: imported_components})
        except Exception as exc:  # noqa: BLE001
            _ = exc
            return _failed_component_patch_result(
                call=call,
                room_id=room_id,
                batch_id=batch_id,
                components=components,
                message="environment import result invalid",
                error_code="environment_import_invalid_result",
                source="runtime_environment_import_invalid",
                user_visible_message="环境组件导入结果不符合系统协议，系统不会伪装为已写入地形或边界。",
            )
        ready_count = sum(
            1
            for component in imported_components.values()
            if bool(component.get("bounds_ready"))
            and str(component.get("engine_lifecycle_status") or "") == "bounds_ready"
        )
        environment_status = (
            "imported"
            if requested_count <= 0 or ready_count >= requested_count
            else "engine_loading"
            if len(imported_components) >= requested_count
            else "partial"
        )
        return ToolResult(
            True,
            "environment components imported",
            state_patch=StatePatch(
                room_id=room_id,
                changes={
                    "environment_components": {batch_id: imported_components},
                    "custom_import_facts": {
                        f"{batch_id}:environment_import_result": _environment_import_result_fact(
                            payload,
                            requested_count=requested_count,
                            imported_count=len(imported_components),
                            failed_count=max(0, requested_count - len(imported_components)),
                            status=environment_status,
                            ready_count=ready_count,
                            import_results=import_results,
                            engine_write_boundary=_environment_import_boundary_fact(
                                result,
                                requested_count=requested_count,
                                imported_count=len(imported_components),
                                import_results=list(result.get("environment_import_results") or []),
                                imported_component_ids=list(imported_components),
                            ),
                        ),
                    },
                },
            ),
            payload={
                "environment_components": imported_components,
                "environment_import_results": import_results,
                "engine_write_result": dict(result.get("engine_write_result") or {}),
                "requested_count": requested_count,
                "ready_count": ready_count,
                "failed_count": max(0, requested_count - len(imported_components)),
            },
        )

    return _tool


def _environment_import_result_fact(
    payload: Mapping[str, Any],
    *,
    requested_count: int,
    imported_count: int,
    failed_count: int,
    status: str,
    ready_count: int | None = None,
    import_results: list[dict[str, Any]],
    engine_write_boundary: dict[str, Any] | None = None,
) -> dict[str, Any]:
    fact = {
        "plan_id": str(payload.get("plan_id") or ""),
        "batch_id": str(payload.get("batch_id") or ""),
        "component_count": int(requested_count),
        "ready_count": int(imported_count if ready_count is None else ready_count),
        "imported_count": int(imported_count),
        "failed_count": int(failed_count),
        "status": str(status or "unknown"),
        "source": "runtime_environment_import_result",
        "environment_import_results": _safe_environment_import_results(import_results),
    }
    if engine_write_boundary:
        fact["engine_write_boundary"] = dict(engine_write_boundary)
    return fact


def _safe_environment_import_results(results: Any) -> list[dict[str, Any]]:
    if not isinstance(results, list):
        return []
    unsafe_tokens = (
        "api_key",
        "authorization",
        "bearer ",
        "c:\\",
        "e:\\",
        "http://",
        "https://",
        "metadata",
        "model_path",
        "prompt",
        "provider",
        "raw",
        "token",
        "url",
        "://",
    )
    safe_results: list[dict[str, Any]] = []
    for item in results:
        if not isinstance(item, Mapping):
            continue
        safe: dict[str, Any] = {}
        for field in (
            "actor_id",
            "asset_id",
            "component_id",
            "component_name",
            "component_type",
            "bounds_source",
            "display_name",
            "engine_lifecycle_status",
            "entity_type",
            "model_ref",
            "name",
            "native_name",
            "requested_name",
            "status",
            "sync_lifecycle_status",
            "sync_status",
            "reason",
            "failure_code",
        ):
            value = item.get(field)
            if isinstance(value, str):
                lowered = value.lower()
                if any(token in lowered for token in unsafe_tokens):
                    if field == "reason":
                        value = "environment import failed"
                    else:
                        continue
                safe[field] = value[:160]
            elif isinstance(value, (int, float, bool)):
                safe[field] = value
        aliases = item.get("aliases")
        if isinstance(aliases, list):
            safe_aliases: list[str] = []
            for alias in aliases:
                if not isinstance(alias, str):
                    continue
                lowered = alias.lower()
                if any(token in lowered for token in unsafe_tokens):
                    continue
                text = alias.strip()[:160]
                if text and text not in safe_aliases:
                    safe_aliases.append(text)
            if safe_aliases:
                safe["aliases"] = safe_aliases[:8]
        for field in ("position", "rotation", "scale"):
            vector = item.get(field)
            if isinstance(vector, (list, tuple)) and len(vector) >= 3:
                try:
                    safe[field] = [round(float(part or 0.0), 4) for part in list(vector[:3])]
                except (TypeError, ValueError):
                    pass
        for field in ("aabb", "bounds", "scene_aabb", "world_aabb", "world_bounds"):
            bounds = _safe_import_aabb(item.get(field))
            if bounds is not None:
                safe["aabb" if field in {"world_aabb", "world_bounds"} else field] = bounds
        if safe:
            safe_results.append(safe)
    return safe_results


def _environment_import_boundary_fact(
    provider_result: Mapping[str, Any],
    *,
    requested_count: int,
    imported_count: int,
    import_results: list[dict[str, Any]],
    imported_component_ids: list[str] | None = None,
) -> dict[str, Any]:
    raw_engine_result = provider_result.get("engine_write_result") if isinstance(provider_result, Mapping) else None
    engine_result = raw_engine_result if isinstance(raw_engine_result, Mapping) else {}
    source = _safe_actor_import_provider_source(
        provider_result.get("source")
        or engine_result.get("provider_source")
        or "environment_import_provider"
    )
    status_counts: dict[str, int] = {}
    for item in import_results:
        status_key = str(item.get("status") or "unknown").strip().lower() or "unknown"
        status_counts[status_key] = status_counts.get(status_key, 0) + 1
    if not status_counts and imported_count > 0:
        status_counts["success"] = int(imported_count)
    safe_component_ids = [
        _safe_import_text(component_id, fallback="")
        for component_id in (imported_component_ids or [])
        if _safe_import_text(component_id, fallback="")
    ]
    missing_identity_count = int(engine_result.get("missing_identity_count") or 0)
    if missing_identity_count <= 0:
        missing_identity_count = sum(
            1
            for item in import_results
            if str(item.get("status") or "").strip().lower() == "failed"
            and "component id" in str(item.get("reason") or "").lower()
        )
    return {
        "provider_source": source,
        "requested_count": int(engine_result.get("requested_count") or requested_count),
        "identity_result_count": int(engine_result.get("identity_result_count") or imported_count),
        "missing_identity_count": max(0, missing_identity_count),
        "status_counts": dict(engine_result.get("status_counts") or status_counts),
        "imported_component_ids": safe_component_ids[:32],
        "bridge_call_count": max(0, int(engine_result.get("bridge_call_count") or 0)),
        "bridge_success_count": max(0, int(engine_result.get("bridge_success_count") or 0)),
        "bridge_failed_count": max(0, int(engine_result.get("bridge_failed_count") or 0)),
        "bridge_method_counts": _safe_import_count_map(engine_result.get("bridge_method_counts")),
        "bridge_error_code_counts": _safe_import_count_map(engine_result.get("bridge_error_code_counts")),
    }


def _safe_environment_component_token(value: Any, fallback: str) -> str:
    text = str(value or "").strip()
    blocked_tokens = (
        "api_key",
        "asset_path",
        "authorization",
        "bearer ",
        "metadata",
        "model_path",
        "prompt",
        "provider",
        "raw",
        "token",
        "url",
        "://",
        ":\\",
        "/",
        "\\",
        " ",
    )
    if not text or any(token in text.lower() for token in blocked_tokens):
        return fallback
    return text[:48]


def _safe_environment_component_text(value: Any, fallback: str) -> str:
    text = str(value or "").strip()
    blocked_tokens = (
        "api_key",
        "asset_path",
        "authorization",
        "bearer ",
        "metadata",
        "model_path",
        "prompt",
        "provider",
        "raw",
        "token",
        "url",
        "://",
        ":\\",
    )
    if not text or any(token in text.lower() for token in blocked_tokens):
        return fallback
    return text[:96]


def _failed_environment_import_components(
    *,
    batch_id: str,
    components: Mapping[str, Any],
    source: str,
) -> dict[str, dict[str, Any]]:
    failed_components: dict[str, dict[str, Any]] = {}
    safe_source = _safe_environment_component_token(source, "runtime_environment_import_failed")
    for index, (component_key, component_raw) in enumerate(components.items(), start=1):
        component = dict(component_raw) if isinstance(component_raw, Mapping) else {}
        fallback_component_id = f"{batch_id}-env-failed-{index:02d}" if batch_id else f"runtime-env-failed-{index:02d}"
        component_id = _safe_environment_component_token(
            component.get("component_id") or component_key,
            fallback_component_id,
        )
        component_type = _safe_environment_component_token(
            component.get("component_type") or "environment",
            "environment",
        )
        failed_components[component_id] = {
            "component_id": component_id,
            "name": _safe_environment_component_text(component.get("name"), component_id),
            "component_type": component_type,
            "handler": _safe_environment_component_text(component.get("handler"), ""),
            "scene_name": _safe_environment_component_text(component.get("scene_name"), ""),
            "status": "failed",
            "source": safe_source,
            "requires_engine_write": False,
        }
    EnvironmentComponentValidator.validate_component_batches({batch_id: failed_components})
    return failed_components


def _default_environment_component_provider(payload: dict[str, Any]) -> dict[str, Any]:
    batch_id = str(payload.get("batch_id") or "")
    identity_scope = str(payload.get("plan_id") or batch_id or "runtime")
    components: dict[str, Any] = {}
    for index, item in enumerate(payload.get("substrate_resolutions") or [], start=1):
        if not isinstance(item, dict):
            continue
        name = str(item.get("name") or "").strip()
        component_type = str(item.get("component_type") or "environment").strip() or "environment"
        if not name:
            continue
        component_id = f"{identity_scope}-env-{index:02d}"
        components[component_id] = {
            "component_id": component_id,
            "name": name,
            "component_type": component_type,
            "handler": str(item.get("handler") or ""),
            "status": "planned",
            "source": "runtime_environment_component",
            "requires_engine_write": False,
            **_environment_semantic_fields(name=name, component_type=component_type, handler=str(item.get("handler") or "")),
        }
    _add_default_framework_components(payload, components)
    return components


def _normalize_environment_components_for_runtime(
    components: dict[str, Any],
    *,
    batch_id: str,
    plan_id: str = "",
    scene_name: str = "",
) -> None:
    """Attach RuntimeState identity/geometry defaults without claiming engine success."""

    for key, raw_component in list(components.items()):
        if not isinstance(raw_component, dict):
            continue
        component_id = str(raw_component.get("component_id") or key or "").strip()
        if not component_id:
            component_id = f"{batch_id}-environment-{len(components) + 1:02d}" if batch_id else "runtime-environment"
        component_type = str(raw_component.get("component_type") or "environment").strip() or "environment"
        raw_component["component_id"] = component_id
        raw_component.setdefault("display_name", str(raw_component.get("name") or component_id))
        raw_component.setdefault("native_name", str(raw_component.get("name") or component_id))
        raw_component.setdefault("requested_name", str(raw_component.get("name") or component_id))
        aliases = raw_component.get("aliases")
        if not isinstance(aliases, list) or not aliases:
            raw_component["aliases"] = [
                value
                for value in (
                    str(raw_component.get("requested_name") or ""),
                    str(raw_component.get("native_name") or ""),
                    str(raw_component.get("name") or ""),
                    component_id,
                )
                if value
            ][:8]
        raw_component.setdefault("asset_id", component_id)
        raw_component.setdefault("status", "planned")
        raw_component.setdefault("source", "runtime_environment_component")
        raw_component.setdefault("requires_engine_write", False)
        raw_component.setdefault("sync_status", "runtime_state")
        raw_component.setdefault("sync_lifecycle_status", raw_component.get("sync_status") or "runtime_state")
        if scene_name:
            raw_component.setdefault("scene_name", scene_name)
        raw_component.setdefault("scale", _default_environment_component_scale(component_type))
        component_scale = list(raw_component.get("scale") or _default_environment_component_scale(component_type))
        raw_component.setdefault(
            "position",
            [0.0, float(component_scale[1]) / 2.0, 0.0]
            if component_type in {"room_box", "room_floor"}
            else [0.0, 0.0, 0.0],
        )
        raw_component.setdefault("rotation", [0.0, 0.0, 0.0])
        if not any(
            isinstance(raw_component.get(field), (dict, list, tuple))
            for field in ("aabb", "bounds", "scene_aabb", "world_aabb", "world_bounds")
        ):
            raw_component["aabb"] = _default_environment_component_aabb(component_type)
        # Gameplay facts are downstream contracts, so only preserve values
        # explicitly supplied by a trusted plan or Engine result.
        raw_component.setdefault("interaction_capability", [])
        raw_component.setdefault("gameplay_tags", [])
        raw_component.setdefault("script_bindings", ["runtime_environment_component"])
        raw_component.setdefault("physics_profile", {})
        raw_component.setdefault("review_status", "pending_review")


def _default_environment_component_scale(component_type: str) -> list[float]:
    normalized = str(component_type or "").strip().lower()
    if normalized == "room_box":
        return [6.0, 3.0, 6.0]
    if normalized == "room_floor":
        return [6.0, 0.05, 6.0]
    if normalized in {"terrain", "ground"}:
        return [12.0, 0.05, 12.0]
    if normalized in {"skybox", "sky"}:
        return [20.0, 10.0, 20.0]
    if normalized in {"boundary", "terrain_boundary"}:
        return [12.0, 1.0, 12.0]
    if normalized == "transition_zone":
        return [4.0, 0.05, 4.0]
    return [1.0, 1.0, 1.0]


def _default_environment_component_aabb(component_type: str) -> dict[str, list[float]]:
    scale = _default_environment_component_scale(component_type)
    width = max(0.1, float(scale[0]))
    height = max(0.01, float(scale[1]))
    depth = max(0.1, float(scale[2]))
    return {
        "min": [round(-width / 2.0, 4), 0.0, round(-depth / 2.0, 4)],
        "max": [round(width / 2.0, 4), round(height, 4), round(depth / 2.0, 4)],
    }


def _add_default_framework_components(payload: dict[str, Any], components: dict[str, Any]) -> None:
    text_parts = [
        str(payload.get("scene_name") or ""),
        str(payload.get("design_brief") or ""),
        " ".join(str(item) for item in (payload.get("layout_items") or [])),
        " ".join(str(item) for item in (payload.get("requested_items") or [])),
    ]
    text = " ".join(part for part in text_parts if part).lower()
    required = {
        str(item or "").strip().lower()
        for item in (payload.get("required_environment_components") or [])
        if str(item or "").strip()
    }
    environment_type = str(payload.get("environment_type") or "").strip().lower()
    if not required:
        if not text:
            return
        if _is_outdoor_environment_text(text) and not _is_indoor_environment_text(text):
            required = {"terrain"}
        elif _is_indoor_environment_text(text):
            required = {"room_box", "room_floor"}
    if environment_type == "mixed":
        required.update({"terrain", "room_box", "room_floor", "transition_zone"})
    room_bounds = payload.get("room_bounds") if isinstance(payload.get("room_bounds"), Mapping) else {}
    width = min(8.5, max(5.5, float(room_bounds.get("width") or 6.0)))
    depth = min(8.5, max(5.5, float(room_bounds.get("depth") or 6.0)))
    height = min(3.8, max(2.6, float(room_bounds.get("height") or 3.0)))
    identity_scope = str(payload.get("plan_id") or payload.get("batch_id") or "runtime")
    if "room_box" in required:
        _ensure_environment_component(
            components,
            identity_scope=identity_scope,
            suffix="framework-room-box",
            name="room_box",
            component_type="room_box",
            handler="runtime_room_box",
            scale=[width, height, depth],
            position=[0.0, height / 2.0, 0.0],
        )
    if "room_floor" in required:
        _ensure_environment_component(
            components,
            identity_scope=identity_scope,
            suffix="framework-room-floor",
            name="room_floor",
            component_type="room_floor",
            handler="runtime_room_floor",
            scale=[width, 0.05, depth],
            position=[0.0, 0.025, 0.0],
        )
    if "terrain" in required:
        _ensure_environment_component(
            components,
            identity_scope=identity_scope,
            suffix="framework-terrain",
            name="terrain",
            component_type="terrain",
            handler="runtime_terrain",
            scale=[12.0, 0.05, 12.0],
            position=[0.0, 0.025, 0.0],
        )
    if "transition_zone" in required:
        _ensure_environment_component(
            components,
            identity_scope=identity_scope,
            suffix="framework-transition-zone",
            name="transition_zone",
            component_type="transition_zone",
            handler="runtime_transition_zone",
            scale=[4.0, 0.05, 4.0],
            position=[0.0, 0.03, depth / 2.0],
        )


def _ensure_environment_component(
    components: dict[str, Any],
    *,
    identity_scope: str,
    suffix: str,
    name: str,
    component_type: str,
    handler: str,
    scale: list[float] | None = None,
    position: list[float] | None = None,
) -> None:
    for component in components.values():
        if not isinstance(component, dict):
            continue
        if str(component.get("name") or "") == name or str(component.get("component_type") or "") == component_type:
            return
    component_id = f"{identity_scope}-{suffix}" if identity_scope else f"runtime-{suffix}"
    components[component_id] = {
        "component_id": component_id,
        "name": name,
        "component_type": component_type,
        "handler": handler,
        "status": "planned",
        "source": "runtime_environment_component",
        "requires_engine_write": False,
        "scale": list(scale or _default_environment_component_scale(component_type)),
        "position": list(position or [0.0, 0.0, 0.0]),
        **_environment_semantic_fields(name=name, component_type=component_type, handler=handler),
    }


def _environment_semantic_fields(*, name: str, component_type: str, handler: str) -> dict[str, Any]:
    text = " ".join([str(name or ""), str(component_type or ""), str(handler or "")]).lower()
    fields: dict[str, Any] = {}
    if component_type == "skybox" or "sky" in text or "天空" in text:
        fields["terrain_profile"] = "outdoor_nature"
        fields["sky_mode"] = "open_sky"
    elif component_type in {"room_box", "room_floor"}:
        fields["terrain_profile"] = "indoor_room"
        if component_type == "room_floor":
            fields["surface"] = "indoor_floor"
    elif component_type == "terrain":
        fields["terrain_profile"] = "outdoor_nature" if _is_nature_environment_text(text) else "terrain"
        if any(term in text for term in ("grass", "草地", "草原", "forest", "森林", "camp", "营地")):
            fields["surface"] = "grass_with_walkable_clearings"
        elif any(term in text for term in ("ground", "terrain", "地面", "地形")):
            fields["surface"] = "walkable_ground"
    elif "boundary" in text or "边界" in text or "栅栏" in text:
        fields["boundary_style"] = "soft_natural_boundary" if _is_nature_environment_text(text) else "boundary"
    if fields:
        fields["environment_profile"] = dict(fields)
    return fields


def _is_nature_environment_text(text: str) -> bool:
    nature_terms = (
        "forest",
        "woods",
        "camp",
        "grass",
        "ground",
        "terrain",
        "sky",
        "森林",
        "树林",
        "营地",
        "草地",
        "草原",
        "地面",
        "地形",
        "天空",
    )
    return any(term in text for term in nature_terms)


def _is_indoor_environment_text(text: str) -> bool:
    indoor_terms = (
        "藏宝室",
        "宝库",
        "密室",
        "库房",
        "地下室",
        "洞穴房间",
        "卧室",
        "室内",
        "房间",
        "大厅",
        "交易厅",
        "treasure",
        "vault",
        "chamber",
        "bedroom",
        "room",
        "hall",
    )
    return any(term.lower() in text for term in indoor_terms)


def _is_outdoor_environment_text(text: str) -> bool:
    outdoor_terms = (
        "森林",
        "草原",
        "营地",
        "天空",
        "草地",
        "室外",
        "户外",
        "集市",
        "夜市",
        "市场",
        "forest",
        "grassland",
        "camp",
        "sky",
        "outdoor",
        "market",
    )
    return any(term.lower() in text for term in outdoor_terms)


def _make_image_resource_tool(
    provider: ResourceProvider | None,
    *,
    strict_image_to_model: bool = False,
) -> Callable[[ToolCall], ToolResult]:
    effective_provider = provider or _default_image_resource_provider

    def _tool(call: ToolCall) -> ToolResult:
        room_id = str(call.args.get("room_id") or "")
        batch_id = str(call.args.get("batch_id") or call.tool_call_id)
        payload = _resource_payload_from_call(call)
        plan_id = str(payload.get("plan_id") or "")
        requested_items = _model_items_from_resource_args(
            payload,
            asset_requests=dict(payload.get("asset_requests") or {}),
        )
        requested_count = len(requested_items)
        if strict_image_to_model and provider is None:
            return _resource_provider_failure_tool_result(
                "image",
                RuntimeError("real image resource provider is required"),
                room_id=room_id,
                batch_id=batch_id,
                plan_id=plan_id,
                requested_items=requested_items,
                user_visible_message="真实图片生成能力不可用，本批已阻断，不会继续生成模型。",
            )
        try:
            image_resources = ResourcePlanValidator.safe_image_resource_map(dict(effective_provider(payload) or {}))
        except Exception as exc:  # noqa: BLE001
            return _resource_provider_failure_tool_result(
                "image",
                exc,
                room_id=room_id,
                batch_id=batch_id,
                plan_id=plan_id,
                requested_items=requested_items,
                user_visible_message="图片资源准备失败，系统会稍后重试或降级处理。",
            )
        if requested_count and not image_resources:
            failed_resources = _failed_resource_entries(
                requested_items,
                kind="image",
                batch_id=batch_id,
                status="failed",
                source="image_resource_unavailable",
            )
            return ToolResult(
                True,
                "image resource provider returned no resources; recorded failed resource facts",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "image_resource_plans": {batch_id: failed_resources},
                        "custom_resource_phase_facts": {
                            _resource_phase_fact_key(batch_id, "image"): _resource_phase_fact(
                                batch_id=batch_id,
                                plan_id=plan_id,
                                phase="image",
                                resources=failed_resources,
                                requested_count=requested_count,
                            )
                        },
                    },
                ),
                payload={
                    "image_resources": failed_resources,
                    "requested_count": requested_count,
                    "ready_count": 0,
                    "failed_count": requested_count,
                },
                user_visible_message="图片资源准备没有返回可用结果，系统会稍后重试或降级处理。",
            )
        ready_count = _ready_image_resource_count(image_resources)
        if strict_image_to_model and ready_count < requested_count:
            return ToolResult(
                False,
                "image resources are incomplete for strict image-to-model generation",
                error_code="image_resource_lineage_incomplete",
                retryable=True,
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "image_resource_plans": {batch_id: image_resources},
                        "custom_resource_phase_facts": {
                            _resource_phase_fact_key(batch_id, "image"): _resource_phase_fact(
                                batch_id=batch_id,
                                plan_id=plan_id,
                                phase="image",
                                resources=image_resources,
                                requested_count=requested_count,
                            )
                        },
                    },
                ),
                payload={
                    "image_resources": image_resources,
                    "requested_count": requested_count,
                    "ready_count": ready_count,
                    "failed_count": max(0, requested_count - ready_count),
                },
                user_visible_message="图片资源未形成完整可追溯结果，本批不会进入图生模型。",
            )
        return ToolResult(
            True,
            "batch image resources prepared",
            state_patch=StatePatch(
                room_id=room_id,
                changes={
                    "image_resource_plans": {batch_id: image_resources},
                    "custom_resource_phase_facts": {
                        _resource_phase_fact_key(batch_id, "image"): _resource_phase_fact(
                            batch_id=batch_id,
                            plan_id=plan_id,
                            phase="image",
                            resources=image_resources,
                            requested_count=requested_count,
                        )
                    },
                },
            ),
            payload={
                "image_resources": image_resources,
                "requested_count": requested_count,
                "ready_count": ready_count,
                "failed_count": max(0, requested_count - ready_count),
            },
            user_visible_message=_resource_ready_user_message(
                "图片资源",
                requested_count=requested_count,
                ready_count=ready_count,
            ),
        )

    return _tool


def _promote_model_resource_import_paths(resources: Mapping[str, Any]) -> dict[str, Any]:
    promoted: dict[str, Any] = {}
    for key, raw_entry in dict(resources or {}).items():
        if not isinstance(raw_entry, Mapping):
            promoted[key] = raw_entry
            continue
        entry = dict(raw_entry)
        metadata = entry.get("metadata") if isinstance(entry.get("metadata"), Mapping) else {}
        import_path = (
            entry.get("local_path")
            or entry.get("model_path")
            or entry.get("path")
            or entry.get("model_folder")
            or metadata.get("local_path")
            or metadata.get("model_path")
            or metadata.get("path")
            or metadata.get("model_folder")
        )
        if import_path and not str(entry.get("local_path") or entry.get("model_path") or "").strip():
            entry["local_path"] = str(import_path)
        promoted[key] = entry
    return promoted


def _promote_adapter_model_resource_handles(resources: Mapping[str, Any]) -> dict[str, Any]:
    promoted: dict[str, Any] = {}
    unavailable_statuses = {"failed", "failure", "error", "missing", "pending", "queued", "running"}
    adapter_markers = ("adapter", "provider")
    for index, (key, raw_entry) in enumerate(dict(resources or {}).items(), start=1):
        if not isinstance(raw_entry, Mapping):
            promoted[key] = raw_entry
            continue
        entry = dict(raw_entry)
        status = str(entry.get("status") or "prepared").strip().lower()
        if status not in unavailable_statuses:
            adapter_signal = " ".join(
                str(entry.get(field) or "").strip().lower()
                for field in ("source", "mode", "status")
            )
            metadata = entry.get("metadata") if isinstance(entry.get("metadata"), Mapping) else {}
            import_path = (
                entry.get("local_path")
                or entry.get("model_path")
                or entry.get("path")
                or entry.get("model_folder")
                or metadata.get("local_path")
                or metadata.get("model_path")
                or metadata.get("path")
                or metadata.get("model_folder")
            )
            if not str(import_path or "").strip() and any(marker in adapter_signal for marker in adapter_markers):
                safe_key = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(key or f"resource_{index}")).strip("._-")
                safe_key = safe_key or f"resource_{index}"
                entry["local_path"] = f"runtime_adapter_import_handle_{safe_key}.glb"
        promoted[key] = entry
    return promoted


def _make_model_resource_tool(
    provider: ResourceProvider | None,
    *,
    strict_image_to_model: bool = False,
) -> Callable[[ToolCall], ToolResult]:
    effective_provider = provider or _default_model_resource_provider

    def _tool(call: ToolCall) -> ToolResult:
        room_id = str(call.args.get("room_id") or "")
        batch_id = str(call.args.get("batch_id") or call.tool_call_id)
        payload = _resource_payload_from_call(call)
        plan_id = str(payload.get("plan_id") or "")
        requested_items = _model_items_from_resource_args(
            payload,
            asset_requests=dict(payload.get("asset_requests") or {}),
        )
        requested_count = len(requested_items)
        image_resources = dict(payload.get("image_resources") or {})
        if strict_image_to_model:
            missing_image_items = [
                name
                for name in requested_items
                if not _image_resource_has_lineage(image_resources.get(name))
            ]
            if provider is None or missing_image_items:
                reason = (
                    "real model resource provider is required"
                    if provider is None
                    else "missing ready image lineage: " + ",".join(missing_image_items)
                )
                return _resource_provider_failure_tool_result(
                    "model",
                    RuntimeError(reason),
                    room_id=room_id,
                    batch_id=batch_id,
                    plan_id=plan_id,
                    requested_items=requested_items,
                    user_visible_message="缺少真实图片血缘，本批模型生成已阻断，不会降级为文生 3D。",
                )
        try:
            raw_model_resources = dict(effective_provider(payload) or {})
            if provider is not None:
                raw_model_resources = _promote_adapter_model_resource_handles(raw_model_resources)
            model_resources = ResourcePlanValidator.safe_model_resource_map(
                _promote_model_resource_import_paths(raw_model_resources)
            )
        except Exception as exc:  # noqa: BLE001
            return _resource_provider_failure_tool_result(
                "model",
                exc,
                room_id=room_id,
                batch_id=batch_id,
                plan_id=plan_id,
                requested_items=requested_items,
                user_visible_message="模型资源准备失败，系统会稍后重试或降级处理。",
            )
        if strict_image_to_model:
            invalid_lineage = []
            for name in requested_items:
                model_resource = model_resources.get(name)
                image_resource = image_resources.get(name)
                if (
                    not isinstance(model_resource, Mapping)
                    or str(model_resource.get("generation_mode") or "") != "image_to_3d"
                    or str(model_resource.get("source_image_ref") or "")
                    != str((image_resource or {}).get("resource_ref") or "")
                    or str(model_resource.get("source_image_hash") or "")
                    != str((image_resource or {}).get("content_hash") or "")
                ):
                    invalid_lineage.append(name)
            if invalid_lineage:
                return ToolResult(
                    False,
                    "model resources do not preserve strict source-image lineage",
                    error_code="model_source_image_lineage_mismatch",
                    retryable=True,
                    state_patch=StatePatch(
                        room_id=room_id,
                        changes={
                            "model_resource_plans": {batch_id: model_resources},
                            "custom_resource_phase_facts": {
                                _resource_phase_fact_key(batch_id, "model"): _resource_phase_fact(
                                    batch_id=batch_id,
                                    plan_id=plan_id,
                                    phase="model",
                                    resources=model_resources,
                                    requested_count=requested_count,
                                )
                            },
                        },
                    ),
                    payload={
                        "model_resources": model_resources,
                        "requested_count": requested_count,
                        "ready_count": 0,
                        "failed_count": requested_count,
                        "lineage_mismatch_items": invalid_lineage,
                    },
                    user_visible_message="模型结果未保留对应图片血缘，本批已阻断，不会导入 Actor。",
                )
        if requested_count and not model_resources:
            failed_resources = _failed_resource_entries(
                requested_items,
                kind="model",
                batch_id=batch_id,
                status="failed",
                source="model_resource_unavailable",
            )
            return ToolResult(
                True,
                "model resource provider returned no resources; recorded failed resource facts",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "model_resource_plans": {batch_id: failed_resources},
                        "custom_resource_phase_facts": {
                            _resource_phase_fact_key(batch_id, "model"): _resource_phase_fact(
                                batch_id=batch_id,
                                plan_id=plan_id,
                                phase="model",
                                resources=failed_resources,
                                requested_count=requested_count,
                            )
                        },
                    },
                ),
                payload={
                    "model_resources": failed_resources,
                    "requested_count": requested_count,
                    "ready_count": 0,
                    "failed_count": requested_count,
                },
                user_visible_message="模型资源准备没有返回可用结果，系统会稍后重试或降级处理。",
            )
        ready_count = _importable_model_resource_count(model_resources)
        legacy_hard_failed = (
            requested_count > 0
            and ready_count <= 0
            and all(
                isinstance(resource, dict)
                and str(resource.get("status") or "").strip().lower() in {"failed", "failure", "error", "missing"}
                and str(resource.get("source") or "").strip().lower()
                in {"legacy_model_failure", "legacy_model_adapter_unavailable"}
                for resource in model_resources.values()
            )
        )
        if legacy_hard_failed:
            return ToolResult(
                False,
                "model resource provider returned only failed resources; recorded failed resource facts",
                error_code="model_resource_unavailable",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "model_resource_plans": {batch_id: model_resources},
                        "custom_resource_phase_facts": {
                            _resource_phase_fact_key(batch_id, "model"): _resource_phase_fact(
                                batch_id=batch_id,
                                plan_id=plan_id,
                                phase="model",
                                resources=model_resources,
                                requested_count=requested_count,
                            )
                        },
                    },
                ),
                payload={
                    "model_resources": model_resources,
                    "requested_count": requested_count,
                    "ready_count": 0,
                    "failed_count": requested_count,
                },
                user_visible_message="模型资源准备失败，本批不会继续导入虚假的场景物体。",
            )
        assets = _assets_from_model_resources(
            model_resources,
            batch_id=batch_id,
            plan_id=plan_id,
            scene_name=str(payload.get("scene_name") or ""),
        )
        return ToolResult(
            True,
            "batch model resources prepared",
            state_patch=StatePatch(
                room_id=room_id,
                changes={
                    "model_resource_plans": {batch_id: model_resources},
                    "assets": assets,
                    "custom_resource_phase_facts": {
                        _resource_phase_fact_key(batch_id, "model"): _resource_phase_fact(
                            batch_id=batch_id,
                            plan_id=plan_id,
                            phase="model",
                            resources=model_resources,
                            requested_count=requested_count,
                        )
                    },
                },
            ),
            payload={
                "model_resources": model_resources,
                "requested_count": requested_count,
                "ready_count": ready_count,
                "failed_count": max(0, requested_count - ready_count),
                "assets": assets,
            },
            user_visible_message=_resource_ready_user_message(
                "模型资源",
                requested_count=requested_count,
                ready_count=ready_count,
            ),
        )

    return _tool


def _failed_resource_entries(
    items: Sequence[str],
    *,
    kind: str,
    batch_id: str,
    status: str,
    source: str,
) -> dict[str, dict[str, Any]]:
    safe_kind = "image" if str(kind or "") == "image" else "model"
    safe_batch_id = _safe_review_text(batch_id, fallback="batch", allow_empty=False)
    safe_status = _safe_review_text(status, fallback="failed", allow_empty=False)
    safe_source = _safe_review_text(source, fallback=f"{safe_kind}_resource_unavailable", allow_empty=False)
    entries: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(items):
        name = _safe_review_text(item, fallback=f"item-{index + 1:02d}", allow_empty=False)
        if safe_kind == "image":
            entries[name] = {
                "image_request_id": f"image-failed-{safe_batch_id}-{index + 1:02d}",
                "name": name,
                "status": safe_status,
                "mode": "unavailable",
                "source": safe_source,
                "failure_code": safe_source,
            }
        else:
            entries[name] = {
                "model_request_id": f"model-failed-{safe_batch_id}-{index + 1:02d}",
                "name": name,
                "status": safe_status,
                "source": safe_source,
                "failure_code": safe_source,
            }
    if safe_kind == "image":
        return ResourcePlanValidator.safe_image_resource_map(entries)
    return ResourcePlanValidator.safe_model_resource_map(entries)


def _resource_phase_fact_key(batch_id: str, phase: str) -> str:
    safe_batch_id = _safe_review_text(batch_id, fallback="batch", allow_empty=False)
    safe_phase = "image" if str(phase or "") == "image" else "model"
    return f"{safe_batch_id}:{safe_phase}"


def _resource_phase_fact(
    *,
    batch_id: str,
    plan_id: str = "",
    phase: str,
    resources: Mapping[str, Any],
    requested_count: int,
) -> dict[str, Any]:
    safe_phase = "image" if str(phase or "") == "image" else "model"
    safe_batch_id = _safe_review_text(batch_id, fallback="batch", allow_empty=False)
    safe_plan_id = _safe_review_text(plan_id, fallback="", allow_empty=True)
    resource_rows = {
        str(key): dict(value)
        for key, value in dict(resources or {}).items()
        if isinstance(value, Mapping) and str(key or "").strip()
    }
    ready_count = (
        _ready_resource_count(resource_rows)
        if safe_phase == "image"
        else _importable_model_resource_count(resource_rows)
    )
    requested = max(0, int(requested_count or 0))
    failed_count = max(0, requested - ready_count)
    if requested > 0 and ready_count <= 0 and failed_count >= requested:
        status = "failed"
    elif failed_count > 0 or (requested > 0 and ready_count < requested):
        status = "partial"
    else:
        status = "completed"
    status_counts: dict[str, int] = {}
    failure_code_counts: dict[str, int] = {}
    for row in resource_rows.values():
        row_status = _safe_review_text(row.get("status") or "unknown", fallback="unknown", allow_empty=False)
        status_counts[row_status] = status_counts.get(row_status, 0) + 1
        failure_code = _safe_review_text(row.get("failure_code") or "", fallback="", allow_empty=True)
        if failure_code:
            failure_code_counts[failure_code] = failure_code_counts.get(failure_code, 0) + 1
    return {
        "batch_id": safe_batch_id,
        "plan_id": safe_plan_id,
        "phase": safe_phase,
        "status": status,
        "requested_count": requested,
        "ready_count": ready_count,
        "failed_count": failed_count,
        "resource_count": len(resource_rows),
        "status_counts": dict(sorted(status_counts.items())),
        "failure_code_counts": dict(sorted(failure_code_counts.items())),
        "source": "runtime_resource_phase_fact",
    }


def _resource_ready_user_message(label: str, *, requested_count: int, ready_count: int) -> str:
    safe_label = _safe_review_text(label, fallback="资源", allow_empty=False)
    requested = max(0, int(requested_count or 0))
    ready = max(0, int(ready_count or 0))
    if requested <= 0:
        return f"{safe_label}准备完成。"
    if ready >= requested:
        return f"{safe_label}准备完成：{ready}/{requested}。"
    failed = max(0, requested - ready)
    return f"{safe_label}部分完成：{ready}/{requested}，{failed} 个待重试或降级。"


def _ready_resource_count(resources: dict[str, Any]) -> int:
    return sum(
        1
        for resource in resources.values()
        if isinstance(resource, dict)
        and str(resource.get("status") or "").strip().lower() not in {"failed", "failure", "error", "missing"}
    )


def _image_resource_has_lineage(resource: Mapping[str, Any] | None) -> bool:
    if not isinstance(resource, Mapping):
        return False
    status = str(resource.get("status") or "").strip().lower()
    mode = str(resource.get("mode") or "").strip().lower()
    location = str(
        resource.get("image_url")
        or resource.get("local_path")
        or ""
    ).strip()
    return bool(
        status == "ready"
        and mode not in {"mock", "mock_reference", "fixture"}
        and location
        and str(resource.get("resource_ref") or "").strip()
        and str(resource.get("content_hash") or "").startswith("sha256:")
        and str(resource.get("prompt_hash") or "").startswith("sha256:")
    )


def _ready_image_resource_count(resources: Mapping[str, Any]) -> int:
    return sum(1 for resource in resources.values() if _image_resource_has_lineage(resource))


def _model_resource_has_importable_path(resource: Mapping[str, Any] | None) -> bool:
    if not isinstance(resource, Mapping):
        return False
    status = str(resource.get("status") or "").strip().lower()
    if status in {"failed", "failure", "error", "missing", "pending", "queued", "running"}:
        return False
    metadata = resource.get("metadata") if isinstance(resource.get("metadata"), Mapping) else {}
    path_text = str(
        resource.get("local_path")
        or resource.get("model_path")
        or resource.get("path")
        or resource.get("model_folder")
        or metadata.get("local_path")
        or metadata.get("model_path")
        or metadata.get("path")
        or metadata.get("model_folder")
        or ""
    ).strip()
    if not path_text:
        return False
    try:
        raw_path = Path(path_text)
        if raw_path.is_absolute():
            candidates = [raw_path]
        else:
            from runtime import project_context

            candidates = [project_context.get_project_root() / raw_path, raw_path]
        supported = {".obj", ".dae", ".glb", ".gltf", ".fbx", ".stl", ".usdz"}
        visible = [candidate for candidate in candidates if candidate.exists()]
        if not visible:
            return True
        for candidate in visible:
            if candidate.is_file() and candidate.suffix.lower() in supported:
                return True
            if candidate.is_dir():
                for child in candidate.rglob("*"):
                    try:
                        if child.is_file() and child.suffix.lower() in supported:
                            return True
                    except OSError:
                        continue
        return False
    except OSError:
        return True


def _model_resource_matches_image_lineage(
    resource: Mapping[str, Any] | None,
    image_resource: Mapping[str, Any] | None,
) -> bool:
    if not isinstance(resource, Mapping):
        return False
    generation_mode = str(resource.get("generation_mode") or "").strip().lower()
    if generation_mode != "image_to_3d":
        return not (
            str(resource.get("source_image_ref") or "").strip()
            or str(resource.get("source_image_hash") or "").strip()
        )
    if not isinstance(image_resource, Mapping):
        return False
    return bool(
        _image_resource_has_lineage(image_resource)
        and str(resource.get("source_image_ref") or "").strip()
        == str(image_resource.get("resource_ref") or "").strip()
        and str(resource.get("source_image_hash") or "").strip()
        == str(image_resource.get("content_hash") or "").strip()
    )


def _model_resource_import_failure_code(
    resource: Mapping[str, Any] | None,
    image_resource: Mapping[str, Any] | None = None,
) -> str:
    if not isinstance(resource, Mapping) or not resource:
        return "missing_ready_model_resource"
    status = str(resource.get("status") or "").strip().lower()
    if status in {"failed", "failure", "error", "missing"}:
        return _safe_import_text(
            resource.get("failure_code")
            or resource.get("source")
            or f"model_resource_{status or 'failed'}",
            fallback="missing_ready_model_resource",
        )
    if image_resource is not None and not _model_resource_matches_image_lineage(
        resource,
        image_resource,
    ):
        return "source_image_lineage_mismatch"
    return "missing_ready_model_resource"


def _importable_model_resource_count(resources: Mapping[str, Any]) -> int:
    return sum(
        1
        for resource in dict(resources or {}).values()
        if _model_resource_has_importable_path(resource)
    )


def _assets_from_model_resources(
    resources: Mapping[str, Any],
    *,
    batch_id: str,
    plan_id: str,
    scene_name: str = "",
) -> dict[str, dict[str, Any]]:
    assets: dict[str, dict[str, Any]] = {}
    for key, raw_resource in dict(resources or {}).items():
        if not isinstance(raw_resource, Mapping):
            continue
        name = str(raw_resource.get("name") or key or "").strip()
        if not name:
            continue
        status = str(raw_resource.get("status") or "").strip().lower()
        failed = status in {"failed", "failure", "error", "missing"}
        local_path = str(raw_resource.get("local_path") or raw_resource.get("model_path") or "")
        assets[name] = {
            "asset_id": name,
            "name": name,
            "asset_type": "model",
            "batch_id": str(batch_id or ""),
            "plan_id": str(plan_id or ""),
            "scene_name": str(scene_name or ""),
            "status": "failed" if failed else "ready",
            "ready": not failed,
            "failed": failed,
            "transfer_status": "failed" if failed else "runtime_state_only",
            "progress": 0 if failed else 100,
            "source": str(raw_resource.get("source") or "runtime_model_resource"),
            "model_ref": str(raw_resource.get("model_ref") or raw_resource.get("model_request_id") or ""),
            "generation_mode": str(raw_resource.get("generation_mode") or ""),
            "source_image_ref": str(raw_resource.get("source_image_ref") or ""),
            "source_image_hash": str(raw_resource.get("source_image_hash") or ""),
        }
        if local_path:
            assets[name]["model_path"] = local_path
            assets[name]["local_path"] = local_path
    return AssetFactValidator.safe_asset_map(assets)


def _assets_from_environment_components(
    components: Mapping[str, Any],
    *,
    batch_id: str,
    plan_id: str,
    scene_name: str = "",
) -> dict[str, dict[str, Any]]:
    assets: dict[str, dict[str, Any]] = {}
    for key, raw_component in dict(components or {}).items():
        if not isinstance(raw_component, Mapping):
            continue
        component_id = str(raw_component.get("component_id") or key or "").strip()
        if not component_id:
            continue
        component_type = str(raw_component.get("component_type") or "environment").strip() or "environment"
        name = str(raw_component.get("name") or component_id).strip() or component_id
        assets[component_id] = {
            "asset_id": component_id,
            "name": name,
            "asset_type": component_type,
            "batch_id": str(batch_id or ""),
            "plan_id": str(plan_id or ""),
            "scene_name": str(scene_name or raw_component.get("scene_name") or ""),
            "status": "ready",
            "ready": True,
            "failed": False,
            "transfer_status": "runtime_state_only",
            "progress": 100,
            "source": str(raw_component.get("source") or "runtime_environment_component"),
        }
    return AssetFactValidator.safe_asset_map(assets)


def _propose_placement_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    model_items = [str(item) for item in (call.args.get("model_items") or []) if str(item or "")]
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    batch_id = str(call.args.get("batch_id") or plan_id or call.tool_call_id)
    layout_items = [str(item) for item in (call.args.get("layout_items") or []) if str(item or "")]
    proposals = build_placement_proposals(model_items, layout_items)
    environment_components = call.args.get("environment_components")
    observed_actors = call.args.get("observed_actors")
    if isinstance(environment_components, dict) and environment_components:
        proposals = _annotate_placements_with_environment(proposals, environment_components)
    if isinstance(observed_actors, dict) and observed_actors:
        proposals = _avoid_observed_actor_overlap(proposals, observed_actors)
    return ToolResult(
        True,
        "placement proposal created",
        state_patch=StatePatch(
            room_id=room_id,
            changes={"placement_proposals": {batch_id: proposals}},
        ),
        payload={"plan_id": plan_id, "batch_id": batch_id, "placements": proposals},
    )


def _annotate_placements_with_environment(
    proposals: dict[str, dict[str, Any]],
    environment_components: dict[str, Any],
) -> dict[str, dict[str, Any]]:
    component_types = [
        str(component.get("component_type") or component.get("type") or "").strip()
        for component in environment_components.values()
        if isinstance(component, dict)
    ]
    component_types = [component_type for component_type in component_types if component_type]
    if not proposals or not component_types:
        return proposals
    adjusted: dict[str, dict[str, Any]] = {}
    for name, proposal in proposals.items():
        row = dict(proposal)
        row.setdefault("environment_hints", list(component_types[:4]))
        adjusted[name] = row
    return adjusted


def _avoid_observed_actor_overlap(
    proposals: dict[str, dict[str, Any]],
    observed_actors: dict[str, dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    occupied: list[tuple[float, float]] = []
    for actor in observed_actors.values():
        if not isinstance(actor, dict):
            continue
        position = actor.get("position")
        if isinstance(position, list) and len(position) >= 3:
            occupied.append((_float(position[0]), _float(position[2])))
    if not occupied:
        return proposals
    adjusted: dict[str, dict[str, Any]] = {}
    for index, (name, proposal) in enumerate(proposals.items(), start=1):
        row = dict(proposal)
        position = list(row.get("position") or [0.0, 0.0, 0.0])
        while len(position) < 3:
            position.append(0.0)
        x = _float(position[0])
        z = _float(position[2])
        nudged = False
        for occupied_x, occupied_z in occupied:
            if abs(x - occupied_x) < 0.75 and abs(z - occupied_z) < 0.75:
                x = min(4.0, max(-4.0, x + 0.85 + 0.15 * index))
                z = min(4.0, max(-4.0, z + 0.45))
                nudged = True
        if nudged:
            position[0] = round(x, 3)
            position[2] = round(z, 3)
            row["position"] = position
            row["observed_actor_avoidance"] = True
        adjusted[name] = row
        occupied.append((_float(row.get("position", [0.0, 0.0, 0.0])[0]), _float(row.get("position", [0.0, 0.0, 0.0])[2])))
    return adjusted


def _plan_actor_import_batch_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    batch_id = str(call.args.get("batch_id") or call.tool_call_id)
    plan_id = str(call.args.get("plan_id") or "")
    model_items = [str(item) for item in (call.args.get("model_items") or []) if str(item or "")]
    model_resources = {
        str(key): dict(value)
        for key, value in dict(call.args.get("model_resources") or {}).items()
        if isinstance(value, dict)
    }
    image_resources = {
        str(key): dict(value)
        for key, value in dict(call.args.get("image_resources") or {}).items()
        if isinstance(value, dict)
    }
    placements = {
        str(key): dict(value)
        for key, value in dict(call.args.get("placements") or {}).items()
        if isinstance(value, dict)
    }
    environment_components = {
        str(key): dict(value)
        for key, value in dict(call.args.get("environment_components") or {}).items()
        if isinstance(value, dict)
    }
    planned_actors: list[dict[str, Any]] = []
    failure_code_counts: dict[str, int] = {}
    for index, name in enumerate(model_items):
        resource = dict(model_resources.get(name) or {})
        placement = dict(placements.get(name) or {})
        status = str(resource.get("status") or "").strip().lower()
        image_resource = dict(image_resources.get(name) or {})
        has_importable_model = (
            _model_resource_has_importable_path(resource)
            and _model_resource_matches_image_lineage(resource, image_resource)
        )
        model_ready = (
            bool(resource)
            and status not in {"failed", "error", "missing"}
            and has_importable_model
        )
        failure_code = ""
        if not model_ready:
            failure_code = _model_resource_import_failure_code(resource, image_resource)
            failure_code_counts[failure_code] = failure_code_counts.get(failure_code, 0) + 1
        planned_actors.append({
            "actor_name": name,
            "source_index": index,
            "model_ready": model_ready,
            "resource_status": status or ("missing" if not resource else "unknown"),
            "resource_source": _safe_import_text(resource.get("source"), fallback="runtime_resource"),
            "failure_code": failure_code,
            "position": _safe_position(placement.get("position")),
            "rotation": _safe_position(placement.get("rotation")),
            "scale": _safe_position(placement.get("scale"), default=[1.0, 1.0, 1.0]),
            "zone_hint": _safe_import_text(placement.get("zone_hint"), fallback="default_zone"),
        })
    actor_count = len(planned_actors)
    ready_count = sum(1 for actor in planned_actors if bool(actor.get("model_ready")))
    if actor_count > 0 and ready_count <= 0:
        plan_status = "failed"
    elif actor_count > 0 and ready_count < actor_count:
        plan_status = "partial"
    else:
        plan_status = "planned"
    fact = {
        "plan_id": plan_id,
        "batch_id": batch_id,
        "actor_count": actor_count,
        "ready_count": ready_count,
        "failed_count": max(0, actor_count - ready_count),
        "failure_code_counts": dict(sorted(failure_code_counts.items())),
        "environment_component_count": len(environment_components),
        "planned_actors": planned_actors,
        "status": plan_status,
        "source": "runtime_actor_import_plan",
    }
    return ToolResult(
        True,
        "actor import plan created",
        state_patch=StatePatch(
            room_id=room_id,
            changes={
                "custom_import_facts": {
                    batch_id: fact,
                    f"{batch_id}:actor_import_plan": fact,
                }
            },
        ),
        payload={
            "batch_id": batch_id,
            "actor_count": fact["actor_count"],
            "ready_count": fact["ready_count"],
            "failed_count": fact["failed_count"],
            "failure_code_counts": dict(fact["failure_code_counts"]),
            "environment_component_count": fact["environment_component_count"],
        },
        user_visible_message=f"导入计划完成：{fact['ready_count']}/{fact['actor_count']} 个物体资源可进入导入。",
    )


def _make_actor_import_tool(
    provider: ResourceProvider | None,
    *,
    require_engine_actor_import: bool = False,
) -> Callable[[ToolCall], ToolResult]:
    effective_provider = provider or _default_actor_import_provider

    def _tool(call: ToolCall) -> ToolResult:
        room_id = str(call.args.get("room_id") or "")
        payload = _resource_payload_from_call(call)
        requested_items = [str(item) for item in (payload.get("model_items") or []) if str(item or "")]
        explicit_model_resources = "model_resources" in call.args
        model_resources = {
            str(key): dict(value)
            for key, value in dict(payload.get("model_resources") or {}).items()
            if isinstance(value, dict)
        }
        image_resources = {
            str(key): dict(value)
            for key, value in dict(payload.get("image_resources") or {}).items()
            if isinstance(value, dict)
        }
        if requested_items and model_resources:
            ready_items = [
                name
                for name in requested_items
                if _model_resource_has_importable_path(model_resources.get(name))
                and _model_resource_matches_image_lineage(
                    model_resources.get(name),
                    image_resources.get(name),
                )
            ]
            payload["model_items"] = ready_items
        elif requested_items and explicit_model_resources:
            payload["model_items"] = []
        if requested_items and explicit_model_resources and not payload.get("model_items"):
            import_results = [
                {
                    "actor_name": name,
                    "status": "failed",
                    "failure_code": _model_resource_import_failure_code(
                        model_resources.get(name),
                        image_resources.get(name),
                    ),
                    "reason": "missing ready model resource",
                }
                for name in requested_items
            ]
            import_result_fact = _actor_import_result_fact(
                payload,
                requested_count=len(requested_items),
                imported_count=0,
                failed_count=len(requested_items),
                status="failed",
                import_results=import_results,
                engine_write_boundary=_actor_import_boundary_fact(
                    {},
                    requested_count=len(requested_items),
                    imported_count=0,
                    import_results=import_results,
                    provider_source="runtime_actor_import_precheck",
                ),
            )
            return ToolResult(
                True,
                "actor import skipped and failed resource fact recorded",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "custom_import_facts": {
                            f"{payload['batch_id']}:actor_import_result": import_result_fact,
                        },
                    },
                ),
                payload={
                    "actor_ids": [],
                    "batch_id": payload["batch_id"],
                    "requested_count": len(requested_items),
                    "imported_count": 0,
                    "failed_count": len(requested_items),
                    "import_results": import_results,
                },
                user_visible_message="模型资源尚未准备完成，本批导入不会创建虚假的场景物体。",
            )
        if require_engine_actor_import and provider is None:
            import_results = [
                {
                    "actor_name": name,
                    "status": "failed",
                    "failure_code": "engine_import_unavailable",
                    "reason": "engine actor import provider unavailable",
                }
                for name in requested_items
            ]
            requested_count = len(requested_items)
            import_result_fact = _actor_import_result_fact(
                payload,
                requested_count=requested_count,
                imported_count=0,
                failed_count=requested_count,
                status="failed",
                import_results=import_results,
                engine_write_boundary=_actor_import_boundary_fact(
                    {},
                    requested_count=requested_count,
                    imported_count=0,
                    import_results=import_results,
                    provider_source="engine_actor_import_provider",
                ),
            )
            return ToolResult(
                False,
                "engine actor import provider unavailable; recorded failed import fact",
                retryable=True,
                error_code="engine_import_unavailable",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "custom_import_facts": {
                            f"{payload['batch_id']}:actor_import_result": import_result_fact,
                        },
                    },
                ),
                payload={
                    "actor_ids": [],
                    "batch_id": payload["batch_id"],
                    "requested_count": requested_count,
                    "imported_count": 0,
                    "failed_count": requested_count,
                    "import_results": import_results,
                },
                user_visible_message="真实引擎导入通道不可用，本批不会创建虚假的场景物体。",
            )
        try:
            provider_result = dict(effective_provider(payload) or {})
        except Exception as exc:  # noqa: BLE001
            import_results = [
                {
                    "actor_name": name,
                    "status": "failed",
                    "failure_code": "actor_import_provider_failed",
                    "reason": "actor import provider failed",
                }
                for name in requested_items
            ]
            import_result_fact = _actor_import_result_fact(
                payload,
                requested_count=len(requested_items),
                imported_count=0,
                failed_count=len(requested_items),
                status="failed",
                import_results=import_results,
                engine_write_boundary=_actor_import_boundary_fact(
                    {},
                    requested_count=len(requested_items),
                    imported_count=0,
                    import_results=import_results,
                    provider_source="actor_import_provider",
                ),
            )
            return ToolResult(
                False,
                "actor_import provider failed; recorded failed import fact",
                retryable=True,
                error_code="actor_import_provider_failed",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "custom_import_facts": {
                            f"{payload['batch_id']}:actor_import_result": import_result_fact,
                        },
                    },
                ),
                payload={
                    "actor_ids": [],
                    "batch_id": payload["batch_id"],
                    "requested_count": len(requested_items),
                    "imported_count": 0,
                    "failed_count": len(requested_items),
                    "import_results": import_results,
                },
                user_visible_message="场景导入失败，系统会稍后重试或等待进一步处理。",
            )
        if isinstance(provider_result.get("actors"), dict):
            raw_actors = dict(provider_result.get("actors") or {})
            import_results = _safe_actor_import_results(provider_result.get("import_results") or [])
        else:
            raw_actors = provider_result
            import_results = []
        try:
            actors = ActorFactValidator.safe_actor_map(raw_actors)
        except Exception as exc:  # noqa: BLE001
            _ = exc
            import_results = [
                {
                    "actor_name": name,
                    "status": "failed",
                    "failure_code": "actor_import_adapter_failed",
                    "reason": "actor import result schema mismatch",
                }
                for name in requested_items
            ]
            requested_count = len(requested_items) if requested_items else max(1, len(import_results))
            import_result_fact = _actor_import_result_fact(
                payload,
                requested_count=requested_count,
                imported_count=0,
                failed_count=requested_count,
                status="failed",
                import_results=import_results,
                engine_write_boundary=_actor_import_boundary_fact(
                    provider_result,
                    requested_count=requested_count,
                    imported_count=0,
                    import_results=import_results,
                ),
            )
            return ToolResult(
                False,
                "actor_import provider returned invalid actor facts; recorded failed import fact",
                retryable=True,
                error_code="actor_import_adapter_failed",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "custom_import_facts": {
                            f"{payload['batch_id']}:actor_import_result": import_result_fact,
                        },
                    },
                ),
                payload={
                    "actor_ids": [],
                    "batch_id": payload["batch_id"],
                    "requested_count": requested_count,
                    "imported_count": 0,
                    "failed_count": requested_count,
                    "import_results": import_results,
                },
                user_visible_message="场景导入结果不符合系统协议，系统会稍后重试或等待进一步处理。",
            )
        for actor in actors.values():
            actor.setdefault("entity_type", "actor")
            actor.setdefault(
                "semantic_role",
                str(actor.get("requested_name") or actor.get("name") or ""),
            )
            actor.setdefault("interaction_capability", [])
            actor.setdefault("gameplay_tags", [])
            actor.setdefault("physics_profile", {})
            actor.setdefault("audio_profile", {"surface": "generic"})
            actor.setdefault(
                "lighting_profile",
                {"receives_light": True, "casts_shadow": True},
            )
            actor.setdefault("script_bindings", ["runtime_scene_entity"])
            actor.setdefault("review_status", "pending_review")
            has_actual_bounds = bool(_safe_import_aabb(actor.get("aabb") or actor.get("bounds")))
            bounds_source = str(actor.get("bounds_source") or "").strip().lower()
            if "bounds_ready" not in actor and has_actual_bounds and bounds_source != "estimated":
                actor["bounds_ready"] = True
                actor["bounds_source"] = "engine_actual"
                actor["engine_lifecycle_status"] = "bounds_ready"
                actor["status"] = "ready"
        imported_count = len(actors)
        ready_count = sum(
            1
            for actor in actors.values()
            if bool(actor.get("bounds_ready"))
            and str(actor.get("engine_lifecycle_status") or "") == "bounds_ready"
        )
        requested_count = len(requested_items) if requested_items else max(imported_count, len(import_results))
        import_results = _enrich_actor_import_results(import_results, actors)
        if not actors and import_results:
            import_result_fact = _actor_import_result_fact(
                payload,
                requested_count=requested_count,
                imported_count=0,
                failed_count=requested_count,
                status="failed",
                import_results=import_results,
                engine_write_boundary=_actor_import_boundary_fact(
                    provider_result,
                    requested_count=requested_count,
                    imported_count=0,
                    import_results=import_results,
                ),
            )
            return ToolResult(
                False,
                "actor import failed and result fact recorded",
                retryable=True,
                error_code="actor_import_failed",
                state_patch=StatePatch(
                    room_id=room_id,
                    changes={
                        "custom_import_facts": {
                            f"{payload['batch_id']}:actor_import_result": import_result_fact,
                        },
                    },
                ),
                payload={
                    "actor_ids": [],
                    "batch_id": payload["batch_id"],
                    "requested_count": requested_count,
                    "imported_count": 0,
                    "failed_count": requested_count,
                    "import_results": import_results,
                },
                user_visible_message="场景导入失败，本批不会创建虚假的场景物体。",
            )
        failed_count = max(0, requested_count - imported_count)
        if requested_count > 0 and imported_count <= 0:
            import_status = "failed"
        elif requested_count > 0 and imported_count < requested_count:
            import_status = "partial"
        elif ready_count < imported_count:
            import_status = "engine_loading"
        else:
            import_status = "imported"
        import_result_fact = _actor_import_result_fact(
            payload,
            requested_count=requested_count,
            imported_count=imported_count,
            ready_count=ready_count,
            failed_count=failed_count,
            status=import_status,
            import_results=import_results,
            engine_write_boundary=_actor_import_boundary_fact(
                provider_result,
                requested_count=requested_count,
                imported_count=imported_count,
                import_results=import_results,
                imported_actor_ids=list(actors),
            ),
        )
        return ToolResult(
            True,
            "batch actors imported",
            state_patch=StatePatch(
                room_id=room_id,
                changes={
                    "actors": actors,
                    "custom_import_facts": {
                        f"{payload['batch_id']}:actor_import_result": import_result_fact,
                    },
                },
            ),
            payload={
                "actor_ids": list(actors),
                "batch_id": payload["batch_id"],
                "requested_count": requested_count,
                "imported_count": imported_count,
                "ready_count": ready_count,
                "failed_count": failed_count,
                "import_results": import_results,
            },
            user_visible_message=_actor_import_user_message(
                requested_count=requested_count,
                imported_count=imported_count,
                failed_count=failed_count,
            ),
        )

    return _tool


def _actor_import_result_fact(
    payload: Mapping[str, Any],
    *,
    requested_count: int,
    imported_count: int,
    ready_count: int | None = None,
    failed_count: int,
    status: str,
    import_results: list[dict[str, Any]],
    engine_write_boundary: dict[str, Any] | None = None,
) -> dict[str, Any]:
    failure_code_counts: dict[str, int] = {}
    for row in import_results or []:
        if not isinstance(row, Mapping):
            continue
        failure_code = _safe_import_text(row.get("failure_code"), fallback="")
        if failure_code:
            failure_code_counts[failure_code] = failure_code_counts.get(failure_code, 0) + 1
    fact = {
        "plan_id": str(payload.get("plan_id") or ""),
        "batch_id": str(payload.get("batch_id") or ""),
        "actor_count": int(requested_count),
        "ready_count": int(imported_count if ready_count is None else ready_count),
        "imported_count": int(imported_count),
        "failed_count": int(failed_count),
        "status": str(status or "unknown"),
        "source": "runtime_actor_import_result",
        "failure_code_counts": dict(sorted(failure_code_counts.items())),
        "import_results": list(import_results or []),
    }
    if engine_write_boundary:
        fact["engine_write_boundary"] = dict(engine_write_boundary)
    return fact


def _actor_import_user_message(*, requested_count: int, imported_count: int, failed_count: int) -> str:
    if requested_count > 0 and imported_count < requested_count:
        return f"场景导入部分完成：已导入 {imported_count}/{requested_count} 个物体，失败 {failed_count} 个。"
    if requested_count > 0:
        return f"场景导入完成：已导入 {imported_count}/{requested_count} 个物体。"
    return f"场景导入完成：已导入 {imported_count} 个物体。"


def _safe_actor_import_results(results: Any) -> list[dict[str, Any]]:
    if not isinstance(results, list):
        return []
    unsafe_tokens = (
        "api_key",
        "authorization",
        "bearer ",
        "c:\\",
        "e:\\",
        "http://",
        "https://",
        "metadata",
        "model_path",
        "prompt",
        "provider",
        "raw",
        "token",
        "url",
        "://",
    )
    safe_results: list[dict[str, Any]] = []
    for item in results:
        if not isinstance(item, dict):
            continue
        safe: dict[str, Any] = {}
        for field in (
            "actor_id",
            "actor_request_id",
            "actor_version",
            "actor_name",
            "asset_id",
            "display_name",
            "entity_id",
            "entity_version",
            "grounding_status",
            "model_ref",
            "native_name",
            "requested_name",
            "semantic_role",
            "review_status",
            "status",
            "sync_lifecycle_status",
            "sync_status",
            "version",
            "reason",
            "failure_code",
        ):
            value = item.get(field)
            if isinstance(value, str):
                lowered = value.lower()
                if any(token in lowered for token in unsafe_tokens):
                    if field == "reason":
                        value = "actor import failed"
                    else:
                        continue
                safe[field] = value[:160]
            elif isinstance(value, (int, float, bool)):
                safe[field] = value
        for field in ("position", "rotation", "scale", "size"):
            vector = _safe_import_vector3(item.get(field))
            if vector is not None:
                safe[field] = vector
        for field in ("aabb", "bounds", "scene_aabb", "world_aabb", "world_bounds"):
            bounds = _safe_import_aabb(item.get(field))
            if bounds is not None:
                safe["aabb" if field in {"world_aabb", "world_bounds"} else field] = bounds
        bounds_ready = item.get("bounds_ready")
        if isinstance(bounds_ready, bool):
            safe["bounds_ready"] = bounds_ready
        aliases = item.get("aliases")
        if isinstance(aliases, list):
            safe_aliases: list[str] = []
            for alias in aliases:
                if not isinstance(alias, str):
                    continue
                lowered = alias.lower()
                if any(token in lowered for token in unsafe_tokens):
                    continue
                text = alias.strip()[:160]
                if text and text not in safe_aliases:
                    safe_aliases.append(text)
            if safe_aliases:
                safe["aliases"] = safe_aliases[:8]
        status_value = str(safe.get("status") or "").strip().lower()
        if (
            status_value in {"failed", "failure", "error", "missing"}
            and not str(safe.get("failure_code") or "").strip()
        ):
            safe["failure_code"] = "actor_import_failed"
        if safe:
            safe_results.append(safe)
    return safe_results


def _safe_import_vector3(value: Any) -> list[float] | None:
    if not isinstance(value, (list, tuple)) or len(value) < 3:
        return None
    try:
        return [round(float(part or 0.0), 4) for part in list(value[:3])]
    except (TypeError, ValueError):
        return None


def _safe_import_aabb(value: Any) -> dict[str, list[float]] | None:
    if isinstance(value, Mapping):
        min_value = _safe_import_vector3(value.get("min"))
        max_value = _safe_import_vector3(value.get("max"))
        if min_value is not None and max_value is not None:
            return {"min": min_value, "max": max_value}
        return None
    if isinstance(value, (list, tuple)) and len(value) >= 6:
        try:
            numbers = [round(float(part or 0.0), 4) for part in list(value[:6])]
        except (TypeError, ValueError):
            return None
        return {"min": numbers[:3], "max": numbers[3:6]}
    return None


def _enrich_actor_import_results(
    import_results: list[dict[str, Any]],
    actors: Mapping[str, Mapping[str, Any]],
) -> list[dict[str, Any]]:
    safe_results = _safe_actor_import_results(import_results)
    if not safe_results:
        return []
    actor_rows = {
        str(actor_id): dict(actor)
        for actor_id, actor in dict(actors or {}).items()
        if isinstance(actor, Mapping) and str(actor_id or "").strip()
    }
    if not actor_rows:
        return safe_results

    def names_for(actor_id: str, actor: Mapping[str, Any]) -> set[str]:
        names = {
            str(actor_id or "").strip(),
            str(actor.get("name") or "").strip(),
            str(actor.get("requested_name") or "").strip(),
            str(actor.get("native_name") or "").strip(),
            str(actor.get("display_name") or "").strip(),
        }
        aliases = actor.get("aliases") if isinstance(actor.get("aliases"), list) else []
        names.update(str(alias or "").strip() for alias in aliases)
        return {name for name in names if name}

    used_actor_ids: set[str] = set()
    enriched: list[dict[str, Any]] = []
    for row in safe_results:
        if not isinstance(row, dict):
            continue
        actor_id = str(row.get("actor_id") or "").strip()
        actor = actor_rows.get(actor_id) if actor_id else None
        if actor is None:
            actor_name = str(row.get("actor_name") or "").strip()
            for candidate_id, candidate in actor_rows.items():
                if candidate_id in used_actor_ids:
                    continue
                if actor_name and actor_name in names_for(candidate_id, candidate):
                    actor_id = candidate_id
                    actor = candidate
                    break
        if actor is None or str(row.get("status") or "").strip().lower() != "success":
            enriched.append(row)
            continue
        used_actor_ids.add(actor_id)
        aliases = [
            str(alias or "").strip()
            for alias in (actor.get("aliases") if isinstance(actor.get("aliases"), list) else [])
            if str(alias or "").strip()
        ]
        enriched.append(
            _safe_actor_import_results([
                {
                    **row,
                    "actor_id": actor_id,
                    "actor_name": str(actor.get("name") or row.get("actor_name") or actor_id),
                    "display_name": str(actor.get("display_name") or actor.get("name") or actor_id),
                    "native_name": str(actor.get("native_name") or actor.get("name") or actor_id),
                    "requested_name": str(actor.get("requested_name") or ""),
                    "aliases": aliases,
                    "asset_id": actor.get("asset_id"),
                    "model_ref": actor.get("model_ref"),
                    "position": actor.get("position"),
                    "rotation": actor.get("rotation"),
                    "scale": actor.get("scale"),
                    "size": actor.get("size"),
                    "aabb": actor.get("aabb"),
                    "bounds_ready": actor.get("bounds_ready"),
                    "grounding_status": actor.get("grounding_status"),
                    "review_status": actor.get("review_status"),
                    "sync_status": actor.get("sync_status"),
                    "sync_lifecycle_status": actor.get("sync_lifecycle_status"),
                }
            ])[0]
        )

    return enriched


def _actor_import_boundary_fact(
    provider_result: Mapping[str, Any],
    *,
    requested_count: int,
    imported_count: int,
    import_results: list[dict[str, Any]],
    provider_source: str = "",
    imported_actor_ids: list[str] | None = None,
) -> dict[str, Any]:
    raw_engine_result = provider_result.get("engine_write_result") if isinstance(provider_result, Mapping) else None
    engine_result = raw_engine_result if isinstance(raw_engine_result, Mapping) else {}
    source = _safe_actor_import_provider_source(
        provider_source
        or provider_result.get("source")
        or engine_result.get("provider_source")
        or "actor_import_provider",
    )
    status_counts: dict[str, int] = {}
    bridge_skip_reason_counts: dict[str, int] = {}
    for item in import_results:
        status_key = str(item.get("status") or "unknown").strip().lower() or "unknown"
        status_counts[status_key] = status_counts.get(status_key, 0) + 1
        failure_code = _safe_import_text(item.get("failure_code"), fallback="")
        if status_key == "failed" and failure_code:
            bridge_skip_reason_counts[failure_code] = bridge_skip_reason_counts.get(failure_code, 0) + 1
    if not status_counts and imported_count > 0:
        status_counts["success"] = int(imported_count)
    engine_bridge_skip_reasons = _safe_import_count_map(engine_result.get("bridge_skip_reason_counts"))
    if engine_bridge_skip_reasons:
        bridge_skip_reason_counts = engine_bridge_skip_reasons
    bridge_skipped_count = int(engine_result.get("bridge_skipped_count") or 0)
    if bridge_skipped_count <= 0:
        bridge_skipped_count = sum(bridge_skip_reason_counts.values())
    safe_actor_ids = [
        _safe_import_text(actor_id, fallback="")
        for actor_id in (imported_actor_ids or [])
        if _safe_import_text(actor_id, fallback="")
    ]
    missing_identity_count = int(engine_result.get("missing_identity_count") or 0)
    if missing_identity_count <= 0:
        missing_identity_count = sum(
            1
            for item in import_results
            if str(item.get("status") or "").strip().lower() == "failed"
            and "actor id" in str(item.get("reason") or "").lower()
        )
    return {
        "provider_source": source,
        "requested_count": int(engine_result.get("requested_count") or requested_count),
        "identity_result_count": int(engine_result.get("identity_result_count") or imported_count),
        "missing_identity_count": max(0, missing_identity_count),
        "status_counts": dict(engine_result.get("status_counts") or status_counts),
        "imported_actor_ids": safe_actor_ids[:32],
        "bridge_call_count": max(0, int(engine_result.get("bridge_call_count") or 0)),
        "bridge_success_count": max(0, int(engine_result.get("bridge_success_count") or 0)),
        "bridge_failed_count": max(0, int(engine_result.get("bridge_failed_count") or 0)),
        "bridge_skipped_count": max(0, bridge_skipped_count),
        "bridge_skip_reason_counts": dict(sorted(bridge_skip_reason_counts.items())),
        "bridge_method_counts": _safe_import_count_map(engine_result.get("bridge_method_counts")),
        "bridge_error_code_counts": _safe_import_count_map(engine_result.get("bridge_error_code_counts")),
    }


def _safe_import_count_map(raw: Any) -> dict[str, int]:
    if not isinstance(raw, Mapping):
        return {}
    safe: dict[str, int] = {}
    for key, value in raw.items():
        label = _safe_import_text(key, fallback="")
        if not label:
            continue
        try:
            count = int(value or 0)
        except (TypeError, ValueError):
            continue
        if count > 0:
            safe[label] = count
    return dict(sorted(safe.items()))


def _safe_actor_import_provider_source(value: Any) -> str:
    text = str(value or "").strip().lower()
    allowed = {
        "actor_import_provider",
        "engine_actor_import_provider",
        "environment_import_provider",
        "engine_environment_import_provider",
        "runtime_actor_import_precheck",
        "runtime_default_import_provider",
        "runtime_default_environment_import",
        "runtime_default_environment_import_provider",
    }
    if text in allowed:
        return text
    return "actor_import_provider"


def _make_geometry_review_tool(provider: ResourceProvider | None) -> Callable[[ToolCall], ToolResult]:
    effective_provider = provider or _default_geometry_review_provider

    def _tool(call: ToolCall) -> ToolResult:
        room_id = str(call.args.get("room_id") or "")
        plan_id = str(call.args.get("plan_id") or call.tool_call_id)
        batch_id = str(call.args.get("batch_id") or plan_id or call.tool_call_id)
        environment_components = dict(call.args.get("environment_components") or {})
        payload = {
            "room_id": room_id,
            "batch_id": batch_id,
            "plan_id": plan_id,
            "contract_version": int(call.args.get("contract_version") or 0),
            "checkpoint_type": str(call.args.get("checkpoint_type") or "geometry_review"),
            "reviewed_targets": [
                str(item or "").strip()
                for item in (call.args.get("reviewed_targets") or [])
                if str(item or "").strip()
            ],
            "placements": dict(call.args.get("placements") or {}),
            "environment_components": environment_components,
            "environment_hints": _environment_component_types(environment_components),
            "room_bounds": list(call.args.get("room_bounds") or [-4.0, 4.0, -4.0, 4.0]),
            "scene_name": str(call.args.get("scene_name") or ""),
            "tool_call_id": call.tool_call_id,
        }
        try:
            review = _normalize_geometry_review_result(
                dict(effective_provider(payload) or {}),
                plan_id=plan_id,
            )
        except Exception as exc:  # noqa: BLE001
            return _provider_failure_tool_result(
                "review",
                exc,
                user_visible_message="场景审查失败，系统会保留当前批次并等待后续复查。",
            )
        return ToolResult(
            True,
            "geometry review completed",
            state_patch=StatePatch(room_id=room_id, changes={"geometry_reviews": {batch_id: review}}),
            payload=review,
        )

    return _tool


def _compute_actor_aabb_facts_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    actors = _safe_actor_map_arg(call.args.get("actors"))
    entries: dict[str, dict[str, Any]] = {}
    skipped: list[dict[str, str]] = []
    for actor_id, actor in actors.items():
        aabb = _safe_aabb(actor.get("aabb"))
        actor_name = _safe_import_text(actor.get("name") or actor_id, fallback="actor")
        if aabb is None:
            skipped.append({"actor_id": actor_id, "actor_name": actor_name, "reason": "aabb_unavailable"})
            continue
        entries[actor_id] = {
            "actor_id": actor_id,
            "actor_name": actor_name,
            "aabb": aabb,
            "center": _aabb_center(aabb),
            "size": _aabb_size(aabb),
            "bottom_y": round(float(aabb["min"][1]), 3),
        }
    fact_key = f"{plan_id}:aabb"
    fact = {
        "fact_type": "runtime_geometry_aabb",
        "plan_id": plan_id,
        "actor_count": len(entries),
        "skipped_count": len(skipped),
        "actors": entries,
        "skipped": skipped[:12],
    }
    return ToolResult(
        True,
        "actor aabb facts computed",
        state_patch=StatePatch(room_id=room_id, changes={"custom_geometry_facts": {fact_key: fact}}),
        payload=fact,
    )


def _check_actor_overlap_facts_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    actors = _safe_actor_map_arg(call.args.get("actors"))
    threshold = max(0.01, min(1.0, _float(call.args.get("overlap_ratio_threshold") or 0.20)))
    actor_aabbs: dict[str, dict[str, Any]] = {}
    actor_names: dict[str, str] = {}
    for actor_id, actor in actors.items():
        aabb = _safe_aabb(actor.get("aabb"))
        if aabb is None:
            continue
        actor_aabbs[actor_id] = aabb
        actor_names[actor_id] = _safe_import_text(actor.get("name") or actor_id, fallback="actor")
    issues: list[dict[str, Any]] = []
    actor_ids = sorted(actor_aabbs)
    for index, left_id in enumerate(actor_ids):
        for right_id in actor_ids[index + 1:]:
            left = actor_aabbs[left_id]
            right = actor_aabbs[right_id]
            overlap_volume = _aabb_overlap_volume(left, right)
            if overlap_volume <= 0:
                continue
            smaller = min(_aabb_volume(left), _aabb_volume(right))
            if smaller <= 0:
                continue
            ratio = overlap_volume / smaller
            if ratio < threshold:
                continue
            issues.append({
                "type": "aabb_overlap",
                "actor_id": left_id,
                "actor_name": actor_names.get(left_id, left_id),
                "related_id": right_id,
                "related_name": actor_names.get(right_id, right_id),
                "overlap_ratio": round(float(ratio), 3),
                "severity": "error" if ratio >= 0.35 else "warn",
            })
    fact_key = f"{plan_id}:overlap"
    fact = {
        "fact_type": "runtime_geometry_overlap",
        "plan_id": plan_id,
        "actor_count": len(actor_aabbs),
        "issue_count": len(issues),
        "overlap_ratio_threshold": round(float(threshold), 3),
        "issues": issues[:24],
        "status": "needs_adjustment" if issues else "ok",
    }
    return ToolResult(
        True,
        "actor overlap facts checked",
        state_patch=StatePatch(room_id=room_id, changes={"custom_geometry_facts": {fact_key: fact}}),
        payload=fact,
    )


def _support_type_for_ground_snap(name: str) -> str:
    return classify_support_type(name)


def _snap_actor_grounding_facts_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    batch_id = str(call.args.get("batch_id") or "")
    actors = _safe_actor_map_arg(call.args.get("actors"))
    ground_y = _float(call.args.get("ground_y") if "ground_y" in call.args else 0.0)
    epsilon = max(0.001, min(0.5, _float(call.args.get("epsilon") or 0.03)))
    issues: list[dict[str, Any]] = []
    reviewed_targets: list[str] = []
    skipped: list[dict[str, str]] = []
    for actor_id, actor in actors.items():
        actor_name = _safe_import_text(actor.get("name") or actor_id, fallback="actor")
        support_type = _support_type_for_ground_snap(actor_name)
        aabb = _safe_aabb(actor.get("aabb"))
        if support_type != "floor_supported":
            skipped.append({"actor_id": actor_id, "actor_name": actor_name, "reason": support_type})
            continue
        if aabb is None:
            skipped.append({"actor_id": actor_id, "actor_name": actor_name, "reason": "aabb_unavailable"})
            continue
        reviewed_targets.append(actor_name)
        bottom_y = round(float(aabb["min"][1]), 3)
        delta_y = round(float(ground_y) - bottom_y, 3)
        if abs(delta_y) <= epsilon:
            continue
        center = _aabb_center(aabb)
        actor_position = _vector3(actor.get("position")) or center
        suggested_position = [
            round(float(actor_position[0]), 3),
            round(float(actor_position[1]) + delta_y, 3),
            round(float(actor_position[2]), 3),
        ]
        issues.append(
            {
                "type": "floating_or_sunken",
                "actor_id": actor_id,
                "actor_name": actor_name,
                "name": actor_name,
                "severity": "warn",
                "current_y": bottom_y,
                "suggested_y": round(float(ground_y), 3),
                "current_position": actor_position,
                "suggested_position": suggested_position,
                "confidence": 0.9,
                "reason": "floor_supported_actor_bottom_not_on_ground",
            }
        )
    fact_key = f"{plan_id}:{batch_id}:ground_snap" if batch_id else f"{plan_id}:ground_snap"
    fact = {
        "fact_type": "runtime_geometry_ground_snap",
        "plan_id": plan_id,
        "batch_id": batch_id,
        "ground_y": round(float(ground_y), 3),
        "epsilon": round(float(epsilon), 3),
        "actor_count": len(reviewed_targets),
        "skipped_count": len(skipped),
        "issue_count": len(issues),
        "issues": issues[:24],
        "skipped": skipped[:12],
        "status": "needs_adjustment" if issues else "ok",
    }
    review = {
        "plan_id": plan_id,
        "batch_id": batch_id,
        "checkpoint_type": "ground_snap_selective",
        "status": fact["status"],
        "overall": "NEEDS_ADJUSTMENT" if issues else "PASS",
        "source": "runtime.geometry.snap_to_ground_selective",
        "contract_version": 0,
        "issue_count": len(issues),
        "score": 0.7 if issues else 1.0,
        "reviewed_targets": reviewed_targets[:24],
        "environment_hints": [],
        "issues": issues[:24],
        "advisory_items": [
            {
                "type": "ground_snap",
                "summary": "floor-supported objects need low-risk ground snap",
                "requires_confirmation": True,
                "confidence": 0.9,
            }
        ] if issues else [],
    }
    patch_changes: dict[str, Any] = {"custom_geometry_facts": {fact_key: fact}}
    review_key = f"{batch_id}:ground_snap" if batch_id else f"{plan_id}:ground_snap"
    patch_changes["geometry_reviews"] = {review_key: review}
    return ToolResult(
        True,
        "selective ground snap facts planned",
        state_patch=StatePatch(room_id=room_id, changes=patch_changes),
        payload=fact,
    )


def _make_vlm_checkpoint_tool(provider: ResourceProvider | None) -> Callable[[ToolCall], ToolResult]:
    def _tool(call: ToolCall) -> ToolResult:
        room_id = str(call.args.get("room_id") or "")
        plan_id = str(call.args.get("plan_id") or "")
        batch_id = str(call.args.get("batch_id") or "")
        checkpoint_type = str(call.args.get("checkpoint_type") or "vlm_checkpoint")
        reviewed_targets = [
            str(item or "").strip()
            for item in (call.args.get("reviewed_targets") or [])
            if str(item or "").strip()
        ]
        payload = {
            "room_id": room_id,
            "plan_id": plan_id,
            "batch_id": batch_id,
            "contract_version": int(call.args.get("contract_version") or 0),
            "checkpoint_type": checkpoint_type,
            "reviewed_targets": reviewed_targets,
            "actors": dict(call.args.get("actors") or {}),
            "placements": dict(call.args.get("placements") or {}),
            "environment_components": dict(call.args.get("environment_components") or {}),
            "scene_name": str(call.args.get("scene_name") or ""),
        }
        if provider is None:
            review = _default_vlm_checkpoint_provider(payload)
        else:
            try:
                review = dict(provider(payload) or {})
            except Exception:  # noqa: BLE001
                review = {
                    "status": "skipped",
                    "overall": "SKIPPED",
                    "issues": [],
                    "advisory_items": [],
                    "skip_reason": "vlm provider failed",
                }

        checkpoint_fact, proposal = _normalize_vlm_checkpoint_result(
            review,
            payload=payload,
            checkpoint_id=call.tool_call_id,
        )
        changes: dict[str, Any] = {
            "custom_vlm_checkpoint_facts": {
                checkpoint_fact["checkpoint_id"]: checkpoint_fact,
            }
        }
        if proposal is not None:
            proposal_key = f"{plan_id}:vlm:{batch_id or checkpoint_type}"
            changes["review_advisory_proposals"] = {proposal_key: proposal}
        user_message = _vlm_checkpoint_user_visible_message(checkpoint_fact)
        return ToolResult(
            True,
            "vlm checkpoint completed",
            state_patch=StatePatch(room_id=room_id, changes=changes),
            payload=checkpoint_fact,
            user_visible_message=user_message,
        )

    return _tool


def _vlm_checkpoint_user_visible_message(checkpoint_fact: dict[str, Any]) -> str:
    checkpoint_type = _safe_review_text(
        checkpoint_fact.get("checkpoint_type"),
        fallback="外观审查",
        allow_empty=False,
    )
    status = str(checkpoint_fact.get("status") or "").strip().lower()
    advisory_count = int(checkpoint_fact.get("advisory_item_count") or 0)
    if advisory_count > 0:
        return f"外观审查完成：{checkpoint_type} 发现 {advisory_count} 条建议，等待房主确认。"
    if status == "skipped":
        return f"外观审查已跳过：{checkpoint_type} 当前未启用或条件不足。"
    return f"外观审查完成：{checkpoint_type} 未发现需要确认的建议。"


def _matching_ground_snap_reviews(raw_reviews: Any, *, plan_id: str, batch_id: str) -> list[dict[str, Any]]:
    reviews: list[dict[str, Any]] = []
    for review in dict(raw_reviews or {}).values():
        if not isinstance(review, dict):
            continue
        if str(review.get("checkpoint_type") or "") != "ground_snap_selective":
            continue
        if plan_id and str(review.get("plan_id") or "") != plan_id:
            continue
        if batch_id and str(review.get("batch_id") or "") != batch_id:
            continue
        reviews.append(dict(review))
    return reviews


def _review_issues(reviews: list[dict[str, Any]]) -> list[dict[str, Any]]:
    issues: list[dict[str, Any]] = []
    for review in reviews:
        for issue in review.get("issues") or []:
            if isinstance(issue, dict):
                issues.append(dict(issue))
    return issues


def _summarize_batch_review_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or "")
    batch_id = str(call.args.get("batch_id") or call.tool_call_id)
    geometry_review = dict(call.args.get("geometry_review") or {})
    ground_snap_reviews = _matching_ground_snap_reviews(
        call.args.get("ground_snap_reviews"),
        plan_id=plan_id,
        batch_id=batch_id,
    )
    vlm_checkpoints = {
        str(key): dict(value)
        for key, value in dict(call.args.get("vlm_checkpoints") or {}).items()
        if isinstance(value, dict)
    }
    actor_import_plan = dict(call.args.get("actor_import_plan") or {})
    actors = {
        str(key): dict(value)
        for key, value in dict(call.args.get("actors") or {}).items()
        if isinstance(value, dict)
    }
    matching_vlm = [
        checkpoint
        for checkpoint in vlm_checkpoints.values()
        if str(checkpoint.get("batch_id") or "") == batch_id
        and (not plan_id or str(checkpoint.get("plan_id") or "") == plan_id)
    ]
    vlm_status_counts: dict[str, int] = {}
    vlm_advisory_count = 0
    reviewed_targets: list[str] = []
    for checkpoint in matching_vlm:
        status = _safe_review_text(checkpoint.get("status"), fallback="unknown", allow_empty=False)
        vlm_status_counts[status] = vlm_status_counts.get(status, 0) + 1
        vlm_advisory_count += int(checkpoint.get("advisory_item_count") or 0)
        for target in checkpoint.get("reviewed_targets") or []:
            safe_target = _safe_review_text(target, fallback="", allow_empty=True)
            if safe_target and safe_target not in reviewed_targets:
                reviewed_targets.append(safe_target)
    geometry_issues = geometry_review.get("issues") if isinstance(geometry_review.get("issues"), list) else []
    ground_snap_issues = _review_issues(ground_snap_reviews)
    planned_actors = actor_import_plan.get("planned_actors") if isinstance(actor_import_plan.get("planned_actors"), list) else []
    geometry_issue_count = int(geometry_review.get("issue_count") or len(geometry_issues))
    ground_snap_issue_count = len(ground_snap_issues)
    fact = {
        "plan_id": plan_id,
        "batch_id": batch_id,
        "status": "needs_attention" if geometry_issues or ground_snap_issues or vlm_advisory_count else "ok",
        "geometry_status": _safe_review_text(geometry_review.get("status"), fallback="unknown", allow_empty=False),
        "geometry_issue_count": geometry_issue_count,
        "ground_snap_review_count": len(ground_snap_reviews),
        "ground_snap_issue_count": ground_snap_issue_count,
        "vlm_checkpoint_count": len(matching_vlm),
        "vlm_advisory_count": vlm_advisory_count,
        "vlm_status_counts": dict(sorted(vlm_status_counts.items())),
        "import_actor_count": int(actor_import_plan.get("actor_count") or len(planned_actors)),
        "import_ready_count": int(actor_import_plan.get("ready_count") or 0),
        "runtime_actor_count": len(actors),
        "reviewed_targets": reviewed_targets[:6],
        "source": "runtime_review_summary",
    }
    return ToolResult(
        True,
        "batch review summary created",
        state_patch=StatePatch(
            room_id=room_id,
            changes={
                "custom_review_summary_facts": {
                    batch_id: fact,
                    f"{plan_id}:{batch_id}": fact,
                }
            },
        ),
        payload=fact,
        user_visible_message=(
            "批次审查汇总完成："
            f"几何问题 {fact['geometry_issue_count']} 项，"
            f"贴地建议 {fact['ground_snap_issue_count']} 项，"
            f"外观建议 {fact['vlm_advisory_count']} 项。"
        ),
    )


def _generate_review_adjustment_proposal_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    batch_id = str(call.args.get("batch_id") or "")
    geometry_review = dict(call.args.get("geometry_review") or {})
    ground_snap_reviews = _matching_ground_snap_reviews(
        call.args.get("ground_snap_reviews"),
        plan_id=plan_id,
        batch_id=batch_id,
    )
    batch_summary = dict(call.args.get("batch_review_summary") or {})
    review_advisories = {
        str(key): dict(value)
        for key, value in dict(call.args.get("review_advisories") or {}).items()
        if isinstance(value, dict)
    }
    issues = [issue for issue in (geometry_review.get("issues") or []) if isinstance(issue, dict)]
    issues.extend(_review_issues(ground_snap_reviews))
    deltas = _layout_deltas_from_review_issues(issues)
    matching_advisory_count = 0
    for advisory in review_advisories.values():
        if plan_id and str(advisory.get("plan_id") or "") != plan_id:
            continue
        if batch_id and str(advisory.get("batch_id") or "") not in {"", batch_id}:
            continue
        matching_advisory_count += int(advisory.get("item_count") or 0)
    if not deltas:
        return ToolResult(
            True,
            "no review adjustment proposal needed",
            payload={
                "plan_id": plan_id,
                "batch_id": batch_id,
                "status": "not_needed",
                "issue_count": len(issues),
                "advisory_count": matching_advisory_count,
            },
            user_visible_message="批次审查未生成需要确认的低风险布局调整。",
        )
    proposal = {
        "proposal_id": f"review-adjust-{plan_id}-{batch_id or 'batch'}",
        "plan_id": plan_id,
        "batch_id": batch_id,
        "status": "proposed",
        "risk_level": "low",
        "reason": "review generated low-risk layout adjustment proposal",
        "deltas": deltas,
        "issue_count": len(issues),
        "advisory_count": matching_advisory_count,
        "review_summary_status": _safe_review_text(batch_summary.get("status"), fallback="unknown", allow_empty=False),
    }
    return ToolResult(
        True,
        "review adjustment proposal created",
        state_patch=StatePatch(room_id=room_id, changes={"layout_adjustment_proposals": {plan_id: proposal}}),
        payload=proposal,
        user_visible_message=f"批次审查生成 {len(deltas)} 条低风险布局调整建议，等待房主确认。",
    )


def _default_vlm_checkpoint_provider(payload: dict[str, Any]) -> dict[str, Any]:
    return {
        "plan_id": str(payload.get("plan_id") or ""),
        "batch_id": str(payload.get("batch_id") or ""),
        "checkpoint_type": str(payload.get("checkpoint_type") or "vlm_checkpoint"),
        "reviewed_targets": list(payload.get("reviewed_targets") or []),
        "status": "skipped",
        "overall": "SKIPPED",
        "issues": [],
        "advisory_items": [],
        "skip_reason": "vlm review provider not configured",
        "source": "runtime_vlm_checkpoint",
    }


def _normalize_vlm_checkpoint_result(
    parsed: dict[str, Any],
    *,
    payload: dict[str, Any],
    checkpoint_id: str,
) -> tuple[dict[str, Any], dict[str, Any] | None]:
    plan_id = str(payload.get("plan_id") or "")
    batch_id = str(payload.get("batch_id") or "")
    checkpoint_type = _safe_review_text(
        parsed.get("checkpoint_type") or payload.get("checkpoint_type"),
        fallback="vlm_checkpoint",
        allow_empty=False,
    )
    reviewed_targets = [
        _safe_review_text(item, fallback="target", allow_empty=False)
        for item in (parsed.get("reviewed_targets") or payload.get("reviewed_targets") or [])
        if str(item or "").strip()
    ][:8]
    advisory_items = _vlm_advisory_items(parsed, checkpoint_type=checkpoint_type)
    status = _safe_review_text(parsed.get("status"), fallback="", allow_empty=True).lower()
    overall = _safe_review_text(parsed.get("overall"), fallback="", allow_empty=True).upper()
    if not status:
        if advisory_items:
            status = "needs_confirmation"
        elif overall == "SKIPPED":
            status = "skipped"
        else:
            status = "ok"
    checkpoint_fact = {
        "checkpoint_id": _safe_review_text(checkpoint_id, fallback="vlm-checkpoint", allow_empty=False),
        "plan_id": plan_id,
        "batch_id": batch_id,
        "checkpoint_type": checkpoint_type,
        "reviewed_targets": reviewed_targets,
        "status": status,
        "overall": overall or status.upper(),
        "advisory_item_count": len(advisory_items),
        "source": "runtime_vlm_checkpoint",
    }
    if str(parsed.get("skip_reason") or "").strip():
        checkpoint_fact["skip_reason"] = _safe_review_text(parsed.get("skip_reason"), fallback="skipped", allow_empty=False)
    if not advisory_items:
        return checkpoint_fact, None
    proposal_id = _safe_review_text(
        parsed.get("proposal_id"),
        fallback=f"vlm-{plan_id}-{batch_id or checkpoint_type}",
        allow_empty=False,
    )
    proposal = {
        "proposal_id": proposal_id,
        "plan_id": plan_id,
        "room_id": str(payload.get("room_id") or ""),
        "batch_id": batch_id,
        "source": "review_advisory",
        "source_review_ids": [checkpoint_fact["checkpoint_id"]],
        "risk_level": "advisory",
        "status": "proposed",
        "requires_confirmation": True,
        "direct_execution_allowed": False,
        "checkpoint_type": checkpoint_type,
        "reviewed_targets": reviewed_targets,
        "item_count": len(advisory_items),
        "items": advisory_items,
    }
    return checkpoint_fact, proposal


def _vlm_advisory_items(parsed: dict[str, Any], *, checkpoint_type: str) -> list[dict[str, Any]]:
    raw_items: list[Any] = []
    raw_items.extend(list(parsed.get("advisory_items") or []))
    raw_items.extend(list(parsed.get("issues") or []))
    items: list[dict[str, Any]] = []
    for index, raw in enumerate(raw_items, start=1):
        item = _vlm_advisory_item(raw, index=index, checkpoint_type=checkpoint_type)
        if item:
            items.append(item)
    return items


def _vlm_advisory_item(raw: Any, *, index: int, checkpoint_type: str) -> dict[str, Any]:
    if isinstance(raw, dict):
        message_raw = (
            raw.get("message")
            or raw.get("summary")
            or raw.get("reason")
            or raw.get("fix_suggestion")
            or raw.get("type")
        )
        confidence_raw = raw.get("confidence")
        target_hint = raw.get("target_hint") or raw.get("target") or raw.get("name")
        issue_type = raw.get("type") or "vlm_advisory"
    else:
        message_raw = raw
        confidence_raw = None
        target_hint = ""
        issue_type = "vlm_advisory"
    message = _safe_review_text(message_raw, fallback="", allow_empty=True)
    if not message:
        return {}
    try:
        confidence = float(confidence_raw) if confidence_raw is not None else 0.8
    except (TypeError, ValueError):
        confidence = 0.8
    if confidence < 0.65:
        return {}
    item = {
        "item_id": f"vlm-{index:02d}",
        "message": message,
        "requires_confirmation": True,
        "confidence": round(confidence, 3),
        "checkpoint_type": _safe_review_text(checkpoint_type, fallback="vlm_checkpoint", allow_empty=False),
        "issue_type": _safe_review_text(issue_type, fallback="vlm_advisory", allow_empty=False),
    }
    if str(target_hint or "").strip():
        item["target_hint"] = _safe_review_text(target_hint, fallback="target", allow_empty=False)
    return item


def _default_geometry_review_provider(payload: dict[str, Any]) -> dict[str, Any]:
    plan_id = str(payload.get("plan_id") or "")
    placements = dict(payload.get("placements") or {})
    room_bounds = payload.get("room_bounds") or [-4.0, 4.0, -4.0, 4.0]
    environment_hints = [
        str(item or "").strip()
        for item in (payload.get("environment_hints") or [])
        if str(item or "").strip()
    ]
    issues: list[dict[str, Any]] = []
    for name, placement in placements.items():
        if not isinstance(placement, dict):
            continue
        position = placement.get("position") or [0.0, 0.0, 0.0]
        if not isinstance(position, list) or len(position) < 3:
            issues.append({"name": str(name), "type": "missing_position", "severity": "medium"})
            continue
        x, y, z = _float(position[0]), _float(position[1]), _float(position[2])
        if abs(y) > 0.03 and _floor_supported_name(str(name)):
            issues.append(
                {
                    "name": str(name),
                    "type": "floating_or_sunken",
                    "severity": "low",
                    "current_y": y,
                    "suggested_y": 0.0,
                }
            )
        min_x, max_x, min_z, max_z = [_float(v) for v in room_bounds[:4]]
        if x < min_x or x > max_x or z < min_z or z > max_z:
            issues.append(
                {
                    "name": str(name),
                    "type": "out_of_bounds",
                    "severity": "low",
                    "current_position": [x, y, z],
                    "bounds": [min_x, max_x, min_z, max_z],
                }
            )
    review = {
        "plan_id": plan_id,
        "batch_id": str(payload.get("batch_id") or ""),
        "contract_version": int(payload.get("contract_version") or 0),
        "checkpoint_type": str(payload.get("checkpoint_type") or "geometry_review"),
        "reviewed_targets": [
            str(item or "").strip()
            for item in (payload.get("reviewed_targets") or [])
            if str(item or "").strip()
        ],
        "status": "needs_adjustment" if issues else "ok",
        "issue_count": len(issues),
        "issues": issues,
        "environment_hints": environment_hints[:4],
        "source": "runtime_geometry_rules",
    }
    return review


def _environment_component_types(environment_components: dict[str, Any]) -> list[str]:
    component_types = [
        _safe_review_text(component.get("component_type") or component.get("type"), allow_empty=True)
        for component in environment_components.values()
        if isinstance(component, dict)
    ]
    return [component_type for component_type in component_types if component_type][:4]


def _normalize_geometry_review_result(parsed: dict[str, Any], *, plan_id: str) -> dict[str, Any]:
    if str(parsed.get("status") or "").lower() == "error" or parsed.get("error"):
        raise RuntimeError(str(parsed.get("message") or parsed.get("error") or "geometry review failed"))
    issues = [
        item
        for item in (_safe_geometry_review_issue(raw_item) for raw_item in (parsed.get("issues") or []))
        if item
    ]
    overall = str(parsed.get("overall") or "").strip().upper()
    status = str(parsed.get("status") or "").strip().lower()
    if not status:
        if overall in {"PASS", "OK"}:
            status = "ok"
        elif overall in {"SKIPPED"}:
            status = "skipped"
        elif issues:
            status = "needs_adjustment"
        else:
            status = "ok"
    return {
        "plan_id": str(parsed.get("plan_id") or plan_id),
        "batch_id": str(parsed.get("batch_id") or ""),
        "contract_version": int(parsed.get("contract_version") or 0),
        "checkpoint_type": str(parsed.get("checkpoint_type") or "geometry_review"),
        "reviewed_targets": [
            str(item or "").strip()
            for item in (parsed.get("reviewed_targets") or [])
            if str(item or "").strip()
        ],
        "status": status,
        "overall": overall or status.upper(),
        "score": parsed.get("score"),
        "issue_count": len(issues),
        "issues": issues,
        "environment_hints": [
            _safe_review_text(item, allow_empty=False)
            for item in (parsed.get("environment_hints") or [])
            if str(item or "").strip()
        ][:4],
        "advisory_items": [
            item
            for item in (_safe_geometry_review_advisory(raw_item) for raw_item in (parsed.get("advisory_items") or []))
            if item
        ],
        "source": _safe_review_text(parsed.get("source"), fallback="review_provider", allow_empty=False),
    }


def _review_text_is_unsafe(value: str) -> bool:
    lowered = str(value or "").lower()
    unsafe_tokens = (
        "api_key",
        "c:/",
        "c:\\",
        "file://",
        "hidden",
        "https://",
        "http://",
        "image_path",
        "local_path",
        "metadata",
        "model_path",
        "prompt=",
        "provider",
        "raw",
        "screenshot_path",
        "token",
        "url",
        "vlm_raw",
    )
    return any(token in lowered for token in unsafe_tokens)


def _safe_review_text(raw: Any, *, fallback: str = "", allow_empty: bool = True) -> str:
    text = str(raw or "").strip()
    if not text:
        return "" if allow_empty else fallback
    if _review_text_is_unsafe(text):
        return fallback or "内部细节已隐藏"
    return text[:500]


def _safe_geometry_review_issue(item: Any) -> dict[str, Any]:
    if not isinstance(item, dict):
        if item:
            return {
                "type": "advisory",
                "message": _safe_review_text(item, fallback="内部细节已隐藏", allow_empty=False),
                "severity": "low",
            }
        return {}
    safe: dict[str, Any] = {}
    for field, value in item.items():
        normalized = str(field or "").strip()
        if normalized not in GeometryReviewValidator._ALLOWED_ISSUE_FIELDS:
            continue
        if normalized in {"current_position", "suggested_position", "bounds"}:
            if isinstance(value, list):
                numbers = [float(number) for number in value if isinstance(number, (int, float))]
                min_length = 4 if normalized == "bounds" else 3
                if len(numbers) >= min_length:
                    safe[normalized] = numbers
            continue
        if isinstance(value, str):
            fallback = "advisory" if normalized == "type" else (
                "low" if normalized == "severity" else "内部细节已隐藏"
            )
            safe[normalized] = _safe_review_text(value, fallback=fallback, allow_empty=False)
        elif isinstance(value, (int, float, bool)):
            safe[normalized] = value
    if not str(safe.get("type") or "").strip():
        safe["type"] = "advisory"
    return safe


def _safe_geometry_review_advisory(item: Any) -> dict[str, Any]:
    if not isinstance(item, dict):
        if item:
            return {
                "type": "advisory",
                "message": _safe_review_text(item, fallback="内部细节已隐藏", allow_empty=False),
            }
        return {}
    safe: dict[str, Any] = {}
    for field, value in item.items():
        normalized = str(field or "").strip()
        if normalized not in GeometryReviewValidator._ALLOWED_ADVISORY_FIELDS:
            continue
        if isinstance(value, str):
            fallback = "advisory" if normalized == "type" else "内部细节已隐藏"
            safe[normalized] = _safe_review_text(value, fallback=fallback, allow_empty=False)
        elif isinstance(value, (int, float, bool)):
            safe[normalized] = value
    if not str(safe.get("message") or safe.get("reason") or safe.get("summary") or "").strip():
        return {}
    return safe


def _layout_deltas_from_review_issues(issues: list[dict[str, Any]]) -> list[dict[str, Any]]:
    deltas: list[dict[str, Any]] = []
    for issue in issues:
        name = str(issue.get("name") or "")
        issue_type = str(issue.get("type") or "")
        if not name:
            continue
        if issue_type == "floating_or_sunken":
            suggested_position = issue.get("suggested_position")
            if isinstance(suggested_position, list) and len(suggested_position) >= 2:
                target_y = _float(suggested_position[1])
            else:
                target_y = _float(issue.get("suggested_y", 0.0))
            deltas.append(
                {
                    "op": "move",
                    "actor_name": name,
                    "batch_id": str(issue.get("batch_id") or ""),
                    "position_patch": {"y": target_y},
                    "reason": "snap floor-supported object to ground",
                    "risk_level": "low",
                }
            )
        elif issue_type == "out_of_bounds":
            current = issue.get("current_position") or [0.0, 0.0, 0.0]
            bounds = issue.get("bounds") or [-4.0, 4.0, -4.0, 4.0]
            x, y, z = _float(current[0]), _float(current[1]), _float(current[2])
            min_x, max_x, min_z, max_z = [_float(v) for v in bounds[:4]]
            deltas.append(
                {
                    "op": "move",
                    "actor_name": name,
                    "batch_id": str(issue.get("batch_id") or ""),
                    "position_patch": {"x": min(max(x, min_x), max_x), "z": min(max(z, min_z), max_z)},
                    "reason": "clamp object inside layout bounds",
                    "risk_level": "low",
                }
            )
    return deltas


def _propose_layout_adjustment_tool(call: ToolCall) -> ToolResult:
    room_id = str(call.args.get("room_id") or "")
    plan_id = str(call.args.get("plan_id") or call.tool_call_id)
    issues = [issue for issue in (call.args.get("issues") or []) if isinstance(issue, dict)]
    deltas = _layout_deltas_from_review_issues(issues)
    proposal = {
        "proposal_id": f"layout-{plan_id}",
        "plan_id": plan_id,
        "status": "proposed",
        "risk_level": "low",
        "deltas": deltas,
        "issue_count": len(issues),
    }
    return ToolResult(
        True,
        "layout adjustment proposal created",
        state_patch=StatePatch(room_id=room_id, changes={"layout_adjustment_proposals": {plan_id: proposal}}),
        payload=proposal,
    )


def _resource_payload_from_call(call: ToolCall) -> dict[str, Any]:
    asset_requests = dict(call.args.get("asset_requests") or {})
    model_items = _model_items_from_resource_args(call.args, asset_requests=asset_requests)
    return {
        "room_id": str(call.args.get("room_id") or ""),
        "batch_id": str(call.args.get("batch_id") or call.tool_call_id),
        "plan_id": str(call.args.get("plan_id") or ""),
        "scene_version": max(1, int(call.args.get("scene_version") or 1)),
        "model_items": model_items,
        "asset_requests": asset_requests,
        "placements": dict(call.args.get("placements") or {}),
        "environment_components": dict(call.args.get("environment_components") or {}),
        "image_resources": dict(call.args.get("image_resources") or {}),
        "model_resources": dict(call.args.get("model_resources") or {}),
        "scene_name": str(call.args.get("scene_name") or ""),
        "tool_call_id": call.tool_call_id,
    }


def _model_items_from_resource_args(args: dict[str, Any], *, asset_requests: dict[str, Any]) -> list[str]:
    explicit = [str(item) for item in (args.get("model_items") or []) if str(item or "")]
    if explicit:
        return explicit
    names: list[str] = []
    for key, request in asset_requests.items():
        if isinstance(request, dict):
            name = str(request.get("name") or key or "").strip()
        else:
            name = str(key or "").strip()
        if name:
            names.append(name)
    return names


def _default_image_resource_provider(payload: dict[str, Any]) -> dict[str, Any]:
    batch_id = str(payload.get("batch_id") or "")
    model_items = _model_items_from_resource_args(payload, asset_requests=dict(payload.get("asset_requests") or {}))
    return {
        name: {
            "image_request_id": f"image-req-{batch_id}-{index + 1:02d}",
            "name": name,
            "status": "prepared",
            "mode": "mock_reference",
        }
        for index, name in enumerate(model_items)
    }


def _default_model_resource_provider(payload: dict[str, Any]) -> dict[str, Any]:
    batch_id = str(payload.get("batch_id") or "")
    model_items = _model_items_from_resource_args(payload, asset_requests=dict(payload.get("asset_requests") or {}))
    return {
        name: {
            "model_request_id": f"model-req-{batch_id}-{index + 1:02d}",
            "name": name,
            "status": "prepared",
            "local_path": f"runtime/default_models/{batch_id or 'batch'}/{index + 1:02d}.glb",
            "source": _preferred_asset_source(name),
        }
        for index, name in enumerate(model_items)
    }


def _default_actor_import_provider(payload: dict[str, Any]) -> dict[str, Any]:
    batch_id = str(payload.get("batch_id") or "")
    plan_id = str(payload.get("plan_id") or "")
    model_items = [str(item) for item in (payload.get("model_items") or []) if str(item or "")]
    placements = dict(payload.get("placements") or {})
    actors = {}
    for index, name in enumerate(model_items):
        actor_id = f"actor-{batch_id}-{index + 1:02d}"
        placement = dict(placements.get(name) or {})
        position = list(placement.get("position") or [0.0, 0.0, 0.0])
        rotation = list(placement.get("rotation") or [0.0, 0.0, 0.0])
        scale = list(placement.get("scale") or [1.0, 1.0, 1.0])
        aabb = _default_actor_aabb(position=position, scale=scale)
        actors[actor_id] = {
            "actor_id": actor_id,
            "asset_id": str(name),
            "aliases": [str(name)],
            "display_name": str(name),
            "model_ref": str(name),
            "name": name,
            "native_name": str(name),
            "plan_id": plan_id,
            "batch_id": batch_id,
            "requested_name": str(name),
            "position": position,
            "rotation": rotation,
            "scale": scale,
            "aabb": aabb,
            "semantic_role": str(name),
            "entity_type": "actor",
            "grounding_status": "grounded",
            "support_type": "floor_supported",
            "interaction_capability": [],
            "gameplay_tags": [],
            "physics_profile": {},
            "audio_profile": {
                "surface": "generic",
            },
            "lighting_profile": {
                "receives_light": True,
                "casts_shadow": True,
            },
            "script_bindings": ["runtime_scene_entity"],
            "sync_status": "runtime_state",
            "sync_lifecycle_status": "runtime_state",
            "review_status": "pending_review",
            "zone_hint": str(placement.get("zone_hint") or ""),
            "source": "runtime_default_import",
        }
    return {
        "actors": actors,
        "source": "runtime_default_import_provider",
        "engine_write_result": {
            "provider_source": "runtime_default_import_provider",
            "requested_count": len(model_items),
            "identity_result_count": len(actors),
            "missing_identity_count": 0,
            "status_counts": {"runtime_state_only": len(actors)} if actors else {},
            "bridge_call_count": 0,
            "bridge_success_count": 0,
            "bridge_failed_count": 0,
            "bridge_method_counts": {},
            "bridge_error_code_counts": {},
        },
    }


def _default_actor_aabb(*, position: Sequence[Any], scale: Sequence[Any]) -> dict[str, list[float]]:
    def number_at(values: Sequence[Any], index: int, fallback: float) -> float:
        try:
            return float(values[index])
        except (IndexError, TypeError, ValueError):
            return fallback

    x = number_at(position, 0, 0.0)
    y = number_at(position, 1, 0.0)
    z = number_at(position, 2, 0.0)
    sx = max(0.1, abs(number_at(scale, 0, 1.0)))
    sy = max(0.1, abs(number_at(scale, 1, 1.0)))
    sz = max(0.1, abs(number_at(scale, 2, 1.0)))
    return {
        "min": [round(x - sx * 0.5, 4), round(y, 4), round(z - sz * 0.5, 4)],
        "max": [round(x + sx * 0.5, 4), round(y + sy, 4), round(z + sz * 0.5, 4)],
    }


def _make_scene_snapshot_tool(provider: Callable[[Any], dict[str, Any]]) -> Callable[[ToolCall], ToolResult]:
    def _tool(call: ToolCall) -> ToolResult:
        room_id = str(call.args.get("room_id") or "")
        try:
            snapshot = dict(provider(dict(call.args)) or {})
        except Exception as exc:  # noqa: BLE001
            return _provider_failure_tool_result(
                "scene_snapshot",
                exc,
                user_visible_message="场景状态读取失败，当前状态可能需要稍后刷新。",
            )
        try:
            plan_id = str(call.args.get("plan_id") or "")
            batch_id = str(call.args.get("batch_id") or "")
            normalized_actors = _normalize_snapshot_actors(snapshot)
            known_actors = {
                str(actor_id): dict(actor)
                for actor_id, actor in dict(call.args.get("known_actors") or {}).items()
                if str(actor_id or "") and isinstance(actor, Mapping)
            }
            if known_actors:
                actor_updates = _match_snapshot_actors_to_runtime(
                    normalized_actors,
                    known_actors,
                )
            elif plan_id or batch_id:
                # A plan-scoped snapshot is an observation, not ownership
                # evidence.  Without Runtime identity facts, copying every
                # native actor into ``actors`` would assign existing scene
                # objects to the current plan/batch and overwrite stable
                # resource identity from earlier batches.  Keep unmatched
                # native rows in ``observed_actors`` until a later reconcile
                # supplies an unambiguous known-actor projection.
                actor_updates = {}
            else:
                # An explicit unscoped scene refresh may still expose native
                # actors as unmanaged Runtime facts for inspection.
                actor_updates = dict(normalized_actors)
            for actor in actor_updates.values():
                if not isinstance(actor, dict):
                    continue
                if plan_id and not str(actor.get("plan_id") or ""):
                    actor["plan_id"] = plan_id
                if batch_id and not str(actor.get("batch_id") or ""):
                    actor["batch_id"] = batch_id
            actors = ActorFactValidator.safe_actor_map(actor_updates)
            observed_actors = ActorFactValidator.safe_actor_map(normalized_actors)
        except Exception as exc:  # noqa: BLE001
            return _provider_failure_tool_result(
                "scene_snapshot",
                exc,
                user_visible_message="场景状态读取结果不符合系统协议，当前状态可能需要稍后刷新。",
            )
        snapshot_id = str(call.args.get("snapshot_id") or call.tool_call_id)
        snapshot_actors = dict(observed_actors)
        for runtime_actor_id, actor in actors.items():
            for alias in actor.get("aliases") or []:
                alias_id = str(alias or "").strip()
                if alias_id and alias_id != runtime_actor_id:
                    snapshot_actors.pop(alias_id, None)
            snapshot_actors[runtime_actor_id] = dict(actor)
        snapshot_payload = {
            "snapshot_id": snapshot_id,
            "room_id": room_id,
            "plan_id": str(call.args.get("plan_id") or ""),
            "batch_id": str(call.args.get("batch_id") or ""),
            "scene_version": max(1, int(call.args.get("scene_version") or 1)),
            "scene_name": str(snapshot.get("scene_name") or call.args.get("scene_name") or ""),
            "actor_count": len(snapshot_actors),
            "source": str(snapshot.get("source") or "scene_snapshot_provider"),
            "actors": list(snapshot_actors.values()),
        }
        return ToolResult(
            True,
            "scene snapshot captured",
            state_patch=StatePatch(
                room_id=room_id,
                changes={
                    "engine_scene_snapshots": {snapshot_id: snapshot_payload},
                    "observed_actors": observed_actors,
                    "actors": actors,
                },
            ),
            payload=snapshot_payload,
        )

    return _tool


def _match_snapshot_actors_to_runtime(
    observed_actors: Mapping[str, Mapping[str, Any]],
    known_actors: Mapping[str, Mapping[str, Any]],
) -> dict[str, dict[str, Any]]:
    """Match native observations to Runtime identity without inventing ownership."""

    indexes: dict[str, dict[str, set[str]]] = {
        "entity_id": {},
        "asset_id": {},
        "model_ref": {},
        "name": {},
    }

    def add_index(kind: str, value: Any, actor_id: str) -> None:
        token = str(value or "").strip().casefold()
        if token:
            indexes[kind].setdefault(token, set()).add(actor_id)

    for actor_id, actor in known_actors.items():
        runtime_actor_id = str(actor_id or "").strip()
        if not runtime_actor_id:
            continue
        add_index("entity_id", actor.get("entity_id"), runtime_actor_id)
        add_index("asset_id", actor.get("asset_id"), runtime_actor_id)
        add_index("model_ref", actor.get("model_ref"), runtime_actor_id)
        for value in (
            actor.get("native_name"),
            actor.get("requested_name"),
            actor.get("display_name"),
            actor.get("name"),
            *(actor.get("aliases") or []),
        ):
            add_index("name", value, runtime_actor_id)

    matched: dict[str, dict[str, Any]] = {}
    claimed_runtime_ids: set[str] = set()
    for observed_id, observed_value in observed_actors.items():
        observed = dict(observed_value or {})
        runtime_actor_id = str(observed_id or "").strip()
        if runtime_actor_id not in known_actors:
            candidates: set[str] = set()
            for kind, values in (
                ("entity_id", (observed.get("entity_id"),)),
                ("asset_id", (observed.get("asset_id"),)),
                ("model_ref", (observed.get("model_ref"),)),
                (
                    "name",
                    (
                        observed.get("native_name"),
                        observed.get("requested_name"),
                        observed.get("display_name"),
                        observed.get("name"),
                        *(observed.get("aliases") or []),
                    ),
                ),
            ):
                for value in values:
                    token = str(value or "").strip().casefold()
                    owners = indexes[kind].get(token, set()) if token else set()
                    if len(owners) == 1:
                        candidates.update(owners)
            if len(candidates) != 1:
                continue
            runtime_actor_id = next(iter(candidates))
        if runtime_actor_id in claimed_runtime_ids:
            continue
        known = dict(known_actors.get(runtime_actor_id) or {})
        aliases = list(dict.fromkeys([
            *(known.get("aliases") or []),
            str(observed_id or "").strip(),
        ]))
        merged = {
            **known,
            **observed,
            "actor_id": runtime_actor_id,
            "aliases": [alias for alias in aliases if alias and alias != runtime_actor_id],
        }
        # Runtime identity remains authoritative; the native snapshot owns only
        # observed geometry, transform, and lifecycle fields.
        for field in (
            "entity_id",
            "asset_id",
            "model_ref",
            "plan_id",
            "batch_id",
            "semantic_role",
            "entity_type",
            "grounding_status",
            "support_type",
            "sync_status",
        ):
            if known.get(field) not in (None, "", [], {}):
                merged[field] = known[field]
        matched[runtime_actor_id] = merged
        claimed_runtime_ids.add(runtime_actor_id)
    return matched


def _empty_scene_snapshot_provider(request: Any) -> dict[str, Any]:
    room_id = str(request.get("room_id") or "") if isinstance(request, dict) else str(request or "")
    return {"room_id": room_id, "source": "empty", "actors": []}


def _provider_failure_tool_result(
    provider_kind: str,
    exc: Exception,
    *,
    user_visible_message: str,
) -> ToolResult:
    kind = str(provider_kind or "provider").strip() or "provider"
    return ToolResult(
        False,
        f"{kind} provider failed",
        retryable=True,
        error_code=f"{kind}_provider_failed",
        user_visible_message=str(user_visible_message or "服务暂时不可用，系统会稍后重试或降级处理。"),
    )


def _resource_provider_failure_tool_result(
    resource_kind: str,
    exc: Exception,
    *,
    room_id: str,
    batch_id: str,
    plan_id: str,
    requested_items: Sequence[str],
    user_visible_message: str,
) -> ToolResult:
    kind = "image" if str(resource_kind or "") == "image" else "model"
    provider_kind = f"{kind}_resource"
    failure_code = f"{provider_kind}_provider_failed"
    failed_resources = _failed_resource_entries(
        list(requested_items or []),
        kind=kind,
        batch_id=batch_id,
        status="failed",
        source=failure_code,
    )
    state_key = "image_resource_plans" if kind == "image" else "model_resource_plans"
    return ToolResult(
        False,
        f"{provider_kind} provider failed; recorded failed resource facts",
        retryable=True,
        error_code=failure_code,
        state_patch=StatePatch(
            room_id=str(room_id or ""),
            changes={
                state_key: {str(batch_id or ""): failed_resources},
                "custom_resource_phase_facts": {
                    _resource_phase_fact_key(batch_id, kind): _resource_phase_fact(
                        batch_id=batch_id,
                        plan_id=plan_id,
                        phase=kind,
                        resources=failed_resources,
                        requested_count=len(list(requested_items or [])),
                    )
                },
            },
        ),
        payload={
            f"{provider_kind}s": failed_resources,
            "requested_count": len(list(requested_items or [])),
            "ready_count": 0,
            "failed_count": len(list(requested_items or [])),
        },
        user_visible_message=str(user_visible_message or "服务暂时不可用，系统会稍后重试或降级处理。"),
    )


def _normalize_snapshot_actors(snapshot: dict[str, Any]) -> dict[str, dict[str, Any]]:
    raw = snapshot.get("actors") or []
    if isinstance(raw, dict):
        raw_iter = raw.values()
    else:
        raw_iter = raw
    actors: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(raw_iter):
        if not isinstance(item, dict):
            continue
        actor_id = str(
            item.get("actor_id")
            or item.get("actor_guid")
            or item.get("guid")
            or item.get("id")
            or item.get("name")
            or f"observed-{index}"
        )
        if not actor_id:
            continue
        actor = {
            "actor_id": actor_id,
            "name": str(item.get("name") or item.get("actor_name") or actor_id),
            "position": list(item.get("position") or [0.0, 0.0, 0.0]),
            "rotation": list(item.get("rotation") or [0.0, 0.0, 0.0]),
            "scale": list(item.get("scale") or [1.0, 1.0, 1.0]),
            "version": item.get("version"),
            "source": str(snapshot.get("source") or "scene_snapshot_provider"),
        }
        for field in (
            "entity_id",
            "asset_id",
            "model_ref",
            "plan_id",
            "batch_id",
            "native_name",
            "requested_name",
            "display_name",
        ):
            value = item.get(field)
            if value not in (None, "", [], {}):
                actor[field] = value
        if isinstance(item.get("aliases"), list):
            actor["aliases"] = list(item.get("aliases") or [])
        aabb = item.get("aabb") or item.get("world_aabb") or item.get("bounds")
        if isinstance(aabb, dict):
            actor["aabb"] = dict(aabb or {})
        elif isinstance(aabb, (list, tuple)) and len(aabb) >= 6:
            actor["aabb"] = list(aabb[:6])
        if "bounds_ready" in item:
            bounds_ready = bool(item.get("bounds_ready"))
            actor["bounds_ready"] = bounds_ready
            actor["bounds_source"] = "engine_actual" if bounds_ready else "estimated"
            actor["engine_lifecycle_status"] = "bounds_ready" if bounds_ready else "engine_loading"
            actor["status"] = "ready" if bounds_ready else "engine_loading"
        for field in ("render_status_observed", "render_ready", "render_failed"):
            if field in item:
                actor[field] = bool(item.get(field))
        gpu_build_state = str(item.get("gpu_build_state") or "").strip()
        if gpu_build_state:
            actor["gpu_build_state"] = gpu_build_state
        for field in ("mesh_count", "renderable_mesh_count", "invalid_mesh_count"):
            if field not in item:
                continue
            try:
                actor[field] = max(0, int(item.get(field) or 0))
            except (TypeError, ValueError):
                actor[field] = 0
        actors[actor_id] = actor
    return actors


def _filter_abstract_items(items: list[str]) -> list[str]:
    return [item for item in items if item and item not in _ABSTRACT_LAYOUT_TERMS]


def _derive_layout_items(text: str) -> list[str]:
    clean = str(text or "")
    layout = ["入口", "通行动线", "视觉焦点"]
    if any(term in clean for term in ("室内", "卧室", "藏宝室", "宝库", "密室")):
        layout.append("room_box")
    if any(term in clean for term in ("森林", "草地", "天空", "室外", "集市", "营地")):
        layout.append("terrain/substrate")
    return layout


def _preferred_asset_source(name: str) -> str:
    if any(term in name for term in ("铁丝网", "栅栏", "围栏", "栏杆")):
        return "text_to_3d_or_procedural"
    return "retrieve_or_generate"


def _preferred_substrate_handler(name: str, pipeline: str = "") -> str:
    text = f"{name} {pipeline}".lower()
    if any(term in name for term in ("天空", "天幕")) or "sky" in text:
        return "skybox"
    if any(term in name for term in ("边界", "栅栏", "围栏", "入口")) or any(
        term in text for term in ("boundary", "fence", "entrance")
    ):
        return "boundary_component"
    if any(term in name for term in ("森林", "树林", "草地", "草原", "地形", "山坡", "道路", "地面", "河流", "小河", "溪流", "湖泊", "湖面", "水面")) or any(
        term in text for term in ("terrain", "ground", "forest", "grass", "road", "river", "stream", "lake", "water")
    ):
        return "terrain_component"
    return "environment_component"


def _substrate_component_type(name: str, handler: str = "") -> str:
    text = f"{name} {handler}".lower()
    if "skybox" in text or any(term in name for term in ("天空", "天幕")):
        return "skybox"
    if "boundary" in text or "fence" in text or any(term in name for term in ("边界", "栅栏", "围栏", "入口")):
        return "boundary"
    if "terrain" in text or any(term in name for term in ("森林", "树林", "草地", "草原", "地形", "山坡", "道路", "地面")):
        return "terrain"
    return "environment"


def build_placement_proposals(model_items: list[str], layout_items: list[str]) -> dict[str, dict[str, Any]]:
    return {
        name: {
            "name": name,
            "position": _placement_position(index, len(model_items), layout_items),
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
            "zone_hint": _zone_hint(index, layout_items),
            "risk_level": "low",
            "status": "proposed",
        }
        for index, name in enumerate(model_items)
    }


def _placement_position(index: int, total: int, layout_items: list[str]) -> list[float]:
    if total <= 1:
        return [0.0, 0.0, 0.0]
    spacing = 1.35 if "room_box" in layout_items else 1.8
    columns = min(3, max(1, total))
    row = index // columns
    col = index % columns
    x = (col - (columns - 1) / 2.0) * spacing
    z = row * spacing - 0.8
    return [round(x, 3), 0.0, round(z, 3)]


def _zone_hint(index: int, layout_items: list[str]) -> str:
    if "room_box" in layout_items:
        return "indoor_floor"
    if "terrain/substrate" in layout_items:
        return "outdoor_ground"
    return "default_zone"


def _floor_supported_name(name: str) -> bool:
    return classify_support_type(name) == "floor_supported"


def _safe_actor_map_arg(value: Any) -> dict[str, dict[str, Any]]:
    if not isinstance(value, dict):
        return {}
    return {
        str(actor_id): dict(actor)
        for actor_id, actor in value.items()
        if str(actor_id).strip() and isinstance(actor, dict)
    }


def _safe_aabb(value: Any) -> dict[str, list[float]] | None:
    if not isinstance(value, dict):
        return None
    min_v = _vector3(value.get("min"))
    max_v = _vector3(value.get("max"))
    if min_v is None or max_v is None:
        return None
    normalized_min = [min(min_v[i], max_v[i]) for i in range(3)]
    normalized_max = [max(min_v[i], max_v[i]) for i in range(3)]
    return {
        "min": [round(float(item), 3) for item in normalized_min],
        "max": [round(float(item), 3) for item in normalized_max],
    }


def _vector3(value: Any) -> list[float] | None:
    if not isinstance(value, list) or len(value) < 3:
        return None
    return [_float(value[0]), _float(value[1]), _float(value[2])]


def _aabb_center(aabb: dict[str, list[float]]) -> list[float]:
    return [round((float(aabb["min"][i]) + float(aabb["max"][i])) / 2.0, 3) for i in range(3)]


def _aabb_size(aabb: dict[str, list[float]]) -> list[float]:
    return [round(max(0.0, float(aabb["max"][i]) - float(aabb["min"][i])), 3) for i in range(3)]


def _aabb_volume(aabb: dict[str, list[float]]) -> float:
    size = _aabb_size(aabb)
    return max(0.0, float(size[0])) * max(0.0, float(size[1])) * max(0.0, float(size[2]))


def _aabb_overlap_volume(left: dict[str, list[float]], right: dict[str, list[float]]) -> float:
    overlaps = [
        min(float(left["max"][i]), float(right["max"][i])) - max(float(left["min"][i]), float(right["min"][i]))
        for i in range(3)
    ]
    if any(value <= 0 for value in overlaps):
        return 0.0
    return float(overlaps[0]) * float(overlaps[1]) * float(overlaps[2])


def _safe_import_text(value: Any, *, fallback: str) -> str:
    text = str(value or "").strip() or fallback
    lowered = text.lower()
    unsafe_tokens = (
        "api_key",
        "authorization",
        "bearer ",
        "c:\\",
        "e:\\",
        "http://",
        "https://",
        "metadata",
        "model_path",
        "prompt",
        "provider",
        "raw",
        "token",
        "url",
        "://",
    )
    if any(token in lowered for token in unsafe_tokens):
        return fallback
    return text[:96]


def _safe_position(value: Any, *, default: list[float] | None = None) -> list[float]:
    fallback = list(default or [0.0, 0.0, 0.0])
    source = value if isinstance(value, list) else fallback
    result = [_float(item) for item in source[:3]]
    while len(result) < 3:
        result.append(0.0)
    return [round(item, 3) for item in result]


def _float(value: Any) -> float:
    try:
        return float(value)
    except Exception:
        return 0.0
