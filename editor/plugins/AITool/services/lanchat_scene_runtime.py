from __future__ import annotations

import re
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any

from .intent_understanding import get_intent_understanding_service


_DISCLOSURE_CANDIDATE_TERMS = (
    "草原", "天空", "森林", "树林", "地形", "地面", "地板", "墙面", "天花板",
    "床", "柜", "桌", "椅", "灯", "灯笼", "台灯", "雕像", "玩偶", "摊位",
    "导视牌", "展示架", "绿植", "植物", "地毯", "沙发", "小狗", "狗", "猫",
    "木门", "石门", "藏宝箱", "宝箱", "金币堆", "金币", "珠宝", "木箱", "酒桶", "武器架", "火把", "烛台",
    "入口", "通道", "主街", "边界", "休息区",
)
_ABSTRACT_MODEL_TERMS = {
    "入口/边界",
    "主活动区",
    "视觉焦点",
    "功能支撑点",
    "通行动线",
    "停留点",
    "重点照明",
    "材质/色彩点缀",
    "主路",
    "路径",
    "安全边界",
}
_UI_INSTRUCTION_PLACEHOLDER_PATTERNS = (
    r"^补充要求[:：]?\s*(写明|写清|描述|填写).*(风格|物件|布局|限制)",
    r"^确认按当前方案生成",
    r"^描述你想设计什么$",
    r"^写清要改的风格、物件、布局或限制$",
)

MODE_DISCUSSING = "DISCUSSING"
MODE_PLANNING = "PLANNING"
MODE_EXECUTING = "EXECUTING"
MODE_PAUSED = "PAUSED"
_VALID_MODES = {MODE_DISCUSSING, MODE_PLANNING, MODE_EXECUTING, MODE_PAUSED}
_PAUSE_MODES = {MODE_DISCUSSING, MODE_PAUSED}


@dataclass
class PlanningConfirmation:
    proposal_id: str
    target_agent: str
    scene_goal: str
    proposed_items: list[str] = field(default_factory=list)
    constraints: list[str] = field(default_factory=list)
    source_context_agents: list[str] = field(default_factory=list)
    status: str = "pending"
    created_at: float = field(default_factory=time.time)


@dataclass
class PendingSceneNote:
    text: str
    kind: str
    source_agent: str
    source_user_id: str = ""
    created_at: float = field(default_factory=time.time)


@dataclass(frozen=True)
class ScenePlanProfile:
    scene_type: str
    style_direction: str
    layout_direction: str
    core_items: list[str]
    decor_items: list[str]


class LanChatSceneRuntime:
    """Small Python-side state for single-user progressive intervention.

    This is deliberately not a chat/network state source. C++ LANChat still owns
    transport/history. The runtime only lets long compose jobs expose a minimal
    side-channel for confirmation and pending scene notes.
    """

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._mode: str = MODE_DISCUSSING
        self._pending_confirmations: dict[str, PlanningConfirmation] = {}
        self._active_agent: str = ""
        self._active_goal: str = ""
        self._active_since: float = 0.0
        self._pending_notes: list[PendingSceneNote] = []

    @staticmethod
    def _agent_key(agent_name: str) -> str:
        return str(agent_name or "agent").strip() or "agent"

    @staticmethod
    def is_direct_generate(text: str) -> bool:
        return get_intent_understanding_service().is_generation_start(text)

    @staticmethod
    def classify_scene_note(text: str) -> str:
        return get_intent_understanding_service().scene_note_kind(text)

    @staticmethod
    def is_plan_supplement(text: str) -> bool:
        return get_intent_understanding_service().is_plan_supplement(text)

    @staticmethod
    def is_plan_elaboration_request(text: str) -> bool:
        return get_intent_understanding_service().is_plan_elaboration_request(text)

    def set_mode(self, mode: str) -> str:
        normalized = str(mode or "").strip().upper()
        if normalized not in _VALID_MODES:
            normalized = MODE_DISCUSSING
        with self._lock:
            self._mode = normalized
        return normalized

    def mode(self) -> str:
        with self._lock:
            return self._mode

    def should_pause_batches(self) -> bool:
        with self._lock:
            return self._mode in _PAUSE_MODES

    def handle_planning_gate(self, agent_name: str, text: str) -> tuple[str, str | None]:
        """Return (action, payload).

        action:
          - reply: payload is user-visible confirmation/update text
          - compose: payload is enriched compose text
          - pass: caller should continue normal routing
        """
        key = self._agent_key(agent_name)
        value = str(text or "").strip()
        if not value:
            return "pass", None
        decision = get_intent_understanding_service().classify(value, allow_llm=False)
        if decision.intent == "discussion" or self._is_ui_instruction_placeholder(value):
            return "pass", None

        with self._lock:
            pending = self._pending_confirmations.get(key)
            if pending and decision.intent == "generation_start":
                compose_text = self._compose_text_from_confirmation(pending, value)
                pending.status = "confirmed"
                self._pending_confirmations.pop(key, None)
                self._mode = MODE_EXECUTING
                return "compose", compose_text

            if pending and self._is_pending_plan_elaboration_text(value):
                return "reply", self._format_plan_elaboration(pending)

            if pending and self.is_plan_supplement(value):
                pending.constraints.append(value)
                for item in self._extract_requested_items(value):
                    if item not in pending.proposed_items:
                        pending.proposed_items.append(item)
                return "reply", self._format_confirmation(pending, updated=True)

            if decision.intent == "plan_drafting":
                confirmation = self._new_confirmation(key, value)
                self._pending_confirmations[key] = confirmation
                self._mode = MODE_PLANNING
                return "reply", self._format_confirmation(confirmation)

        return "pass", None

    def handle_pending_planning_message(self, text: str) -> tuple[str, str | None, str | None]:
        """Route an unmentioned room message to the only pending planning gate."""
        value = str(text or "").strip()
        if not value:
            return "pass", None, None
        with self._lock:
            pending_keys = [
                key
                for key, pending in self._pending_confirmations.items()
                if pending.status == "pending"
            ]
        if not pending_keys:
            return "pass", None, None
        decision = get_intent_understanding_service().classify(value, allow_llm=False)
        if len(pending_keys) != 1:
            if (
                decision.intent == "generation_start"
                or self.is_plan_supplement(value)
                or self.is_plan_elaboration_request(value)
            ):
                return "reply", self._format_pending_disambiguation(pending_keys), "系统"
            return "pass", None, None
        agent_key = pending_keys[0]
        action, payload = self.handle_planning_gate(agent_key, value)
        if action == "pass":
            return "pass", None, None
        return action, payload, agent_key

    def handle_targeted_planning_message(
        self,
        target: str,
        text: str,
        *,
        draft_action: str = "",
        source_context_agent: str = "",
    ) -> tuple[str, str | None, str | None]:
        """Route a structured UI action to a pending planning gate.

        `target` can be a target agent name/id or a pending proposal id. This is
        used by the LANChat UI metadata path so the user does not have to type
        @Agent or magic confirmation words.
        """
        value = str(text or "").strip()
        if not value:
            return "pass", None, None
        action = str(draft_action or "").strip().lower()
        with self._lock:
            agent_key = self._pending_agent_for_target(target)
            if not agent_key:
                agent_key = self._agent_key(target)
                source = self._pending_agent_for_target(source_context_agent)
                if action in {"plan", "supplement"} and source:
                    base = self._pending_confirmations[source]
                    confirmation = self._clone_confirmation_for_target(
                        base,
                        agent_key,
                        value,
                        source_context_agent=source,
                    )
                    self._pending_confirmations[agent_key] = confirmation
                    self._mode = MODE_PLANNING
                    return "reply", self._format_confirmation(confirmation, updated=True), agent_key
                if action == "plan":
                    confirmation = self._new_confirmation(agent_key, value)
                    self._pending_confirmations[agent_key] = confirmation
                    self._mode = MODE_PLANNING
                    return "reply", self._format_confirmation(confirmation), agent_key
                if action == "supplement":
                    return "pass", None, None
        routed_text = value
        if action == "generate" and not self.is_direct_generate(routed_text):
            routed_text = f"确认开始：{routed_text}"
        elif action == "supplement" and not self.is_plan_supplement(routed_text):
            routed_text = f"补充要求：{routed_text}"
        result, payload = self.handle_planning_gate(agent_key, routed_text)
        if result == "pass":
            return "pass", None, None
        return result, payload, agent_key

    def pending_planning_snapshot(self) -> list[dict[str, Any]]:
        with self._lock:
            pending = [
                confirmation
                for confirmation in self._pending_confirmations.values()
                if confirmation.status == "pending"
            ]
            pending.sort(key=lambda item: item.created_at)
            return [
                {
                    "proposal_id": item.proposal_id,
                    "target_agent": item.target_agent,
                    "scene_goal": item.scene_goal,
                    "proposed_items": list(item.proposed_items),
                    "constraints": list(item.constraints),
                    "source_context_agents": list(item.source_context_agents),
                    "status": item.status,
                    "created_at": item.created_at,
                }
                for item in pending
            ]

    def _pending_agent_for_target(self, target: str) -> str:
        wanted = str(target or "").strip()
        if not wanted:
            return ""
        wanted_lower = wanted.lower()
        for key, pending in self._pending_confirmations.items():
            if pending.status != "pending":
                continue
            if (
                key == wanted
                or key.lower() == wanted_lower
                or pending.target_agent == wanted
                or pending.target_agent.lower() == wanted_lower
                or pending.proposal_id == wanted
            ):
                return key
        return ""

    @staticmethod
    def _format_pending_disambiguation(pending_keys: list[str]) -> str:
        targets = "、".join(str(key) for key in pending_keys if str(key).strip())
        return (
            "请先 @ 指定要更新哪个方案。"
            f"当前有多个待确认方案：{targets}。"
            "例如：@小女孩 补充要求：减少细碎装饰，保留核心家具。"
        )

    def clear_pending_planning(self, agent_name: str | None = None) -> None:
        with self._lock:
            if agent_name:
                self._pending_confirmations.pop(self._agent_key(agent_name), None)
            else:
                self._pending_confirmations.clear()

    def start_compose(self, agent_name: str, goal: str) -> None:
        with self._lock:
            self._mode = MODE_EXECUTING
            self._active_agent = self._agent_key(agent_name)
            self._active_goal = str(goal or "")[:300]
            self._active_since = time.time()
            self._pending_notes.clear()

    def end_compose(self, agent_name: str | None = None) -> None:
        with self._lock:
            if agent_name and self._active_agent and self._agent_key(agent_name) != self._active_agent:
                return
            self._active_agent = ""
            self._active_goal = ""
            self._active_since = 0.0
            self._pending_notes.clear()
            self._mode = MODE_DISCUSSING

    def active_snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "active": bool(self._active_agent),
                "mode": self._mode,
                "active_agent": self._active_agent,
                "active_goal": self._active_goal,
                "active_since": self._active_since,
                "pending_count": len(self._pending_notes),
            }

    def record_busy_message(
        self,
        *,
        agent_name: str,
        text: str,
        source_user_id: str = "",
    ) -> str | None:
        value = str(text or "").strip()
        if not value:
            return None
        with self._lock:
            if not self._active_agent:
                return None
            kind = self.classify_scene_note(value)
            if kind != "chat":
                self._pending_notes.append(PendingSceneNote(
                    text=value,
                    kind=kind,
                    source_agent=self._agent_key(agent_name),
                    source_user_id=source_user_id,
                ))
                if kind == "edit_existing":
                    return "已收到这条编辑请求；如果物体已经出现，我会在下一批前尝试应用，未出现则先挂起。"
                if kind == "layout_constraint":
                    return f"已记录布局要求：{value}。后续摆放会在下一批前吸收。"
                requested = self._extract_requested_items(value)
                if requested:
                    return f"已记录后续补充：{'、'.join(requested[:4])}。我会优先尝试加入后续批次；若当前没有可用模型，会在最终报告里标为待补。"
                return "已记录后续生成补充。我会优先尝试加入后续批次；若当前没有可用模型，会在最终报告里标为待补。"
        return None

    def consume_notes_for_prompt(self) -> str:
        with self._lock:
            notes = list(self._pending_notes)
            self._pending_notes.clear()
        if not notes:
            return ""
        grouped: dict[str, list[str]] = {"generation_delta": [], "layout_constraint": [], "edit_existing": []}
        for note in notes:
            grouped.setdefault(note.kind, []).append(note.text)
        lines = ["## 生成中用户介入（下一批前吸收）"]
        if grouped.get("generation_delta"):
            lines.append("后续生成补充：" + "；".join(grouped["generation_delta"]))
        if grouped.get("layout_constraint"):
            lines.append("后续布局约束：" + "；".join(grouped["layout_constraint"]))
        if grouped.get("edit_existing"):
            lines.append("已有物体编辑请求：" + "；".join(grouped["edit_existing"]))
        return "\n".join(lines)

    def consume_notes(self) -> list[PendingSceneNote]:
        with self._lock:
            notes = list(self._pending_notes)
            self._pending_notes.clear()
        return notes

    def _compose_text_from_confirmation(self, confirmation: PlanningConfirmation, user_text: str) -> str:
        brief = self._design_brief_lines(confirmation)
        parts = [
            f"用户确认开始生成：{confirmation.scene_goal}",
            "确认方案内容：",
            *brief,
            "建议物体清单：" + "、".join(confirmation.proposed_items),
        ]
        if confirmation.constraints:
            parts.append("补充要求：" + "；".join(confirmation.constraints))
        if confirmation.source_context_agents:
            parts.append("参考来源 Agent：" + "、".join(confirmation.source_context_agents))
        parts.append("最新指令：" + user_text)
        return "\n".join(parts)

    def _format_confirmation(self, confirmation: PlanningConfirmation, *, updated: bool = False) -> str:
        prefix = f"我已更新方案：{confirmation.scene_goal}。" if updated else f"我理解你的目标是：{confirmation.scene_goal}。"
        brief = self._design_brief_lines(confirmation)
        lines = [
            self._role_opening_line(confirmation.target_agent, updated=updated),
            "",
            prefix,
            "",
            "方案内容：",
            *brief,
        ]
        if confirmation.constraints:
            lines.extend([
                "",
                "已纳入补充要求：" + "；".join(confirmation.constraints),
            ])
        if confirmation.source_context_agents:
            lines.extend([
                "",
                "参考来源：" + "、".join(confirmation.source_context_agents),
            ])
        lines.extend([
            "",
            "建议先做：",
            "1. 锁定空间边界和主要动线",
            "2. 先生成核心物件：" + "、".join(self._core_items(confirmation)[:4]),
            "3. 再补足氛围装饰：" + "、".join(self._decor_items(confirmation)[:4]),
            "4. 最后检查比例、留空和遮挡关系",
            "",
            "你可以回复：",
            "- 确认开始：按当前方案进入 3D 场景生成。",
            "- 补充要求：写明要改的风格、物件、布局或限制；我会先更新方案，不会立刻生成。",
            "- 例如：补充要求：床边加一个小书架，整体更粉一点。",
            "- 直接生成：跳过继续讨论，立即按当前方案开始生成。",
        ])
        disclosure = self._classification_disclosure(confirmation.scene_goal, confirmation.proposed_items)
        if disclosure:
            lines.extend(["", "提炼结果：", disclosure])
        return "\n".join(lines)

    def _format_plan_elaboration(self, confirmation: PlanningConfirmation) -> str:
        profile = self._build_scene_plan_profile(confirmation.scene_goal)
        core = self._core_items(confirmation, profile)
        decor = self._decor_items(confirmation, profile)
        agent_name = str(confirmation.target_agent or "").strip() or "设计助手"
        lines = [
            f"{agent_name}继续把当前方案展开，先把可落地的细节说清楚。",
            "",
            f"方案展开：{confirmation.scene_goal}",
            "",
            "风格方案：",
            f"- {profile.style_direction}",
            "- 色彩、材质和灯光先保持同一套语言，避免局部装饰抢走主要空间。",
            "",
            "布局：",
            f"- {profile.layout_direction}",
            "- 入口、主通道、停留点和视觉焦点分开处理，核心家具先定位置，再补装饰。",
            "",
            "物品清单：",
            "- 核心物件：" + "、".join(core[:6]),
            "- 氛围装饰：" + "、".join(decor[:6]),
        ]
        if confirmation.constraints:
            lines.extend([
                "",
                "已纳入要求：" + "；".join(confirmation.constraints),
            ])
        if confirmation.source_context_agents:
            lines.extend([
                "",
                "参考来源：" + "、".join(confirmation.source_context_agents),
            ])
        lines.extend([
            "",
            "当前仍处于方案确认阶段，还没有进入生成队列。你可以继续补充要求，或回复“确认开始”进入生成。",
        ])
        return "\n".join(lines)

    @staticmethod
    def _role_opening_line(agent_name: str, *, updated: bool = False) -> str:
        name = str(agent_name or "").strip()
        if "长者" in name:
            return "长者先给你一个稳妥版方案，重点放在结构、动线和可执行性上。" if not updated else "长者已把补充要求并入方案，我会继续优先保持结构稳妥。"
        if "小女孩" in name:
            return "小女孩先帮你整理一个温柔、好落地的版本。" if not updated else "小女孩已把你的新想法加进去，会尽量保持整体温柔协调。"
        if "商人" in name:
            return "商人先按实用和展示效果给你整理方案。" if not updated else "商人已把补充点纳入方案，会继续兼顾实用和展示效果。"
        return f"{name or '设计助手'}先给你整理一个可执行方案。" if not updated else f"{name or '设计助手'}已把补充要求纳入方案。"

    def _design_brief_lines(self, confirmation: PlanningConfirmation) -> list[str]:
        profile = self._build_scene_plan_profile(confirmation.scene_goal)
        core = self._core_items(confirmation, profile)
        decor = self._decor_items(confirmation, profile)
        return [
            f"1. 风格定位：{profile.style_direction}",
            f"2. 空间布局：{profile.layout_direction}",
            "3. 核心物件：" + "、".join(core[:5]),
            "4. 氛围装饰：" + "、".join(decor[:5]),
        ]

    @classmethod
    def _build_scene_plan_profile(cls, goal: str) -> ScenePlanProfile:
        value = str(goal or "")
        scene_type, layout, core = cls._scene_type_parts(value)
        style = cls._style_direction(value)
        decor = cls._decor_items_for_style(value, scene_type)
        return ScenePlanProfile(
            scene_type=scene_type,
            style_direction=style,
            layout_direction=layout,
            core_items=core,
            decor_items=decor,
        )

    @staticmethod
    def _style_direction(goal: str) -> str:
        value = str(goal or "")
        directions: list[str] = []
        style_rules = (
            (("现代", "现代风", "简约"), "现代简约：中性色/低饱和配色，线条干净，材质对比清晰"),
            (("科技", "未来", "数字", "赛博"), "科技感：冷暖光对比、金属/玻璃材质、清晰的信息层级"),
            (("可爱", "少女", "小女孩", "童趣", "亲子"), "亲和明亮：柔和色彩、圆润比例、低压迫感装饰"),
            (("温暖", "暖", "治愈", "舒适"), "温暖治愈：暖色灯光、柔软材质、适合停留的细节"),
            (("暗黑", "神秘", "奇幻"), "奇幻神秘：深色基调、重点光源、带故事感的符号"),
            (("自然", "森林", "草原", "户外", "室外"), "自然户外：地形和植物形成层次，材质保持朴素真实"),
            (("商业", "品牌", "店", "咖啡", "展厅"), "商业展示：品牌色统一，入口识别清楚，重点展品/服务区突出"),
            (("复古", "怀旧", "古典"), "复古质感：木质/织物/旧金属元素，色彩克制而有年代感"),
        )
        for keywords, label in style_rules:
            if any(word in value for word in keywords):
                directions.append(label)
        if not directions:
            directions.append("围绕目标主题统一色彩、材质和装饰语言，先保证整体识别度和可执行性")
        return "；".join(directions[:3])

    @staticmethod
    def _scene_type_parts(goal: str) -> tuple[str, str, list[str]]:
        value = str(goal or "")
        scene_rules: tuple[tuple[tuple[str, ...], str, str, list[str]], ...] = (
            (
                ("藏宝室", "宝库", "密室", "宝藏室", "地下宝库", "treasure room", "vault", "chamber"),
                "treasure_room",
                "以窄入口进入，中央宝藏区作为视觉焦点，一侧安排分赃/休息区，后墙保留暗门或首领位",
                ["木门/石门", "藏宝箱", "金币堆", "珠宝堆", "木桌", "木椅", "木箱", "酒桶"],
            ),
            (
                ("卧室", "睡眠", "房间"),
                "residential_bedroom",
                "以床区为视觉中心，保留入口到床边的通行动线，侧边安排收纳和学习/梳妆角",
                ["床", "床头柜", "衣柜/收纳柜", "书桌或梳妆台", "地毯", "床尾留白"],
            ),
            (
                ("客厅", "会客", "起居"),
                "residential_living",
                "以交流区为中心，视觉焦点和收纳区分列两侧，主通道保持连续",
                ["沙发/座椅组", "茶几", "电视/媒体墙", "边柜/收纳柜", "地毯", "通行动线"],
            ),
            (
                ("展厅", "展览", "展馆", "展示", "陈列"),
                "exhibition",
                "用入口导视引入参观路线，核心展台和展墙形成主视觉，留出回看和停留空间",
                ["入口导视", "核心展台", "展墙/展板", "互动屏", "参观动线", "停留拍照点"],
            ),
            (
                ("咖啡", "餐厅", "商业", "店铺", "门店", "酒吧", "零售"),
                "commercial",
                "入口先建立品牌识别，服务台和座位区分区明确，排队/停留/离开路线不互相打架",
                ["门头/招牌", "吧台/服务台", "收银点", "座位区", "展示柜/货架", "等候区"],
            ),
            (
                ("集市", "摊", "街", "市集"),
                "market",
                "以主路为中轴，两侧布置摊位和停留点，入口保持开阔，避免摊位阻断视线",
                ["入口牌", "主路", "摊位组", "展示桌", "休息点", "导视牌"],
            ),
            (
                ("户外", "室外", "公园", "亲子", "活动区", "草原", "森林", "露营"),
                "outdoor_activity",
                "用开阔地形承载主活动区，边缘安排自然元素、安全边界和休息看护点",
                ["入口集合点", "主活动场地", "互动设施", "休息看护区", "安全边界", "路径"],
            ),
            (
                ("游戏", "关卡", "营地", "副本", "战斗"),
                "gameplay",
                "先定义玩家起点、目标点和主要路径，再用遮挡、奖励点和危险区形成节奏",
                ["出生点", "目标点", "主路径", "掩体/障碍", "奖励点", "边界提示"],
            ),
        )
        for keywords, scene_type, layout, core in scene_rules:
            if any(word in value for word in keywords):
                return scene_type, layout, core
        return (
            "generic_scene",
            "先确定入口、主活动区和视觉焦点，再围绕功能区、通行动线和留白组织内容",
            ["入口/边界", "主活动区", "视觉焦点", "功能支撑点", "通行动线", "停留点"],
        )

    @staticmethod
    def _decor_items_for_style(goal: str, scene_type: str) -> list[str]:
        value = str(goal or "")
        decor: list[str] = []
        if any(word in value for word in ("科技", "未来", "数字", "赛博")):
            decor.extend(["线性灯带", "信息屏", "金属材质点缀", "冷暖重点光"])
        if any(word in value for word in ("可爱", "童趣", "亲子", "少女")):
            decor.extend(["圆润软装", "低饱和亮色点缀", "互动小物", "安全缓冲边角"])
        if any(word in value for word in ("温暖", "治愈", "舒适", "暖")):
            decor.extend(["暖色重点灯", "织物软装", "木质细节", "停留氛围灯"])
        if any(word in value for word in ("自然", "森林", "草原", "户外", "室外")):
            decor.extend(["植物层次", "自然材质", "地面纹理", "环境边界"])
        if any(word in value for word in ("暗黑", "神秘", "奇幻")):
            decor.extend(["重点烛光/灯笼", "符号旗帜", "雾气氛围", "故事道具"])
        if any(word in value for word in ("现代", "简约", "现代风")):
            decor.extend(["装饰画", "绿植", "几何灯具", "低饱和色彩点缀"])
        scene_decor = {
            "treasure_room": ["火把/烛台", "武器架", "旧地图", "麻袋/货物", "暗门提示"],
            "exhibition": ["导视系统", "展品标签", "重点照明", "拍照点"],
            "commercial": ["品牌标识", "菜单/价签", "橱窗展示", "氛围照明"],
            "market": ["摊位招牌", "挂灯", "商品陈列", "休息标识"],
            "outdoor_activity": ["指示牌", "遮阳/休息设施", "安全提示", "自然边界"],
            "gameplay": ["路线提示", "目标标识", "危险提示", "奖励视觉点"],
        }
        decor.extend(scene_decor.get(scene_type, ["重点照明", "材质/色彩点缀", "标识/导视", "绿植/软装"]))
        return LanChatSceneRuntime._merge_unique(decor, limit=8)

    @staticmethod
    def _core_items(confirmation: PlanningConfirmation, profile: ScenePlanProfile | None = None) -> list[str]:
        items = LanChatSceneRuntime._concrete_items(confirmation.proposed_items or [])
        profile = profile or LanChatSceneRuntime._build_scene_plan_profile(confirmation.scene_goal)
        return LanChatSceneRuntime._merge_unique(profile.core_items, items, limit=8)

    @staticmethod
    def _decor_items(confirmation: PlanningConfirmation, profile: ScenePlanProfile | None = None) -> list[str]:
        items = LanChatSceneRuntime._concrete_items(confirmation.proposed_items or [])
        profile = profile or LanChatSceneRuntime._build_scene_plan_profile(confirmation.scene_goal)
        return LanChatSceneRuntime._merge_unique(profile.decor_items, items, limit=8)

    @staticmethod
    def _concrete_items(items: list[str]) -> list[str]:
        return [
            str(item or "").strip()
            for item in items
            if str(item or "").strip() and str(item or "").strip() not in _ABSTRACT_MODEL_TERMS
        ]

    @staticmethod
    def _merge_unique(*groups: list[str], limit: int = 8) -> list[str]:
        merged: list[str] = []
        for group in groups:
            for item in group:
                value = str(item or "").strip()
                if value and value not in merged:
                    merged.append(value)
                if len(merged) >= limit:
                    return merged
        return merged

    @staticmethod
    def _extract_scene_goal(text: str) -> str:
        value = re.sub(r"@\S+\s*", "", str(text or "")).strip()
        value = re.sub(r"^.*?(我有一个计划[，,]?)", "", value).strip()
        value = re.sub(r"^(?:请|麻烦你|帮我|给我|我想要|我想做|我们来)?\s*", "", value).strip()
        value = re.sub(r"^(?:建立|搭建|设计|规划|做|布置)(?:一个|一下|一间)?", "", value).strip()
        return value or str(text or "").strip() or "新的开放场景"

    @staticmethod
    def _extract_requested_items(text: str) -> list[str]:
        value = re.sub(r"@\S+\s*", "", str(text or "")).strip()
        value = re.sub(
            r"^(?:后面|后续|接下来|之后)?\s*(?:再)?(?:补充|增加|新增|添加|加入|加|再加)\s*[:：]?",
            "",
            value,
        ).strip()
        chunks = re.split(r"[、，,和以及;；\s]+", value)
        out: list[str] = []
        for chunk in chunks:
            item = LanChatSceneRuntime._normalize_requested_item(chunk)
            if 1 < len(item) <= 12 and not any(word in item for word in ("补充", "增加", "新增", "添加", "加入", "再加", "需要", "想要")):
                out.append(item)
        return out[:6]

    @staticmethod
    def _normalize_requested_item(value: str) -> str:
        item = str(value or "").strip(" “‘”\"'，。；;,.")
        item = re.sub(r"^(?:后面|后续|接下来|之后|再|新增|增加|添加|加入|加|补|帮我|给我|设计|规划|做|布置|一个|一间|一只|一座|一盏|一张|一把)+", "", item).strip()
        item = re.sub(r"^(?:个|只|座|盏|张|把)", "", item).strip()
        item = re.sub(r"(?:要|得|需要|应该|放在|摆在).*$", "", item).strip()
        return item

    @classmethod
    def _seed_items_from_text(cls, text: str) -> list[str]:
        items = cls._extract_requested_items(text)
        for item in cls._candidate_items_from_text(text):
            if item not in items:
                items.append(item)
        profile = cls._build_scene_plan_profile(cls._extract_scene_goal(text))
        for item in cls._merge_unique(profile.core_items, profile.decor_items, limit=8):
            if len(items) >= 8:
                break
            if item not in items and item not in _ABSTRACT_MODEL_TERMS:
                items.append(item)
        return cls._concrete_items(items)

    @staticmethod
    def _is_pending_plan_elaboration_text(text: str) -> bool:
        value = str(text or "").strip()
        if not value:
            return False
        if LanChatSceneRuntime.is_plan_elaboration_request(value):
            return True
        return bool(re.search(r"(整理|汇总|总结|梳理).{0,12}(方案|计划|讨论)", value))

    @classmethod
    def _new_confirmation(cls, agent_key: str, text: str) -> PlanningConfirmation:
        return PlanningConfirmation(
            proposal_id=f"plan-{uuid.uuid4().hex[:8]}",
            target_agent=agent_key,
            scene_goal=cls._extract_scene_goal(text),
            proposed_items=cls._seed_items_from_text(text),
            constraints=[],
        )

    @staticmethod
    def _clone_confirmation_for_target(
        base: PlanningConfirmation,
        target_agent: str,
        constraint: str,
        *,
        source_context_agent: str = "",
    ) -> PlanningConfirmation:
        source = str(source_context_agent or base.target_agent or "").strip()
        sources = list(base.source_context_agents)
        if source and source not in sources:
            sources.append(source)
        constraints = list(base.constraints)
        value = str(constraint or "").strip()
        if value:
            constraints.append(value)
        return PlanningConfirmation(
            proposal_id=f"plan-{uuid.uuid4().hex[:8]}",
            target_agent=target_agent,
            scene_goal=base.scene_goal,
            proposed_items=LanChatSceneRuntime._concrete_items(list(base.proposed_items)),
            constraints=constraints,
            source_context_agents=sources,
        )

    @staticmethod
    def _is_ui_instruction_placeholder(text: str) -> bool:
        value = str(text or "").strip()
        if not value:
            return False
        return any(re.search(pattern, value) for pattern in _UI_INSTRUCTION_PLACEHOLDER_PATTERNS)

    @staticmethod
    def _candidate_items_from_text(text: str) -> list[str]:
        value = str(text or "")
        out: list[str] = []
        for term in _DISCLOSURE_CANDIDATE_TERMS:
            if term in value and term not in out:
                out.append(term)
        return out[:8]

    @staticmethod
    def _classification_disclosure(scene_goal: str, proposed_items: list[str]) -> str:
        try:
            from plugins.AITool.cai_extensions.agent.scene_element_classifier import (
                route_model_items,
                summarize_classification,
            )
        except Exception:
            return LanChatSceneRuntime._fallback_classification_disclosure(proposed_items)
        rows = [{"name": item} for item in LanChatSceneRuntime._concrete_items(proposed_items) if str(item or "").strip()]
        if not rows:
            return ""
        _, routes = route_model_items(scene_goal, rows)
        return summarize_classification(routes)

    @staticmethod
    def _fallback_classification_disclosure(proposed_items: list[str]) -> str:
        substrate_terms = (
            "草原", "天空", "森林", "树林", "地形", "地面", "地板", "墙面", "天花板",
            "夜空", "云", "河流", "湖面", "海面", "雪地", "沙地", "庭院",
        )
        layout_terms = (
            "入口", "通道", "主街", "边界", "区域", "休息区", "动线", "主路",
        )
        model_names: list[str] = []
        substrate_names: list[str] = []
        layout_names: list[str] = []
        for name in LanChatSceneRuntime._concrete_items(proposed_items):
            clean = str(name or "").strip()
            if not clean:
                continue
            if any(term == clean or term in clean for term in substrate_terms):
                substrate_names.append(clean)
            elif any(term == clean or term in clean for term in layout_terms):
                layout_names.append(clean)
            else:
                model_names.append(clean)
        parts: list[str] = []
        if model_names:
            parts.append("准备生成模型：" + "、".join(model_names))
        if substrate_names:
            parts.append("环境/地形：" + "、".join(substrate_names) + " 将作为场景基底处理，不单独生成模型")
        if layout_names:
            parts.append("布局结构：" + "、".join(layout_names) + " 会进入摆放/结构规划")
        return "；".join(parts)


_RUNTIME = LanChatSceneRuntime()


def get_lanchat_scene_runtime() -> LanChatSceneRuntime:
    return _RUNTIME
