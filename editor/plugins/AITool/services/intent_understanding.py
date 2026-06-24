from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Any, Callable, Literal


SceneIntent = Literal[
    "discussion",
    "status_query",
    "plan_drafting",
    "plan_revision",
    "generation_start",
    "intervention_add",
    "intervention_modify",
    "intervention_delete",
    "post_generation_add",
    "final_adjustment_request",
]


@dataclass(frozen=True)
class IntentDecision:
    intent: SceneIntent
    confidence: float
    target_agent: str | None = None
    requires_confirmation: bool = False
    risk_level: Literal["low", "medium", "high"] = "low"
    entities: list[dict[str, Any]] = field(default_factory=list)
    reason: str = ""

    @property
    def route(self) -> str:
        if self.intent == "discussion":
            if str(self.target_agent or "").strip().lower() == "gm":
                return "gm_control"
            return "agent_self" if self.target_agent else "chat"
        if self.intent in {
            "status_query",
            "plan_drafting",
            "plan_revision",
            "generation_start",
        }:
            return self.intent
        if self.intent in {
            "intervention_add",
            "intervention_modify",
            "intervention_delete",
            "post_generation_add",
            "final_adjustment_request",
        }:
            return "edit_existing"
        return "chat"

    @property
    def state_hint(self) -> str:
        if self.intent in {"plan_drafting", "plan_revision", "generation_start"}:
            return "planning"
        if self.intent in {"intervention_add", "intervention_modify", "intervention_delete"}:
            if str(self.reason or "").startswith("active generation"):
                return "active_generation"
            return "completed_scene"
        if self.intent in {"post_generation_add", "final_adjustment_request"}:
            return "completed_scene"
        if self.intent == "status_query":
            return "any"
        return "discussion"

    def as_dict(self) -> dict[str, Any]:
        return {
            "intent": self.intent,
            "route": self.route,
            "state_hint": self.state_hint,
            "confidence": self.confidence,
            "target_agent": self.target_agent,
            "requires_confirmation": self.requires_confirmation,
            "risk_level": self.risk_level,
            "entities": list(self.entities),
            "reason": self.reason,
        }


_STATUS_QUERY_PATTERNS = (
    r"(现在|当前)?(生成|执行|进度|计划|方案).*(哪里|哪步|情况|状态|是什么)",
    r"(到哪|到哪里|哪一步|卡住|为什么不执行|开始了吗)",
)
_GENERATION_START_PATTERNS = (
    r"(确认开始|直接生成|开始生成|执行生成|按.*方案.*生成|按照.*方案.*生成)",
)
_PLAN_DRAFT_PATTERNS = (
    r"(帮我)?(写|整理|给出|生成|设计).*(方案|计划)",
    r"(我想做|我希望做|帮我设计|设计一个|规划一个|搭建一个|建立一个|做一个)",
)
_PLAN_REVISION_PATTERNS = (
    r"^(补充|说明|调整|修改)[:：]?",
    r"(我希望|希望|不要|别|不能|更|风格|统一|一致|温暖|灯光|灯笼|休息区|暗黑风|恐怖)",
)
_PLAN_SUPPLEMENT_PATTERNS = (
    r"^(补充|补充要求|补充方案|说明)[:：，,\s]?",
    r"^(我希望|希望|不要|别|不能|整体|风格|布局|限制|要求)",
    r"(更|统一|一致|温暖|灯光|灯笼|休息区|暗黑风|恐怖)",
)
_PLAN_ELABORATION_PATTERNS = (
    r"(继续|展开|详细|具体|细化|列出|输出|给出).*(方案|风格|布局|清单|物品|动线|材质|灯光)",
    r"(风格方案|布局方案|物品清单|设计细节|详细方案|具体方案)",
)
_ADD_PATTERNS = (
    r"(再加|增加|新增|添加|加入|生成添加|添加生成)",
)
_MODIFY_PATTERNS = (
    r"(调整|换成|改成|变大|变小|放大|缩小|移动|移到|挪到|旋转|贴地|穿模)",
)
_DELETE_PATTERNS = (
    r"(删除|删掉|去掉|移除|不要这个)",
)
_FINAL_LAYOUT_PATTERNS = (
    r"(布局|动线|摆放|组装).*(不合理|不好|奇怪|调整|优化)",
)
_LAYOUT_CONSTRAINT_PATTERNS = (
    r"(不要挡|别挡|留空|开阔|中央活动区|活动区|入口|门口|通道|主街|动线|轴线)",
)
_DISCUSSION_PATTERNS = (
    r"^(你好|你是谁|谢谢|哈喽|hello|hi|在吗|介绍一下)",
    r"(怎么看|觉得|建议|可以吗|为什么)",
)
_OPINION_DISCUSSION_PATTERNS = (
    r"(怎么看|怎么评价|如何评价|看法|观点|觉得|认为).*(方案|设计|布局|计划)",
    r"(方案|设计|布局|计划).*(怎么看|怎么评价|如何评价|看法|观点|觉得|认为|是否合理|合不合理)",
)
_HIGH_RISK_PATTERNS = (
    r"(删除|删掉|移除|清空|重置|覆盖|全部|所有)",
)


_STATUS_QUERY_PATTERNS += (
    r"(现在|当前)?(生成|执行|进度|计划|方案).*(哪里|哪步|情况|状态|是什么)",
    r"(到哪|到哪里|哪一步|卡住|为什么不执行|开始了吗)",
)
_GENERATION_START_PATTERNS += (
    r"(确认开始|确认生成|确认执行|直接生成|开始生成|执行生成|按.*方案.*生成|按照.*方案.*生成|就按.*生成)",
)
_PLAN_DRAFT_PATTERNS += (
    r"(帮我)?(写|整理|给出|生成|设计).*(方案|计划)",
    r"(我想做|我希望做|帮我设计|设计一个|规划一个|搭建一个|建立一个|做一个|围绕.*进行讨论)",
)
_PLAN_REVISION_PATTERNS += (
    r"^(补充|说明|调整|修改)[:：]?",
    r"(在.*方案基础上|基于.*方案|我希望|希望|不要|别|不能|更|风格|统一|一致|温暖|灯光|休息区|暗黑风|恐怖)",
)
_PLAN_SUPPLEMENT_PATTERNS += (
    r"^(补充|补充要求|补充方案|说明)[:：，,\s]?",
    r"^(我希望|希望|不要|别|不能|整体|风格|布局|限制|要求)",
    r"(在.*方案基础上|基于.*方案|更|统一|一致|温暖|灯光|休息区|暗黑风|恐怖)",
)
_PLAN_ELABORATION_PATTERNS += (
    r"(整理|汇总|总结|梳理|继续|展开|详细|具体|细化|列出|输出|给出).*(方案|风格|布局|清单|物品|动线|材质|灯光|讨论)",
    r"(风格方案|布局方案|物品清单|设计细节|详细方案|具体方案)",
)
_ADD_PATTERNS += (r"(再加|增加|新增|添加|加入|生成添加|添加生成)",)
_MODIFY_PATTERNS += (r"(调整|换成|改成|变大|变小|放大|缩小|移动|移到|挪到|旋转|贴地|穿模)",)
_DELETE_PATTERNS += (r"(删除|删掉|去掉|移除|不要这个)",)
_FINAL_LAYOUT_PATTERNS += (r"(布局|动线|摆放|组装).*(不合理|不好|奇怪|调整|优化)",)
_FINAL_LAYOUT_PATTERNS += (r"(浮空|飘起来|飘起|没贴地|不贴地|悬空|离地|接地|贴地)",)
_LAYOUT_CONSTRAINT_PATTERNS += (r"(不要挡|别挡|留空|开阔|中央活动区|活动区|入口|门口|通道|主街|动线|轴线)",)
_DISCUSSION_PATTERNS += (
    r"^(你好|你是谁|谢谢|哈喽|hello|hi|在吗|介绍一下)",
    r"(怎么看|觉得|建议|可以吗|为什么)",
)
_OPINION_DISCUSSION_PATTERNS += (
    r"(怎么看|怎么评价|如何评价|看法|观点|觉得|认为).*(方案|设计|布局|计划)",
    r"(方案|设计|布局|计划).*(怎么看|怎么评价|如何评价|看法|观点|觉得|认为|是否合理|合不合理)",
)
_HIGH_RISK_PATTERNS += (r"(删除|删掉|移除|清空|重置|覆盖|全部|所有)",)


def _contains(patterns: tuple[str, ...], text: str) -> bool:
    return any(re.search(pattern, text, flags=re.IGNORECASE) for pattern in patterns)


def _extract_target_agent(text: str) -> str | None:
    match = re.search(r"@([^\s:：，,。]+)", text)
    if not match:
        return None
    return match.group(1).strip() or None


def _json_object(raw: str) -> dict[str, Any]:
    text = str(raw or "").strip()
    if "```" in text:
        start = text.find("{")
        end = text.rfind("}")
        if start != -1 and end != -1 and end > start:
            text = text[start:end + 1]
    data = json.loads(text)
    return data if isinstance(data, dict) else {}


class IntentUnderstandingService:
    """Central semantic router for scene-generation chat.

    LLM classification is advisory and optional. Protocol/safety guardrails
    remain deterministic so generation cannot be triggered by a weak keyword.
    """

    _LLM_INTENTS: set[str] = {
        "discussion",
        "status_query",
        "plan_drafting",
        "plan_revision",
        "generation_start",
        "intervention_add",
        "intervention_modify",
        "intervention_delete",
        "post_generation_add",
        "final_adjustment_request",
    }

    def __init__(self, llm_classifier: Callable[[str], dict[str, Any] | None] | None = None) -> None:
        self._llm_classifier = llm_classifier

    def classify(self, text: str, *, allow_llm: bool = True, generation_active: bool = False) -> IntentDecision:
        value = str(text or "").strip()
        target_agent = _extract_target_agent(value)
        if not value:
            return IntentDecision("discussion", 1.0, target_agent, reason="empty")

        guardrail = self._protocol_guardrail(value, target_agent, generation_active=generation_active)
        if guardrail is not None:
            return guardrail

        if allow_llm:
            llm = self._classify_via_llm(value, target_agent)
            if llm is not None:
                return self._apply_safety_guardrail(llm, value)

        return self._fallback_classify(value, target_agent, generation_active=generation_active)

    def to_compose_edit_chat(self, text: str, *, allow_llm: bool = True) -> str:
        decision = self.classify(text, allow_llm=allow_llm)
        if decision.intent in ("generation_start",):
            return "compose"
        if decision.intent in ("intervention_add", "intervention_modify", "intervention_delete", "post_generation_add"):
            return "edit"
        if decision.intent in ("plan_drafting", "plan_revision"):
            return "compose"
        return "chat"

    def scene_note_kind(self, text: str) -> str:
        if _contains(_LAYOUT_CONSTRAINT_PATTERNS, str(text or "")):
            return "layout_constraint"
        decision = self.classify(text, allow_llm=False, generation_active=True)
        if decision.intent in ("intervention_add", "post_generation_add"):
            return "generation_delta"
        if decision.intent in ("intervention_modify", "final_adjustment_request"):
            if _contains(_FINAL_LAYOUT_PATTERNS, str(text or "")):
                return "layout_constraint"
            return "edit_existing"
        if decision.intent == "intervention_delete":
            return "edit_existing"
        return "chat"

    def is_generation_start(self, text: str) -> bool:
        decision = self.classify(text, allow_llm=False)
        return decision.intent == "generation_start"

    def is_plan_supplement(self, text: str) -> bool:
        value = str(text or "").strip()
        if not value:
            return False
        decision = self.classify(value, allow_llm=False)
        return decision.intent == "plan_revision" and _contains(_PLAN_SUPPLEMENT_PATTERNS, value)

    def is_plan_elaboration_request(self, text: str) -> bool:
        value = str(text or "").strip()
        if not value:
            return False
        if self.is_generation_start(value) or self.is_plan_supplement(value):
            return False
        if _contains(_ADD_PATTERNS + _MODIFY_PATTERNS + _DELETE_PATTERNS, value):
            return False
        decision = self.classify(value, allow_llm=False)
        return decision.intent in {"discussion", "plan_revision"} and _contains(_PLAN_ELABORATION_PATTERNS, value)

    def _protocol_guardrail(
        self,
        value: str,
        target_agent: str | None,
        *,
        generation_active: bool,
    ) -> IntentDecision | None:
        normalized = re.sub(r"@\S+\s*", "", value).strip()
        if _contains(_STATUS_QUERY_PATTERNS, normalized):
            return IntentDecision("status_query", 0.98, target_agent, reason="protocol/status query")
        if _contains(_OPINION_DISCUSSION_PATTERNS, normalized):
            return IntentDecision("discussion", 0.96, target_agent, reason="protocol/design opinion discussion")
        if _contains(_GENERATION_START_PATTERNS, normalized):
            return IntentDecision(
                "generation_start",
                0.96,
                target_agent,
                requires_confirmation=True,
                risk_level="medium",
                reason="protocol/generation start",
            )
        if generation_active and _contains(_ADD_PATTERNS, normalized):
            return IntentDecision("intervention_add", 0.94, target_agent, reason="active generation add")
        if generation_active and _contains(_MODIFY_PATTERNS, normalized):
            return IntentDecision("intervention_modify", 0.92, target_agent, reason="active generation modify")
        if generation_active and _contains(_DELETE_PATTERNS, normalized):
            return IntentDecision(
                "intervention_delete",
                0.93,
                target_agent,
                requires_confirmation=True,
                risk_level="high",
                reason="active generation delete",
            )
        return None

    def _classify_via_llm(self, value: str, target_agent: str | None) -> IntentDecision | None:
        classifier = self._llm_classifier or self._default_llm_classifier
        try:
            data = classifier(value)
        except Exception:
            return None
        if not isinstance(data, dict):
            return None
        intent = str(data.get("intent") or "").strip()
        if intent not in self._LLM_INTENTS:
            return None
        try:
            confidence = float(data.get("confidence") if data.get("confidence") is not None else 0.7)
        except Exception:
            confidence = 0.7
        risk = str(data.get("risk_level") or "low").strip()
        if risk not in ("low", "medium", "high"):
            risk = "low"
        entities = data.get("entities")
        if not isinstance(entities, list):
            entities = []
        return IntentDecision(
            intent=intent,  # type: ignore[arg-type]
            confidence=max(0.0, min(1.0, confidence)),
            target_agent=target_agent or data.get("target_agent"),
            requires_confirmation=bool(data.get("requires_confirmation", False)),
            risk_level=risk,  # type: ignore[arg-type]
            entities=[item for item in entities if isinstance(item, dict)],
            reason=str(data.get("reason") or "llm"),
        )

    @staticmethod
    def _default_llm_classifier(value: str) -> dict[str, Any] | None:
        try:
            from langchain_core.messages import HumanMessage, SystemMessage
            from Quasar.ai_models.base_pool.registry import get_chat_model
        except Exception:
            return None
        system = (
            "你是 AI 场景生成链路的意图分类器。只输出 JSON 对象。"
            "intent 必须是 discussion/status_query/plan_drafting/plan_revision/"
            "generation_start/intervention_add/intervention_modify/intervention_delete/"
            "post_generation_add/final_adjustment_request 之一。"
            "生成方案讨论不要直接判为 generation_start；只有确认开始、直接生成、按方案执行才是 generation_start。"
            "状态询问不要触发生成。"
        )
        model = get_chat_model(temperature=0, request_timeout=16.0)
        resp = model.invoke([
            SystemMessage(content=system),
            HumanMessage(content=value[:800]),
        ])
        return _json_object(resp.content if hasattr(resp, "content") else str(resp))

    def _fallback_classify(
        self,
        value: str,
        target_agent: str | None,
        *,
        generation_active: bool,
    ) -> IntentDecision:
        normalized = re.sub(r"@\S+\s*", "", value).strip()
        if _contains(_OPINION_DISCUSSION_PATTERNS, normalized):
            return IntentDecision("discussion", 0.9, target_agent, reason="fallback design opinion discussion")
        if _contains(_DISCUSSION_PATTERNS, normalized) and not _contains(_PLAN_DRAFT_PATTERNS, normalized):
            return IntentDecision("discussion", 0.88, target_agent, reason="fallback discussion")
        if _contains(_FINAL_LAYOUT_PATTERNS, normalized):
            return IntentDecision("final_adjustment_request", 0.9, target_agent, reason="fallback layout")
        if _contains(_PLAN_DRAFT_PATTERNS, normalized):
            return IntentDecision("plan_drafting", 0.82, target_agent, reason="fallback plan")
        if _contains(_PLAN_REVISION_PATTERNS, normalized):
            return IntentDecision("plan_revision", 0.88, target_agent, reason="fallback plan revision")
        if _contains(_DELETE_PATTERNS, normalized):
            return IntentDecision(
                "intervention_delete",
                0.87,
                target_agent,
                requires_confirmation=True,
                risk_level="high",
                reason="fallback delete",
            )
        if _contains(_ADD_PATTERNS, normalized):
            return IntentDecision(
                "intervention_add" if generation_active else "post_generation_add",
                0.86,
                target_agent,
                reason="fallback add",
            )
        if _contains(_MODIFY_PATTERNS, normalized):
            return IntentDecision("intervention_modify", 0.84, target_agent, reason="fallback modify")
        return IntentDecision("discussion", 0.7, target_agent, reason="fallback default")

    def _apply_safety_guardrail(self, decision: IntentDecision, value: str) -> IntentDecision:
        if decision.intent == "generation_start" and _contains(_STATUS_QUERY_PATTERNS, value):
            return IntentDecision("status_query", 0.98, decision.target_agent, reason="guardrail status overrides llm")
        if decision.intent == "generation_start" and not _contains(_GENERATION_START_PATTERNS, value):
            return IntentDecision("plan_drafting", 0.78, decision.target_agent, reason="guardrail weak generation start")
        if _contains(_HIGH_RISK_PATTERNS, value) and decision.risk_level != "high":
            return IntentDecision(
                decision.intent,
                decision.confidence,
                decision.target_agent,
                requires_confirmation=True,
                risk_level="high",
                entities=decision.entities,
                reason=f"{decision.reason}; high-risk guardrail",
            )
        return decision


_SERVICE = IntentUnderstandingService()


def get_intent_understanding_service() -> IntentUnderstandingService:
    return _SERVICE


__all__ = [
    "IntentDecision",
    "IntentUnderstandingService",
    "get_intent_understanding_service",
]
