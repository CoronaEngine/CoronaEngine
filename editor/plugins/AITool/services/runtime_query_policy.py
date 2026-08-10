"""Stateless recognition policy for AgentRuntime chat commands and queries.

This module deliberately contains no worker state or reply construction.  The
LANChat worker owns orchestration and keeps compatibility forwarding methods;
this module owns the vocabulary and normalization rules.
"""

from __future__ import annotations

from typing import Any


def runtime_command_from_text(text: str) -> str:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return ""
    if any(word in normalized for word in ("取消生成", "取消任务", "停止生成", "终止生成", "不要生成", "cancel generation", "cancel task")):
        return "cancel"
    if any(word in normalized for word in ("暂停生成", "暂停一下", "先暂停", "暂停任务", "pause generation", "pause task")):
        return "pause"
    if any(word in normalized for word in ("继续生成", "恢复生成", "继续执行", "恢复执行", "resume generation", "resume task")):
        return "resume"
    return ""


def is_runtime_worker_drain_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    runtime_markers = ("runtime", "agentruntime", "agent runtime", "worker", "drain", "闃熷垪", "鎵ц闃熷垪")
    drain_markers = ("worker drain", "drain queue", "runtime drain", "drain", "执行队列", "推进队列", "跑队列", "消费队列")
    return any(marker in normalized for marker in runtime_markers) and any(
        marker in normalized for marker in drain_markers
    )


def runtime_worker_drain_limit_from_text(text: str) -> int:
    normalized = str(text or "").strip().lower()
    if any(token in normalized for token in ("鍏ㄩ儴", "鍏ㄩ噺", "鍓╀綑", "all", "rest")):
        return 1000
    return 1


def is_runtime_provider_status_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    provider_markers = (
        "provider", "adapter", "runtime preflight", "runtime provider",
        "provider status", "真实provider", "真实 provider", "适配器", "预检",
        "通道", "真实通道", "接上",
    )
    runtime_markers = ("runtime", "agentruntime", "agent runtime")
    return any(marker in normalized for marker in provider_markers) and any(
        marker in normalized for marker in runtime_markers
    )


def is_runtime_enqueue_generation_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    runtime_markers = ("runtime", "agentruntime", "agent runtime")
    enqueue_markers = (
        "confirm_and_enqueue", "enqueue generation", "runtime enqueue", "鍏ラ槦",
        "运行时", "纭鍏ラ槦", "鎺掑叆闃熷垪",
    )
    generation_markers = ("generate", "generation", "鐢熸垚", "鎵ц", "start")
    return any(marker in normalized for marker in runtime_markers) and any(
        marker in normalized for marker in enqueue_markers
    ) and any(marker in normalized for marker in generation_markers)


def is_runtime_engine_write_status_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    engine_markers = (
        "engine write", "engine bridge", "runtime engine", "actor import",
        "layout transform", "import provider", "transform provider", "寮曟搸鍐欏叆",
        "鐪熷疄瀵煎叆", "鐪熷疄鍐欏叆", "瀵煎叆閫氶亾", "鍙樻崲閫氶亾", "鍐欏叆閫氶亾",
    )
    runtime_markers = ("runtime", "engine", "provider", "adapter", "寮曟搸", "瀵煎叆", "鍐欏叆", "閫氶亾")
    return any(marker in normalized for marker in engine_markers) and any(
        marker in normalized for marker in runtime_markers
    )


def is_runtime_scene_snapshot_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    snapshot_markers = (
        "scene snapshot", "runtime scene snapshot", "snapshot status", "refresh scene snapshot",
        "鍦烘櫙蹇収", "鍒锋柊鍦烘櫙蹇収", "褰撳墠鍦烘櫙蹇収", "寮曟搸蹇収",
        "actor蹇収", "actor 蹇収",
    )
    runtime_markers = ("runtime", "agentruntime", "agent runtime", "鍦烘櫙蹇収", "寮曟搸蹇収", "actor蹇収", "actor 蹇収")
    return any(marker in normalized for marker in snapshot_markers) and any(
        marker in normalized for marker in runtime_markers
    )


def is_runtime_tool_manifest_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    manifest_markers = (
        "tool manifest", "tool capabilities", "runtime tools", "runtime tool",
        "工具清单", "工具能力", "可用工具", "能力清单",
    )
    runtime_markers = ("runtime", "agentruntime", "agent runtime", "工具", "tool")
    return any(marker in normalized for marker in manifest_markers) and any(
        marker in normalized for marker in runtime_markers
    )


def is_runtime_operation_replay_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    replay_markers = (
        "operation replay", "operation log", "runtime replay", "runtime operation",
        "鎵ц鍥炴斁", "鎿嶄綔鍥炴斁", "鎿嶄綔鏃ュ織", "澶嶇洏鏃ュ織", "杩愯鏃ュ織",
    )
    runtime_markers = ("runtime", "agentruntime", "agent runtime", "鍥炴斁", "鏃ュ織", "澶嶇洏")
    return any(marker in normalized for marker in replay_markers) and any(
        marker in normalized for marker in runtime_markers
    )


def is_runtime_report_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    report_markers = (
        "runtime report", "runtime final report", "agent runtime report", "final report",
        "generate report", "show report", "report summary", "最终报告", "鐢熸垚鎶ュ憡",
        "鎶ュ憡鎽樿", "鏌ョ湅鎶ュ憡", "杩愯鎶ュ憡", "runtime 鎶ュ憡",
    )
    runtime_markers = ("runtime", "agentruntime", "agent runtime", "鎶ュ憡", "report")
    return any(marker in normalized for marker in report_markers) and any(
        marker in normalized for marker in runtime_markers
    )


def is_runtime_sync_status_query(text: str) -> bool:
    normalized = str(text or "").strip().lower()
    if not normalized:
        return False
    sync_markers = (
        "sync status", "runtime sync", "sync summary", "运行时", "鍚屾鎽樿",
        "澶氫汉鍚屾", "鑱旀満鍚屾", "actor鍚屾", "actor 鍚屾", "璧勬簮鍚屾",
    )
    runtime_markers = ("runtime", "agentruntime", "agent runtime", "鍚屾", "sync")
    return any(marker in normalized for marker in sync_markers) and any(
        marker in normalized for marker in runtime_markers
    )


def _is_gm_target_trigger(trigger: dict[str, Any]) -> bool:
    agent_id = str((trigger or {}).get("agent_id") or (trigger or {}).get("target_agent_id") or "").strip().lower()
    agent_name = str((trigger or {}).get("agent_name") or (trigger or {}).get("target_agent_name") or "").strip().lower()
    return agent_id == "gm" or agent_name in {"gm", "主持人", "裁判", "game master"}


def is_runtime_r3_gate_query(trigger: dict[str, Any]) -> bool:
    text = str((trigger or {}).get("text") or "").strip()
    if not text:
        return False
    normalized = text.lower().replace(" ", "")
    is_gm_target = _is_gm_target_trigger(trigger) or normalized.startswith("@gm")
    if not is_gm_target:
        return False
    return any(token in normalized for token in (
        "r3门禁", "r3gate", "r3readiness", "game-ready门禁", "gameready门禁", "就绪门禁",
    ))


def is_runtime_gm_summary_query(trigger: dict[str, Any]) -> bool:
    text = str((trigger or {}).get("text") or "").strip()
    if not text:
        return False
    is_gm_target = _is_gm_target_trigger(trigger) or text.startswith("@GM")
    if not is_gm_target:
        return False
    summary_tokens = ("总结", "整理", "汇总", "当前方案", "当前共识", "复盘", "gm summary", "runtime summary")
    status_only_tokens = ("进度", "到哪", "到哪里", "什么情况", "现在情况", "生成到哪里")
    return any(word in text for word in summary_tokens) and not any(
        word in text for word in status_only_tokens
    )


def is_runtime_status_summary_query(trigger: dict[str, Any]) -> bool:
    text = str((trigger or {}).get("text") or "").strip()
    if not text:
        return False
    is_gm_target = _is_gm_target_trigger(trigger) or text.startswith("@GM")
    if not is_gm_target:
        return False
    return any(word in text for word in ("运行时", "进度", "到哪", "到哪里", "什么情况", "现在情况"))


def is_runtime_status_query_text(text: str) -> bool:
    value = str(text or "").strip()
    if not value:
        return False
    try:
        from .intent_understanding import IntentUnderstandingService

        decision = IntentUnderstandingService().classify(
            value,
            allow_llm=False,
            generation_active=False,
        )
        return decision.intent == "status_query"
    except Exception:  # noqa: BLE001
        return any(word in value for word in (
            "到哪步", "到哪一步", "到哪里", "生成到哪里", "生成情况", "查看生成情况",
            "运行时", "运行时", "了解现在的生成方案", "我们开始生成了吗", "现在情况", "什么情况",
            "情况是什么", "生成计划是什么", "为什么执行生成计划", "现在生成到哪里",
        ))
