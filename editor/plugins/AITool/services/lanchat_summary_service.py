from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class DiscussionState:
    """Structured silent-GM state derived from LANChat history."""

    summary: str = ""
    accepted_intents: list[str] = field(default_factory=list)
    pending_intents: list[str] = field(default_factory=list)
    conflicts: list[str] = field(default_factory=list)
    required_confirmations: list[str] = field(default_factory=list)
    next_batch_suggestions: list[str] = field(default_factory=list)

    def to_prompt_context(self) -> str:
        parts = []
        if self.summary:
            parts.append(f"摘要: {self.summary}")
        if self.pending_intents:
            parts.append("待执行: " + "；".join(self.pending_intents))
        if self.conflicts:
            parts.append("冲突: " + "；".join(self.conflicts))
        if self.required_confirmations:
            parts.append("需确认: " + "；".join(self.required_confirmations))
        return "\n".join(parts)


class LANChatSummaryService:
    """Pure Python semantic listener for C++ LANChat history.

    It owns no network state and does not write to the engine. The service is
    intentionally heuristic for v1 so it can run in the worker thread without
    blocking normal agent replies on LLM summarization.
    """

    _CONFIRM_WORDS = ("确认", "同意", "按方案", "执行", "可以")
    _REJECT_WORDS = ("拒绝", "不要", "取消", "暂停")
    _CONFLICT_WORDS = ("冲突", "同时", "覆盖", "但是", "不同意", "反对")
    _ACTION_WORDS = ("生成", "添加", "移动", "删除", "改", "放", "布置", "调整")

    def __init__(self, max_history: int = 20) -> None:
        self.max_history = max(1, int(max_history))
        self.state = DiscussionState()
        self._last_message_id = ""

    def monitor(self, history: list[dict[str, Any]] | None) -> DiscussionState:
        messages = self._normalize_history(history or [])[-self.max_history:]
        if not messages:
            return self.state

        latest_id = str(messages[-1].get("message_id") or "")
        if latest_id and latest_id == self._last_message_id:
            return self.state
        self._last_message_id = latest_id

        self.state = self._build_state(messages)
        return self.state

    def monitor_for_gm_summary(self, history: list[dict[str, Any]] | None) -> DiscussionState:
        """Build a read-only GM summary state with selected Agent design replies.

        Normal agent routing intentionally keeps using ``monitor()``, which only
        tracks user chat. This path is only for explicit @GM summary requests, so
        GM can summarize multi-Agent design discussion without changing the
        ordinary RoleAgent prompt context or execution path.
        """
        messages = self._normalize_history_for_gm(history or [])[-self.max_history:]
        return self._build_state(messages) if messages else self.state

    def _build_state(self, messages: list[dict[str, Any]]) -> DiscussionState:
        snippets = []
        pending: list[str] = []
        conflicts: list[str] = []
        confirmations: list[str] = []
        accepted: list[str] = []

        for msg in messages:
            speaker = str(msg.get("from") or msg.get("sender_name") or msg.get("sender_id") or "")
            text = str(msg.get("text") or "").strip()
            if not text:
                continue
            snippets.append(f"{speaker}: {text}" if speaker else text)
            if any(word in text for word in self._ACTION_WORDS):
                pending.append(self._compact_intent(speaker, text))
            if any(word in text for word in self._CONFLICT_WORDS):
                conflicts.append(self._compact_intent(speaker, text))
            if any(word in text for word in self._CONFIRM_WORDS):
                accepted.append(self._compact_intent(speaker, text))
            if any(word in text for word in self._REJECT_WORDS):
                confirmations.append(self._compact_intent(speaker, text))

        return DiscussionState(
            summary=" | ".join(snippets[-6:]),
            accepted_intents=self._dedupe(accepted[-4:]),
            pending_intents=self._dedupe(pending[-6:]),
            conflicts=self._dedupe(conflicts[-4:]),
            required_confirmations=self._dedupe(confirmations[-4:]),
            next_batch_suggestions=self._dedupe(pending[-3:]),
        )

    @staticmethod
    def _normalize_history(history: list[dict[str, Any]]) -> list[dict[str, Any]]:
        normalized: list[dict[str, Any]] = []
        for item in history:
            if not isinstance(item, dict):
                continue
            kind = str(item.get("message_kind") or "chat").strip().lower()
            sender_type = str(item.get("sender_type") or "user").strip().lower()
            if kind != "chat" or sender_type != "user":
                continue
            normalized.append(item)
        return normalized

    @classmethod
    def _normalize_history_for_gm(cls, history: list[dict[str, Any]]) -> list[dict[str, Any]]:
        normalized: list[dict[str, Any]] = []
        for item in history:
            if not isinstance(item, dict):
                continue
            kind = str(item.get("message_kind") or "chat").strip().lower()
            sender_type = str(item.get("sender_type") or "user").strip().lower()
            text = str(item.get("text") or "").strip()
            if not text:
                continue
            if kind == "chat" and sender_type == "user":
                normalized.append(item)
                continue
            if (
                kind in {"agent_reply", "chat"}
                and sender_type in {"agent", "ai", "assistant"}
                and cls._looks_like_agent_design_context(text)
            ):
                normalized.append(item)
        return normalized

    @staticmethod
    def _looks_like_agent_design_context(text: str) -> bool:
        value = str(text or "").strip()
        if not value:
            return False
        noise_words = (
            "已收到", "正在", "排队中", "生成进度", "进度", "SeedPlan",
            "工具服务暂时不可用", "场景里没有可编辑的物体", "模型服务",
            "导入", "资源准备", "图片生成", "模型生成",
        )
        if any(word in value for word in noise_words):
            return False
        design_words = (
            "方案", "建议", "布局", "清单", "风格", "氛围", "总结", "评价",
            "改进", "最终", "物品", "元素", "空间", "灯光", "材质", "主题",
            "场景", "核心", "区域", "动线", "入口", "颜色", "地形", "边界",
        )
        return any(word in value for word in design_words)

    @staticmethod
    def _compact_intent(speaker: str, text: str, limit: int = 48) -> str:
        body = text if len(text) <= limit else text[:limit] + "..."
        return f"{speaker}: {body}" if speaker else body

    @staticmethod
    def _dedupe(items: list[str]) -> list[str]:
        seen = set()
        out = []
        for item in items:
            if item in seen:
                continue
            seen.add(item)
            out.append(item)
        return out


__all__ = ["DiscussionState", "LANChatSummaryService"]
